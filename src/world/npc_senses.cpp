#include "world/npc_senses.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
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

using UpdateFn = void (*)(void* intention, void* context);

constexpr auto kSummaryEvery   = std::chrono::seconds(60);
constexpr int  kMaxCognitions  = 512;    // an NPC's known characters (read live: up to ~30)
constexpr float kSeenNow       = 1e-8f;  // the look's own test: a timer at or under it was seen this update
constexpr float kSetAside      = 1.0f;   // a timer the look skips, for the one call

// What the grid is doing this tick (the reading and the log name it).
enum class Grid : std::uint8_t
{
    Off,         // switched off, player 2 out, or a character unread: the game's centre
    Battle,      // the game's centre (the battle heat map)
    Inside,      // player 2 within reach of player 1's centre
    Shifted,     // moved toward player 2
    TooFar,      // the game's centre: too far apart to hold both
    OtherFloor,  // the game's centre: more than a floor apart
};

const wchar_t* GridText(Grid grid) noexcept
{
    switch (grid)
    {
    case Grid::Off: return L"on player 1 (no player 2)";
    case Grid::Battle: return L"on player 1 (battle)";
    case Grid::Inside: return L"on player 1, player 2 inside";
    case Grid::Shifted: return L"moved toward player 2";
    case Grid::TooFar: return L"on player 1, player 2 too far to hold both";
    case Grid::OtherFloor: return L"on player 1, player 2 on another floor";
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
std::atomic<void*> g_two{nullptr};  // player 2's character, while in
unsigned long g_gameThread = 0;
Grid          g_grid       = Grid::Off;
code::Cell    g_centreNow;

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

/// The grid's state for this tick, and the centre to write when it moved.
Grid Decide(code::Cell& centre) noexcept
{
    float one[3]{}, two[3]{};
    if (!g_enabled || g_one.load() == nullptr || g_two.load() == nullptr || !Location(g_one.load(), one) ||
        !Location(g_two.load(), two))
    {
        return Grid::Off;
    }
    if (InBattle())
    {
        return Grid::Battle;
    }
    const auto choice = code::ChooseCentre(code::CellOf(one), code::CellOf(two));
    centre            = choice.centre;
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
    void* const two = g_two.load(std::memory_order_acquire);
    if (!g_enabled.load(std::memory_order_relaxed) || one == nullptr || two == nullptr)
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
    // Both players must be candidates for this NPC this update; every other candidate is set aside for the one call.
    std::array<SetAside, kMaxCognitions> aside{};
    int  asideCount = 0;
    bool oneSeen = false, twoSeen = false;
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
        twoSeen = twoSeen || actor == two;
        aside[asideCount++] = {entry, timer};
    }
    if (!oneSeen || !twoSeen)
    {
        g_update(intention, context);  // the game's pick: player 1 is not competing with player 2 here
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
        platform::LogDebug(L"npc senses: {} looks at player 1 first (player 2 was in its sight too)",
                           ObjectName(ReadPointer(context, facts::kAiContextCharacter)));
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
    g_summary = std::format(L"last minute: grid moved toward player 2 on {} ticks, {} grid writes in all; both players "
                            L"in one NPC's sight {} times, player 1 looked at first {}, the game's own pick after {}; "
                            L"looks run off the game thread {}",
                            g_shiftedTicks, g_centre.writes, bothSeen, oneFirst, gameAfter, offThread);
    platform::LogDebug(L"npc senses: {}", g_summary);
    g_shiftedTicks = 0;
}
catch (...)
{
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
                       .help  = L"NPCs see player 2 as they see player 1, player 1's look first (world/npc_senses.h; on)",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled.load()}; }});
    control::Register({.name = L"world.npc-senses", .shape = control::Shape::Text,
                       .help = L"where the NPCs' sensing grid is, and the looks given to player 1 first",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"npc senses: installed (grid {}, look {}); while player 2 is in, NPCs can see both players, player 1 "
                  L"first", grid ? L"on" : L"off", look ? L"on" : L"off");
    return true;
}

void Tick(void* memberOne, void* memberTwo) noexcept
{
    g_gameThread = GetCurrentThreadId();
    g_one.store(memberOne, std::memory_order_release);
    g_two.store(memberTwo, std::memory_order_release);
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
    if (grid != g_grid)
    {
        platform::LogDebug(L"npc senses: the grid is {}{}", GridText(grid),
                           grid == Grid::Shifted ? std::format(L" (centre cell ({}, {}, {}))", centre.x, centre.y, centre.z)
                                                 : std::wstring{});
        g_grid = grid;
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
    return std::format(L"{}; grid {}{}; looks given to player 1 first so far {}; {}", g_enabled.load() ? L"on" : L"off",
                       GridText(g_grid),
                       g_grid == Grid::Shifted ? std::format(L" (centre cell ({}, {}, {}))", g_centreNow.x, g_centreNow.y,
                                                             g_centreNow.z)
                                               : std::wstring{},
                       g_oneFirstTotal.load(), g_summary);
}
catch (...)
{
    return L"?";
}

}  // namespace lss::world::npc_senses
