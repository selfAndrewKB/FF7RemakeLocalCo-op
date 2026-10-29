#include "world/field_targets.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/byte_scope.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/action_key.h"
#include "players/field_root.h"
#include "players/pad_buttons.h"
#include "players/party_member.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "world/target_icons.h"
#include "world/talk_presses.h"
#include "world/traversal_action.h"

#include <Unreal/NameTypes.hpp>

#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <format>
#include <string>

#include <Windows.h>

namespace lss::world::field_targets {

namespace {

using platform::Read;
using platform::ReadPointer;
using players::SeatName;

using TickFn       = void (*)(void* mgr, float dt);
using FirstLocalFn = void* (*)(void* world);
using BeginFn      = char (*)(void* runner);
using GateFn       = bool (*)();
using KeyTestFn    = bool (*)(void* key);
using FindRowFn    = void* (*)(const void* tables, std::uint64_t name);
using StartsWithFn = bool (*)(const std::uint64_t* name, const char* prefix);
using FreeFn       = void (*)(void* block);
using FindEntryFn  = void* (*)(void* mgr, const std::uint64_t* name);

constexpr int         kSeats   = players::scopes::kMaxPlayers;
constexpr std::size_t kItems   = facts::kTargetsItems;
constexpr std::size_t kPending = 16;  // chests a further player's press queued that have not begun yet

/// What a target is, by the dispatch's own order (research/RE-chest-two.md §8.1, RE-talk-two.md §2.2): chests, ground
/// items, NPCs, doors, curtains, grappling-hook points and the player's own swing are pressed for a further player.
enum class Kind : std::uint8_t
{
    None,
    Situation,  // a SituationTrigger row on its state: the situation system (player 1's)
    Layout,     // an ObjectLayout row: examine points, push-button events, battle starts (player 1's)
    Npc,        // an ObjectActor row: an NPC's talk (quest NPCs, shopkeepers): its state trigger
    Enemy,      // an ObjectEnemy row (player 1's)
    Chest,      // an ObjectTreasure row: a chest or a shining item on the ground
    Party,   // an ObjectParty row
    Door,      // Door_ / HeavyDoor_: the member's own door action
    Swing,     // TarzanJump_ with the member's own tag: its swing's press
    HookShot,  // HookShot_: the grappling hook (its target shows only while the member stands in its hook area)
    Curtain,   // Curtain_: a door-like pass-through (admitted as an experiment)
    Other,
};

const wchar_t* KindName(Kind kind) noexcept
{
    switch (kind)
    {
        case Kind::None: return L"nothing";
        case Kind::Situation: return L"a situation";
        case Kind::Layout: return L"an examine point or object event";
        case Kind::Npc: return L"an NPC";
        case Kind::Enemy: return L"an enemy object";
        case Kind::Chest: return L"a chest or item";
        case Kind::Party: return L"a party member";
        case Kind::Door: return L"a door";
        case Kind::Swing: return L"a swing";
        case Kind::HookShot: return L"a grappling-hook point";
        case Kind::Curtain: return L"a curtain";
        case Kind::Other: return L"another target";
    }
    return L"?";
}

/// Pressed for a further player now: a chest and an NPC always; a door, a curtain or the member's own swing only while
/// the member's field actions run as its player's (world/traversal_action.h), the grappling hook only while its three
/// lock calls are also redirected (traversal_action::HookShotReady); read at each use (a level change can stop that).
bool Admitted(Kind kind) noexcept
{
    switch (kind)
    {
        case Kind::Chest:
        case Kind::Npc: return true;
        case Kind::Door:
        case Kind::Swing:
        case Kind::Curtain: return traversal_action::Scoping();
        case Kind::HookShot: return traversal_action::HookShotReady();
        default: return false;
    }
}

/// A seat's own copy of the manager's per-player state, and what it last saw.
struct SeatState
{
    void*                                     localPlayer = nullptr;
    void*                                     member      = nullptr;
    platform::ByteContext                     copy;           // the four ranges at `copyOf`
    void*                                     copyOf = nullptr;
    std::array<std::uint8_t, kItems>          wasCurrent{};  // each pool entry's "was the current target" byte
    std::uint64_t                             target = 0, state = 0;  // the copy's current target when last classified
    Kind                                      kind   = Kind::None;
    std::array<std::uint64_t, kItems>         itemName{}, itemState{};  // each pool item as last classified for this seat
    std::array<Kind, kItems>                  itemKind{};
    std::uint32_t picks = 0, changes = 0, presses = 0, chests = 0, opens = 0, screens = 0;
    std::uint32_t leftToPlayerOne = 0;  // targets the last pick left to player 1 (marked disabled for the seat)
};

struct Pending
{
    std::uint64_t name = 0;
    int           seat = 0;
};

// Set at install, then only read.
std::uintptr_t g_key = 0;
std::array<std::uintptr_t, 6> g_tables{};  // situation, layout, actor, enemy, treasure, party
GateFn       g_gate       = nullptr;
KeyTestFn    g_held       = nullptr;
KeyTestFn    g_pressed    = nullptr;
FindRowFn    g_findRow    = nullptr;
StartsWithFn g_startsWith = nullptr;
FreeFn       g_free       = nullptr;
FindEntryFn  g_findEntry  = nullptr;
std::uintptr_t g_pickSite = 0, g_pickTarget = 0, g_screenSite = 0, g_screenTarget = 0;

platform::CallRedirect g_pick, g_press, g_screen;
platform::VTableEntry  g_begin;
bool                   g_installed = false;
bool                   g_placed    = false;  // the redirects inside the pick tried (on the game thread)
bool                   g_broken    = false;  // a further player's run raised: the passes are off

// Game thread.
std::array<SeatState, kSeats> g_seats{};
std::array<Pending, kPending> g_pending{};
talk_presses::Table           g_talkPresses;  // the further players' NPC presses, the rules in world/talk_presses.h
std::uint32_t                 g_talkClaims = 0, g_talkLapsed = 0, g_talkDropped = 0;
std::uint64_t                 g_playerOnePressed = 0;  // player 1's press in this Press call (the seats' runs follow it)
void*                         g_notesManager     = nullptr;  // the target manager the notes were made under
int                           g_pickSeat = 0;  // the seat whose pick runs now (the screen test answers with its player)
std::uint32_t                 g_playerOneOpens = 0;
bool                          g_pressSkipLogged = false;

constexpr std::size_t kRangeVisited = 0;  // the copy's ranges (FreshCopy's order)

void NameText(std::int64_t bits, std::wstring* out) noexcept
try
{
    *out = RC::Unreal::FName(bits).ToString();
}
catch (...)
{
    out->clear();
}

std::wstring Name(std::uint64_t bits) noexcept
{
    if (bits == 0)
    {
        return L"none";
    }
    std::wstring text;
    if (!platform::CallGuarded(&NameText, static_cast<std::int64_t>(bits), &text) || text.empty())
    {
        return std::format(L"name#{:x}", bits);
    }
    return text;
}

std::uint8_t* Root() noexcept
{
    return players::field_root::Root();
}

/// The chests waiting for their open: {data, count} of the state manager's list.
bool ChestQueue(const std::uint64_t*& data, std::int32_t& count) noexcept
{
    const void* list = ReadPointer(players::field_root::StateManager(), facts::kStateChestQueue);
    data             = static_cast<const std::uint64_t*>(ReadPointer(list, 0));
    count            = 0;
    return list != nullptr && Read(static_cast<const std::uint8_t*>(list) + 8, count) && count >= 0 && (count == 0 || data != nullptr);
}

std::uint64_t QueuedAt(const std::uint64_t* data, std::int32_t index) noexcept
{
    std::uint64_t name = 0;
    Read(data + index, name);
    return name;
}

/// A value in a seat's copy at a manager offset (inside one of the ranges), or zero.
template <class T>
T InCopy(const SeatState& state, std::uint32_t offset) noexcept
{
    T value{};
    for (std::size_t i = 0; i < state.copy.Count(); ++i)
    {
        const auto& range = state.copy.RangeAt(i);
        const auto  start = range.live - reinterpret_cast<std::uintptr_t>(state.copyOf);
        if (offset >= start && offset + sizeof(T) <= start + range.size)
        {
            std::memcpy(&value, state.copy.Value(i).data() + (offset - start), sizeof(T));
            break;
        }
    }
    return value;
}

/// Frees what the copy owns (the five heap blocks the manager's dtor frees) and forgets it. Not while open.
void DropCopy(SeatState& state) noexcept
{
    if (state.copy.IsOpen())
    {
        return;
    }
    for (const std::uint32_t offset : facts::kTargetsOwned)
    {
        void* block = InCopy<void*>(state, offset);
        if (block != nullptr && g_free != nullptr && !platform::CallGuarded(g_free, block))
        {
            platform::LogWarning(L"field targets: freeing a block of a further player's copy raised; it is left");
        }
    }
    state.copy.Clear();
    state.copyOf = nullptr;
    state.wasCurrent.fill(0);
    state.target = state.state = 0;
    state.kind   = Kind::None;
    state.itemName.fill(0);  // classified again (a swing is the member's own)
    state.itemState.fill(0);
    state.itemKind.fill(Kind::None);
}

/// A fresh copy at `mgr`, as the game's ctor leaves the state: all zero, the map's first free index -1.
bool FreshCopy(SeatState& state, void* mgr) noexcept
{
    DropCopy(state);
    const auto live = reinterpret_cast<std::uintptr_t>(mgr);
    using Range               = platform::ByteContext::Range;
    constexpr auto exchange   = platform::ByteContext::Kind::Exchange;
    const Range    ranges[]   = {{live + facts::kTargetsVisited, facts::kTargetsVisitedSize, exchange},
                                 {live + facts::kTargetsCurrent, facts::kTargetsCurrentSize, exchange},
                                 {live + facts::kTargetsHeld, 3, exchange},
                                 {live + facts::kTargetsSweepTimer, 4, exchange}};
    if (!state.copy.Define(ranges))
    {
        return false;
    }
    constexpr std::int32_t none = -1;
    std::memcpy(state.copy.Value(kRangeVisited).data() + (facts::kTargetsMapFirstFree - facts::kTargetsVisited), &none, sizeof(none));
    state.copyOf = mgr;
    return true;
}

/// The first `count` pool entries' "was the current target" byte exchanged with the seat's (twice puts both back).
/// Returns how many were exchanged (fewer than `count` if one faulted).
std::size_t ExchangeWasCurrent(std::uint8_t* mgr, std::uint8_t* own, std::size_t count) noexcept
{
    std::size_t done = 0;
    __try
    {
        for (; done < count; ++done)
        {
            std::uint8_t* live = mgr + facts::kTargetsPool + done * facts::kTargetsItemSize + facts::kTargetsItemWasCurrent;
            const std::uint8_t value = *live;
            *live                    = own[done];
            own[done]                = value;
        }
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
    }
    return done;
}

bool Found(std::size_t table, std::uint64_t name) noexcept
{
    void* row = nullptr;
    return name != 0 && platform::CallGuarded(g_findRow, row, reinterpret_cast<const void*>(g_tables[table]), name) && row != nullptr;
}

bool StartsWith(std::uint64_t name, const char* prefix) noexcept
{
    bool yes = false;
    return platform::CallGuarded(g_startsWith, yes, static_cast<const std::uint64_t*>(&name), prefix) && yes;
}

std::wstring Lower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

/// The swing named after this member ("TarzanJump_" and its first tag).
bool OwnSwing(std::uint64_t name, const void* member) noexcept
try
{
    const auto*   tags  = static_cast<const std::uint8_t*>(ReadPointer(member, facts::kActorTags));
    std::int32_t  count = 0;
    std::uint64_t tag   = 0;
    return tags != nullptr && Read(static_cast<const std::uint8_t*>(member) + facts::kActorTags + 8, count) && count > 0 &&
           Read(tags, tag) && tag != 0 && Lower(Name(name)) == Lower(L"TarzanJump_" + Name(tag));
}
catch (...)
{
    return false;
}

/// What the target is, in the dispatch's order (§8.5).
Kind Classify(std::uint64_t name, std::uint64_t state, const void* member) noexcept
{
    if (name == 0)
    {
        return Kind::None;
    }
    if (Found(0, state))
    {
        return Kind::Situation;
    }
    if (Found(1, name))
    {
        return Kind::Layout;
    }
    if (Found(2, name))
    {
        return Kind::Npc;
    }
    if (Found(3, name))
    {
        return Kind::Enemy;
    }
    if (Found(4, name))
    {
        return Kind::Chest;
    }
    if (Found(5, name))
    {
        return Kind::Party;
    }
    if (StartsWith(name, "Door_") || StartsWith(name, "HeavyDoor_"))
    {
        return Kind::Door;
    }
    if (StartsWith(name, "TarzanJump_"))
    {
        return OwnSwing(name, member) ? Kind::Swing : Kind::Other;
    }
    if (StartsWith(name, "HookShot_"))
    {
        return Kind::HookShot;
    }
    if (StartsWith(name, "Curtain_"))
    {
        return Kind::Curtain;
    }
    return Kind::Other;
}

/// The seat's current target, classified when it changed (logged).
void NoteTarget(int seat, SeatState& state) noexcept
{
    const auto name  = InCopy<std::uint64_t>(state, facts::kTargetsCurrent);
    const auto named = InCopy<std::uint64_t>(state, facts::kTargetsState);
    if (name == state.target && named == state.state)
    {
        return;
    }
    state.target = name;
    state.state  = named;
    state.kind   = Classify(name, named, state.member);
    ++state.changes;
    if (platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"field targets: {}'s target: {} ({})", SeatName(seat), Name(name), KindName(state.kind));
    }
}

/// For one run of the seat's pick: every active target the seat may not press is marked disabled (entry +0x94, the
/// pick's own rejection test), so the game gives the seat neither an icon nor a prompt for it (situations, examine
/// points and object events stay player 1's); put back after. Targets are classified once per name and state.
class LeftToPlayerOne
{
public:
    LeftToPlayerOne(SeatState& state, std::uint8_t* mgr) noexcept : mgr_(mgr)
    {
        std::bitset<kItems> active;
        if (!target_icons::ActiveItems(mgr, active))
        {
            return;  // a list read mid-change: this run marks nothing (its presses still pass the admit test)
        }
        for (std::size_t i = 0; i < kItems; ++i)
        {
            std::uint8_t* item  = mgr + facts::kTargetsPool + i * facts::kTargetsItemSize;
            std::uint64_t name  = 0, named = 0;
            std::uint8_t  off   = 0;
            if (!active.test(i) || !Read(item + facts::kTargetsItemName, name) || !Read(item + facts::kTargetsItemStateName, named))
            {
                continue;
            }
            if (name != state.itemName[i] || named != state.itemState[i])
            {
                state.itemName[i]  = name;
                state.itemState[i] = named;
                state.itemKind[i]  = Classify(name, named, state.member);
            }
            constexpr std::uint8_t disabled = 1;
            if (name == 0 || Admitted(state.itemKind[i]) || !Read(item + facts::kTargetsItemDisabled, off) || off != 0 ||
                !platform::WriteGuarded(item + facts::kTargetsItemDisabled, &disabled, 1))
            {
                continue;
            }
            marked_.set(i);
            names_[i] = name;
        }
        // The pick also applies a pending write of one entry's byte (the field-action component's, for the seat's copy's
        // name): that entry's byte is then the game's, not put back.
        std::uint64_t pending = 0;
        pending_     = Read(mgr + facts::kTargetsPendingEnable, pending) && pending != 0;
        pendingName_ = InCopy<std::uint64_t>(state, facts::kTargetsPendingName);
        state.leftToPlayerOne = static_cast<std::uint32_t>(marked_.count());
    }

