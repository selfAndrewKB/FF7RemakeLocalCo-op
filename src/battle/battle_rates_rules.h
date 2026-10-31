#pragma once

// The battle's rates (battle/battle_rates.h): the multipliers' words and the arithmetic, done the game's way. Pure:
// tested on the host (tests/battle_rates_rules_test.cpp).
//
// A multiplier is held as tenths (1 to 100: 0.1 to 10.0), the settings table's words (platform/settings.h). Every
// scaled value is rounded as the game rounds its own rates (research/RE-enemy-stat-scaling.md §3.2-§3.3):
// `(int)(2 * v * m + 0.5) >> 1` in float (157.5 gives 157, 157.75 gives 158); a scaled HP, stagger gauge or hit is at
// least 1, as the game's own difficulty step keeps a hit. The EXP and AP rates are the game's integer percents (RE-streamlined-progression.md
// §2.2), whole at every tenth.

#include "platform/settings.h"

#include <climits>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lss::battle::battle_rates_rules {

inline constexpr int kOne = 10;  // 1.0: the game's own value, nothing changed

/// The tenths a word stands for (its place in the table's words, plus one); none if it is no multiplier word.
inline std::optional<int> TenthsOf(std::wstring_view word) noexcept
{
    for (std::size_t i = 0; i < platform::settings::kTenthsWords.size(); ++i)
    {
        if (word == platform::settings::kTenthsWords[i])
        {
            return static_cast<int>(i) + 1;
        }
    }
    return std::nullopt;
}

/// The word for `tenths` (1 to 100), or "1.0" outside them.
inline const wchar_t* WordOf(int tenths) noexcept
{
    return tenths >= 1 && tenths <= static_cast<int>(platform::settings::kTenthsMax)
               ? platform::settings::kTenthsWords[static_cast<std::size_t>(tenths - 1)]
               : L"1.0";
}

/// `value` times `tenths / 10`, rounded as the game rounds its rates (float, `(int)(2x + 0.5) >> 1`), at least 1 for a
/// positive value; a value of 0 or less is returned as it is. Capped below INT_MAX / 2 so the doubling cannot overflow.
inline std::int32_t Scaled(std::int32_t value, int tenths) noexcept
{
    if (value <= 0 || tenths == kOne)
    {
        return value;
    }
    const float m     = static_cast<float>(tenths) * 0.1f;
    const float twice = static_cast<float>(value) * m + static_cast<float>(value) * m + 0.5f;
    if (twice >= static_cast<float>(INT_MAX - 1))
    {
        return INT_MAX / 2;
    }
    const std::int32_t scaled = static_cast<std::int32_t>(twice) >> 1;
    return scaled < 1 ? 1 : scaled;
}

/// The game's float rate times `tenths / 10` (the difficulty coefficient rows, Easy and Hard); exactly the game's at 1.0.
inline float ScaledRate(float stock, int tenths) noexcept
{
    return tenths == kOne ? stock : stock * static_cast<float>(tenths) * 0.1f;
}

/// The percent an EXP or AP bonus row carries for one award: the game's own row (`stock`, 200 or 300) while its bonus
/// applies (`open`: the story cleared, or the save's 2x EXP & 3x AP on), else 100; times `tenths / 10`, rounded half
/// up to a whole percent (exact at every tenth).
inline std::int32_t AwardPercent(std::int32_t stock, bool open, int tenths) noexcept
{
    const std::int64_t base = open ? stock : 100;
    const std::int64_t pct  = (base * tenths + 5) / 10;
    return pct > INT_MAX ? INT_MAX : static_cast<std::int32_t>(pct);
}

/// Whether the game's own per-hit difficulty step would see this hit, by its damage source's kind (the step's own
/// conditions besides the sides and heal: research/RE-enemy-stat-scaling.md §3.3): damage types 2 to 9 (limit,
/// status and ATB effects) and 12 and above (eat, steal) never reach it, nor power type 11 (HP to one digit).
inline bool HitKindScales(std::uint8_t damageType, std::uint8_t powerType) noexcept
{
    const bool typeScales = damageType < 2 || damageType == 10 || damageType == 11;
    return typeScales && powerType != 11;
}

}  // namespace lss::battle::battle_rates_rules
