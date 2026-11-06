#include "setup/view.h"

#include "setup/theme.h"

#include <algorithm>
#include <format>
#include <string_view>

namespace lss::setup::view {

namespace {

using theme::Colour;
using theme::Fade;

// The characters beyond ASCII, by number: a compiler reads this file in the system's code page.
constexpr wchar_t kArrowBefore[]{0x2039, 0};         // a single angle pointing left
constexpr wchar_t kArrowAfter[]{0x203A, 0};          // and right
constexpr wchar_t kTimes[]{L' ', 0x00D7, L' ', 0};   // the multiplication sign, between a width and a height

RECT ToRect(const row::Rect& rect) noexcept
{
    return {rect.left, rect.top, rect.right, rect.bottom};
}

void Fill(HDC dc, const RECT& rect, Colour colour) noexcept
{
    SetDCBrushColor(dc, colour);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Fill(HDC dc, const RECT& rect, const Fade& fade) noexcept
{
    const auto corner = [](LONG x, LONG y, Colour colour) {
        return TRIVERTEX{x, y, static_cast<COLOR16>((colour & 0xFF) << 8), static_cast<COLOR16>(((colour >> 8) & 0xFF) << 8),
                         static_cast<COLOR16>(((colour >> 16) & 0xFF) << 8), 0};
    };
    TRIVERTEX     corners[2]{corner(rect.left, rect.top, fade.top), corner(rect.right, rect.bottom, fade.bottom)};
    GRADIENT_RECT whole{0, 1};
    GradientFill(dc, corners, 2, &whole, 1, GRADIENT_FILL_RECT_V);
}

/// A border of `thickness` pixels just inside `rect`.
void Edge(HDC dc, RECT rect, Colour colour, int thickness) noexcept
{
    SetDCBrushColor(dc, colour);
    for (int i = 0; i < thickness; ++i)
    {
        FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        InflateRect(&rect, -1, -1);
    }
}

/// A short angle at each corner of `rect`: the mark of a monitor that is chosen.
void Brackets(HDC dc, const RECT& rect, Colour colour, int length, int thickness) noexcept
{
    length = std::min({length, static_cast<int>(rect.right - rect.left) / 2, static_cast<int>(rect.bottom - rect.top) / 2});
    for (const bool right : {false, true})
    {
        for (const bool bottom : {false, true})
        {
            const LONG x = right ? rect.right - length : rect.left;
            const LONG y = bottom ? rect.bottom - thickness : rect.top;
            Fill(dc, RECT{x, y, x + length, y + thickness}, colour);
            const LONG sideX = right ? rect.right - thickness : rect.left;
            const LONG sideY = bottom ? rect.bottom - length : rect.top;
            Fill(dc, RECT{sideX, sideY, sideX + thickness, sideY + length}, colour);
        }
    }
}

/// One line of text in `rect`. `spacing` widens the gaps between letters, for the small capitals.
void Line(HDC dc, HFONT font, Colour colour, RECT rect, std::wstring_view text, UINT format, int spacing = 0) noexcept
{
    SelectObject(dc, font);
    SetTextColor(dc, colour);
    if (spacing != 0 && (format & DT_CENTER) != 0)
    {
        // The text is centred by its width without the wider gaps, then drawn with them: moved back by half of what they add.
        OffsetRect(&rect, -spacing * static_cast<int>(text.size()) / 2, 0);
    }
    SetTextCharacterExtra(dc, spacing);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect, format | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SetTextCharacterExtra(dc, 0);
}

int LineHeight(HDC dc, HFONT font) noexcept
{
    SelectObject(dc, font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    return metrics.tmHeight;
}

int TextWidth(HDC dc, HFONT font, std::wstring_view text) noexcept
{
    SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
    return size.cx;
}

Colour ToneColour(rules::Tone tone) noexcept
{
    switch (tone)
    {
        case rules::Tone::Good:
            return theme::kGood;
        case rules::Tone::Warning:
            return theme::kWarning;
        case rules::Tone::Problem:
            return theme::kProblem;
        case rules::Tone::Plain:
            break;
    }
    return theme::kAccent;
}

/// One paint of the window: the scene, the fonts and the surface it is drawn on.
struct Painter
{
    HDC          dc;
    const Scene& scene;
    const Fonts& fonts;
    UINT         dpi;

    int Px(int units) const noexcept { return MulDiv(units, static_cast<int>(dpi), 96); }

    bool Hot(rules::Part part) const noexcept { return scene.hover.part == part; }
    bool Down(rules::Part part) const noexcept { return scene.pressed.part == part && scene.hover.part == part; }
    bool Focused(rules::Part part) const noexcept { return scene.showFocus && scene.focus.part == part; }

    /// The keyboard's mark: a thin line a little outside the part.
    void FocusRing(RECT rect) const noexcept
    {
        InflateRect(&rect, Px(3), Px(3));
        Edge(dc, rect, theme::kAccent, 1);
    }

    void Option(const row::Rect& cell, rules::Part part, std::wstring_view label, std::wstring_view value) const noexcept
    {
        const RECT rect = ToRect(cell);
        Fill(dc, rect, theme::kPanel);
        Edge(dc, rect, Hot(part) ? theme::kEdgeHot : theme::kEdge, 1);
        RECT name = rect;
        name.left += Px(14);
        const rules::OptionParts parts = rules::PartsOf(cell, dpi);
        name.right = parts.previous.left;
        Line(dc, fonts.body, theme::kText, name, label, DT_LEFT | DT_VCENTER);
        const bool before = Hot(part) && scene.hover.step < 0;
        const bool after  = Hot(part) && scene.hover.step > 0;
        Line(dc, fonts.arrow, before ? theme::kText : theme::kAccent, ToRect(parts.previous), kArrowBefore, DT_CENTER | DT_VCENTER);
        Line(dc, fonts.strong, theme::kText, ToRect(parts.value), value, DT_CENTER | DT_VCENTER);
        Line(dc, fonts.arrow, after ? theme::kText : theme::kAccent, ToRect(parts.next), kArrowAfter, DT_CENTER | DT_VCENTER);
        if (Focused(part))
        {
            FocusRing(rect);
        }
    }

    void Caption(const row::Rect& at, std::wstring_view heading, std::wstring_view hint) const noexcept
    {
        const RECT rect = ToRect(at);
        Line(dc, fonts.heading, theme::kTextDim, rect, heading, DT_LEFT | DT_VCENTER, Px(2));
        Line(dc, fonts.minor, theme::kTextDim, rect, hint, DT_RIGHT | DT_VCENTER);
    }

    /// Lines of text stacked in the middle of `rect`, each in its own font and colour; the lines that do not fit are left out
    /// from the bottom.
    struct Stacked
    {
        HFONT             font;
        Colour            colour;
        std::wstring_view text;
        int               spacing = 0;
    };

    void Stack(RECT rect, std::initializer_list<Stacked> lines) const noexcept
    {
        int total = 0;
        int count = 0;
        for (const Stacked& line : lines)
        {
            const int height = line.text.empty() ? 0 : LineHeight(dc, line.font);
            if (total + height > rect.bottom - rect.top)
            {
                break;
            }
            total += height;
            ++count;
        }
        LONG y = rect.top + (rect.bottom - rect.top - total) / 2;
        for (const Stacked& line : lines)
        {
            if (count-- <= 0)
            {
                break;
            }
            if (line.text.empty())
            {
                continue;
            }
            const int height = LineHeight(dc, line.font);
            Line(dc, line.font, line.colour, RECT{rect.left, y, rect.right, y + height}, line.text, DT_CENTER | DT_TOP, line.spacing);
            y += height;
        }
    }

    void Picture() const
    {
        const RECT well = ToRect(scene.frame.picture);
        Fill(dc, well, theme::kWell);
        Edge(dc, well, theme::kEdge, 1);
        for (std::size_t i = 0; i < scene.tiles.size(); ++i)
        {
            const Tile& tile    = scene.tiles[i];
            const RECT  rect    = ToRect(tile.rect);
            const bool  live    = tile.inRow && scene.rowLive;
            const bool  hot     = scene.hover.part == rules::Part::Tile && scene.hover.tile == i;
            const bool  chosen  = tile.pick > 0 || live;
            const Colour mark   = tile.problem ? theme::kWarning : live ? theme::kAccent : theme::kAccentDim;
            Fill(dc, rect, live ? theme::kTileLive : theme::kTile);
            Edge(dc, rect, hot ? theme::kEdgeHot : chosen ? theme::kAccentDim : theme::kEdge, 1);
            if (chosen)
            {
                Brackets(dc, rect, mark, Px(12), Px(2));
            }
            RECT inner = rect;
            InflateRect(&inner, -Px(8), -Px(8));
            if (tile.pick > 0)
            {
                const RECT badge{inner.left, inner.top, inner.left + Px(22), inner.top + Px(22)};
                Fill(dc, badge, mark);
                Line(dc, fonts.strong, theme::kTextOnAccent, badge, std::to_wstring(tile.pick), DT_CENTER | DT_VCENTER);
            }
            const std::wstring player = live && tile.player > 0 ? std::format(L"PLAYER {}", tile.player) : std::wstring();
            std::wstring       size   = std::to_wstring(tile.width) + kTimes + std::to_wstring(tile.height);
            // A small tile: who plays on it stays, its size goes; and a name that would run under the badge starts beside it.
            const int lines = LineHeight(dc, fonts.strong) + LineHeight(dc, fonts.minor) + (player.empty() ? 0 : LineHeight(dc, fonts.heading));
            if (!player.empty() && lines > inner.bottom - inner.top)
            {
                size.clear();
            }
            if (tile.pick > 0 && (inner.right - inner.left - TextWidth(dc, fonts.strong, tile.name)) / 2 < Px(22 + 6))
            {
                inner.left += Px(22 + 6);
            }
            Stack(inner, {{fonts.strong, theme::kText, tile.name}, {fonts.minor, theme::kTextDim, size}, {fonts.heading, theme::kAccent, player, Px(2)}});
            if (scene.showFocus && scene.focus.part == rules::Part::Tile && scene.focus.tile == i)
            {
                FocusRing(rect);
            }
        }
    }

    /// The status with its coloured mark, when there is one, and the version at the far end of its line.
    void Status() const noexcept
    {
        RECT rect = ToRect(scene.frame.status);
        Line(dc, fonts.minor, theme::kTextDim, rect, scene.version, DT_RIGHT | DT_VCENTER);
        if (scene.status.text.empty())
        {
            return;
        }
        rect.right -= TextWidth(dc, fonts.minor, scene.version) + Px(16);
        const LONG side = Px(8);
        const LONG top  = rect.top + (rect.bottom - rect.top - side) / 2;
        Fill(dc, RECT{rect.left, top, rect.left + side, top + side}, ToneColour(scene.status.tone));
        rect.left += side + Px(10);
        Line(dc, fonts.body, theme::kText, rect, scene.status.text, DT_LEFT | DT_VCENTER);
    }

    void Button(const row::Rect& at, rules::Part part, std::wstring_view label, bool primary, bool enabled) const noexcept
    {
        const RECT rect = ToRect(at);
        if (primary)
        {
            Fill(dc, rect, !enabled ? theme::kPrimaryOff : Down(part) ? theme::kPrimaryDown : Hot(part) ? theme::kPrimaryHot : theme::kPrimary);
            if (!enabled)
            {
                Edge(dc, rect, theme::kEdge, 1);
            }
            Line(dc, fonts.strong, enabled ? theme::kTextOnAccent : theme::kTextOff, rect, label, DT_CENTER | DT_VCENTER);
        }
        else
        {
            Fill(dc, rect, Down(part) ? theme::kPanelDown : theme::kPanel);
            Edge(dc, rect, Hot(part) ? theme::kAccent : theme::kEdgeHot, 1);
            Line(dc, fonts.strong, theme::kText, rect, label, DT_CENTER | DT_VCENTER);
        }
        if (Focused(part))
        {
            FocusRing(rect);
        }
    }

    void All(const RECT& client) const
    {
        Fill(dc, client, theme::kWindow);
        Option(scene.frame.multi, rules::Part::Multi, scene.multiLabel, scene.multiValue);
        Option(scene.frame.order, rules::Part::Order, scene.orderLabel, scene.orderValue);
        Caption(scene.frame.caption, L"DISPLAYS", L"Click monitors in order, left to right");
        Picture();
        Status();
        if (scene.canRestore)
        {
            Button(scene.frame.restore, rules::Part::Restore, L"Restore Monitors", false, true);
        }
        Button(scene.frame.save, rules::Part::Save, L"Save", true, scene.canSave);
        Button(scene.frame.close, rules::Part::Close, L"Close", false, true);
    }
};

/// A drawing surface the size of the window, with what it took given back however the drawing ends.
struct Surface
{
    HDC     dc     = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    HGDIOBJ oldFont   = nullptr;

    Surface(HDC like, int width, int height, HFONT font) noexcept
        : dc(CreateCompatibleDC(like)), bitmap(CreateCompatibleBitmap(like, width, height))
    {
        if (Ready())
        {
            oldBitmap = SelectObject(dc, bitmap);
            oldFont   = SelectObject(dc, font);
        }
    }
    ~Surface()
    {
        if (oldFont != nullptr)
        {
            SelectObject(dc, oldFont);
        }
        if (oldBitmap != nullptr)
        {
            SelectObject(dc, oldBitmap);
        }
        if (bitmap != nullptr)
        {
            DeleteObject(bitmap);
        }
        if (dc != nullptr)
        {
            DeleteDC(dc);
        }
    }
    Surface(const Surface&)            = delete;
    Surface& operator=(const Surface&) = delete;

    bool Ready() const noexcept { return dc != nullptr && bitmap != nullptr; }
};

HFONT MakeFont(UINT dpi, int size, int weight) noexcept
{
    return CreateFontW(-MulDiv(size, static_cast<int>(dpi), 96), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

}  // namespace

Fonts MakeFonts(UINT dpi)
{
    Fonts fonts;
    fonts.body    = MakeFont(dpi, 14, FW_NORMAL);
    fonts.strong  = MakeFont(dpi, 14, FW_SEMIBOLD);
    fonts.minor   = MakeFont(dpi, 12, FW_NORMAL);
    fonts.heading = MakeFont(dpi, 11, FW_BOLD);
    fonts.arrow   = MakeFont(dpi, 30, FW_BOLD);
    return fonts;
}

void Free(Fonts& fonts) noexcept
{
    for (HFONT* font : {&fonts.body, &fonts.strong, &fonts.minor, &fonts.heading, &fonts.arrow})
    {
        if (*font != nullptr)
        {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
}

void Draw(HDC target, const RECT& client, const Scene& scene, const Fonts& fonts, UINT dpi)
{
    const int width  = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
    {
        return;
    }
    // Drawn whole on a surface of its own, then shown in one step: no part is ever seen half painted.
    Surface surface(target, width, height, fonts.body);
    if (!surface.Ready())
    {
        return;
    }
    SetBkMode(surface.dc, TRANSPARENT);
    Painter{surface.dc, scene, fonts, dpi}.All(RECT{0, 0, width, height});
    BitBlt(target, client.left, client.top, width, height, surface.dc, 0, 0, SRCCOPY);
}

}  // namespace lss::setup::view
