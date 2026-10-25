#include "players/nav_area_two.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/scopes.h"
#include "players/seat_objects.h"
#include "players/seats.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::players::nav_area_two {

namespace {

using platform::Read;

// Stock UActorComponent::TickComponent(DeltaTime, TickType, ThisTickFunction); the override reads only this and
// the delta, but every argument is passed on.
using TickFn      = void (*)(void* component, float deltaTime, int tickType, void* tickFunction);
using RefreshFn   = void (*)(void* component, float deltaTime);   // the diff the tick tail-jumps to
using RemoveKeyFn = std::uint64_t (*)(void* map, std::uint64_t key);  // the entered map's own Remove, 1 if removed

constexpr std::size_t kMaxEntered = 32;  // a leader stands in at most 16 nav areas

// The seat's camera operator, leader and primary controller: what the diff's camera settings and marks reach.
constexpr unsigned kSeatScopes = scopes::Primary | scopes::Operator | scopes::Leader;

// Set at install, then only read.
TickFn                g_tick      = nullptr;
RefreshFn             g_refresh   = nullptr;  // null: the re-enter at a spawn is off
RemoveKeyFn           g_removeKey = nullptr;
platform::VTableEntry g_slot;

// Game thread. Index = seat ([0] unused).
struct Counts
{
    std::uint32_t ticks     = 0;  // ticks of the seat's controller's component
    std::uint32_t scoped    = 0;  // of them, run with all three scopes held
    std::uint32_t ownRow    = 0;  // of those, with the seat's command controller in the UI slot too
    std::uint32_t rowOnly   = 0;  // the three not held (a scene holds the member), the seat's row held
    std::uint32_t spawns    = 0;  // operator spawns the re-enter ran at
    std::uint32_t reentered = 0;  // volumes forgotten and entered again over them
};
std::array<Counts, scopes::kMaxPlayers> g_counts;

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
        text += std::format(L"{}{}: ticks {}, scoped {}, own row {}, own row only {}, spawns {}, re-entered {}", seat == 1 ? L"" : L"; ",
                            SeatName(seat), c.ticks, c.scoped, c.ownRow, c.rowOnly, c.spawns, c.reentered);
    }
    return text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tick = platform::FindFunction(image, facts::kNavAreaComponentTick.target);
    if (!tick)
    {
        platform::Log(L"nav area 2: the component's tick differs; a seat's InDoor camera settings reach player 1's operator");
        return false;
    }
    g_tick = reinterpret_cast<TickFn>(*tick);
    if (!platform::ReplaceSlot(image, facts::kNavAreaComponentTick, reinterpret_cast<std::uintptr_t>(&TickComponent), g_slot))
    {
        platform::Log(L"nav area 2: the component's tick slot could not be replaced; a seat's InDoor camera settings reach player 1's operator");
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
        platform::Log(L"nav area 2: the component's diff or its map's Remove differs; a respawned operator keeps missing the InDoor setting pushed before it");
    }
    control::Register({.name = L"nav-area-two", .shape = control::Shape::Text,
                       .help = L"each seat's controller's nav-area component ticks: all, of them in the seat's scopes, of those with its own command row, with its row alone; operator spawns re-entered at, volumes entered again",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"nav area 2: each seat's nav-area component ticks in that seat's scopes{}",
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
        platform::LogDebug(L"nav area 2: {}'s component has not begun play at the operator spawn; its first tick enters", name);
        return;
    }
    const scopes::Scope scope(seat, kSeatScopes);
    if (!scope.Held())
    {
        platform::Log(L"nav area 2: {}'s scopes could not be held at the operator spawn; the volumes entered before it stay missing", name);
        return;
    }
    const scopes::Scope row(seat, scopes::UiSlot);
    if (!row.Held())
    {
        platform::LogDebug(L"nav area 2: {}'s UI slot could not be held at the operator spawn (no command controller bound?); a hook area "
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
    platform::Log(L"nav area 2: {}'s component listed {} volumes at the operator spawn, {} forgotten and entered again on the new operator",
                  name, listed, removed);
}

void Remove() noexcept
{
    g_slot.Restore();
}

}  // namespace lss::players::nav_area_two
