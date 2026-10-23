#include "world/npc_senses.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "world/npc_senses_code.h"

#include <Unreal/NameTypes.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>

namespace lss::world::npc_senses {

namespace {

namespace code = npc_senses_code;
using platform::Read;
using platform::ReadPointer;
using players::kSeatsBuilt;
using players::SeatBuilt;
using players::SeatName;

constexpr int kMaxPlayers = players::scopes::kMaxPlayers;
static_assert(kSeatsBuilt <= code::kMaxOthers, "the centre choice holds every built seat");

using UpdateFn = void (*)(void* intention, void* context);

constexpr auto kSummaryEvery   = std::chrono::seconds(60);
constexpr int  kMaxCognitions  = 512;    // an NPC's known characters (read live: up to ~30)
constexpr float kSeenNow       = 1e-8f;  // the look's own test: a timer at or under it was seen this update
constexpr float kSetAside      = 1.0f;   // a timer the look skips, for the one call

// What the grid is doing this tick (the reading and the log name it).
enum class Grid : std::uint8_t
{
    Off,         // switched off, no further player in, or a character unread: the game's centre
    Battle,      // the game's centre (the battle heat map)
    Inside,      // the game's centre: every further player held sits within reach of it (each player's fit says which)
    Shifted,     // moved toward a further player (each player's fit says who is held)
    TooFar,      // the game's centre: no further player held, the first too far to hold with player 1
    OtherFloor,  // the game's centre: no further player held, the first on another floor
};

const wchar_t* GridText(Grid grid) noexcept
{
    switch (grid)
    {
    case Grid::Off: return L"on player 1 (no further player)";
    case Grid::Battle: return L"on player 1 (battle)";
    case Grid::Inside: return L"on player 1";
    case Grid::Shifted: return L"moved toward a further player";
    case Grid::TooFar: return L"on player 1, a further player too far to hold with player 1";
    case Grid::OtherFloor: return L"on player 1, a further player on another floor";
    }
    return L"?";
}

const wchar_t* FitText(code::Fit fit) noexcept
{
    switch (fit)
    {
    case code::Fit::Inside: return L"inside";
    case code::Fit::Shifted: return L"reached by the move";
    case code::Fit::TooFar: return L"too far, left out";
    case code::Fit::OtherFloor: return L"on another floor, left out";
    }
    return L"?";
}

platform::CodeRedirect        g_centreCode;
alignas(16) code::CentreState g_centre;  // read by the grid code (game thread)
platform::VTableEntry         g_lookSlot;
UpdateFn                      g_update = nullptr;
std::uint8_t**                g_root   = nullptr;  // &g_EndGameRoot

// Written by the game thread; the look reads them on the AI's worker threads (research/RE-npc-reactions.md §10.5).
std::atomic<bool>  g_enabled{true};
std::atomic<void*> g_one{nullptr};  // player 1's character
std::array<std::atomic<void*>, kMaxPlayers> g_members{};  // each seat's character while in; [0] unused
unsigned long g_gameThread = 0;
Grid          g_grid       = Grid::Off;
code::Cell    g_centreNow;
std::array<code::Fit, kMaxPlayers> g_fits{};  // each seat's fit at the last decision (game thread; the reading)
std::array<bool, kMaxPlayers>      g_fitRead{};  // the seat had a readable character at the last decision

std::wstring  g_fitsLogged;        // the fits as last logged (game thread)
std::uint64_t g_shiftedTicks = 0;  // game thread
std::atomic<std::uint64_t> g_bothSeen{0}, g_oneFirst{0}, g_gameAfter{0}, g_offThread{0}, g_oneFirstTotal{0};
std::wstring  g_summary = L"no summary yet";
std::chrono::steady_clock::time_point g_lastSummary = std::chrono::steady_clock::now();

void NameText(std::int64_t bits, std::wstring* out) noexcept
try
{
    *out = RC::Unreal::FName(bits).ToString();
}
catch (...)
{
    out->clear();
}

std::wstring ObjectName(const void* object) noexcept
{
    std::int64_t bits = 0;
    std::wstring text;
    if (!Read(static_cast<const std::uint8_t*>(object) + 0x18, bits) ||
        !platform::CallGuarded(&NameText, bits, &text) || text.empty())
    {
        return L"?";
    }
    return text;
}

bool Location(void* character, float (&out)[3]) noexcept
{
    void* root = ReadPointer(character, facts::kActorRootComponent);
    return root != nullptr && Read(static_cast<const std::uint8_t*>(root) + facts::kComponentWorldLocation, out);
}

bool InBattle() noexcept
{
    std::uint8_t state = 0;
    void*        owner = ReadPointer(g_root != nullptr ? *g_root : nullptr, facts::kEndGameRootCharaOwner);
    return owner != nullptr && Read(static_cast<const std::uint8_t*>(owner) + facts::kRootFieldBattleState, state) &&
           state == facts::kFieldBattleStateBattle;
}

/// The grid's state for this tick, and the centre to write when it moved; each seat's fit kept for the reading.
Grid Decide(code::Cell& centre) noexcept
{
    g_fitRead = {};
    float one[3]{};
    if (!g_enabled || g_one.load() == nullptr || !Location(g_one.load(), one))
    {
        return Grid::Off;
    }
    // Every seat with a readable character, in seat order (the lower seat held first by the rule).
    std::array<code::Cell, code::kMaxOthers> cells{};
    std::array<int, code::kMaxOthers>        seatOf{};
    int                                      count = 0;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        void* member = g_members[static_cast<std::size_t>(seat)].load();
        float at[3]{};
        if (member != nullptr && Location(member, at))
        {
            cells[static_cast<std::size_t>(count)]  = code::CellOf(at);
            seatOf[static_cast<std::size_t>(count)] = seat;
            ++count;
        }
    }
    if (count == 0)
    {
        return Grid::Off;
    }
    if (InBattle())
    {
        return Grid::Battle;
    }
    const auto choice = code::ChooseCentre(code::CellOf(one), cells.data(), count);
    centre            = choice.centre;
    for (int i = 0; i < count; ++i)
    {
        const int seat = seatOf[static_cast<std::size_t>(i)];
        g_fits[static_cast<std::size_t>(seat)]    = choice.fits[static_cast<std::size_t>(i)];
        g_fitRead[static_cast<std::size_t>(seat)] = true;
    }
    switch (choice.fit)
    {
    case code::Fit::Inside: return Grid::Inside;
    case code::Fit::Shifted: return Grid::Shifted;
    case code::Fit::TooFar: return Grid::TooFar;
    case code::Fit::OtherFloor: return Grid::OtherFloor;
    }
    return Grid::Off;
}

// --- Player 1 first in the look ---------------------------------------------------------------------------------------

struct SetAside
{
    std::uint8_t* entry;
    float         timer;
};

/// The look's own candidate test, without its reach (the reach needs the NPC's head, which only the game measures).
bool Candidate(float timer, float interest, float experience, float threshold) noexcept
{
    return timer <= kSeenNow && interest >= threshold && interest - experience > 0.0f;
}

/// Any thread: the look runs in each NPC's own AI tick, on worker threads, one NPC never twice at once. Only this
/// NPC's cognition entries are touched, around its own look's single call, and never while a look is running (so the
/// look's current target is never set aside); the save and restore are exact (research/RE-npc-reactions.md §10.5).
void Update(void* intention, void* context) noexcept
{
    void* const one = g_one.load(std::memory_order_acquire);  // this call's snapshot of the players
    std::array<void*, kMaxPlayers> members{};
    bool                           anyMember = false;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        members[static_cast<std::size_t>(seat)] = g_members[static_cast<std::size_t>(seat)].load(std::memory_order_acquire);
        anyMember = anyMember || members[static_cast<std::size_t>(seat)] != nullptr;
    }
    if (!g_enabled.load(std::memory_order_relaxed) || one == nullptr || !anyMember)
    {
        g_update(intention, context);
        return;
    }
    if (GetCurrentThreadId() != g_gameThread)
    {
        g_offThread.fetch_add(1, std::memory_order_relaxed);  // counted only: the worker threads are the look's own
    }
    auto* const   self    = static_cast<std::uint8_t*>(intention);
    std::uint8_t  looking = 1;
    std::uint32_t serial  = 1;
    float         threshold = 0.0f;
    auto* const   setting = static_cast<std::uint8_t*>(ReadPointer(self, facts::kCuriositySetting));
    auto* const   board   = static_cast<std::uint8_t*>(ReadPointer(context, facts::kAiContextBoard));
    auto* const   map     = board != nullptr ? board + facts::kBoardCognitions : nullptr;
    auto* const   data    = static_cast<std::uint8_t*>(ReadPointer(map, 0));
    std::int32_t  num = 0, bitCount = 0;
    if (!Read(self + facts::kCuriosityLooking, looking) || looking != 0 ||
        !Read(self + facts::kCuriosityTargetSerial, serial) || serial != 0 || setting == nullptr ||
        !Read(setting + facts::kCuriosityInterestThreshold, threshold) || data == nullptr ||
        !Read(map + facts::kSparseNum, num) || !Read(map + facts::kSparseBitCount, bitCount))
    {
        g_update(intention, context);  // a look running, or nothing readable: the game's pick
        return;
    }
    const int count = std::min({num, bitCount, kMaxCognitions});
    auto*     bits  = static_cast<std::uint8_t*>(ReadPointer(map, facts::kSparseHeapBits));
    std::array<std::uint32_t, kMaxCognitions / 32> words{};
    if (count <= 0 || !platform::CopyGuarded(words.data(), bits != nullptr ? bits : map + facts::kSparseInlineBits,
                                             static_cast<std::size_t>((count + 31) / 32) * sizeof(std::uint32_t)))
    {
        g_update(intention, context);
        return;
    }
    // Player 1 and a further player must both be candidates for this NPC this update; every other candidate is set
    // aside for the one call.
    std::array<SetAside, kMaxCognitions> aside{};
    int  asideCount = 0;
    bool oneSeen    = false;
    int  seatSeen   = 0;  // the lowest seat whose character is a candidate (0 none)
    for (int i = 0; i < count; ++i)
    {
        if ((words[i / 32] >> (i % 32) & 1u) == 0)
        {
            continue;
        }
        std::uint8_t* const entry = data + static_cast<std::size_t>(i) * facts::kCognitionBytes;
        void*               actor = nullptr;
        float               timer = 1.0f, interest = 0.0f, experience = 0.0f;
        if (!Read(entry, actor) || !Read(entry + facts::kCognitionTimer, timer) ||
            !Read(entry + facts::kCognitionInterest, interest) || !Read(entry + facts::kCognitionExperience, experience) ||
            !Candidate(timer, interest, experience, threshold))
        {
            continue;
        }
        if (actor == one)
        {
            oneSeen = true;
            continue;
        }
        for (int seat = 1; seat <= kSeatsBuilt && seatSeen == 0; ++seat)
        {
            if (members[static_cast<std::size_t>(seat)] != nullptr && actor == members[static_cast<std::size_t>(seat)])
            {
                seatSeen = seat;  // the lowest seat whose character is a candidate
            }
        }
        aside[asideCount++] = {entry, timer};
    }
    if (!oneSeen || seatSeen == 0)
    {
        g_update(intention, context);  // the game's pick: player 1 is not competing with a further player here
        return;
    }
    g_bothSeen.fetch_add(1, std::memory_order_relaxed);
    int written = 0;
    for (; written < asideCount; ++written)
    {
        if (!platform::WriteGuarded(aside[written].entry + facts::kCognitionTimer, &kSetAside, sizeof(kSetAside)))
        {
            break;
        }
    }
    const bool allAside = written == asideCount;
    if (allAside)
    {
        g_update(intention, context);  // the game's pick with player 1 the only candidate
    }
    for (int i = 0; i < written; ++i)
    {
        platform::WriteGuarded(aside[i].entry + facts::kCognitionTimer, &aside[i].timer, sizeof(float));
    }
    serial = 0;
    if (allAside && Read(self + facts::kCuriosityTargetSerial, serial) && serial != 0)
    {
        g_oneFirst.fetch_add(1, std::memory_order_relaxed);
        g_oneFirstTotal.fetch_add(1, std::memory_order_relaxed);
        platform::LogDebug(L"npc senses: {} looks at player 1 first ({} was in its sight too)",
                           ObjectName(ReadPointer(context, facts::kAiContextCharacter)), SeatName(seatSeen));
        return;
    }
    g_gameAfter.fetch_add(1, std::memory_order_relaxed);
    g_update(intention, context);  // player 1 out of the look's reach: the game's own pick
}