    ~LeftToPlayerOne()
    {
        std::uint64_t pending = 1;
        const bool    applied = pending_ && Read(mgr_ + facts::kTargetsPendingEnable, pending) && pending == 0;
        for (std::size_t i = 0; marked_.any() && i < kItems; ++i)
        {
            std::uint8_t* item = mgr_ + facts::kTargetsPool + i * facts::kTargetsItemSize;
            std::uint8_t  off  = 0;
            if (!marked_.test(i) || (applied && names_[i] == pendingName_) || !Read(item + facts::kTargetsItemDisabled, off) || off != 1)
            {
                continue;
            }
            constexpr std::uint8_t enabled = 0;
            platform::WriteGuarded(item + facts::kTargetsItemDisabled, &enabled, 1);
        }
    }

    LeftToPlayerOne(const LeftToPlayerOne&)            = delete;
    LeftToPlayerOne& operator=(const LeftToPlayerOne&) = delete;

private:
    std::uint8_t*                     mgr_;
    std::bitset<kItems>               marked_;
    std::array<std::uint64_t, kItems> names_{};
    std::uint64_t                     pendingName_ = 0;
    bool                              pending_     = false;
};

/// The seat can run now: in, with its member, and its copy at this manager (a new manager drops the old copy and the
/// seat's icons; the manager is one object for the game's life, §8.3).
bool Ready(int seat, SeatState& state, void* mgr) noexcept
{
    if (g_broken || state.member == nullptr || state.localPlayer == nullptr || mgr == nullptr)
    {
        return false;
    }
    if (state.copyOf == mgr)
    {
        return true;
    }
    target_icons::DropSeat(seat);
    return FreshCopy(state, mgr);
}

void Break(int seat, const wchar_t* what) noexcept
{
    g_broken = true;
    for (int other = 1; other <= players::kSeatsBuilt; ++other)
    {
        target_icons::DropSeat(other);
    }
    platform::LogWarning(L"field targets: {}'s {} raised an exception; further players' prompts, icons and presses are off from here on",
                         SeatName(seat), what);
}

/// One seat's run of the game's pick on its own copy.
void PickFor(int seat, void* mgr, float dt, TickFn original) noexcept
{
    SeatState& state = g_seats[seat];
    if (!Ready(seat, state, mgr))
    {
        return;
    }
    bool ran = false, ok = true;
    {
        // The seat's own "no new action" share while its member acts (its targets dimmed, nothing to press, as the
        // game's leader's are; player 1's own action's share kept); opened before the Leader scope.
        const traversal_action::NoActionScope noAction(seat, false);
        const players::scopes::Scope scope(seat, players::scopes::Leader | players::scopes::Primary | players::scopes::UiSlot);
        if (!scope.Held())
        {
            return;
        }
        auto*                 live = static_cast<std::uint8_t*>(mgr);
        constexpr std::size_t all  = kItems;
        if (const std::size_t done = ExchangeWasCurrent(live, state.wasCurrent.data(), all); done != all)
        {
            ExchangeWasCurrent(live, state.wasCurrent.data(), done);  // the ones exchanged back
            Break(seat, L"entry swap");
            return;
        }
        {
            // The seat's own icons (its far-icon sweep, in its copy's timer, meets only those), its targets only.
            const LeftToPlayerOne        leftOut(state, live);
            const target_icons::SeatSwap icons(seat, mgr, state.localPlayer, true);
            const platform::ByteScope    swapped(state.copy);
            if (swapped.Held() && icons.Held())
            {
                g_pickSeat = seat;
                ok         = platform::CallGuarded(original, mgr, dt);
                g_pickSeat = 0;
                ran        = true;
            }
        }
        if (ExchangeWasCurrent(live, state.wasCurrent.data(), all) != all)
        {
            ok = false;  // player 1's bytes may be partly the seat's
        }
    }
    if (!ok)
    {
        Break(seat, L"target pick");
        return;
    }
    if (ran)
    {
        ++state.picks;
        NoteTarget(seat, state);
    }
}

/// The FieldAction read the press's caller makes inline, for the seat (inside its scopes and pad): the gate, then the
/// game's own held and pressed getters on a copy of the key, written as the game writes them.
bool ReadFieldAction(std::uint8_t* mgr) noexcept
{
    bool held = false, pressed = false, open = false;
    if (!platform::CallGuarded(g_gate, open))
    {
        return false;
    }
    if (open)
    {
        if (!players::action_key::Test(g_held, g_key, held) || !players::action_key::Test(g_pressed, g_key, pressed))
        {
            return false;
        }
    }
    const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(held), static_cast<std::uint8_t>(pressed)};
    platform::WriteGuarded(mgr + facts::kTargetsHeld, bytes, sizeof(bytes));
    if (!held)
    {
        const std::uint8_t clear = 0;
        platform::WriteGuarded(mgr + facts::kTargetsLatch, &clear, 1);
    }
    return true;
}

