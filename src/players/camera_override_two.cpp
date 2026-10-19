#include "players/camera_override_two.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/scopes.h"

#include <cstdint>
#include <format>

namespace lss::players::camera_override_two {

namespace {

// The game's handler: (callable storage, the character's override params, four values, two more by pointer); its
// result goes back to the delegate's caller unread by the mod.
using HandlerFn = std::int64_t (*)(void*, void*, void*, void*, void*, void*, void*, void*);

constexpr int kPlayer = 1;

// Set at install, then only read.
std::uintptr_t g_handler = 0;

// Game thread.
void*         g_routed      = nullptr;  // the character whose delegate holds the routing handler
std::uint32_t g_calls       = 0;        // routing handler calls
std::uint32_t g_callsScoped = 0;        // of them, run with player 2's operator scope held

std::uintptr_t* Slot(void* character) noexcept
{
    return reinterpret_cast<std::uintptr_t*>(static_cast<std::uint8_t*>(character) + facts::kCharaCameraOverrideDelegate);
}

/// In player 2's member's delegate: the game's handler, on player 2's operator.
std::int64_t Routing(void* storage, void* params, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8)
{
    ++g_calls;
    const scopes::Scope scope(kPlayer, scopes::Operator);
    if (scope.Held())
    {
        ++g_callsScoped;
    }
    return reinterpret_cast<HandlerFn>(g_handler)(storage, params, a3, a4, a5, a6, a7, a8);
}

/// Writes `value` into `character`'s handler pointer if it holds `expected`. True if written.
bool Swap(void* character, std::uintptr_t expected, std::uintptr_t value) noexcept
{
    std::uintptr_t current = 0;
    return character != nullptr && platform::CopyGuarded(&current, Slot(character), sizeof(current)) && current == expected &&
           platform::WriteGuarded(Slot(character), &value, sizeof(value));
}

void Release() noexcept
{
    if (g_routed != nullptr)
    {
        const bool restored = Swap(g_routed, reinterpret_cast<std::uintptr_t>(&Routing), g_handler);
        platform::LogDebug(L"camera override 2: the game's handler back in the member's delegate: {}", restored ? L"yes" : L"it was no longer there");
        g_routed = nullptr;
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto handler = platform::FindFunction(image, facts::kCharaCameraOverrideHandler);
    if (!handler)
    {
        platform::Log(L"camera override 2: the game's handler differs; player 2's attack cameras stay on player 1's camera");
        return false;
    }
    g_handler = *handler;
    control::Register({.name = L"camera-override-two", .shape = control::Shape::Text,
                       .help = L"player 2's member's camera-override calls routed to player 2's operator: calls, of them scoped",
                       .get  = [] { return control::Value{std::format(L"calls {}, scoped {}", g_calls, g_callsScoped)}; }});
    return true;
}

void Remove() noexcept
{
    Release();
}

void Route(void* character) noexcept
{
    if (g_handler == 0)
    {
        return;
    }
    if (character != g_routed)
    {
        Release();
    }
    if (character == nullptr)
    {
        return;
    }
    if (Swap(character, g_handler, reinterpret_cast<std::uintptr_t>(&Routing)))
    {
        g_routed = character;
        platform::LogDebug(L"camera override 2: player 2's member's camera overrides routed to player 2's operator");
    }
}

}  // namespace lss::players::camera_override_two
