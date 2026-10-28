#include "players/nav_area.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/scopes.h"
#include "players/seat_objects.h"
#include "players/seats.h"
#include "players/weak_object.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::players::nav_area {

namespace {

using platform::Read;

// Stock UActorComponent::TickComponent(DeltaTime, TickType, ThisTickFunction); the override reads only this and
// the delta, but every argument is passed on.
using TickFn      = void (*)(void* component, float deltaTime, int tickType, void* tickFunction);
using RefreshFn   = void (*)(void* component, float deltaTime);   // the diff the tick tail-jumps to
using RemoveKeyFn = std::uint64_t (*)(void* map, std::uint64_t key);  // the entered map's own Remove, 1 if removed
using LeaderFn    = void* (*)(void* partyManager, std::uint8_t selector);

constexpr std::size_t kMaxEntered = 32;  // a leader stands in at most 16 nav areas

// The seat's camera operator, leader and primary controller: what the diff's camera settings and marks reach.
constexpr unsigned kSeatScopes = scopes::Primary | scopes::Operator | scopes::Leader;

// Set at install, then only read.
TickFn                g_tick      = nullptr;
RefreshFn             g_refresh   = nullptr;  // null: the re-enter at a spawn is off
RemoveKeyFn           g_removeKey = nullptr;
platform::VTableEntry g_slot;
LeaderFn              g_leader = nullptr;               // PartyManager_GetPartyLeader
std::array<std::uintptr_t, facts::kRoomLeaderCalls.size()> g_roomSites{};  // live sites; 0: the room redirects are off

// Game thread.
std::array<platform::CallRedirect, facts::kRoomLeaderCalls.size()> g_roomCalls;  // Enter, Stay, Leave
bool g_roomPlaced = false;  // the placing was tried (once, from the game thread)

/// While a seat's component runs the game's diff: that seat (0 = none) and its controller's own field character.
struct RoomRun
{
    int   seat      = 0;
    void* character = nullptr;
};
RoomRun g_roomRun;

/// Marks a seat's diff for the room handlers' leader calls, the outer value kept (game thread).
class RoomRunScope
{
public:
    RoomRunScope(int seat, void* controller) noexcept : outer_(g_roomRun)
    {
        void* character = nullptr;
        Read(static_cast<std::uint8_t*>(controller) + facts::kControllerLeaderCharacter, character);
        g_roomRun = {seat, character};
    }
    ~RoomRunScope() { g_roomRun = outer_; }

