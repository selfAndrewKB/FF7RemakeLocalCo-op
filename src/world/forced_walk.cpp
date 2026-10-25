#include "world/forced_walk.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "players/weak_object.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>

#include <windows.h>

namespace lss::world::forced_walk {

namespace {

using platform::Read;

using BeginEndFn       = std::uintptr_t (*)(void* runner);
using TickFn           = char (*)(void* runner, float dt);
using GetPartyLeaderFn = void* (*)(void* partyManager, std::uint8_t selector);

/// One story act that limits the leader's walk: its layer and which leader it writes.
struct Act
{
    const wchar_t* name;
    std::uint8_t   layer;
    std::uint8_t   selector;
};
enum ActIndex : std::size_t
{
    kScriptWalk,
    kGuideWalk,
    kAfterScene,
    kActCount
};
constexpr std::array<Act, kActCount> kActs{{
    {L"scripted walk", facts::kCapLayerScript, facts::kLeaderDefault},
    {L"guided walk", facts::kCapLayerGuide, facts::kLeaderDefault},
    {L"walk after a scene", facts::kCapLayerAfterScene, facts::kLeaderCurrent},
}};
constexpr std::array<std::uint8_t, 3> kCopiedLayers{facts::kCapLayerScript, facts::kCapLayerAfterScene, facts::kCapLayerGuide};

platform::VTableEntry g_scriptTick, g_scriptEnd, g_guideTick, g_guideEnd, g_afterBegin, g_afterTick, g_afterEnd;
GetPartyLeaderFn      g_getPartyLeader = nullptr;
void**                g_partyManager   = nullptr;
std::atomic<bool>     g_installed{false};       // all seven slots placed
bool                  g_enabled        = true;   // switch forced-walk
std::atomic<DWORD>    g_gameThread{0};

/// Each seat's member as it was last seen bound (so a leave clears the one given back).
std::array<players::weak_object::Weak, players::kSeatsBuilt + 1> g_held{};

struct Counts
{
    std::uint64_t followed  = 0;  // calls whose write was copied to the seats
    std::uint64_t notMine   = 0;  // calls that wrote another character than player 1's
    std::uint64_t ends      = 0;  // ends whose clear reached the seats
    std::uint64_t offThread = 0;
    std::uint64_t scoped    = 0;  // calls made while a seat's Leader scope was open (left alone)
};
std::array<Counts, kActCount> g_counts{};
std::uint64_t                 g_joinCopies = 0, g_leaveClears = 0;
std::array<bool, kActCount>   g_wroteOther{};  // the last call of the act wrote another character (logged once)

void* PartyManager() noexcept
{
    void* manager = nullptr;
    return Read(g_partyManager, manager) ? manager : nullptr;
}

/// Player 1's character: the party's current leader, read outside every scope (the acts run there too).
void* PlayerOne() noexcept
{
    void* manager = PartyManager();
    void* leader  = nullptr;
    return manager != nullptr && platform::CallGuarded(g_getPartyLeader, leader, manager, facts::kLeaderCurrent) ? leader : nullptr;
}

/// A seat's Leader scope is open: the party leader reads as that seat's member (never expected around an act; if it
/// ever is, nothing is copied rather than a seat's member being taken for player 1's).
bool AnySeatLeaderScope() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::scopes::OtherLeaderType(seat) != facts::kPlayerTypeNone)
        {
            return true;
        }
    }
    return false;
}

/// A seat's member, if it is a live object (a level change can leave a stale pointer for a tick).
void* LiveMember(int seat) noexcept
{
    void* member = players::scopes::Member(seat);
    return member != nullptr && players::weak_object::Live(member) ? member : nullptr;
}

struct Pair
{
    std::uint8_t move = facts::kMoveCapNone;
    std::uint8_t dash = facts::kDashCapNone;
};

bool ReadPair(const void* character, std::uint8_t layer, Pair& pair) noexcept
{
    const auto* bytes = static_cast<const std::uint8_t*>(character);
    return Read(bytes + facts::kCharacterMoveCaps + layer, pair.move) && Read(bytes + facts::kCharacterDashCaps + layer, pair.dash);
}

