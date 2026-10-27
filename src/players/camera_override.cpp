#include "players/camera_override.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace lss::players::camera_override {

namespace {

// The game's handler: (callable storage, the character's override params, four values, two more by pointer); its
// result goes back to the delegate's caller unread by the mod.
using HandlerFn = std::int64_t (*)(void*, void*, void*, void*, void*, void*, void*, void*);

// Set at install, then only read.
std::uintptr_t g_handler = 0;

// Game thread. Index = seat ([0] unused).
struct SeatRoute
{
    void*         routed      = nullptr;  // the character whose delegate holds the seat's routing handler
    std::uint32_t calls       = 0;        // routing handler calls
    std::uint32_t callsScoped = 0;        // of them, run with the seat's operator scope held
};
std::array<SeatRoute, scopes::kMaxPlayers> g_seats;

std::uintptr_t* Slot(void* character) noexcept
{
    return reinterpret_cast<std::uintptr_t*>(static_cast<std::uint8_t*>(character) + facts::kCharaCameraOverrideDelegate);
}

/// In the seat's member's delegate: the game's handler, on the seat's operator. One function per seat: the
/// delegate passes the character's parameters and nothing that names the seat. Each body touches its own
/// g_seats[Seat], so the linker cannot fold the instances into one address (Swap tells the seats apart by it).
template <int Seat>
std::int64_t Routing(void* storage, void* params, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8)
{
    ++g_seats[Seat].calls;
    const scopes::Scope scope(Seat, scopes::Operator);
    if (scope.Held())
    {
        ++g_seats[Seat].callsScoped;
    }
    return reinterpret_cast<HandlerFn>(g_handler)(storage, params, a3, a4, a5, a6, a7, a8);
}

template <std::size_t... I>
constexpr std::array<HandlerFn, sizeof...(I)> MakeRouting(std::index_sequence<I...>) noexcept
{
    return {&Routing<static_cast<int>(I)>...};  // [0] is never used (player 1 is the game's)
}
constexpr auto kRouting = MakeRouting(std::make_index_sequence<scopes::kMaxPlayers>{});

std::uintptr_t RoutingOf(int seat) noexcept
{
    return seat >= 0 && seat < scopes::kMaxPlayers ? reinterpret_cast<std::uintptr_t>(kRouting[seat]) : 0;
}

/// Writes `value` into `character`'s handler pointer if it holds `expected`. True if written.
bool Swap(void* character, std::uintptr_t expected, std::uintptr_t value) noexcept
{
    std::uintptr_t current = 0;
    return character != nullptr && platform::CopyGuarded(&current, Slot(character), sizeof(current)) && current == expected &&
           platform::WriteGuarded(Slot(character), &value, sizeof(value));
}

void Release(int seat) noexcept
{
    if (g_seats[seat].routed != nullptr)
    {
        const bool restored = Swap(g_seats[seat].routed, RoutingOf(seat), g_handler);
        platform::LogDebug(L"camera override: the game's handler back in {}'s member's delegate: {}", SeatName(seat),
                           restored ? L"yes" : L"it was no longer there");
        g_seats[seat].routed = nullptr;
    }
}

std::wstring Reading()
{
    std::wstring text;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        text += std::format(L"{}{}: calls {}, scoped {}", seat == 1 ? L"" : L"; ", SeatName(seat), g_seats[seat].calls, g_seats[seat].callsScoped);
    }
    return text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto handler = platform::FindFunction(image, facts::kCharaCameraOverrideHandler);
    if (!handler)
    {
        platform::Log(L"camera override: the game's handler differs; a seat's attack cameras stay on player 1's camera");
        return false;
    }
    g_handler = *handler;
    control::Register({.name = L"camera-override", .shape = control::Shape::Text,
                       .help = L"each seat's member's camera-override calls routed to that seat's operator: calls, of them scoped",
                       .get  = [] { return control::Value{Reading()}; }});
    return true;
}

void Remove() noexcept
{
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        Release(seat);
    }
}

void Route(int seat, void* character) noexcept
{
    if (g_handler == 0 || !SeatBuilt(seat))
    {
        return;
    }
    if (character != g_seats[seat].routed)
    {
        Release(seat);
    }
    if (character == nullptr)
    {
        return;
    }
    if (Swap(character, g_handler, RoutingOf(seat)))
    {
        g_seats[seat].routed = character;
        platform::LogDebug(L"camera override: {}'s member's camera overrides routed to {}'s operator", SeatName(seat), SeatName(seat));
    }
}

}  // namespace lss::players::camera_override
