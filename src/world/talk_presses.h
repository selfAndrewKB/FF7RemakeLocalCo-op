#pragma once

// A further player's NPC presses, waiting for the act the NPC's talk state queues (world/shop_act, world/talk_gather),
// as a pure table: the rules without the game (world/field_targets holds the game side: the press itself, the
// pressed object's place from the game's target entry, the field lock, the target manager, the log lines). The rules
// (research/RE-talk-two.md, RE-talk-far.md; built over v0.100.0-v0.103.1 inside field_targets, moved here in cleanup
// step 4, plan/code-alignment.md C2):
//   1. a newer press of the same seat replaces its older one;
//   2. player 1's press of the same object drops every seat's press of it (the talk is player 1's);
//   3. a world change drops every press;
//   4. a press lapses after kLapseMs with no act, but its wait counts only while no field lock holds (a running talk
//      or greeting holds it, so a long talk or a choice before the shop never lapses a waiting press);
//   5. the pressed object's place, noted at the press, stays with the press;
//   6. once the gather judged a press (player 1's character moved or not), a later act of the talk never moves it
//      again; a claim takes every seat's press of the object: one act answers one press.
// A seat that leaves or changes character loses its presses. Host-tested (tests/talk_presses_test.cpp). Game thread.

#include "players/seats.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::world::talk_presses {

constexpr std::size_t   kCapacity = 8;      // presses waiting for their acts, all seats together
constexpr std::uint64_t kLapseMs  = 10000;  // the wait with no field lock held

struct Press
{
    std::uint64_t        name      = 0;  // the pressed object (the act channel's object); 0: no press
    int                  seat      = 0;
    std::uint64_t        at        = 0;  // the press's time, moved on while the field lock holds
    bool                 judged    = false;  // whether to bring player 1's character to this talk was decided
    std::array<float, 3> spot{};         // the pressed object's place at the press, when known
    bool                 spotKnown = false;
};

/// What an operation dropped, for the caller's log lines.
struct Dropped
{
    std::array<Press, kCapacity> presses{};
    int                          count = 0;

    void Add(const Press& press) noexcept
    {
        if (count < static_cast<int>(kCapacity)) presses[static_cast<std::size_t>(count++)] = press;
    }
};

class Table
{
public:
    /// Rule 1. The press noted, or null when the table is full (the caller warns). `dropped` gets the seat's older
    /// press, if any.
    Press* Note(int seat, std::uint64_t name, std::uint64_t now, Dropped& dropped) noexcept
    {
        Press* free = nullptr;
        for (Press& press : presses_)
        {
            if (press.name != 0 && press.seat == seat)
            {
                dropped.Add(press);
                press = {};
            }
            if (free == nullptr && press.name == 0)
            {
                free = &press;
            }
        }
        if (free == nullptr)
        {
            return nullptr;
        }
        *free = Press{.name = name, .seat = seat, .at = now};
        return free;
    }

    /// Rule 2: player 1 pressed `name`.
    void DropOf(std::uint64_t name, Dropped& dropped) noexcept
    {
        DropWhere([name](const Press& p) { return p.name == name; }, dropped);
    }

    /// Rule 3: the world changed.
    void DropAll(Dropped& dropped) noexcept
    {
        DropWhere([](const Press&) { return true; }, dropped);
    }

    /// The seat left or changed character.
    void DropSeat(int seat, Dropped& dropped) noexcept
    {
        DropWhere([seat](const Press& p) { return p.seat == seat; }, dropped);
    }

    /// Rule 4, each tick: `locked` restarts every wait; else the presses older than kLapseMs lapse into `lapsed`. The
    /// caller applies rule 3 (DropAll at a world change) before this.
    void Tick(std::uint64_t now, bool locked, Dropped& lapsed) noexcept
    {
        for (Press& press : presses_)
        {
            if (press.name == 0)
            {
                continue;
            }
            if (locked)
            {
                press.at = now;
            }
            else if (now - press.at > kLapseMs)
            {
                lapsed.Add(press);
                press = {};
            }
        }
    }

    /// The seat whose press named `object`; 0 when none (player 1's talk, or the press lapsed). A press of a seat that
    /// is not built does not count (it cannot be answered).
    int Peek(std::uint64_t object) const noexcept
    {
        for (const Press& press : presses_)
        {
            if (object != 0 && press.name == object && players::SeatBuilt(press.seat))
            {
                return press.seat;
            }
        }
        return 0;
    }

    /// Rule 6: one act answers one press. The seat Peek named; every seat's press of the object is taken.
    int Claim(std::uint64_t object) noexcept
    {
        const int seat = Peek(object);
        for (Press& press : presses_)
        {
            if (object != 0 && press.name == object)
            {
                press = {};
            }
        }
        return seat;
    }

    /// Rule 6: whether the gather judged a press of `object`, and marking it.
    bool Judged(std::uint64_t object) const noexcept
    {
        for (const Press& press : presses_)
        {
            if (object != 0 && press.name == object && press.judged) return true;
        }
        return false;
    }
    void MarkJudged(std::uint64_t object) noexcept
    {
        for (Press& press : presses_)
        {
            if (object != 0 && press.name == object) press.judged = true;
        }
    }

    /// Rule 5: the pressed object's place as noted at the press (a built seat's press of the object with a known place:
    /// the same object's place whichever seat's record it is); false when unknown.
    bool Spot(std::uint64_t object, std::array<float, 3>& out) const noexcept
    {
        for (const Press& press : presses_)
        {
            if (object != 0 && press.name == object && press.spotKnown && players::SeatBuilt(press.seat))
            {
                out = press.spot;
                return true;
            }
        }
        return false;
    }

    bool Any() const noexcept
    {
        for (const Press& press : presses_)
        {
            if (press.name != 0) return true;
        }
        return false;
    }

    void Clear() noexcept { presses_ = {}; }

private:
    template <class Pred>
    void DropWhere(Pred pred, Dropped& dropped) noexcept
    {
        for (Press& press : presses_)
        {
            if (press.name != 0 && pred(press))
            {
                dropped.Add(press);
                press = {};
            }
        }
    }

    std::array<Press, kCapacity> presses_{};
};

}  // namespace lss::world::talk_presses
