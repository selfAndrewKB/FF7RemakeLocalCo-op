// Host-side tests for the setup app's rules, run without the game. The desktop: the user's row of 2560 | 3440 | 2560 with a
// fourth monitor above it, and the same with a monitor scaled by Windows.

#include "platform/settings.h"
#include "setup/setup_rules.h"

#include <cstdio>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace lss::setup::rules;
using lss::views::monitor_row::Monitor;
using lss::views::monitor_row::Rect;

int g_failures = 0;

#define EXPECT(condition)                                                \
    do                                                                   \
    {                                                                    \
        if (!(condition))                                                \
        {                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition); \
            ++g_failures;                                                \
        }                                                                \
    } while (false)

Monitor At(std::int32_t left, std::int32_t top, std::int32_t width = 2560, std::int32_t height = 1440, std::uint32_t dpi = 96)
{
    return {{left, top, left + width, top + height}, dpi};
}

// Primary first, as Windows lists it.
const std::vector<Monitor> kDesktop{At(0, 0, 3440), At(3440, 0), At(-2560, 0), At(2585, -1440)};

Look LookOf(std::span<const Monitor> monitors, std::vector<std::size_t> picks, std::size_t game = 0)
{
    return LookAt(monitors, picks, game);
}

void NonePicked()
{
    const Look look = LookOf(kDesktop, {});
    EXPECT(look.ok && !look.picked);
    EXPECT((look.screens == std::vector<std::size_t>{2, 0, 1}));  // left to right, the monitor above left alone
}

void PickedPair()
{
    Look look = LookOf(kDesktop, {0, 1});
    EXPECT(look.ok && look.picked);
    EXPECT((look.screens == std::vector<std::size_t>{0, 1}));
    EXPECT(look.tone == Tone::Good && look.text.empty() && look.detail.empty());  // already a row: the picture shows it, nothing to add

    // Any monitors, in the order clicked: a gap between them, one above the other, the order reversed.
    look = LookOf(kDesktop, {1, 2});
    EXPECT(look.ok && (look.screens == std::vector<std::size_t>{1, 2}));
    EXPECT(look.detail.find(L"go back afterwards") != std::wstring::npos);  // said on hover, the line itself stays short
    EXPECT(look.text.find(L"go back") == std::wstring::npos);
    EXPECT(LookOf(kDesktop, {0, 3}).ok);
    look = LookOf(kDesktop, {1, 0, 2});
    EXPECT(look.ok && (look.screens == std::vector<std::size_t>{1, 0, 2}));
}

void PickedBadly()
{
    EXPECT(!LookOf(kDesktop, {0}).ok);            // one monitor
    EXPECT(!LookOf(kDesktop, {0, 1, 2, 3}).ok);   // more than a row takes
}

/// A pick as saved must read back through the settings file's own reader, which lower-cases and drops spaces and commas, and
/// still find its monitor when its path has changed: a virtual monitor comes back with a new one, under its old name.
void PickSurvivesTheSettingsFile()
{
    namespace row = lss::views::monitor_row;
    namespace settings = lss::platform::settings;
    const std::wstring before = L"\\\\?\\display#smkd1ce#1&15ecd195&1&uid256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
    const std::wstring after  = L"\\\\?\\display#smkd1ce#1&15ecd195&1&uid257#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
    const std::wstring dell   = L"\\\\?\\display#dela199#5&21434701&1&uid4355#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
    const std::vector<std::wstring> words{row::PickWord(dell, row::IdentityKey(dell, L"DELL S2721D")),
                                          row::PickWord(before, row::IdentityKey(before, L"Steam Deck, Living Room   "))};
    const std::wstring saved = row::NamesWord(words);
    const std::wstring read  = settings::Judge(settings::kMonitors, saved).word;
    EXPECT(read == saved);

    const std::vector<std::wstring> ids{after, dell};
    const std::vector<std::wstring> keys{row::IdentityKey(after, L"Steam Deck, Living Room   "), row::IdentityKey(dell, L"DELL S2721D")};
    const row::Picks picks = row::FindPicks(ids, keys, read);
    EXPECT(picks.named == 2 && (picks.found == std::vector<std::size_t>{1, 0}));
}