/// The presses an operation of the table dropped, each logged at Debug with `why` and counted.
void LogDropped(const talk_presses::Dropped& dropped, const wchar_t* why) noexcept
{
    for (int i = 0; i < dropped.count; ++i)
    {
        const talk_presses::Press& press = dropped.presses[static_cast<std::size_t>(i)];
        platform::LogDebug(L"field targets: {}'s press of {} is dropped ({})", SeatName(press.seat), Name(press.name), why);
    }
    g_talkDropped += static_cast<std::uint32_t>(dropped.count);
}

/// The presses are dropped when the target manager changes (a world change).
void DropTalkPressesAtWorldChange(void* targets) noexcept
{
    if (targets != g_notesManager)
    {
        talk_presses::Dropped dropped;
        g_talkPresses.DropAll(dropped);
        LogDropped(dropped, L"the world changed");
        g_notesManager = targets;
    }
}

/// A further player's NPC press, for the act that the NPC's talk state queues (world/shop_act.h): rule 1 of the table,
/// then the pressed object's own place from the game's target entry while it still exists (the object's talk state
/// removes it at the next state-manager tick): the talk's gather faces player 1's character toward it.
void NoteTalkPress(int seat, std::uint64_t name) noexcept
{
    void* const targets = ReadPointer(Root(), facts::kRootFieldTargets);
    DropTalkPressesAtWorldChange(targets);
    talk_presses::Dropped dropped;
    talk_presses::Press*  press = g_talkPresses.Note(seat, name, GetTickCount64(), dropped);
    LogDropped(dropped, L"the player pressed again");
    if (press == nullptr)
    {
        platform::LogWarning(L"field targets: more than {} NPC presses wait for their acts; {}'s is not noted",
                             talk_presses::kCapacity, SeatName(seat));
        return;
    }
    void* entry = nullptr;
    if (targets != nullptr && platform::CallGuarded(g_findEntry, entry, targets, &press->name) && entry != nullptr &&
        platform::CopyGuarded(press->spot.data(), static_cast<const std::uint8_t*>(entry) + facts::kTargetsEntryLocation,
                              sizeof(press->spot)))
    {
        press->spotKnown = true;
    }
    else
    {
        platform::LogDebug(L"field targets: {}'s press of {} finds no target entry now; where it stands is not noted",
                           SeatName(seat), Name(name));
    }
}

