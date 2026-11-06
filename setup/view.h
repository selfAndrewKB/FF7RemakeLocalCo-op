#pragma once

// The setup app's picture: one call draws the whole window from a description of what it shows. Nothing here reads the
// desktop or the settings; the window (setup/main.cpp) fills the description in, and the layout comes from setup/setup_rules.h.

#include "setup/setup_rules.h"

#include <Windows.h>

#include <string>
#include <vector>

namespace lss::setup::view {

namespace row = views::monitor_row;

/// The fonts of one dpi. Made by MakeFonts, given back with Free.
struct Fonts
{
    HFONT body    = nullptr;  // names and the status line
    HFONT strong  = nullptr;  // values, monitor names, button labels
    HFONT minor   = nullptr;  // sizes and hints
    HFONT heading = nullptr;  // the small capitals: the picture's caption and a monitor's player
    HFONT arrow   = nullptr;  // the arrows beside an option's value
};

Fonts MakeFonts(UINT dpi);
void  Free(Fonts& fonts) noexcept;

/// A monitor in the desktop's picture.
struct Tile
{
    row::Rect    rect;
    std::wstring name;
    std::int32_t width  = 0;
    std::int32_t height = 0;
    int          pick   = 0;      // its place among the clicked monitors, from 1; 0 when not clicked
    int          player = 0;      // the player it shows in the row, from 1; 0 for none
    bool         inRow   = false;
    bool         problem = false;  // clicked, and the clicked monitors make no row
};

/// Everything the window shows.
struct Scene
{
    rules::Frame      frame;
    std::wstring      multiLabel, multiValue;
    std::wstring      orderLabel, orderValue;
    std::vector<Tile> tiles;
    bool              rowLive = false;  // the game will take this row: a row that works, with Multiple Monitors on
    rules::Status     status;
    bool              canSave    = false;
    bool              canRestore = false;
    rules::Hit        hover;    // under the mouse
    rules::Hit        pressed;  // held down by the mouse
    rules::Hit        focus;    // chosen by the keyboard; drawn only when `showFocus`
    bool              showFocus = false;
};

/// Draws `scene` over the whole client area, without flicker.
void Draw(HDC target, const RECT& client, const Scene& scene, const Fonts& fonts, UINT dpi);

}  // namespace lss::setup::view
