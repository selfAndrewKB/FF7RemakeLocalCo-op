#pragma once

// What each screen of the claim says (players/pad_claim; the rules: players/pad_claim_rules.h). Pure: host-tested
// (tests/pad_claim_text_test.cpp).
//
// One text, two forms. The game's form is markup for its message window (hud/view_window; research/RE-game-widgets.md
// §12): button icons by physical name (`<button=cross>` is A on an Xbox pad and ✕ on a DualShock; physical, because the
// claim reads the physical buttons, never the confirm swap), each forced to a pad's art (`,pad`: the game otherwise
// draws the last device's, a keyboard's keycaps after a key press, seen live 2026-10-31, and only pads take part in a
// claim), the character in the game's yellow, lines joined by CR LF as the game's own strings are. The plain form is
// the same lines in words, for the fallback drawn on the engine's canvas (hud/view_prompt) when no game window can be
// opened. A screen says, top to bottom: the character there, what that screen's person can press, and how to cancel
// the claim.

#include "players/pad_claim_rules.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lss::players::pad_claim_text {

namespace rules = pad_claim_rules;

inline constexpr int kLines = 3;

struct ScreenText
{
    std::wstring                         markup;  // the game window's text
    std::array<std::wstring, kLines>     plain;   // the fallback's lines, the character's first
};

/// The buttons the claim names, as the game's markup names them and in words.
enum class Button : std::uint8_t
{
    A,
    B,
    X,
    Y,
    Left,
    Right,
    View,
};

inline const wchar_t* MarkupName(Button button) noexcept
{
    switch (button)
    {
    case Button::A: return L"cross";
    case Button::B: return L"circle";
    case Button::X: return L"square";
    case Button::Y: return L"triangle";
    case Button::Left: return L"left";
    case Button::Right: return L"right";
    case Button::View: return L"touch";  // the View key's icon (key byte 0xD0; `view` names the same key, §12.7)
    }
    return L"cross";
}

inline const wchar_t* PlainName(Button button) noexcept
{
    switch (button)
    {
    case Button::A: return L"A";
    case Button::B: return L"B";
    case Button::X: return L"X";
    case Button::Y: return L"Y";
    case Button::Left: return L"Left";
    case Button::Right: return L"Right";
    case Button::View: return L"View";
    }
    return L"A";
}

/// Each screen position's button, as pad_claim_rules::kScreenButton.
inline constexpr std::array<Button, rules::kScreens> kScreenButton{Button::A, Button::X, Button::Y};

/// The player on a screen can step: two players are on screens, this one among them.
constexpr bool CanStep(const rules::Claim& c, int seat) noexcept
{
    int count = 0;
    for (int s = 0; s < c.screens; ++s)
    {
        count += c.seatOn[static_cast<std::size_t>(s)] >= 0 ? 1 : 0;
    }
    return count >= 2 && seat >= 0;
}

/// Screen `screen`'s text; `names` the characters by seat (upper case, "TIFA"). Empty for a screen with no player.
inline ScreenText TextFor(const rules::Claim& c, int screen, const std::array<std::wstring, rules::kScreens>& names)
{
    if (screen < 0 || screen >= c.screens || c.seatOn[static_cast<std::size_t>(screen)] < 0)
    {
        return {};
    }
    const auto  at   = static_cast<std::size_t>(screen);
    const int   seat = c.seatOn[at];
    const auto& name = names[static_cast<std::size_t>(seat)];
    const auto  icon = [](Button b) { return std::wstring(L"<button=") + MarkupName(b) + L",pad>"; };
    const auto  word = [](Button b) { return std::wstring(PlainName(b)); };
    const std::wstring yellow = L"<color=yellow>" + name + L"</color>";

    std::array<std::wstring, kLines> markup;
    std::array<std::wstring, kLines> plain;
    if (c.takenBy[at] < 0)
    {
        const Button take = kScreenButton[at];
        markup[0]         = yellow;
        plain[0]          = name;
        markup[1]         = L"Press " + icon(take) + L" to play this screen";
        plain[1]          = L"Press " + word(take) + L" to play this screen";
    }
    else if (!c.kept[at])
    {
        const bool steps = CanStep(c, seat);
        markup[0]        = steps ? icon(Button::Left) + L"  " + yellow + L"  " + icon(Button::Right) : yellow;
        plain[0]         = steps ? L"<   " + name + L"   >" : name;
        markup[1]        = icon(Button::A) + L" Keep    " + icon(Button::B) + L" Back";
        plain[1]         = steps ? L"Left/Right: character    A: keep    B: back" : L"A: keep    B: back";
    }
    else
    {
        markup[0] = yellow;
        plain[0]  = name;
        markup[1] = L"Kept    " + icon(Button::B) + L" Change";
        plain[1]  = L"Kept    (B: change)";
    }
    markup[2] = L"Hold " + icon(Button::View) + L" to cancel";
    plain[2]  = L"Changing pads    (hold View to cancel)";

    ScreenText text;
    text.plain = plain;
    for (int line = 0; line < kLines; ++line)
    {
        text.markup += (line == 0 ? L"" : L"\r\n") + markup[static_cast<std::size_t>(line)];
    }
    return text;
}

}  // namespace lss::players::pad_claim_text
