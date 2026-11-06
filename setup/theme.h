#pragma once

// The setup app's colours: the look of the game's menus, dark blue panels with thin light edges and cyan for what is chosen.
// Everything is drawn from these numbers; no picture, font or logo of the game is in the app.

#include <cstdint>

namespace lss::setup::theme {

/// A colour as GDI takes it: 0x00BBGGRR.
using Colour = std::uint32_t;

constexpr Colour Rgb(std::uint32_t red, std::uint32_t green, std::uint32_t blue) noexcept
{
    return red | (green << 8) | (blue << 16);
}

/// A fill that runs from one colour at the top to another at the bottom.
struct Fade
{
    Colour top;
    Colour bottom;
};

inline constexpr Fade kWindow{Rgb(14, 24, 44), Rgb(5, 9, 18)};
inline constexpr Fade kPanel{Rgb(26, 44, 76), Rgb(14, 25, 48)};
inline constexpr Fade kPanelDown{Rgb(12, 22, 42), Rgb(18, 32, 58)};  // a plain button held down
inline constexpr Fade kWell{Rgb(6, 10, 20), Rgb(9, 15, 28)};         // the hollow the desktop's picture sits in
inline constexpr Fade kTile{Rgb(36, 44, 60), Rgb(24, 30, 42)};       // a monitor outside the row
inline constexpr Fade kTileLive{Rgb(28, 86, 140), Rgb(14, 46, 88)};  // a monitor of the row
inline constexpr Fade kPrimary{Rgb(112, 228, 246), Rgb(58, 176, 204)};
inline constexpr Fade kPrimaryHot{Rgb(156, 240, 252), Rgb(88, 202, 226)};
inline constexpr Fade kPrimaryDown{Rgb(58, 176, 204), Rgb(96, 214, 236)};
inline constexpr Fade kPrimaryOff{Rgb(34, 44, 62), Rgb(28, 36, 52)};

inline constexpr Colour kEdge         = Rgb(58, 74, 104);
inline constexpr Colour kEdgeHot      = Rgb(132, 152, 188);
inline constexpr Colour kAccent       = Rgb(98, 222, 242);
inline constexpr Colour kAccentDim    = Rgb(52, 130, 154);
inline constexpr Colour kText         = Rgb(236, 242, 250);
inline constexpr Colour kTextDim      = Rgb(140, 156, 182);
inline constexpr Colour kTextOff      = Rgb(92, 104, 126);
inline constexpr Colour kTextOnAccent = Rgb(6, 22, 34);
inline constexpr Colour kGood         = Rgb(104, 222, 160);
inline constexpr Colour kWarning      = Rgb(244, 186, 84);
inline constexpr Colour kProblem      = Rgb(240, 104, 104);

}  // namespace lss::setup::theme