/// Writes one layer's pair on a member; true if it changed.
bool WritePair(void* character, std::uint8_t layer, const Pair& pair) noexcept
{
    Pair now;
    if (!ReadPair(character, layer, now) || (now.move == pair.move && now.dash == pair.dash))
    {
        return false;
    }
    auto* bytes = static_cast<std::uint8_t*>(character);
    return platform::WriteGuarded(bytes + facts::kCharacterMoveCaps + layer, &pair.move, 1) &&
           platform::WriteGuarded(bytes + facts::kCharacterDashCaps + layer, &pair.dash, 1);
}

const wchar_t* MoveName(std::uint8_t move) noexcept
{
    switch (move)
    {
    case 0:
        return L"walk";
    case 1:
        return L"brisk walk";
    case 2:
        return L"jog";
    case 3:
        return L"run";
    case 4:
        return L"free";
    case facts::kMoveCapNone:
        return L"none";
    }
    return L"?";
}

/// Writes `pair` on every live seat member but `one`; logs each change.
void ToSeats(ActIndex act, const Pair& pair, const void* one) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        void* member = LiveMember(seat);
        if (member != nullptr && member != one && WritePair(member, kActs[act].layer, pair))
        {
            if (pair.move == facts::kMoveCapNone && pair.dash == facts::kDashCapNone)
            {
                platform::LogDebug(L"forced walk: {}'s member's limit from the {} cleared, as player 1's", players::SeatName(seat),
                                   kActs[act].name);
            }
            else
            {
                platform::LogDebug(L"forced walk: {}'s member now limited as player 1's by the {}: {} (dash type {})",
                                   players::SeatName(seat), kActs[act].name, MoveName(pair.move), pair.dash);
            }
        }
    }
}

/// True if a call may be followed now (installed, the switch on, the game thread; a copy also needs no seat's Leader
/// scope open, a clear does not: the scoped seat's member is then what PlayerOne reads, and it is skipped).
bool MayFollow(Counts& counts, bool clear) noexcept
{
    if (!g_installed.load(std::memory_order_relaxed) || !g_enabled)
    {
        return false;
    }
    if (GetCurrentThreadId() != g_gameThread.load(std::memory_order_relaxed))
    {
        ++counts.offThread;  // the acts run on the game thread (research §2); a call elsewhere is left alone
        return false;
    }
    if (!clear && AnySeatLeaderScope())
    {
        ++counts.scoped;
        return false;
    }
    return true;
}

/// After the game's Begin or Tick: the layer it wrote on player 1's character, put on every further player's member.
void Follow(ActIndex act) noexcept
{
    Counts& counts = g_counts[act];
    if (!MayFollow(counts, false))
    {
        return;
    }
    void* manager = PartyManager();
    void* one     = PlayerOne();
    void* written = nullptr;
    if (manager == nullptr || one == nullptr ||
        !platform::CallGuarded(g_getPartyLeader, written, manager, kActs[act].selector) || written != one)
    {
        ++counts.notMine;
        if (!g_wroteOther[act])
        {
            g_wroteOther[act] = true;
            platform::LogDebug(L"forced walk: the {} wrote another character than player 1's; further players left as they are",
                               kActs[act].name);
        }
        return;
    }
    g_wroteOther[act] = false;
    Pair pair;
    if (ReadPair(one, kActs[act].layer, pair))
    {
        ++counts.followed;
        platform::LogTrace(L"forced walk: the {} holds player 1 at {} (dash type {})", kActs[act].name, MoveName(pair.move), pair.dash);
        ToSeats(act, pair, one);
    }
}

/// After the game's End: its clear (the layer's none) on every further player's member, whichever character the game
/// wrote (player 1 may have switched members since the act began; an act of the same kind still running copies its
/// own value again at its next Tick).
void FollowEnd(ActIndex act) noexcept
{
    Counts& counts = g_counts[act];
    if (!MayFollow(counts, true))
    {
        return;
    }
    ++counts.ends;
    g_wroteOther[act] = false;
    ToSeats(act, Pair{}, PlayerOne());
}

template <ActIndex A>
std::uintptr_t Begin(platform::VTableEntry& slot, void* runner)
{
    const std::uintptr_t result = reinterpret_cast<BeginEndFn>(slot.Original())(runner);
    Follow(A);
    return result;
}

template <ActIndex A>
std::uintptr_t End(platform::VTableEntry& slot, void* runner)
{
    const std::uintptr_t result = reinterpret_cast<BeginEndFn>(slot.Original())(runner);
    FollowEnd(A);
    return result;
}