/// Any monitors make a row when picked: other heights, Windows scaling. Only a view too wide for a clean picture is warned about.
void AnyMonitors()
{
    const std::vector<Monitor> mixed{At(0, 0, 3440), At(3440, 0, 1280, 720), At(-2560, 0, 2560, 1440, 144), At(4720, 0, 5120, 1440)};
    Look look = LookOf(mixed, {0, 1});
    EXPECT(look.ok && (look.screens == std::vector<std::size_t>{0, 1}));
    EXPECT(look.text.find(L"wrong colours") == std::wstring::npos);
    EXPECT(LookOf(mixed, {2, 0}).ok);
    look = LookOf(mixed, {0, 3});
    EXPECT(look.ok && look.text.find(L"A view over") == 0 && look.text.find(L"wrong colours") != std::wstring::npos && look.tone == Tone::Warning);
}

/// One line, the most pressing thing first: what stops a save, what the last action did, then the choice itself.
void OneStatusLine()
{
    const Look row  = LookOf(kDesktop, {1, 2});
    const Look none = LookOf(kDesktop, {0});
    const Look wide = LookOf(std::vector<Monitor>{At(0, 0, 3440), At(3440, 0, 5120, 1440)}, {0, 1});
    // A row that works says nothing: no words, and so no hover text over an empty line. What moving the monitors means is
    // said over the picture instead, and only for a row the game will use.
    EXPECT(StatusLine(row, {}).text.empty() && StatusLine(row, {}).detail.empty());
    EXPECT(PictureHelp(row, {}) == kMovesNote);
    EXPECT(StatusLine(wide, {}).text == wide.text && StatusLine(wide, {}).tone == Tone::Warning);
    // A row with a warning whose monitors must also move: the line warns, and both hover texts say what moving means.
    const Look wideMoved = LookOf(std::vector<Monitor>{At(0, 0, 3440), At(3440, 0), At(-5120, 0, 5120, 1440)}, {1, 2});
    EXPECT(wideMoved.ok && StatusLine(wideMoved, {}).text == wideMoved.text && StatusLine(wideMoved, {}).tone == Tone::Warning);
    EXPECT(StatusLine(wideMoved, {}).detail == kMovesNote && PictureHelp(wideMoved, {}) == kMovesNote);
    EXPECT(StatusLine(none, {}).text == none.text && StatusLine(none, {}).detail == none.detail && StatusLine(none, {}).tone == Tone::Plain);
    EXPECT(PictureHelp(none, {}).empty() && PictureHelp(LookOf(kDesktop, {}), {}).empty());  // the status line speaks for these
    EXPECT(PictureHelp(LookOf(kDesktop, {0, 1}), {}).empty());                               // already a row: nothing moves

    Notes notes;
    notes.note = L"Saved.";
    EXPECT(StatusLine(row, notes).text == L"Saved." && StatusLine(row, notes).tone == Tone::Good);
    notes.notConnected = 2;  // the last action still comes first
    EXPECT(StatusLine(row, notes).text == L"Saved.");
    notes.note = {};
    EXPECT(StatusLine(row, notes).text.find(L"2 saved monitors are not connected") == 0 && StatusLine(row, notes).tone == Tone::Warning);
    notes.notConnected = 1;
    EXPECT(StatusLine(row, notes).text.find(L"1 saved monitor is not connected") == 0);
    notes.notConnected = 0;
    notes.spanOn       = false;
    EXPECT(StatusLine(row, notes).text.find(L"Multiple Monitors is off") == 0 && StatusLine(row, notes).tone == Tone::Plain);
    EXPECT(StatusLine(row, notes).detail.empty() && PictureHelp(row, notes).empty());  // nothing moves for a row left unused

    // Clicked monitors that make no row are said before the option being off: they are what stops the save.
    notes.picksChanged = true;
    EXPECT(StatusLine(none, notes).text == none.text);
    notes.picksChanged = false;

    notes.note        = L"Saved.";
    notes.inModFolder = false;  // a file the game will not read is said with what was just done to it, never instead of it
    EXPECT(StatusLine(row, notes).text.find(L"Saved. Not in the mod's folder") == 0 && StatusLine(row, notes).tone == Tone::Problem);
    notes.note = {};
    EXPECT(StatusLine(row, notes).text.find(L"Not in the mod's folder") == 0);
    notes.note     = L"Couldn't save (Windows error 5).";
    notes.noteTone = Tone::Problem;  // a failure is said alone
    EXPECT(StatusLine(row, notes).text == notes.note);
    notes.gameRunning = true;
    EXPECT(StatusLine(row, notes).text.find(L"Close the game") == 0);

    // Every line is short enough for one line of the window.
    for (const Look& look : {wide, none, LookOf(kDesktop, {}), LookOf(kDesktop, {0, 1, 2, 3})})
    {
        EXPECT(!look.text.empty() && look.text.size() <= 70);
    }
}

