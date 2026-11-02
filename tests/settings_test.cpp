// Host-side tests for the settings table (platform/settings.h): the table's own consistency, the judging of a
// file's text (trim, case, spaces, the default, an invalid word), the word list and the texts the owners and the
// warning use, and the file write the Co-op page saves with (platform/settings_write.h) on a temporary file. Run
// without the game (Load, Of, Why, Save and Path are the game side, not built here).

#include "platform/settings.h"
#include "platform/settings_write.h"

#include <cstdio>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>

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
        // An experimental option's default leaves the game as it plays without it (its word is the first: off or guide).
        EXPECT(!option->experimental || std::wcscmp(option->fallback, option->words[0]) == 0);
    }
    EXPECT(kOptions.size() == 11);
    EXPECT(kLeadHolder.experimental && std::wcscmp(kLeadHolder.fallback, L"guide") == 0);
    EXPECT(std::wcscmp(kLeadHolder.section, L"Experimental") == 0);
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
    EXPECT(Judge(kMenuSlow, L"").word == L"off");
    EXPECT(Judge(kMenuSlow, L"").source == Source::Default);
    EXPECT(Judge(kMenuSlow, L"   ").source == Source::Default);
    // A word that is none of the option's: the default, the file's text kept for the warning.
    const Reading bad = Judge(kBattleWarps, L" sometimes ");
    EXPECT(bad.source == Source::Invalid);
    EXPECT(bad.word == L"players-stay");
    EXPECT(bad.raw == L"sometimes");
    EXPECT(Judge(kColumnOrder, L"2 1 3").word == L"213");  // player 1 may take any screen (2026-10-31)
    EXPECT(Judge(kColumnOrder, L"1 2 4").source == Source::Invalid);
    EXPECT(Judge(kColumnOrder, L"1 2 4").word == L"123");
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

/// The value the file now holds for an option, read as Load reads it.
std::wstring ReadBack(const std::wstring& path, const Option& option)
{
    wchar_t    buffer[256]{};
    const auto length = GetPrivateProfileStringW(option.section, option.key, L"", buffer, 256, path.c_str());
    return Judge(option, std::wstring_view(buffer, length)).word;
}

/// The Co-op page's save on a real file: the key's line replaced, a missing key and section added, the comments and
/// every other line kept, the words read back as the next start reads them.
void FileWrite()
{
    wchar_t folder[MAX_PATH]{};
    EXPECT(GetTempPathW(MAX_PATH, folder) > 0);
    const std::wstring path = std::wstring(folder) + L"lss_settings_test.ini";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "; the mod's settings\r\n\r\n[Views]\r\n; multi-monitor = off | on | auto\r\nmulti-monitor = auto\r\n\r\n"
                "; column-order = 1 2 3 | 1 3 2\r\ncolumn-order = 1 3 2\r\n";
    }
    EXPECT(WriteWord(path.c_str(), kMultiMonitor, L"on") == 0);
    EXPECT(WriteWord(path.c_str(), kEnemyDamage, L"1.5") == 0);  // no [Battle] section yet
    EXPECT(WriteWord(path.c_str(), kExpGain, L"2.0") == 0);      // a second key in the new section
    EXPECT(ReadBack(path, kMultiMonitor) == L"on");
    EXPECT(ReadBack(path, kColumnOrder) == L"132");  // untouched
    EXPECT(ReadBack(path, kEnemyDamage) == L"1.5");
    EXPECT(ReadBack(path, kExpGain) == L"2.0");
    std::ifstream     file(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    EXPECT(text.find("; the mod's settings") == 0);
    EXPECT(text.find("; multi-monitor = off | on | auto") != std::string::npos);
    EXPECT(text.find("; column-order = 1 2 3 | 1 3 2") != std::string::npos);
    EXPECT(text.find("multi-monitor = auto") == std::string::npos);  // the old line replaced, not kept beside the new
    EXPECT(text.find("[Battle]") != std::string::npos);
    file.close();
    DeleteFileW(path.c_str());
}

}  // namespace

int main()
{
    TableConsistent();
    JudgeFile();
    Texts();
    FileWrite();
    return lss::test::Report();
}