/// Player 1 pressed `name`: a further player's press of it is no longer the one its acts answer (rule 2).
void DropTalkPresses(std::uint64_t name) noexcept
{
    talk_presses::Dropped dropped;
    g_talkPresses.DropOf(name, dropped);
    LogDropped(dropped, L"player 1 pressed it too");
}

/// The NPC presses, each tick: dropped at a world change (rule 3); their wait counts only while no field lock holds
/// (rule 4: a running talk or greeting holds it, so a long talk or a choice before the shop never lapses them).
void TickTalkPresses() noexcept
{
    void* const targets = ReadPointer(Root(), facts::kRootFieldTargets);
    if (!g_talkPresses.Any())
    {
        g_notesManager = targets;
        return;
    }
    DropTalkPressesAtWorldChange(targets);
    talk_presses::Dropped lapsed;
    g_talkPresses.Tick(GetTickCount64(), players::field_root::ControlLockMask() != 0, lapsed);
    for (int i = 0; i < lapsed.count; ++i)
    {
        const talk_presses::Press& press = lapsed.presses[static_cast<std::size_t>(i)];
        platform::LogDebug(L"field targets: {}'s press of {} lapsed (no show-menu act on its channel within the wait)",
                           SeatName(press.seat), Name(press.name));
    }
    g_talkLapsed += static_cast<std::uint32_t>(lapsed.count);
}

