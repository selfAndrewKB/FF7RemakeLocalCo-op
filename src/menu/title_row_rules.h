#pragma once

// Where the title menu's Co-op row goes among its rows' kinds (research/RE-title-menu.md): directly below Options, so
// the rows through Options keep their places. Pure: tested on the host (tests/title_row_rules_test.cpp).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace lss::menu::title_row_rules {

/// The place to insert `coop` in `kinds`, directly below `options`. None when the row is already there (the construct
/// did not rebuild the list) or the list has no Options row.
inline std::optional<std::size_t> PlaceBelow(std::span<const std::int32_t> kinds, std::int32_t options, std::int32_t coop) noexcept
{
    std::optional<std::size_t> place;
    for (std::size_t i = 0; i < kinds.size(); ++i)
    {
        if (kinds[i] == coop)
        {
            return std::nullopt;
        }
        if (kinds[i] == options)
        {
            place = i + 1;
        }
    }
    return place;
}

}  // namespace lss::menu::title_row_rules
