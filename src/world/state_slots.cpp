#include "world/state_slots.h"

#include "platform/name_text.h"
#include "world/data_rows.h"

#include <algorithm>

namespace lss::world::state_slots {

namespace {

using platform::Read;

/// A next-state name is read whole (the FName limit), or its lookup misses.
constexpr std::size_t kMaxStateChars = 1024;
/// A row's NextStateChange entries are FStrings.
constexpr std::size_t kFStringBytes = 16;
/// A state's enter actions read (acts or act groups).
constexpr std::uint32_t kEnterActionsRead = 64;

}  // namespace

std::vector<Slot> Active(const std::uint8_t* sm) noexcept
try
{
    std::vector<Slot> slots;
    slots.reserve(8192);
    ForEach(sm, [&](const std::uint8_t* slot) noexcept {
        Slot s;
        if (Read(slot + facts::kStateSlotObject, s.object) && Read(slot + facts::kStateSlotState, s.state) &&
            Read(slot + facts::kStateSlotRow, s.row))
        {
            slots.push_back(s);
        }
        return false;
    });
    return slots;
}
catch (...)
{
    return {};
}

std::wstring Text(const std::uint8_t* fstring, std::size_t cap) noexcept
try
{
    const wchar_t* data = nullptr;
    std::int32_t   num  = 0;
    if (!Read(fstring, data) || !Read(fstring + 8, num) || data == nullptr || num <= 1)
    {
        return L"";
    }
    const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(num) - 1, cap);
    std::wstring      text(count, L'\0');
    if (!platform::CopyGuarded(text.data(), data, count * sizeof(wchar_t)))
    {
        return L"";
    }
    return text;
}
catch (...)
{
    return L"";
}

const std::uint8_t* StateRow(const std::wstring& state) noexcept
{
    const auto name = static_cast<std::int64_t>(platform::FindName(state.c_str()));
    return name == 0 ? nullptr : data_rows::Row(data_rows::Table::StateChange, name);
}

std::vector<std::int64_t> Triggers(const std::uint8_t* row)
{
    return data_rows::Names(row + facts::kStateRowTriggers, facts::kStateRowTriggersRead);
}

std::optional<std::wstring> NextStateAt(const std::uint8_t* row, std::size_t index) noexcept
{
    const std::uint8_t* data = nullptr;
    std::int32_t        num  = 0;
    if (!Read(row + facts::kStateRowNextStates, data) || !Read(row + facts::kStateRowNextStates + 8, num) || data == nullptr ||
        num < 0 || index >= static_cast<std::size_t>(num))
    {
        return std::nullopt;
    }
    return Text(data + kFStringBytes * index, kMaxStateChars);
}

std::vector<std::wstring> NextStates(const std::uint8_t* row) noexcept
try
{
    std::vector<std::wstring> out;
    for (std::size_t i = 0; i < facts::kStateRowTriggersRead; ++i)
    {
        std::optional<std::wstring> next = NextStateAt(row, i);
        if (!next)
        {
            break;
        }
        if (!next->empty())
        {
            out.push_back(std::move(*next));
        }
    }
    return out;
}
catch (...)
{
    return {};
}

std::vector<Step> Steps(const std::uint8_t* row)
{
    std::vector<Step>               steps;
    const std::vector<std::int64_t> triggers = Triggers(row);
    for (std::size_t i = 0; i < triggers.size() && triggers[i] != 0; ++i)
    {
        steps.push_back({triggers[i], NextStateAt(row, i)});
    }
    return steps;
}

std::vector<std::int64_t> EnteredActs(const std::uint8_t* row)
{
    std::vector<std::int64_t> acts;
    for (const std::int64_t entered : data_rows::Names(row + facts::kStateRowEnterActions, kEnterActionsRead))
    {
        if (entered == 0)
        {
            continue;
        }
        if (const std::uint8_t* group = data_rows::Row(data_rows::Table::ActionGroup, entered))
        {
            const std::vector<std::int64_t> grouped = data_rows::Names(group + facts::kActionGroupRowActions, facts::kActionGroupRowActionsMax);
            acts.insert(acts.end(), grouped.begin(), grouped.end());
        }
        else
        {
            acts.push_back(entered);
        }
    }
    return acts;
}

}  // namespace lss::world::state_slots