void NotePending(int seat, std::uint64_t name) noexcept
{
    for (Pending& slot : g_pending)
    {
        if (slot.name == 0)
        {
            slot = {name, seat};
            return;
        }
    }
    platform::LogWarning(L"field targets: more than {} chests wait for their open; {}'s is opened as the game's", kPending, SeatName(seat));
}

/// One seat's run of the game's press on its own copy: only for a target it may press.
void PressFor(int seat, void* mgr, float dt, TickFn original) noexcept
{
    SeatState& state = g_seats[seat];
    if (!Ready(seat, state, mgr) || players::party_member::HandedToAi(state.member))
    {
        return;  // with its AI (a lead, a scene): no press of the player's starts anything on it (world/traversal_action.h)
    }
    auto*                        live = static_cast<std::uint8_t*>(mgr);
    auto*                        push = static_cast<std::uint8_t*>(ReadPointer(Root(), facts::kRootPushButtons));
    const std::uint64_t*         queue = nullptr;
    std::int32_t                 before = 0, after = 0;
    bool                         pressedNow = false, ok = true, queued = false;
    {
        const players::scopes::Scope scope(seat, players::scopes::Leader | players::scopes::Primary | players::scopes::UiSlot);
        if (!scope.Held())
        {
            return;
        }
        const players::pad_buttons::ButtonScope buttons(static_cast<unsigned>(seat));
        if (!buttons.Held())
        {
            return;  // without the pad's own buttons the read would be player 1's: player 1's press would open the seat's target
        }
        const platform::ByteScope swapped(state.copy);
        if (!swapped.Held())
        {
            return;
        }
        if (!ReadFieldAction(live))
        {
            ok = false;
        }
        std::uint8_t latch = 0;
        Read(live + facts::kTargetsLatch, latch);
        // Released too: the game's press then resets the hold timer.
        if (ok && Admitted(state.kind))
        {
            std::uint8_t pushHeld = 0;
            const bool   kept     = push != nullptr && Read(push + facts::kPushButtonsHeld, pushHeld);
            queued                = ChestQueue(queue, before);
            {
                // The seat's icons in: a press's feedback anchors on the pressed target's icon.
                const target_icons::SeatSwap icons(seat, mgr, state.localPlayer, false);
                if (icons.Held())
                {
                    ok = platform::CallGuarded(original, mgr, dt);
                }
                else if (!g_pressSkipLogged)
                {
                    g_pressSkipLogged = true;
                    platform::LogDebug(L"field targets: {}'s press skipped (its icons could not be exchanged in)", SeatName(seat));
                }
            }
            if (kept)
            {
                platform::WriteGuarded(push + facts::kPushButtonsHeld, &pushHeld, 1);  // player 1's, as the game's run left it
            }
            std::uint8_t latchAfter = 0;
            Read(live + facts::kTargetsLatch, latchAfter);
            pressedNow = latch == 0 && latchAfter != 0;  // the press went through (the game sets the latch at its dispatch)
            queued = queued && ChestQueue(queue, after);
        }
    }
    if (!ok)
    {
        Break(seat, L"FieldAction press");
        return;
    }
    if (!pressedNow)
    {
        return;
    }
    ++state.presses;
    const bool debug = platform::Enabled(platform::Level::Debug);
    if (debug)
    {
        platform::LogDebug(L"field targets: {} pressed {} ({})", SeatName(seat), Name(state.target), KindName(state.kind));
    }
    if (state.kind == Kind::Npc)
    {
        const auto pressed = InCopy<std::uint64_t>(state, facts::kTargetsPendingName);  // the game's own record of the press
        const auto name    = pressed != 0 ? pressed : state.target;
        if (name == g_playerOnePressed)
        {
            platform::LogDebug(L"field targets: {} pressed {} in the frame player 1 did; the talk is player 1's", SeatName(seat),
                               Name(name));
        }
        else
        {
            NoteTalkPress(seat, name);
        }
    }
    if (queued && after == before + 1 && queue != nullptr)
    {
        const std::uint64_t chest = QueuedAt(queue, after - 1);
        NotePending(seat, chest);
        ++state.chests;
        if (debug)
        {
            platform::LogDebug(L"field targets: {}'s chest {} waits for its open ({} queued)", SeatName(seat), Name(chest), after);
        }
    }
}

