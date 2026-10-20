#include "players/nav_area_two.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/scopes.h"

#include <cstdint>
#include <format>

namespace lss::players::nav_area_two {

namespace {

using platform::Read;

// Stock UActorComponent::TickComponent(DeltaTime, TickType, ThisTickFunction); the override reads only this and
// the delta, but every argument is passed on.
using TickFn = void (*)(void* component, float deltaTime, int tickType, void* tickFunction);

constexpr int kPlayer = 1;

// Set at install, then only read.
TickFn                g_tick = nullptr;
platform::VTableEntry g_slot;

// Game thread.
std::uint32_t g_ticksTwo   = 0;  // ticks of player 2's controller's component
std::uint32_t g_ticksScoped = 0;  // of them, run with all three scopes held

void TickComponent(void* component, float deltaTime, int tickType, void* tickFunction)
{
    void* controller = nullptr;
    if (!Read(static_cast<std::uint8_t*>(component) + facts::kNavAreaComponentController, controller) ||
        controller == nullptr || controller != scopes::Controller(kPlayer))
    {
        g_tick(component, deltaTime, tickType, tickFunction);
        return;
    }
    ++g_ticksTwo;
    const scopes::Scope scope(kPlayer, scopes::Primary | scopes::Operator | scopes::Leader);
    if (scope.Held())
    {
        ++g_ticksScoped;
    }
    g_tick(component, deltaTime, tickType, tickFunction);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tick = platform::FindFunction(image, facts::kNavAreaComponentTick.target);
    if (!tick)
    {
        platform::Log(L"nav area 2: the component's tick differs; player 2's InDoor camera settings reach player 1's operator");
        return false;
    }
    g_tick = reinterpret_cast<TickFn>(*tick);
    if (!platform::ReplaceSlot(image, facts::kNavAreaComponentTick, reinterpret_cast<std::uintptr_t>(&TickComponent), g_slot))
    {
        platform::Log(L"nav area 2: the component's tick slot could not be replaced; player 2's InDoor camera settings reach player 1's operator");
        return false;
    }
    control::Register({.name = L"nav-area-two", .shape = control::Shape::Text,
                       .help = L"player 2's controller's nav-area component ticks: all, of them in player 2's scopes",
                       .get  = [] { return control::Value{std::format(L"ticks {}, scoped {}", g_ticksTwo, g_ticksScoped)}; }});
    platform::Log(L"nav area 2: player 2's nav-area component ticks in player 2's scopes");
    return true;
}

void Remove() noexcept
{
    g_slot.Restore();
}

}  // namespace lss::players::nav_area_two
