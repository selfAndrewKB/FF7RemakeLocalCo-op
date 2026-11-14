// Host-side tests for the lead marker's size word (hud/lead_mark_rules.h). Run without the game.

#include "hud/lead_mark_rules.h"
#include "platform/settings.h"

#include <cstdio>
#include <string_view>

namespace {

using namespace lss::hud::lead_mark_rules;

int g_failures = 0;

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

/// Each word the setting offers is its own factor.
void TheSettingsWords()
{
    EXPECT(ScaleOf(L"1.0") == 1.0f);
    EXPECT(ScaleOf(L"1.5") == 1.5f);
    EXPECT(ScaleOf(L"2.0") == 2.0f);
    EXPECT(ScaleOf(L"2.5") == 2.5f);
    EXPECT(ScaleOf(L"3.0") == 3.0f);
    for (const wchar_t* word : lss::platform::settings::kLeadMarkerSizeWords)
    {
        EXPECT(ScaleOf(word) >= kSmallest && ScaleOf(word) <= kLargest);
    }
    EXPECT(ScaleOf(lss::platform::settings::kLeadMarkerSize.fallback) == 2.0f);
}

/// Anything else is the game's own size.
void OtherWords()
{
    EXPECT(ScaleOf(L"") == 1.0f);
    EXPECT(ScaleOf(L"2") == 1.0f);
    EXPECT(ScaleOf(L"2.0x") == 1.0f);
    EXPECT(ScaleOf(L"2,0") == 1.0f);
    EXPECT(ScaleOf(L"a.b") == 1.0f);
    EXPECT(ScaleOf(L"0.5") == 1.0f);  // smaller than the game's
    EXPECT(ScaleOf(L"3.5") == 1.0f);  // larger than the largest
}

}  // namespace

int main()
{
    TheSettingsWords();
    OtherWords();
    if (g_failures != 0)
    {
        std::printf("lead_mark_rules: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("lead_mark_rules: all passed\n");
    return 0;
}