// --- The redirects -----------------------------------------------------------------------------------------------------

void Pick(void* mgr, float dt)
{
    const auto original = reinterpret_cast<TickFn>(g_pickTarget);
    if (g_installed)
    {
        target_icons::BeforePick(mgr);  // the location refreshes posted since, kept for each seat's run
    }
    original(mgr, dt);  // player 1's
    if (!g_installed)
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        PickFor(seat, mgr, dt, original);
    }
}

void Press(void* mgr, float dt)
{
    const auto    original = reinterpret_cast<TickFn>(g_press.Original());
    auto*         live     = static_cast<std::uint8_t*>(mgr);
    std::uint8_t  latch    = 0, latchAfter = 0;
    const bool    before   = g_installed && Read(live + facts::kTargetsLatch, latch);
    original(mgr, dt);  // player 1's
    if (!g_installed)
    {
        return;
    }
    std::uint64_t pressed = 0;
    if (before && latch == 0 && Read(live + facts::kTargetsLatch, latchAfter) && latchAfter != 0 &&
        Read(live + facts::kTargetsPendingName, pressed) && pressed != 0)
    {
        DropTalkPresses(pressed);
        g_playerOnePressed = pressed;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        PressFor(seat, mgr, dt, original);
    }
    g_playerOnePressed = 0;
}

void* FirstLocalPlayer(void* world)
{
    if (g_pickSeat != 0)
    {
        SeatState& state = g_seats[g_pickSeat];
        if (state.localPlayer != nullptr)
        {
            ++state.screens;
            return state.localPlayer;  // the screen test of this player's pick: its own view
        }
    }
    return reinterpret_cast<FirstLocalFn>(g_screenTarget)(world);
}