/// Save writes what it can stand behind: the file's own monitors untouched, a row, or no monitors at all.
void WhatCanBeSaved()
{
    const Look row  = LookOf(kDesktop, {1, 2});
    const Look one  = LookOf(kDesktop, {0});
    const Look many = LookOf(kDesktop, {0, 1, 2, 3});
    const Look auto_ = LookOf(kDesktop, {});
    Notes      notes;
    // The file names two monitors and one is connected: nothing was clicked, so another option can still be changed and saved.
    EXPECT(CanSave(one, notes));
    EXPECT(CanSave(row, notes) && CanSave(auto_, notes));
    notes.picksChanged = true;
    EXPECT(CanSave(row, notes));     // a row
    EXPECT(CanSave(auto_, notes));   // every monitor taken out
    EXPECT(!CanSave(one, notes));    // one clicked monitor is no row
    EXPECT(!CanSave(many, notes));
    notes.spanOn = false;            // the option off does not make a bad choice a good one
    EXPECT(!CanSave(one, notes));
    notes.picksChanged = false;
    notes.gameRunning  = true;       // never while the game runs
    EXPECT(!CanSave(row, notes) && !CanSave(one, notes));
}

bool Within(const Rect& inner, const Rect& outer)
{
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom &&
           inner.Width() > 0 && inner.Height() > 0;
}

bool Apart(const Rect& a, const Rect& b)
{
    return a.right <= b.left || b.right <= a.left || a.bottom <= b.top || b.bottom <= a.top;
}

/// Every part inside the window and clear of the others, at the smallest size and scaled.
void WindowLayout()
{
    for (const std::uint32_t dpi : {96u, 144u, 192u})
    {
        for (const auto& [width, height] : {std::pair{kMinWidth, kMinHeight}, std::pair{880, 560}, std::pair{1600, 1000}})
        {
            const std::int32_t w = Scaled(width, dpi), h = Scaled(height, dpi);
            const Frame        frame = Arrange(w, h, dpi);
            const Rect         client{0, 0, w, h};
            const Rect         parts[]{frame.multi, frame.order, frame.caption, frame.picture, frame.status, frame.restore, frame.save, frame.close};
            for (std::size_t i = 0; i < std::size(parts); ++i)
            {
                EXPECT(Within(parts[i], client));
                for (std::size_t j = i + 1; j < std::size(parts); ++j)
                {
                    EXPECT(Apart(parts[i], parts[j]));
                }
            }
            EXPECT(frame.picture.Height() >= Scaled(120, dpi));  // room for the desktop at the smallest size
            const OptionParts option = PartsOf(frame.multi, dpi);
            EXPECT(Within(option.previous, frame.multi) && Within(option.value, frame.multi) && Within(option.next, frame.multi));
            EXPECT(option.previous.right == option.value.left && option.value.right == option.next.left);
            EXPECT(option.previous.left - frame.multi.left >= Scaled(120, dpi));  // room for the option's name
        }
    }
}

