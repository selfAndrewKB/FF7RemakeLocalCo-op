#pragma once

// The level's states as the state manager keeps them, read for the modules that judge them (players/lead_gate's
// waiters and paths, world/stuck_spots' repair): the active list walked slot by slot, each active slot's object, current
// state and state row, a state row's triggers with the next state each one leads to (same index), and the state rows by
// name (research/RE-stuck-spot-repair.md, RE-lead-arrival-wait.md). Reads only, each one guarded; game thread, where
// the game changes the slots.

#include "engine_facts.h"
#include "platform/guarded.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lss::world::state_slots {

/// An active slot: its level object, its current state and that state's row (null when unread).
struct Slot
{
    std::int64_t        object = 0, state = 0;
    const std::uint8_t* row    = nullptr;
};

/// How a walk of the active list ended: `fn` stopped it, the list ended where it began, or the walk was cut short (a
/// node, an index or a slot pointer unread, or the cap reached): a census that must be whole treats Cut as a failure.
enum class Walk : std::uint8_t
{
    Stopped,
    Ended,
    Cut,
};

/// Walks the state manager `sm`'s active list, calling `fn(slot)` with each active slot's address (the list's node names
/// a slot of the array) until `fn` returns true or the list ends; capped at the slot array's size.
template <typename Fn>
Walk ForEach(const std::uint8_t* sm, Fn&& fn) noexcept
{
    const std::uint8_t* node = nullptr;
    const std::uint8_t* end  = sm + facts::kStateManagerActiveList;
    if (!platform::Read(sm + facts::kStateManagerActiveHead, node))
    {
        return Walk::Cut;
    }
    bool whole = true;
    for (std::uint32_t n = 0; node != nullptr && node != end; ++n)
    {
        if (n == facts::kStateManagerSlotCount)
        {
            return Walk::Cut;
        }
        std::int32_t        index = -1;
        const std::uint8_t* slot  = nullptr;
        if (platform::Read(node + facts::kStateNodeSlotIndex, index) && index >= 0 &&
            static_cast<std::uint32_t>(index) < facts::kStateManagerSlotCount &&
            platform::Read(sm + facts::kStateManagerSlots + 8u * static_cast<std::uint32_t>(index), slot) && slot != nullptr)
        {
            if (fn(slot))
            {
                return Walk::Stopped;
            }
        }
        else
        {
            whole = false;
        }
        if (!platform::Read(node + facts::kStateNodeNext, node))
        {
            return Walk::Cut;
        }
    }
    return node == end && whole ? Walk::Ended : Walk::Cut;
}

/// The state manager's active slots (object, state, row), capped at the slot array's size.
std::vector<Slot> Active(const std::uint8_t* sm) noexcept;

/// An FString's text (`data` pointer, `num` count with the terminator), guarded and capped at `cap` characters.
std::wstring Text(const std::uint8_t* fstring, std::size_t cap) noexcept;

/// The state row named `state` (the game's state-change tables); null if unknown.
const std::uint8_t* StateRow(const std::wstring& state) noexcept;

/// A state row's triggers, the ones the game reads (None entries included: the game stops at the first). May throw
/// (allocation): callers catch.
std::vector<std::int64_t> Triggers(const std::uint8_t* row);

/// The next state a row's trigger at `index` leads to (its NextStateChange entry at the same index, read whole);
/// nullopt when the row's list is shorter or cannot be read; an empty text when that entry is empty.
std::optional<std::wstring> NextStateAt(const std::uint8_t* row, std::size_t index) noexcept;

/// A row's next states, the empty ones skipped, at most one per trigger the game reads.
std::vector<std::wstring> NextStates(const std::uint8_t* row) noexcept;

/// One trigger a state waits on and the next state it leads to (nullopt when the row's next list is shorter or unread).
struct Step
{
    std::int64_t                trigger = 0;
    std::optional<std::wstring> next;
};

/// A state row's steps as the game reads them: its triggers up to the first None, each with its next state. May throw
/// (allocation): callers catch.
std::vector<Step> Steps(const std::uint8_t* row);

/// The acts a state row starts on entry: each of its enter actions that names an act group gives the group's acts (the
/// group tables tried first, as ActSequence_QueueActionOrGroup does), any other the name itself. May throw (allocation):
/// callers catch.
std::vector<std::int64_t> EnteredActs(const std::uint8_t* row);

}  // namespace lss::world::state_slots