/// The chest open's Begin: for a chest a further player's press queued, run in that player's Leader scope, so the game
/// takes the player's member as the opener. The noted chest stays until it leaves the queue (Tick), so a Begin that
/// runs again for it still runs as the seat's.
char Begin(void* runner)
{
    const auto           original = reinterpret_cast<BeginFn>(g_begin.Original());
    const std::uint64_t* queue    = nullptr;
    std::int32_t         count    = 0;
    const std::uint64_t  head     = ChestQueue(queue, count) && count > 0 ? QueuedAt(queue, 0) : 0;
    int                  seat     = 0;
    for (Pending& slot : g_pending)
    {
        if (head != 0 && slot.name == head)
        {
            seat = slot.seat;
            break;
        }
    }
    if (seat == 0)
    {
        ++g_playerOneOpens;
        return original(runner);
    }
    char result = 0;
    bool held   = false;
    {
        const players::scopes::Scope scope(seat, players::scopes::Leader);
        held   = scope.Held();
        result = original(runner);
    }
    std::uint64_t chest    = 0;
    std::uint8_t  isLeader = 0;
    Read(static_cast<const std::uint8_t*>(runner) + facts::kChestRunnerName, chest);
    Read(static_cast<const std::uint8_t*>(runner) + facts::kChestRunnerOpenerIsLeader, isLeader);
    ++g_seats[seat].opens;
    if (platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"field targets: {}'s chest {} opens{} (the chest the open took: {}, opener is the leader: {})",
                           SeatName(seat), Name(head), held ? L" by its member" : L" by player 1's character (the seat's scope did not open)",
                           Name(chest), isLeader != 0 ? L"yes" : L"no");
    }
    return result;
}

/// The redirects inside the pick (the icons' own, then the pick's call and its screen test; two cross a block), placed
/// from the game thread outside the pick.
void PlaceCrossing() noexcept
{
    if (g_placed)
    {
        return;
    }
    g_placed = true;
    // The icons first (world/target_icons.h): a further player's pick never runs without its own icons.
    const bool icons = target_icons::Place();
    if (!icons || !g_pick.InstallFromOwnThread(g_pickSite, g_pickTarget, reinterpret_cast<std::uintptr_t>(&Pick)))
    {
        platform::LogWarning(L"field targets: the pick's {} could not be redirected; further players get no prompts and press nothing",
                             icons ? L"call" : L"icon calls");
        return;
    }
    if (!g_screen.InstallFromOwnThread(g_screenSite, g_screenTarget, reinterpret_cast<std::uintptr_t>(&FirstLocalPlayer)))
    {
        platform::LogWarning(L"field targets: the pick's screen test could not be redirected; a target that must be on screen is "
                             L"tested against player 1's view for every player");
    }
    platform::Log(L"field targets: further players pick and press chests, ground items, NPCs and doors on their own pads");
}

std::wstring Report()
{
    std::wstring out = !g_installed           ? L"not installed"
                       : g_broken              ? L"off (a run raised)"
                       : g_pick.IsInstalled()  ? L"on"
                       : g_placed              ? L"off (the pick could not be redirected)"
                                               : L"waiting for the game thread";
    std::int32_t count = 0;
    const std::uint64_t* queue = nullptr;
    ChestQueue(queue, count);
    out += std::format(L"; chests queued {}, player 1's opens {}; NPC presses claimed by an act {}, lapsed {}, dropped {}",
                       count, g_playerOneOpens, g_talkClaims, g_talkLapsed, g_talkDropped);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const SeatState& s = g_seats[seat];
        out += std::format(L"; {}: target {} ({}), picks {}, changes {}, targets left to player 1 {}, presses {}, chests noted {}, opens {}, "
                           L"screen tests {}",
                           SeatName(seat), Name(s.target), KindName(s.kind), s.picks, s.changes, s.leftToPlayerOne, s.presses,
                           s.chests, s.opens, s.screens);
    }
    out += L"; " + target_icons::Report();
    return out;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto fail = [](const wchar_t* what) {
        platform::LogWarning(L"field targets: {} is not as the research read it; further players cannot open chests or doors", what);
        return false;
    };
    const auto key        = image.Rebase(facts::kFieldActionKey, 24);
    const auto pickSite   = image.Rebase(facts::kFieldTargetPickCall, 5);
    const auto pick       = image.Rebase(facts::kFieldTargetPick);
    const auto pressSite  = image.Rebase(facts::kFieldTargetPressCall, 5);
    const auto press      = image.Rebase(facts::kFieldTargetPress);
    const auto screenSite = image.Rebase(facts::kFieldTargetScreenCall, 5);
    const auto screen     = image.Rebase(facts::kFirstLocalPlayer);
    const auto gate       = platform::FindFunction(image, facts::kFieldActionGate);
    const auto held       = platform::FindFunction(image, facts::kActionHeldByKey);
    const auto pressed    = platform::FindFunction(image, facts::kActionPressedByKey);
    const auto findRow    = platform::FindFunction(image, facts::kFindRowInTables);
    const auto startsWith = platform::FindFunction(image, facts::kNameStartsWith);
    const auto freeFn     = platform::FindFunction(image, facts::kGameFree);
    const auto findEntry  = platform::FindFunction(image, facts::kFindTargetEntry);
    if (!target_icons::Install(image))
    {
        return fail(L"the pick's icon calls");
    }
    if (!players::field_root::Installed() || !key || !pickSite || !pick || !pressSite || !press || !screenSite || !screen || !gate || !held || !pressed || !findRow ||
        !startsWith || !freeFn || !findEntry)
    {
        return fail(L"a function, call site or global");
    }
    const std::uintptr_t tables[] = {facts::kTableSituationTrigger, facts::kTableObjectLayout, facts::kTableObjectActor,
                                     facts::kTableObjectEnemy, facts::kTableObjectTreasure, facts::kTableObjectParty};
    for (std::size_t i = 0; i < g_tables.size(); ++i)
    {
        const auto live = image.Rebase(tables[i], 24);
        if (!live)
        {
            return fail(L"a row table");
        }
        g_tables[i] = *live;
    }
    g_key = *key;
    g_gate       = reinterpret_cast<GateFn>(*gate);
    g_held       = reinterpret_cast<KeyTestFn>(*held);
    g_pressed    = reinterpret_cast<KeyTestFn>(*pressed);
    g_findRow    = reinterpret_cast<FindRowFn>(*findRow);
    g_startsWith = reinterpret_cast<StartsWithFn>(*startsWith);
    g_free       = reinterpret_cast<FreeFn>(*freeFn);
    g_findEntry  = reinterpret_cast<FindEntryFn>(*findEntry);
    g_pickSite = *pickSite, g_pickTarget = *pick, g_screenSite = *screenSite, g_screenTarget = *screen;
    if (!platform::ReplaceSlot(image, facts::kChestOpenBegin, reinterpret_cast<std::uintptr_t>(&Begin), g_begin))
    {
        return fail(L"the chest open's Begin");
    }
    if (!g_press.Install(*pressSite, *press, reinterpret_cast<std::uintptr_t>(&Press)))
    {
        g_begin.Restore();
        return fail(L"the press's call");
    }
    g_installed = true;
    control::Register({.name  = L"field.targets",
                       .shape = control::Shape::Text,
                       .help  = L"each further player's field target (chest, door, talk...), its counts, and the chests waiting to open",
                       .get   = [] { return control::Value{Report()}; }});
    platform::Log(L"field targets: installed; the pick's calls are placed on the first game-thread tick");
    return true;
}

