#pragma once

// The size of a further player's own lead marker (hud/lead_mark), from the setting's word (platform/settings.h
// kLeadMarkerSize: "1.0" ... "3.0"). Pure (host-tested in tests/lead_mark_rules_test.cpp).

#include <string_view>

namespace lss::hud::lead_mark_rules {

inline constexpr float kSmallest = 1.0f;  // the game's own size
inline constexpr float kLargest  = 3.0f;

/// The factor a word names: one digit, a point and one digit, from kSmallest to kLargest. Anything else (another
/// shape, out of range, empty) is the game's own size.
constexpr float ScaleOf(std::wstring_view word) noexcept
{
    if (word.size() != 3 || word[1] != L'.' || word[0] < L'0' || word[0] > L'9' || word[2] < L'0' || word[2] > L'9')
    {
        return kSmallest;
    }
    const float scale = static_cast<float>(word[0] - L'0') + static_cast<float>(word[2] - L'0') / 10.0f;
    return scale >= kSmallest && scale <= kLargest ? scale : kSmallest;
}

}  // namespace lss::hud::lead_mark_rules