template <ActIndex A>
char Tick(platform::VTableEntry& slot, void* runner, float dt)
{
    const char result = reinterpret_cast<TickFn>(slot.Original())(runner, dt);
    Follow(A);
    return result;
}

std::uintptr_t ScriptWalkEnd(void* runner) { return End<kScriptWalk>(g_scriptEnd, runner); }
char           ScriptWalkTick(void* runner, float dt) { return Tick<kScriptWalk>(g_scriptTick, runner, dt); }
std::uintptr_t GuideWalkEnd(void* runner) { return End<kGuideWalk>(g_guideEnd, runner); }
char           GuideWalkTick(void* runner, float dt) { return Tick<kGuideWalk>(g_guideTick, runner, dt); }
std::uintptr_t AfterSceneBegin(void* runner) { return Begin<kAfterScene>(g_afterBegin, runner); }
char           AfterSceneTick(void* runner, float dt) { return Tick<kAfterScene>(g_afterTick, runner, dt); }
std::uintptr_t AfterSceneEnd(void* runner) { return End<kAfterScene>(g_afterEnd, runner); }

/// A member taken: player 1's current story walk limits put on it.
void Take(int seat, void* member) noexcept
{
    void* one = PlayerOne();
    if (one == nullptr || member == one)
    {
        return;
    }
    bool changed = false;
    for (const std::uint8_t layer : kCopiedLayers)
    {
        Pair pair;
        changed = (ReadPair(one, layer, pair) && WritePair(member, layer, pair)) || changed;
    }
    if (changed)
    {
        ++g_joinCopies;
        platform::LogDebug(L"forced walk: {}'s member took player 1's story walk limit", players::SeatName(seat));
    }
}

/// The story walk limits cleared on a member no player holds now (player 1's own and another seat's are left).
bool Clear(void* member) noexcept
{
    if (member == nullptr || member == PlayerOne())
    {
        return false;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::scopes::Member(seat) == member)
        {
            return false;  // another seat took it (a swap between seats): that seat's take keeps it right
        }
    }
    bool changed = false;
    for (const std::uint8_t layer : kCopiedLayers)
    {
        changed = WritePair(member, layer, Pair{}) || changed;
    }
    return changed;
}

void GiveBack(int seat, void* member) noexcept
{
    if (Clear(member))
    {
        ++g_leaveClears;
        platform::LogDebug(L"forced walk: the member {} gave back has its story walk limit cleared", players::SeatName(seat));
    }
}