void TickSeat(int seat, void* localPlayer, void* member) noexcept
{
    if (!g_installed || !players::SeatBuilt(seat))
    {
        return;
    }
    SeatState& state = g_seats[seat];
    if (member != state.member || localPlayer != state.localPlayer)
    {
        if (state.copyOf != nullptr)
        {
            platform::LogDebug(L"field targets: {}'s target state starts again ({})", SeatName(seat),
                               member == nullptr ? L"the player left" : L"a new member or local player");
        }
        DropCopy(state);  // the next pick starts from the game's empty state
        target_icons::DropSeat(seat);
        talk_presses::Dropped dropped;
        g_talkPresses.DropSeat(seat, dropped);
        LogDropped(dropped, L"the player left or changed character");
    }
    state.localPlayer = localPlayer;
    state.member      = member;
}

void Tick() noexcept
{
    if (!g_installed)
    {
        return;
    }
    PlaceCrossing();
    target_icons::Tick(ReadPointer(Root(), facts::kRootFieldTargets));
    TickTalkPresses();
    const std::uint64_t* queue = nullptr;
    std::int32_t         count = 0;
    if (!ChestQueue(queue, count))
    {
        return;
    }
    // A noted chest is forgotten once it leaves the queue (its open ended, or a level change emptied the queue).
    for (Pending& slot : g_pending)
    {
        bool waits = false;
        for (std::int32_t i = 0; slot.name != 0 && !waits && i < count; ++i)
        {
            waits = QueuedAt(queue, i) == slot.name;
        }
        if (!waits)
        {
            slot = {};
        }
    }
}

void Remove() noexcept
{
    // The pick's two redirects stay: restoring them must also come from the game thread outside the pick, and with nothing
    // installed they pass straight through. Called with no seat in (mod.cpp), so no pick is using a copy.
    g_installed = false;
    g_press.Restore();
    g_begin.Restore();
    target_icons::Remove();
    g_pending.fill({});
    g_talkPresses.Clear();
    for (SeatState& state : g_seats)
    {
        DropCopy(state);
        state.member = state.localPlayer = nullptr;
    }
}

int PeekTalkPress(std::uint64_t object) noexcept
{
    return g_talkPresses.Peek(object);
}

bool TalkJudged(std::uint64_t object) noexcept
{
    return g_talkPresses.Judged(object);
}

bool TalkPressSpot(std::uint64_t object, float (&out)[3]) noexcept
{
    std::array<float, 3> spot{};
    if (!g_talkPresses.Spot(object, spot))
    {
        return false;
    }
    std::memcpy(out, spot.data(), sizeof(out));
    return true;
}

void MarkTalkJudged(std::uint64_t object) noexcept
{
    g_talkPresses.MarkJudged(object);
}

int ClaimTalkPress(std::uint64_t object) noexcept
{
    const int seat = g_talkPresses.Claim(object);
    if (seat != 0)
    {
        ++g_talkClaims;
    }
    return seat;
}

std::wstring NameOf(std::uint64_t name) noexcept
try
{
    return Name(name);
}
catch (...)
{
    return L"?";
}

std::wstring Reading() noexcept
try
{
    return Report();
}
catch (...)
{
    return L"unreadable";
}

}  // namespace lss::world::field_targets