void Summarise() noexcept
try
{
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastSummary < kSummaryEvery)
    {
        return;
    }
    g_lastSummary = now;
    const std::uint64_t bothSeen = g_bothSeen.exchange(0), oneFirst = g_oneFirst.exchange(0);
    const std::uint64_t gameAfter = g_gameAfter.exchange(0), offThread = g_offThread.exchange(0);
    if (g_shiftedTicks == 0 && bothSeen == 0 && offThread == 0)
    {
        return;
    }
    g_summary = std::format(L"last minute: grid moved toward a further player on {} ticks, {} grid writes in all; player 1 "
                            L"and a further player in one NPC's sight {} times, player 1 looked at first {}, the game's own "
                            L"pick after {}; looks run off the game thread {}",
                            g_shiftedTicks, g_centre.writes, bothSeen, oneFirst, gameAfter, offThread);
    platform::LogDebug(L"npc senses: {}", g_summary);
    g_shiftedTicks = 0;
}
catch (...)
{
}

/// Each further player's fit at the last decision, for the reading and the log (empty when none was decided).
std::wstring FitsText() noexcept
try
{
    std::wstring out;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (g_fitRead[static_cast<std::size_t>(seat)])
        {
            out += std::format(L"{}{} {}", out.empty() ? L"" : L", ", SeatName(seat), FitText(g_fits[static_cast<std::size_t>(seat)]));
        }
    }
    return out;
}
catch (...)
{
    return {};
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto root     = image.Rebase(facts::kEndGameRootGlobal, sizeof(void*));
    const auto site     = image.Rebase(code::kCentreSite, sizeof(code::kCentreLoad));
    const auto resume   = image.Rebase(code::kCentreResume);
    const auto cellSize = image.Rebase(code::kCellSizeGlobal, sizeof(float));
    g_root              = root ? reinterpret_cast<std::uint8_t**>(*root) : nullptr;
    bool grid           = false;
    if (!root || !site || !resume || !cellSize || !image.BytesMatch(code::kCentreCheckStart, code::kCentreBytes))
    {
        platform::LogWarning(L"npc senses: the grid build is not what the research read; NPCs sense around player 1 only");
    }
    else if (!g_centreCode.Install(*site, code::kCentreLoad, code::CentreCode(g_centre, *cellSize), *resume))
    {
        platform::LogWarning(L"npc senses: the grid build could not be redirected; NPCs sense around player 1 only");
    }
    else
    {
        grid = true;
    }
    bool look = false;
    if (!platform::ReplaceSlot(image, facts::kCuriosityUpdateSlot, reinterpret_cast<std::uintptr_t>(&Update), g_lookSlot))
    {
        platform::LogWarning(L"npc senses: the look's slot differs; NPCs pick between the players as the game does");
    }
    else
    {
        g_update = reinterpret_cast<UpdateFn>(g_lookSlot.Original());
        look     = true;
    }
    if (!grid && !look)
    {
        return false;
    }
    control::Register({.name  = L"npc-senses",
                       .shape = control::Shape::OnOff,
                       .help  = L"NPCs see every further player as they see player 1, player 1's look first (world/npc_senses.h; on)",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled.load()}; }});
    control::Register({.name = L"world.npc-senses", .shape = control::Shape::Text,
                       .help = L"where the NPCs' sensing grid is, each further player's fit, and the looks given to player 1 first",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"npc senses: installed (grid {}, look {}); while a further player is in, NPCs can see every player, "
                  L"player 1 first", grid ? L"on" : L"off", look ? L"on" : L"off");
    return true;
}