void SetEnabled(bool on) noexcept
{
    if (on == g_enabled)
    {
        return;
    }
    g_enabled = on;
    int cleared = 0;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        void* member = players::weak_object::Resolve(g_held[seat]);
        if (on && member != nullptr)
        {
            Take(seat, member);
        }
        else if (!on && member != nullptr && member != PlayerOne())
        {
            // Off: each held member's copy cleared now (written directly: Clear leaves a member a seat holds).
            bool changed = false;
            for (const std::uint8_t layer : kCopiedLayers)
            {
                changed = WritePair(member, layer, Pair{}) || changed;
            }
            cleared += changed ? 1 : 0;
        }
    }
    if (on)
    {
        platform::Log(L"forced walk: on: further players walk where the story makes player 1 walk");
    }
    else
    {
        platform::Log(L"forced walk: off: further players run free ({} member(s) cleared)", cleared);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto leader  = platform::FindFunction(image, facts::kGetPartyLeader);
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    if (!leader || !manager || !players::weak_object::Installed())
    {
        platform::LogWarning(L"forced walk: a check failed; further players run free in a story walk");
        return false;
    }
    g_getPartyLeader = reinterpret_cast<GetPartyLeaderFn>(*leader);
    g_partyManager   = reinterpret_cast<void**>(*manager);
    struct Place
    {
        const platform::SlotFact& fact;
        std::uintptr_t            thunk;
        platform::VTableEntry&    entry;
    };
    const std::array<Place, 7> places{{
        {facts::kActScriptWalkTick, reinterpret_cast<std::uintptr_t>(&ScriptWalkTick), g_scriptTick},
        {facts::kActScriptWalkEnd, reinterpret_cast<std::uintptr_t>(&ScriptWalkEnd), g_scriptEnd},
        {facts::kActGuideWalkTick, reinterpret_cast<std::uintptr_t>(&GuideWalkTick), g_guideTick},
        {facts::kActGuideWalkEnd, reinterpret_cast<std::uintptr_t>(&GuideWalkEnd), g_guideEnd},
        {facts::kActAfterSceneBegin, reinterpret_cast<std::uintptr_t>(&AfterSceneBegin), g_afterBegin},
        {facts::kActAfterSceneTick, reinterpret_cast<std::uintptr_t>(&AfterSceneTick), g_afterTick},
        {facts::kActAfterSceneEnd, reinterpret_cast<std::uintptr_t>(&AfterSceneEnd), g_afterEnd},
    }};
    int placed = 0;
    for (const Place& place : places)
    {
        placed += platform::ReplaceSlot(image, place.fact, place.thunk, place.entry) ? 1 : 0;
    }
    // An act whose set or clear could not be followed would leave a member limited or free on its own: all or none.
    if (placed != static_cast<int>(places.size()))
    {
        Remove();
        platform::LogWarning(L"forced walk: {} of {} act slots held the researched function; none kept, further players run free in a "
                             L"story walk",
                             placed, places.size());
        return false;
    }
    g_installed.store(true, std::memory_order_relaxed);
    control::Register({.name  = L"forced-walk",
                       .shape = control::Shape::OnOff,
                       .help  = L"further players walk where the story makes player 1 walk (world/forced_walk.h; on); off: they run "
                                L"free",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"world.forced-walk", .shape = control::Shape::Text,
                       .help = L"(read only) player 1's and each further player's member's story walk limits, and the acts followed",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"forced walk: installed; further players walk where the story makes player 1 walk (scripted, guided and "
                  L"after-scene walks)");
    return true;
}

void Remove() noexcept
{
    g_installed.store(false, std::memory_order_relaxed);
    for (platform::VTableEntry* entry : {&g_scriptTick, &g_scriptEnd, &g_guideTick, &g_guideEnd, &g_afterBegin, &g_afterTick, &g_afterEnd})
    {
        entry->Restore();
    }
}

void TickSeat(int seat, void* member) noexcept
{
    if (!g_installed.load(std::memory_order_relaxed) || seat < 1 || seat > players::kSeatsBuilt)
    {
        return;
    }
    if (member != nullptr && !players::weak_object::Live(member))
    {
        return;  // a stale pointer for a tick after a level change: nothing read or written through it
    }
    g_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    players::weak_object::Weak& held = g_held[seat];
    void*                       was  = players::weak_object::Resolve(held);
    if (member == was)
    {
        return;
    }
    if (g_enabled)
    {
        GiveBack(seat, was);
    }
    held = member != nullptr ? players::weak_object::MakeWeak(member) : players::weak_object::Weak{};
    if (member != nullptr && !held.Set())
    {
        platform::LogDebug(L"forced walk: {}'s member could not be named for later (its leave will not clear its copy)",
                           players::SeatName(seat));
    }
    if (g_enabled && member != nullptr)
    {
        Take(seat, member);
    }
}

std::wstring Reading() noexcept
try
{
    const auto limits = [](const void* character) {
        std::wstring out;
        for (const std::uint8_t layer : kCopiedLayers)
        {
            Pair pair;
            out += std::format(L"{}L{} {}", out.empty() ? L"" : L" ", layer,
                               character != nullptr && ReadPair(character, layer, pair)
                                   ? std::format(L"{}/{}", MoveName(pair.move), pair.dash)
                                   : std::wstring(L"?"));
        }
        return out;
    };
    if (!g_installed.load(std::memory_order_relaxed))
    {
        return L"not installed";
    }
    std::wstring out = std::format(L"switch {}; player 1: {}", g_enabled ? L"on" : L"off", limits(PlayerOne()));
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (void* member = LiveMember(seat))
        {
            out += std::format(L"; {}: {}", players::SeatName(seat), limits(member));
        }
    }
    for (std::size_t act = 0; act < kActCount; ++act)
    {
        const Counts& c = g_counts[act];
        out += std::format(L"; {}: {} followed, {} ends, {} on another character, {} off the game thread, {} in a seat's scope",
                           kActs[act].name, c.followed, c.ends, c.notMine, c.offThread, c.scoped);
    }
    out += std::format(L"; takes copied {}, give-backs cleared {}", g_joinCopies, g_leaveClears);
    return out;
}
catch (...)
{
    return L"unread";
}

}  // namespace lss::world::forced_walk
