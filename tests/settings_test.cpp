// Host-side tests for the settings table (platform/settings.h): the table's own consistency, the judging of a
// file's text (trim, case, spaces, the default, an invalid word), the word list and the texts the owners and the
// warning use. Run without the game (Load, Of, Why and Path are the game side, not built here).

#include "platform/settings.h"

#include <cstdio>
#include <cwchar>
#include <set>
#include <string>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "settings: all passed\n" : "settings: %d failure(s)\n", g_failures);
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

using namespace lss::platform::settings;

/// Every option: at least two words, a fallback among them, a section, a key and help; the keys unique (they are
/// the console's command names).
void TableConsistent()
{
    std::set<std::wstring> keys;
    for (const Option* option : kOptions)
    {
        EXPECT(option->words.size() >= 2);
        bool fallbackAllowed = false;
        for (const wchar_t* word : option->words)
        {
            fallbackAllowed = fallbackAllowed || std::wcscmp(word, option->fallback) == 0;
            EXPECT(Judge(*option, word).word == word);  // every word is its own lower-case, space-free form
        }
        EXPECT(fallbackAllowed);
        EXPECT(std::wcslen(option->section) > 0);
        EXPECT(std::wcslen(option->key) > 0);
        EXPECT(std::wcslen(option->help) > 0);
        EXPECT(keys.insert(option->key).second);
    }
    EXPECT(kOptions.size() == 4);
}

void JudgeFile()
{
    // A word of the option's, as written and with the case, the spaces and the ends the user may add.
    EXPECT(Judge(kMultiMonitor, L"on").source == Source::File);
    EXPECT(Judge(kMultiMonitor, L"on").word == L"on");
    EXPECT(Judge(kMultiMonitor, L"  On \t").word == L"on");
    EXPECT(Judge(kMultiMonitor, L"  On \t").source == Source::File);
    EXPECT(Judge(kColumnOrder, L"1 3 2").word == L"132");
    EXPECT(Judge(kColumnOrder, L"1 3 2").source == Source::File);
    EXPECT(Judge(kColumnOrder, L"132").word == L"132");
    EXPECT(Judge(kColumnOrder, L"1, 3, 2").word == L"132");  // the form ParseOrder took before the table
    EXPECT(Judge(kBattleWarps, L"Players-Stay").word == L"players-stay");
    // No key, or an empty value: the default.
    EXPECT(Judge(kMenuSlow, L"").word == L"on");
    EXPECT(Judge(kMenuSlow, L"").source == Source::Default);
    EXPECT(Judge(kMenuSlow, L"   ").source == Source::Default);
    // A word that is none of the option's: the default, the file's text kept for the warning.
    const Reading bad = Judge(kBattleWarps, L" sometimes ");
    EXPECT(bad.source == Source::Invalid);
    EXPECT(bad.word == L"players-stay");
    EXPECT(bad.raw == L"sometimes");
    EXPECT(Judge(kColumnOrder, L"2 1 3").source == Source::Invalid);
    EXPECT(Judge(kColumnOrder, L"2 1 3").word == L"123");
}

void Texts()
{
    EXPECT(WordList(kMultiMonitor) == L"auto, on or off");
    EXPECT(WordList(kMenuSlow) == L"on or off");
    EXPECT(WordList(kBattleWarps) == L"players-stay, off or game");
    EXPECT(WhyText(kMenuSlow, Judge(kMenuSlow, L"off")) == L"the settings file");
    EXPECT(WhyText(kMenuSlow, Judge(kMenuSlow, L"")) == L"the default; LocalSplitScreen.ini has no [Battle] menu-slow");
    EXPECT(WhyText(kMenuSlow, Judge(kMenuSlow, L"maybe")) ==
           L"the default; LocalSplitScreen.ini [Battle] menu-slow = 'maybe' is not on or off");
    EXPECT(InvalidText(kMultiMonitor, Judge(kMultiMonitor, L"yes")) ==
           L"LocalSplitScreen.ini [Views] multi-monitor = 'yes' is not auto, on or off; read as auto");
}

}  // namespace

int main()
{
    TableConsistent();
    JudgeFile();
    Texts();
    return lss::test::Report();
}