    RoomRunScope(const RoomRunScope&)            = delete;
    RoomRunScope& operator=(const RoomRunScope&) = delete;

private:
    RoomRun outer_;
};

// Game thread. Index = seat ([0] unused).
struct Counts
{
    std::uint32_t ticks     = 0;  // ticks of the seat's controller's component
    std::uint32_t scoped    = 0;  // of them, run with all three scopes held
    std::uint32_t ownRow    = 0;  // of those, with the seat's command controller in the UI slot too
    std::uint32_t rowOnly   = 0;  // the three not held (a scene holds the member), the seat's row held
    std::uint32_t spawns    = 0;  // operator spawns the re-enter ran at
    std::uint32_t reentered = 0;  // volumes forgotten and entered again over them
    std::uint32_t roomEnters = 0, roomStays = 0, roomLeaves = 0;  // room limits written on the seat's own character
    std::uint32_t roomNone   = 0;  // room handler calls with no character read (nothing written)
    std::uint32_t roomClears = 0;  // a member taken or given back: its room limit cleared for the diff to set again
};
std::array<Counts, scopes::kMaxPlayers> g_counts;
std::array<players::weak_object::Weak, scopes::kMaxPlayers> g_seatMember;  // the member each seat's room limit was on

enum class RoomEvent
{
    Enter,
    Stay,
    Leave
};

int SeatOf(const void* controller) noexcept;  // below

/// The diff now running with no mark: the game's camera reset, the only other caller of the diff, runs it on the
/// primary controller's component (`*(PM+0)`, swapped by a seat's Primary scope: any game code the mod runs in a seat's
/// scopes, such as a shop act or a field action, would reset that seat's camera). Its seat and character, else none.
RoomRun UnmarkedRun(void* partyManager) noexcept
{
    void* controller = nullptr;
    if (partyManager == nullptr || !Read(partyManager, controller) || controller == nullptr)
    {
        return {};
    }
    const int seat = SeatOf(controller);
    if (seat == 0)
    {
        return {};
    }
    void* character = nullptr;
    Read(static_cast<std::uint8_t*>(controller) + facts::kControllerLeaderCharacter, character);
    return {seat, character};
}

/// The room handlers' "default leader": in a seat's diff (its tick or spawn re-enter, marked; or the camera reset on
/// that seat's controller), that seat's own character (null: the handler writes nothing), so a room limit lands on the
/// character the diff found in the room; else the game's answer.
void* RoomLeader(void* partyManager, std::uint8_t selector, RoomEvent event) noexcept
{
    if (selector != facts::kLeaderDefault)
    {
        return g_leader(partyManager, selector);
    }
    const RoomRun run = g_roomRun.seat != 0 ? g_roomRun : UnmarkedRun(partyManager);
    if (run.seat == 0)
    {
        return g_leader(partyManager, selector);
    }
    Counts& c = g_counts[run.seat];
    if (run.character == nullptr)
    {
        ++c.roomNone;
        return nullptr;
    }
    switch (event)
    {
    case RoomEvent::Enter:
        ++c.roomEnters;
        platform::LogDebug(L"nav area: {} entered a room; its walk limit goes on {}'s own character", SeatName(run.seat),
                           SeatName(run.seat));
        break;
    case RoomEvent::Stay:
        ++c.roomStays;
        platform::LogTrace(L"nav area: {} stays in a room; its walk limit on its own character", SeatName(run.seat));
        break;
    case RoomEvent::Leave:
        ++c.roomLeaves;
        platform::LogDebug(L"nav area: {} left a room; its walk limit comes off {}'s own character (or the next room's)",
                           SeatName(run.seat), SeatName(run.seat));
        break;
    }
    return run.character;
}

void* RoomEnterLeader(void* partyManager, std::uint8_t selector) { return RoomLeader(partyManager, selector, RoomEvent::Enter); }
void* RoomStayLeader(void* partyManager, std::uint8_t selector) { return RoomLeader(partyManager, selector, RoomEvent::Stay); }
void* RoomLeaveLeader(void* partyManager, std::uint8_t selector) { return RoomLeader(partyManager, selector, RoomEvent::Leave); }

/// The room limit (cap layer 6) back to none on `character`: the value the game's own Leave writes with no room left.
bool ClearRoomLimit(void* character) noexcept
{
    auto*        bytes = static_cast<std::uint8_t*>(character);
    std::uint8_t move = 0, dash = 0;
    if (!Read(bytes + facts::kCharacterMoveCaps + facts::kCapLayerRoom, move) ||
        !Read(bytes + facts::kCharacterDashCaps + facts::kCapLayerRoom, dash) ||
        (move == facts::kMoveCapNone && dash == facts::kDashCapNone))
    {
        return false;
    }
    const std::uint8_t none[2] = {facts::kMoveCapNone, facts::kDashCapNone};
    return platform::WriteGuarded(bytes + facts::kCharacterMoveCaps + facts::kCapLayerRoom, &none[0], 1) &&
           platform::WriteGuarded(bytes + facts::kCharacterDashCaps + facts::kCapLayerRoom, &none[1], 1);
}

/// Player 1's room character: the default leader, the game's answer (its own component's diff keeps it right).
void* RoomPlayerOne() noexcept
{
    void* const manager = reinterpret_cast<void*>(scopes::PartyManagerAddress());
    void*       leader  = nullptr;
    return manager != nullptr && g_leader != nullptr && platform::CallGuarded(g_leader, leader, manager, facts::kLeaderDefault)
               ? leader
               : nullptr;
}

/// The seat whose controller owns `controller`, else 0 (the registry's liveness-checked answer).
int SeatOf(const void* controller) noexcept
{
    const int seat = seat_objects::SeatOf(controller);
    return seat != 0 && seat_objects::Controller(seat) == controller ? seat : 0;
}

/// The keys (FNames) of the volumes the component lists as entered: the map's elements whose allocation bit is set.
std::size_t EnteredKeys(void* component, std::array<std::uint64_t, kMaxEntered>& keys) noexcept
{
    auto*         map      = static_cast<std::uint8_t*>(component) + facts::kNavAreaEnteredMap;
    std::uint8_t* elements = nullptr;
    std::int32_t  num      = 0;
    std::uint8_t* heapBits = nullptr;
    std::int32_t  numBits  = 0;
    if (!Read(map, elements) || elements == nullptr || !Read(map + (facts::kNavAreaEnteredNum - facts::kNavAreaEnteredMap), num) ||
        !Read(map + (facts::kNavAreaEnteredBitsHeap - facts::kNavAreaEnteredMap), heapBits) ||
        !Read(map + (facts::kNavAreaEnteredNumBits - facts::kNavAreaEnteredMap), numBits))
    {
        return 0;
    }
    const std::uint8_t* bits  = heapBits != nullptr ? heapBits : map + (facts::kNavAreaEnteredBitsInline - facts::kNavAreaEnteredMap);
    const std::int32_t  slots = num < numBits ? num : numBits;
    std::size_t         count = 0;
    for (std::int32_t i = 0; i < slots && count < keys.size(); ++i)
    {
        std::uint32_t word = 0;
        if (!Read(bits + 4 * (i / 32), word) || ((word >> (i % 32)) & 1u) == 0)
        {
            continue;
        }
        std::uint64_t key = 0;
        if (Read(elements + facts::kNavAreaEnteredElement * i, key))
        {
            keys[count++] = key;
        }
    }
    return count;
}

void TickComponent(void* component, float deltaTime, int tickType, void* tickFunction)
{
    void* controller = nullptr;
    Read(static_cast<std::uint8_t*>(component) + facts::kNavAreaComponentController, controller);
    const int seat = SeatOf(controller);
    if (seat == 0)
    {
        g_tick(component, deltaTime, tickType, tickFunction);
        return;
    }
    ++g_counts[seat].ticks;
    const RoomRunScope  room(seat, controller);  // keyed on the component's owner: held or not, rooms limit the seat's own
    const scopes::Scope scope(seat, kSeatScopes);
    // A hook area's prompt row: the seat's own. Declared after `scope` so it closes first, as one four-context scope would.
    const scopes::Scope row(seat, scopes::UiSlot);
    if (scope.Held())
    {
        ++g_counts[seat].scoped;
        if (row.Held())
        {
            ++g_counts[seat].ownRow;
        }
    }
    else if (row.Held())
    {
        ++g_counts[seat].rowOnly;
    }
    g_tick(component, deltaTime, tickType, tickFunction);
}

std::wstring Reading()
{
    std::wstring text;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const Counts& c = g_counts[seat];
        text += std::format(L"{}{}: ticks {}, scoped {}, own row {}, own row only {}, spawns {}, re-entered {}; room limits on its own "
                            L"character: enters {}, stays {}, leaves {}, none read {}, cleared at a take or give-back {}",
                            seat == 1 ? L"" : L"; ", SeatName(seat), c.ticks, c.scoped, c.ownRow, c.rowOnly, c.spawns,
                            c.reentered, c.roomEnters, c.roomStays, c.roomLeaves, c.roomNone, c.roomClears);
    }
    text += std::format(L"; rooms per player {}", g_roomCalls[0].IsInstalled() ? L"on" : L"off");
    return text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tick = platform::FindFunction(image, facts::kNavAreaComponentTick.target);
    if (!tick)
    {
        platform::Log(L"nav area: the component's tick differs; a seat's InDoor camera settings reach player 1's operator");
        return false;
    }
    g_tick = reinterpret_cast<TickFn>(*tick);
    if (!platform::ReplaceSlot(image, facts::kNavAreaComponentTick, reinterpret_cast<std::uintptr_t>(&TickComponent), g_slot))
    {
        platform::Log(L"nav area: the component's tick slot could not be replaced; a seat's InDoor camera settings reach player 1's operator");
        return false;
    }
    const auto refresh   = platform::FindFunction(image, facts::kNavAreaRefreshFromLeader);
    const auto removeKey = platform::FindFunction(image, facts::kNavAreaMapRemoveKey);
    if (refresh && removeKey)
    {
        g_refresh   = reinterpret_cast<RefreshFn>(*refresh);
        g_removeKey = reinterpret_cast<RemoveKeyFn>(*removeKey);
    }
    else
    {
        platform::Log(L"nav area: the component's diff or its map's Remove differs; a respawned operator keeps missing the InDoor setting pushed before it");
    }
    const auto leader = platform::FindFunction(image, facts::kGetPartyLeader);
    bool       sites  = leader.has_value();
    for (std::size_t i = 0; sites && i < g_roomSites.size(); ++i)
    {
        const auto site = image.Rebase(facts::kRoomLeaderCalls[i], 5);
        sites           = site.has_value();
        g_roomSites[i]  = site ? *site : 0;
    }
    if (sites)
    {
        g_leader = reinterpret_cast<LeaderFn>(*leader);
    }
    else
    {
        g_roomSites = {};
        platform::Log(L"nav area: the room handlers' leader calls could not be checked; a further player's room limits player 1's "
                      L"character");
    }
    control::Register({.name = L"nav-area", .shape = control::Shape::Text,
                       .help = L"each seat's controller's nav-area component ticks: all, of them in the seat's scopes, of those with its own command row, with its row alone; operator spawns re-entered at, volumes entered again; room walk limits written on the seat's own character (enters, stays, leaves, none read) and cleared at a take or give-back",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"nav area: each seat's nav-area component ticks in that seat's scopes{}",
                  g_refresh != nullptr ? L"; its volumes enter again at each operator spawn" : L"");
    return true;
}

void OnOperatorStarted(int seat, void* seatController) noexcept
{
    void* component = nullptr;
    void* owner     = nullptr;
    if (g_refresh == nullptr || !SeatBuilt(seat) || seatController == nullptr ||
        !Read(static_cast<std::uint8_t*>(seatController) + facts::kControllerNavAreaComponent, component) || component == nullptr)
    {
        return;
    }
    const wchar_t* name = SeatName(seat);
    if (!Read(static_cast<std::uint8_t*>(component) + facts::kNavAreaComponentController, owner) || owner != seatController)
    {
        platform::LogDebug(L"nav area: {}'s component has not begun play at the operator spawn; its first tick enters", name);
        return;
    }
    const RoomRunScope  room(seat, seatController);
    const scopes::Scope scope(seat, kSeatScopes);
    if (!scope.Held())
    {
        platform::Log(L"nav area: {}'s scopes could not be held at the operator spawn; the volumes entered before it stay missing", name);
        return;
    }
    const scopes::Scope row(seat, scopes::UiSlot);
    if (!row.Held())
    {
        platform::LogDebug(L"nav area: {}'s UI slot could not be held at the operator spawn (no command controller bound?); a hook area "
                           L"entered again shows its row in player 1's menu", name);
    }
    // The game's own diff once, out of band as its camera reset runs it: volumes left during the scene Leave, new
    // ones Enter on the new operator. Then the listed volumes are forgotten with the map's own Remove, and the
    // diff again Enters each of them on the new operator through the game's own sequence.
    platform::CallGuarded(g_refresh, component, 0.0f);
    std::array<std::uint64_t, kMaxEntered> keys{};
    const std::size_t                      listed  = EnteredKeys(component, keys);
    std::uint32_t                          removed = 0;
    for (std::size_t i = 0; i < listed; ++i)
    {
        std::uint64_t gone = 0;
        if (platform::CallGuarded(g_removeKey, gone, static_cast<void*>(static_cast<std::uint8_t*>(component) + facts::kNavAreaEnteredMap), keys[i]) &&
            gone != 0)
        {
            ++removed;
        }
    }
    platform::CallGuarded(g_refresh, component, 0.0f);
    ++g_counts[seat].spawns;
    g_counts[seat].reentered += removed;
    platform::Log(L"nav area: {}'s component listed {} volumes at the operator spawn, {} forgotten and entered again on the new operator",
                  name, listed, removed);
}

void Tick() noexcept
{
    if (g_roomPlaced || g_roomSites[0] == 0)
    {
        return;
    }
    g_roomPlaced = true;  // once, whatever the outcome
    const auto target = reinterpret_cast<std::uintptr_t>(g_leader);
    static_assert(facts::kRoomLeaderCalls.size() == 3, "Enter, Stay, Leave");
    const std::array<std::uintptr_t, 3> replacements{reinterpret_cast<std::uintptr_t>(&RoomEnterLeader),
                                                     reinterpret_cast<std::uintptr_t>(&RoomStayLeader),
                                                     reinterpret_cast<std::uintptr_t>(&RoomLeaveLeader)};
    // All three or none: an Enter or Stay on the seat's character without its Leave would never be cleared.
    bool placed = true;
    for (std::size_t i = 0; placed && i < g_roomCalls.size(); ++i)
    {
        placed = g_roomCalls[i].InstallFromOwnThread(g_roomSites[i], target, replacements[i]);
    }
    if (!placed)
    {
        for (auto& call : g_roomCalls)
        {
            call.Restore();
        }
        platform::Log(L"nav area: a room handler's leader call could not be redirected; a further player's room limits player 1's "
                      L"character");
        return;
    }
    platform::Log(L"nav area: rooms per player: a room a further player walks in limits that player's own character");
}

void TickSeat(int seat, void* member) noexcept
{
    if (!SeatBuilt(seat) || !g_roomCalls[0].IsInstalled())
    {
        return;
    }
    if (member != nullptr && !weak_object::Live(member))
    {
        return;  // a stale pointer for a tick after a level change
    }
    weak_object::Weak& held = g_seatMember[seat];
    void* const        was  = weak_object::Resolve(held);
    if (member == was)
    {
        return;
    }
    // A member taken or given back keeps the room limit its last component wrote: a seat's controller ends with no
    // Leave, and running the game's Leave instead would pop records the next diff still needs (RE-room-walk-limit.md
    // §8). Cleared to the game's own "no room" value; a component holding it in a room writes it again on its next
    // diff (a new controller's Enters, a kept one's Stay). Player 1's room character is left to its own component.
    void* const one = RoomPlayerOne();
    const auto  otherSeats = [seat](const void* character) {
        for (int other = 1; other <= kSeatsBuilt; ++other)
        {
            if (other != seat && scopes::Member(other) == character)
            {
                return true;  // another seat's: its own component keeps it right
            }
        }
        return false;
    };
    for (void* character : {was, member})
    {
        if (character != nullptr && character != one && !otherSeats(character) && ClearRoomLimit(character))
        {
            ++g_counts[seat].roomClears;
            platform::LogDebug(L"nav area: {}'s {} member has its room walk limit cleared (its room check sets it again if "
                               L"it stands in one)",
                               SeatName(seat), character == member ? L"new" : L"former");
        }
    }
    held = member != nullptr ? weak_object::MakeWeak(member) : weak_object::Weak{};
}

void Remove() noexcept
{
    for (auto& call : g_roomCalls)
    {
        call.Restore();
    }
    g_slot.Restore();
}

}  // namespace lss::players::nav_area
