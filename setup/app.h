#pragma once

// The setup app's state and what changes it (setup/app.cpp): the one object the window shows, and the calls the window
// procedure makes on it. Everything runs on the window's thread.

#include "platform/settings.h"
#include "setup/setup_rules.h"
#include "setup/view.h"
#include "views/display_move.h"
#include "views/monitor_list.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lss::setup::app {

namespace settings = platform::settings;
namespace row      = views::monitor_row;
namespace picked   = views::monitor_picks;
namespace listing  = views::monitor_list;
namespace displays = views::display_move;

struct App
{
    HWND         window = nullptr;
    view::Fonts  fonts;
    UINT         dpi = 96;
    std::wstring path;
    bool         inModFolder = true;
    std::array<settings::Reading, settings::kOptions.size()> readings;
    std::vector<listing::Listed> monitors;
    std::vector<std::size_t>     picks;   // the monitors of the row, in the order clicked: left to right
    bool                         picksChanged = false;  // the player clicked a monitor: the file's own names stay otherwise
    std::int32_t                 namedNotHere = 0;      // monitors the file names that are not connected now
    std::size_t                  primary = 0;
    std::wstring                 fileWord;  // the picks as the file has them: monitors not connected now stay named until a click
    bool                         gameRunning = false;
    std::wstring                 note;      // what the last action did, until the choice changes
    rules::Tone                  noteTone = rules::Tone::Good;
    // Worked out by Refresh from the above, for the paint, the mouse and the keyboard.
    rules::Look   look;
    rules::Status status;
    std::wstring  pictureHelp;  // the hover text over the desktop's picture
    bool          canSave    = false;
    bool          canRestore = false;
    rules::Hit    hover, pressed, focus;
    bool          showFocus = false;  // the keyboard was used last: the chosen part is marked
    bool          tracking  = false;  // the mouse's leaving is asked for
};

extern App g_app;

/// The parts with a longer explanation on hover: as many as PlaceTips places.
inline constexpr std::size_t kTipCount = 5;

/// `value`, written for a 96 dpi screen, at the window's dpi.
int Px(int value);

/// The option's place in the settings table and in `App::readings`.
std::size_t IndexOf(const settings::Option& option);

bool GameIsRunning();

/// Reads the connected monitors and finds the picks among them: the player's clicks, else the file's word.
void ReadMonitors();

/// The record of moved monitors, beside the settings file.
std::wstring RecordPath();

rules::Frame FrameNow();

/// What the status line says about the last action, until the choice changes.
void Say(std::wstring note, rules::Tone tone);

/// The part at a point of the client area; a button that cannot be pressed is no part.
rules::Hit HitAt(int x, int y);

/// Works out what the window shows from what changed, and asks for a repaint.
void Refresh();

/// Each part's hover text at the part's place.
void PlaceTips();

view::Scene SceneNow();

/// A press on a part, from the mouse or the keyboard.
void Press(const rules::Hit& hit);

/// `held` is true for the repeats of a key kept down: they move from part to part, but never press or step an option, so a
/// held Enter saves once.
void OnKey(std::uintptr_t key, bool held);

}  // namespace lss::setup::app
