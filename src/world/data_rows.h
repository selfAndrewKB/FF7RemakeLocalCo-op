#pragma once

// The level's data rows, as the game reads them to drive its story and its spots: the state-trigger, state-change,
// action and action-group data tables (three tables each, searched in turn through the tables' own row lookup, the one
// StateMgr_FireStateTrigger and StateSlot_SetState call), and the engine arrays a row holds. Rows are static data,
// valid while their table is loaded: callers read them within one game-thread call and keep only names. The trigger and
// state-change tables are required; the action tables are optional (a module that needs them asks Installed).

#include "platform/guarded.h"

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace lss::platform {
class GameImage;
}

namespace lss::world::data_rows {

enum class Table : std::uint8_t
{
    Trigger,
    StateChange,
    Action,
    ActionGroup,
};

/// Finds the tables in the image; false when the trigger or state-change tables are not where the facts say (the
/// action tables missing is logged and leaves them out).
bool Install(const platform::GameImage& image) noexcept;

/// The table set was found at install.
bool Installed(Table table) noexcept;

/// The address of the table set's three table pointers, 0 when it was not found.
std::uintptr_t Tables(Table table) noexcept;

/// The row named `name` in the table set, through the tables' own lookup; null when no table has it.
const std::uint8_t* Row(Table table, std::int64_t name) noexcept;

/// An engine array field (data pointer at `field`, count at `field + 8`): its elements, at most `cap`; empty when it
/// cannot be read. May throw (allocation): callers catch.
template <typename T>
std::vector<T> Array(const std::uint8_t* field, std::uint32_t cap)
{
    static_assert(std::is_trivially_copyable_v<T>, "copied as raw bytes");
    const std::uint8_t* data = nullptr;
    std::int32_t        num  = 0;
    std::vector<T>      out;
    if (!platform::Read(field, data) || !platform::Read(field + 8, num) || data == nullptr || num <= 0)
    {
        return out;
    }
    out.resize(std::min<std::uint32_t>(static_cast<std::uint32_t>(num), cap));
    if (!platform::CopyGuarded(out.data(), data, out.size() * sizeof(T)))
    {
        out.clear();
    }
    return out;
}

/// An FName array field's names (Array of their 8 bytes).
inline std::vector<std::int64_t> Names(const std::uint8_t* field, std::uint32_t cap)
{
    return Array<std::int64_t>(field, cap);
}

}  // namespace lss::world::data_rows
