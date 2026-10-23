#include "players/seat_objects.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "players/seats.h"
#include "players/weak_object.h"

#include <array>

namespace lss::players::seat_objects {

namespace {

using weak_object::MakeWeak;
using weak_object::Resolve;
using weak_object::Weak;

/// One seat's objects as weak pairs; unset when unbound.
struct Seat
{
    Weak controller;
    Weak cameraOperator;
    Weak member;
};
std::array<Seat, scopes::kMaxPlayers> g_seats;  // by seat; [0] unused (player 1's objects are the game's)

}  // namespace

void BindController(int seat, void* controller) noexcept
{
    if (SeatBuilt(seat))
    {
        g_seats[static_cast<std::size_t>(seat)].controller = MakeWeak(controller);
    }
}

void BindOperator(int seat, void* cameraOperator) noexcept
{
    if (SeatBuilt(seat))
    {
        g_seats[static_cast<std::size_t>(seat)].cameraOperator = MakeWeak(cameraOperator);
    }
}

void BindMember(int seat, void* member) noexcept
{
    if (SeatBuilt(seat))
    {
        g_seats[static_cast<std::size_t>(seat)].member = MakeWeak(member);
    }
}

void Forget(int seat) noexcept
{
    if (SeatBuilt(seat))
    {
        g_seats[static_cast<std::size_t>(seat)] = {};
    }
}

void* Controller(int seat) noexcept
{
    return SeatBuilt(seat) ? Resolve(g_seats[static_cast<std::size_t>(seat)].controller) : nullptr;
}

void* Operator(int seat) noexcept
{
    return SeatBuilt(seat) ? Resolve(g_seats[static_cast<std::size_t>(seat)].cameraOperator) : nullptr;
}

void* Member(int seat) noexcept
{
    return SeatBuilt(seat) ? Resolve(g_seats[static_cast<std::size_t>(seat)].member) : nullptr;
}

Weak OperatorWeak(int seat) noexcept
{
    return SeatBuilt(seat) ? g_seats[static_cast<std::size_t>(seat)].cameraOperator : Weak{};
}

int SeatOf(const void* object) noexcept
{
    for (int seat = 1; object != nullptr && seat <= kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[static_cast<std::size_t>(seat)];
        if (Resolve(s.controller) == object || Resolve(s.cameraOperator) == object || Resolve(s.member) == object)
        {
            return seat;
        }
    }
    return 0;
}

int SeatOfCameraManager(const void* manager) noexcept
{
    for (int seat = 1; manager != nullptr && seat <= kSeatsBuilt; ++seat)
    {
        void* controller = Resolve(g_seats[static_cast<std::size_t>(seat)].controller);
        if (controller != nullptr && platform::ReadPointer(controller, facts::kControllerCameraManager) == manager)
        {
            return seat;
        }
    }
    return 0;
}

bool AnySeatIn() noexcept
{
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (Resolve(g_seats[static_cast<std::size_t>(seat)].controller) != nullptr)
        {
            return true;
        }
    }
    return false;
}

}  // namespace lss::players::seat_objects