void TickSeat(int seat, void* seatMember) noexcept
{
    if (!SeatBuilt(seat))
    {
        return;
    }
    g_members[static_cast<std::size_t>(seat)].store(seatMember, std::memory_order_release);
}

void Tick(void* memberOne) noexcept
{
    g_gameThread = GetCurrentThreadId();
    g_one.store(memberOne, std::memory_order_release);
    if (!g_centreCode.IsInstalled())
    {
        Summarise();
        return;
    }
    code::Cell centre;
    const Grid grid = Decide(centre);
    if (grid == Grid::Shifted)
    {
        // The cells first, then the switch: the grid code runs on this thread, later in the frame.
        g_centre.x  = centre.x;
        g_centre.y  = centre.y;
        g_centre.z  = centre.z;
        g_centre.on = 1;
        ++g_shiftedTicks;
        if (g_grid == Grid::Shifted && !(centre == g_centreNow))
        {
            platform::LogTrace(L"npc senses: the grid's centre now cell ({}, {}, {})", centre.x, centre.y, centre.z);
        }
        g_centreNow = centre;
    }
    else
    {
        g_centre.on = 0;
    }
    const std::wstring fits = FitsText();
    if (grid != g_grid || fits != g_fitsLogged)
    {
        // A state change, or one player's fit changing under the same state (a third player left out or held again).
        platform::LogDebug(L"npc senses: the grid is {}{}{}", GridText(grid),
                           grid == Grid::Shifted ? std::format(L" (centre cell ({}, {}, {}))", centre.x, centre.y, centre.z)
                                                 : std::wstring{},
                           fits.empty() ? std::wstring{} : L": " + fits);
        g_grid       = grid;
        g_fitsLogged = fits;
    }
    Summarise();
}

void SetEnabled(bool on) noexcept
{
    g_enabled = on;
    if (!on)
    {
        g_centre.on = 0;
    }
    platform::Log(L"npc senses {}", on ? L"on" : L"off (the grid stays on player 1, the look is the game's alone)");
}

std::wstring Reading() noexcept
try
{
    const std::wstring fits = FitsText();
    return std::format(L"{}; grid {}{}{}; looks given to player 1 first so far {}; {}", g_enabled.load() ? L"on" : L"off",
                       GridText(g_grid),
                       g_grid == Grid::Shifted ? std::format(L" (centre cell ({}, {}, {}))", g_centreNow.x, g_centreNow.y,
                                                             g_centreNow.z)
                                               : std::wstring{},
                       fits.empty() ? std::wstring{} : L" (" + fits + L")", g_oneFirstTotal.load(), g_summary);
}
catch (...)
{
    return L"?";
}

}  // namespace lss::world::npc_senses
