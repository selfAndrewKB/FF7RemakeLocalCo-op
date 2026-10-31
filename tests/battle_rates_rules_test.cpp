// Host-side tests for the battle's rates (battle/battle_rates_rules.h): the multiplier words, the game's rounding with
// the floor of 1, the EXP and AP award percents with the game's bonus on and off, and which hit kinds the game's own
// per-hit step sees. Run without the game.

#include "battle/battle_rates_rules.h"

#include <climits>
#include <cstdio>
#include <cwchar>
#include <string>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "battle rates rules: all passed\n" : "battle rates rules: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
}  // namespace lss::test

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++lss::test::g_failures;                                         \
        }                                                                    \
    } while (false)

namespace {

using namespace lss::battle::battle_rates_rules;
namespace settings = lss::platform::settings;

/// The words run 0.1 to 10.0 in tenths, each its own place plus one; the fallback is 1.0.
void Words()
{
    EXPECT(settings::kTenthsWords.size() == 100);
    EXPECT(std::wcscmp(settings::kTenthsWords[0], L"0.1") == 0);
    EXPECT(std::wcscmp(settings::kTenthsWords[9], L"1.0") == 0);
    EXPECT(std::wcscmp(settings::kTenthsWords[14], L"1.5") == 0);
    EXPECT(std::wcscmp(settings::kTenthsWords[98], L"9.9") == 0);
    EXPECT(std::wcscmp(settings::kTenthsWords[99], L"10.0") == 0);
    for (int tenths = 1; tenths <= 100; ++tenths)
    {
        EXPECT(TenthsOf(WordOf(tenths)) == tenths);
    }
    EXPECT(!TenthsOf(L"1"));
    EXPECT(!TenthsOf(L"0.0"));
    EXPECT(!TenthsOf(L"10.1"));
    EXPECT(!TenthsOf(L""));
    EXPECT(std::wcscmp(WordOf(0), L"1.0") == 0);
    EXPECT(std::wcscmp(WordOf(101), L"1.0") == 0);
    for (const settings::Option* option : {&settings::kEnemyDamage, &settings::kEnemyHp, &settings::kBossHp,
                                           &settings::kEnemyStagger, &settings::kExpGain, &settings::kApGain})
    {
        EXPECT(TenthsOf(option->fallback) == kOne);
        EXPECT(std::wstring(option->section) == L"Battle");
        EXPECT(option->wordLabels.size() == option->words.size());
        const auto judged = settings::Judge(*option, L" 2.5 ");
        EXPECT(judged.word == L"2.5" && judged.source == settings::Source::File);
    }
}

/// The game's rounding, at least 1 for a positive value; 1.0 and non-positive values untouched.
void Scaling()
{
    EXPECT(Scaled(9432, kOne) == 9432);
    EXPECT(Scaled(9432, 15) == 14148);
    EXPECT(Scaled(9432, 30) == 28296);
    EXPECT(Scaled(105, 15) == 157);  // 157.5: the game's (int)(2x + 0.5) >> 1 gives 157
    EXPECT(Scaled(105, 16) == 168);
    EXPECT(Scaled(3, 1) == 1);       // 0.3: kept at 1, as the game's step keeps a hit
    EXPECT(Scaled(1, 1) == 1);
    EXPECT(Scaled(0, 20) == 0);      // no stagger gauge stays none
    EXPECT(Scaled(-5, 20) == -5);
    EXPECT(Scaled(9999, 100) == 99990);
    EXPECT(Scaled(INT_MAX / 2, 100) == INT_MAX / 2);  // capped, never overflowing
    EXPECT(ScaledRate(1.4f, 20) > 2.79f && ScaledRate(1.4f, 20) < 2.81f);
    EXPECT(ScaledRate(0.4f, kOne) == 0.4f);
}

/// The award rows: the game's own percent while its bonus applies, 100 otherwise, times the rate.
void AwardPercents()
{
    EXPECT(AwardPercent(200, true, kOne) == 200);
    EXPECT(AwardPercent(300, true, kOne) == 300);
    EXPECT(AwardPercent(200, false, kOne) == 100);
    EXPECT(AwardPercent(200, false, 13) == 130);
    EXPECT(AwardPercent(200, true, 13) == 260);
    EXPECT(AwardPercent(300, true, 5) == 150);
    EXPECT(AwardPercent(200, false, 1) == 10);
    EXPECT(AwardPercent(200, true, 100) == 2000);
}

/// The hit kinds the game's own step sees: HP and MP damage (0, 1), 10 and 11; never 2-9, 12 and up, nor power 11.
void HitKinds()
{
    EXPECT(HitKindScales(0, 0));
    EXPECT(HitKindScales(1, 0));
    EXPECT(HitKindScales(0, 1));
    EXPECT(HitKindScales(0, 5));
    EXPECT(HitKindScales(10, 0));
    EXPECT(HitKindScales(11, 0));
    for (std::uint8_t type = 2; type <= 9; ++type)
    {
        EXPECT(!HitKindScales(type, 0));
    }
    EXPECT(!HitKindScales(12, 0));
    EXPECT(!HitKindScales(13, 0));
    EXPECT(!HitKindScales(0, 11));
}

}  // namespace

int main()
{
    Words();
    Scaling();
    AwardPercents();
    HitKinds();
    return lss::test::Report();
}
