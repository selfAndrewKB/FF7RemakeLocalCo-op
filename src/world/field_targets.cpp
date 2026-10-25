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
#include "players/pad_buttons.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "world/target_icons.h"
#include "world/traversal_action.h"

#include <Unreal/NameTypes.hpp>

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <format>
#include <string>

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

constexpr int         kSeats   = players::scopes::kMaxPlayers;
constexpr std::size_t kItems   = facts::kTargetsItems;
constexpr std::size_t kPending = 16;  // chests a further player's press queued that have not begun yet

/// What a target is, by the dispatch's own order (research/RE-chest-two.md §8.1): only chests, ground items, doors,
/// curtains, grappling-hook points and the player's own swing are pressed for a further player.
enum class Kind : std::uint8_t
{
    None,
    Event,   // a SituationTrigger row on its state: talk and events (player 1's)
    Object,  // an ObjectLayout, ObjectActor or ObjectEnemy row: push-button events and state signals (player 1's)
    Chest,   // an ObjectTreasure row: a chest or a shining item on the ground
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
        case Kind::Event: return L"a talk or event";
        case Kind::Object: return L"an object event";
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

/// Pressed for a further player now: a chest always; a door, a curtain or the member's own swing only while the member's
/// field actions run as its player's (world/traversal_action.h), the grappling hook only while its three lock calls are
/// also redirected (traversal_action::HookShotReady); read at each use (a level change can stop that).
bool Admitted(Kind kind) noexcept
{
    switch (kind)
    {
        case Kind::Chest: return true;
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
std::uintptr_t g_root = 0, g_key = 0;
std::array<std::uintptr_t, 6> g_tables{};  // situation, layout, actor, enemy, treasure, party
GateFn       g_gate       = nullptr;
KeyTestFn    g_held       = nullptr;
KeyTestFn    g_pressed    = nullptr;
FindRowFn    g_findRow    = nullptr;
StartsWithFn g_startsWith = nullptr;
FreeFn       g_free       = nullptr;
std::uintptr_t g_pickSite = 0, g_pickTarget = 0, g_screenSite = 0, g_screenTarget = 0;

platform::CallRedirect g_pick, g_press, g_screen;
platform::VTableEntry  g_begin;
bool                   g_installed = false;
bool                   g_placed    = false;  // the redirects inside the pick tried (on the game thread)
bool                   g_broken    = false;  // a further player's run raised: the passes are off

// Game thread.
std::array<SeatState, kSeats> g_seats{};
std::array<Pending, kPending> g_pending{};
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
    return static_cast<std::uint8_t*>(ReadPointer(reinterpret_cast<const void*>(g_root), 0));
}

/// The chests waiting for their open: {data, count} of the state manager's list.
bool ChestQueue(const std::uint64_t*& data, std::int32_t& count) noexcept
{
    const void* list = ReadPointer(ReadPointer(Root(), facts::kRootStateManager), facts::kStateChestQueue);
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
        return Kind::Event;
    }
    if (Found(1, name) || Found(2, name) || Found(3, name))
    {
        return Kind::Object;
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
/// pick's own rejection test), so the game gives the seat neither an icon nor a prompt for it (talk, events, object
/// events stay player 1's); put back after. Targets are classified once per name and state.
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
        for (int pass = 0; pass < 2; ++pass)
        {
            alignas(16) std::array<std::uint8_t, 24> key{};
            if (!platform::CopyGuarded(key.data(), reinterpret_cast<const void*>(g_key), key.size()))
            {
                return false;
            }
            void* controller = nullptr;
            std::memcpy(&controller, key.data() + 16, sizeof(controller));
            if (controller != nullptr)
            {
                // The copy's reference, which the getter releases (the game's own ++ on the shared reference count).
                std::int32_t count = 0;
                if (!Read(static_cast<std::uint8_t*>(controller) + 8, count))
                {
                    return false;
                }
                ++count;
                if (!platform::WriteGuarded(static_cast<std::uint8_t*>(controller) + 8, &count, sizeof(count)))
                {
                    return false;  // without the reference the getter would release one it never got
                }
            }
            bool& out = pass == 0 ? held : pressed;
            if (!platform::CallGuarded(pass == 0 ? g_held : g_pressed, out, static_cast<void*>(key.data())))
            {
                return false;
            }
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
    if (!Ready(seat, state, mgr))
    {
        return;
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
    const auto original = reinterpret_cast<TickFn>(g_press.Original());
    original(mgr, dt);  // player 1's
    if (!g_installed)
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        PressFor(seat, mgr, dt, original);
    }
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
    platform::Log(L"field targets: further players pick and press chests, ground items and doors on their own pads");
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
    out += std::format(L"; chests queued {}, player 1's opens {}", count, g_playerOneOpens);
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
    const auto root       = image.Rebase(facts::kEndGameRootGlobal, 8);
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
    if (!target_icons::Install(image))
    {
        return fail(L"the pick's icon calls");
    }
    if (!root || !key || !pickSite || !pick || !pressSite || !press || !screenSite || !screen || !gate || !held || !pressed || !findRow ||
        !startsWith || !freeFn)
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
    g_root = *root, g_key = *key;
    g_gate       = reinterpret_cast<GateFn>(*gate);
    g_held       = reinterpret_cast<KeyTestFn>(*held);
    g_pressed    = reinterpret_cast<KeyTestFn>(*pressed);
    g_findRow    = reinterpret_cast<FindRowFn>(*findRow);
    g_startsWith = reinterpret_cast<StartsWithFn>(*startsWith);
    g_free       = reinterpret_cast<FreeFn>(*freeFn);
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
    for (SeatState& state : g_seats)
    {
        DropCopy(state);
        state.member = state.localPlayer = nullptr;
    }
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
