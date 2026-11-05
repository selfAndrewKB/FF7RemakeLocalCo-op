// Host-side tests for the settings table (platform/settings.h): the table's own consistency, the judging of a
// file's text (trim, case, spaces, the default, an invalid word), the word list and the texts the owners and the
// warning use, and the file the mod writes from the table (platform/settings_write.h), as text and on a temporary
// file read back as Load reads it. Run without the game (Load, Of, Why, Save and Path are the game side, not built
// here).

#include "platform/settings.h"
#include "platform/settings_write.h"

#include <array>
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
        EXPECT(std::wcslen(option->label) > 0);     // the file names each option by its Co-op page row
        EXPECT(std::wcslen(option->menuHelp) > 0);  // and describes it with the page's help
        EXPECT(keys.insert(option->key).second);
        // An experimental option's default leaves the game as it has played (its word is the first: guide, or a switch on).
        EXPECT(!option->experimental || std::wcscmp(option->fallback, option->words[0]) == 0);
    }
    // Each section's options are together in the table, so the file lists each section once.
    std::set<std::wstring> closed;
    std::wstring           open;
    for (const Option* option : kOptions)
    {
        if (open != option->section)
        {
            EXPECT(closed.insert(option->section).second);
            open = option->section;
        }
    }
    EXPECT(kOptions.size() == 15);
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

/// Every option at its default, as Load reads a missing file.
std::array<Reading, kOptions.size()> Defaults()
{
    std::array<Reading, kOptions.size()> readings;
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        readings[i] = Judge(*kOptions[i], L"");
    }
    return readings;
}

std::size_t Count(const std::wstring& text, const std::wstring& part)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::wstring::npos; at = text.find(part, at + 1))
    {
        ++count;
    }
    return count;
}

/// An option's place in the table, which the readings follow.
std::size_t PlaceOf(const Option& option)
{
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        if (kOptions[i] == &option)
        {
            return i;
        }
    }
    return kOptions.size();
}

/// The file's text from the table: every option once with its value line, each section once, plain ASCII, no line
/// past the width, a long run of values as a range, a word the mod cannot read kept as written with a note.
void FileTextFromTable()
{
    auto readings = Defaults();
    readings[PlaceOf(kColumnOrder)]   = Judge(kColumnOrder, L"2 1 3");
    readings[PlaceOf(kBattleWarps)]   = Judge(kBattleWarps, L"sometimes");
    const std::wstring text = FileText(readings);
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        const Option& option = *kOptions[i];
        EXPECT(Count(text, std::wstring(L"\r\n") + option.key + L" = ") == 1);
        EXPECT(Count(text, std::wstring(L"; ") + option.label + L": ") == 1);
        EXPECT(Count(text, std::wstring(L"[") + option.section + L"]\r\n") == 1);
    }
    EXPECT(text.find(L"\r\nmulti-monitor = auto\r\n") != std::wstring::npos);
    EXPECT(text.find(L"\r\ncolumn-order = 213\r\n") != std::wstring::npos);
    EXPECT(text.find(L"\r\nbattle-warps = sometimes\r\n") != std::wstring::npos);
    EXPECT(Count(text, L"none of these words: read as players-stay") == 1);
    EXPECT(text.find(L"; 0.1 to 10.0 (the default: 1.0)") != std::wstring::npos);
    EXPECT(text.find(L"; auto | on | off (the default: auto)") != std::wstring::npos);
    std::size_t lineStart = 0;
    bool        ascii     = true, narrow = true;
    for (std::size_t at = 0; at < text.size(); ++at)
    {
        ascii = ascii && text[at] < 0x80;
        if (text[at] == L'\n')
        {
            narrow    = narrow && at - lineStart <= kFileWidth + 1;  // the CR before the LF
            lineStart = at + 1;
        }
    }
    EXPECT(ascii);
    EXPECT(narrow);
    EXPECT(text.size() >= 2 && text.substr(text.size() - 2) == L"\r\n");
}

/// The value the file now holds for an option, read as Load reads it.
Reading ReadBack(const std::wstring& path, const Option& option)
{
    wchar_t    buffer[256]{};
    const auto length = GetPrivateProfileStringW(option.section, option.key, L"", buffer, 256, path.c_str());
    return Judge(option, std::wstring_view(buffer, length));
}

/// The written file on disk: read back as the next start reads it, every value as it was written; written again over
/// an old file whose comments and stale keys are gone.
void FileWrittenAndReadBack()
{
    wchar_t folder[MAX_PATH]{};
    EXPECT(GetTempPathW(MAX_PATH, folder) > 0);
    const std::wstring path = std::wstring(folder) + L"lss_settings_test.ini";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "; an old copy\r\n[Views]\r\nmulti-monitor = auto\r\nold-option = yes\r\n";
    }
    auto readings = Defaults();
    readings[PlaceOf(kMultiMonitor)] = Judge(kMultiMonitor, L"on");
    readings[PlaceOf(kColumnOrder)]  = Judge(kColumnOrder, L"213");
    readings[PlaceOf(kEnemyDamage)]  = Judge(kEnemyDamage, L"1.5");
    readings[PlaceOf(kBattleWarps)]   = Judge(kBattleWarps, L"sometimes");
    const std::string bytes = Utf8(FileText(readings));
    EXPECT(WriteFileBytes(path.c_str(), bytes) == 0);
    EXPECT(FileBytes(path.c_str()) == bytes);
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        const Reading back = ReadBack(path, *kOptions[i]);
        EXPECT(back.word == readings[i].word && back.source == (readings[i].source == Source::Default ? Source::File
                                                                                                       : readings[i].source));
    }
    EXPECT(ReadBack(path, kBattleWarps).raw == L"sometimes");
    // A value with characters outside ASCII: kept with `?`, and a second start writes the same bytes (no drift).
    const std::wstring typed = std::wstring(L"1,5") + wchar_t{0x00D7} + L' ' + wchar_t{0x2713};  // "1,5x" and a tick
    readings[PlaceOf(kEnemyHp)] = Judge(kEnemyHp, typed);
    EXPECT(readings[PlaceOf(kEnemyHp)].source == Source::Invalid);
    const std::string first = Utf8(FileText(readings));
    EXPECT(WriteFileBytes(path.c_str(), first) == 0);
    auto again  = readings;
    again[PlaceOf(kEnemyHp)] = ReadBack(path, kEnemyHp);
    EXPECT(again[PlaceOf(kEnemyHp)].source == Source::Invalid && again[PlaceOf(kEnemyHp)].raw == L"1,5? ?");    EXPECT(Utf8(FileText(again)) == first);
    const std::string text = FileBytes(path.c_str());
    EXPECT(text.find("an old copy") == std::string::npos);
    EXPECT(text.find("old-option") == std::string::npos);
    std::wstring temp = path + L".tmp";
    EXPECT(GetFileAttributesW(temp.c_str()) == INVALID_FILE_ATTRIBUTES);  // the temporary file moved, none left
    DeleteFileW(path.c_str());
}

}  // namespace

int main()
{
    TableConsistent();
    JudgeFile();
    Texts();
    FileTextFromTable();
    FileWrittenAndReadBack();
    return lss::test::Report();
}
