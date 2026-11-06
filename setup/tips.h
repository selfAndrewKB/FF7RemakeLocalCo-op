#pragma once

// The window's hover texts: one tooltip control, a text per part of the window, in the window's own colours and font. A part
// with an empty text or an empty place shows nothing.

#include "views/monitor_row.h"

#include <cstddef>
#include <span>

struct HWND__;
struct HFONT__;

namespace lss::setup::tips {

struct Tip
{
    views::monitor_row::Rect at;    // in the window's client pixels
    const wchar_t*           text;  // copied by the control
};

/// Makes the control for `count` parts of `window`. Without it the other calls do nothing.
void Make(HWND__* window, std::size_t count, HFONT__* font, unsigned dpi);

/// The control exists.
bool Made() noexcept;

/// The texts' font and sizes at the window's dpi. The font must outlive the control or be replaced here first.
void Style(HFONT__* font, unsigned dpi);

/// Every part's place and words, in the order they were counted.
void Place(std::span<const Tip> parts);

/// Before the font it holds is freed.
void Destroy();

}  // namespace lss::setup::tips