/// A click lands on the part drawn there; an option answers on its arrows and value only.
void Clicks()
{
    const std::uint32_t dpi   = 96;
    const Frame         frame = Arrange(880, 560, dpi);
    const auto          tiles = Tiles(kDesktop, frame.picture, 10, 4);
    EXPECT(tiles.size() == kDesktop.size());
    for (const Rect& tile : tiles)
    {
        EXPECT(Within(tile, frame.picture));
    }
    const auto at     = [&](const Rect& rect) { return HitTest(frame, tiles, dpi, (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2); };
    const auto option = PartsOf(frame.order, dpi);
    EXPECT((at(option.previous) == Hit{Part::Order, 0, -1}));
    EXPECT((at(option.value) == Hit{Part::Order, 0, 1}));
    EXPECT((at(option.next) == Hit{Part::Order, 0, 1}));
    EXPECT(HitTest(frame, tiles, dpi, frame.order.left + 10, frame.order.top + 10).part == Part::None);  // its name
    EXPECT(at(PartsOf(frame.multi, dpi).next).part == Part::Multi);
    for (std::size_t i = 0; i < tiles.size(); ++i)
    {
        EXPECT((at(tiles[i]) == Hit{Part::Tile, i, 0}));
    }
    EXPECT(at(frame.save).part == Part::Save && at(frame.close).part == Part::Close && at(frame.restore).part == Part::Restore);
    EXPECT(at(frame.status).part == Part::None && at(frame.caption).part == Part::None);
    EXPECT(HitTest(frame, tiles, dpi, -5, -5).part == Part::None);
}

/// An option's values go round in both directions; a word the list lacks starts at the first.
void OptionSteps()
{
    namespace settings = lss::platform::settings;
    const auto words   = settings::kColumnOrder.words;
    EXPECT(std::wstring_view(Stepped(words, L"123", 1)) == L"132");
    EXPECT(std::wstring_view(Stepped(words, L"123", -1)) == L"321");
    EXPECT(std::wstring_view(Stepped(words, L"321", 1)) == L"123");
    EXPECT(std::wstring_view(Stepped(words, L"nonsense", 1)) == L"123");
    EXPECT(std::wstring_view(Stepped(settings::kMultiMonitor.words, L"on", 1)) == L"off");
    EXPECT(std::wstring_view(Stepped(settings::kMultiMonitor.words, L"on", -1)) == L"off");
    EXPECT(std::wstring_view(Stepped({}, L"on", 1)).empty());
}

void PlayersOnScreens()
{
    EXPECT(PlayerOnScreen(L"123", 3, 0) == 1 && PlayerOnScreen(L"123", 3, 2) == 3);
    EXPECT(PlayerOnScreen(L"213", 3, 0) == 2 && PlayerOnScreen(L"213", 3, 1) == 1);
    EXPECT(PlayerOnScreen(L"312", 2, 0) == 1 && PlayerOnScreen(L"312", 2, 1) == 2);  // no third screen: player 3 skipped
    EXPECT(!PlayerOnScreen(L"123", 2, 2));
}

void PictureKeepsTheArrangement()
{
    const auto placed = Fit(kDesktop, 800, 400, 10);
    EXPECT(placed.size() == 4);
    EXPECT(placed[2].right <= placed[0].left + 1 && placed[0].right <= placed[1].left + 1);  // left, middle, right
    EXPECT(placed[3].bottom <= placed[0].top + 1);                                            // above
    for (const Rect& rect : placed)
    {
        EXPECT(rect.left >= 10 && rect.top >= 10 && rect.right <= 790 && rect.bottom <= 390);
    }
    EXPECT(Fit({}, 800, 400, 10).empty());
}

}  // namespace

int main()
{
    NonePicked();
    PickedPair();
    PickedBadly();
    PickSurvivesTheSettingsFile();
    AnyMonitors();
    OneStatusLine();
    WhatCanBeSaved();
    WindowLayout();
    Clicks();
    OptionSteps();
    PlayersOnScreens();
    PictureKeepsTheArrangement();
    std::printf(g_failures == 0 ? "setup rules: all passed\n" : "setup rules: %d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
