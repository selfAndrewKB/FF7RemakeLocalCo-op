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
// opened. A screen says, top to bottom: its player's number (and, once a pad took it, the character), or, on an open
// screen (a copy's), that a new player can join there; what that screen's person can press, or a note for a while
// (a refusal, a join that waits); and how to cancel the claim.

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
    Start,
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
    case Button::Start: return L"options";
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
    case Button::Start: return L"Start";
    }
    return L"A";
}

/// Each screen position's button, as pad_claim_rules::kScreenButton.
inline constexpr std::array<Button, rules::kScreens> kScreenButton{Button::A, Button::X, Button::Y};

/// What the claim knows of each seat (by seat, 0 player 1's): its character's name in capitals ("TIFA"), and whether
/// its player can step to another party member (players/session.h CanStepMember; never player 1).
struct Seats
{
    std::array<std::wstring, rules::kScreens> names;
    std::array<bool, rules::kScreens>         canStep{};
    bool startJoins = false;  // no screen is open and a seat is free: Start on another pad adds a view (one window)
};

/// A line a screen shows for a while in place of its keys.
enum class Note : std::uint8_t
{
    None,
    Waits,           // the join is made once a party member is free
    NoMember,        // refused: no party member is free to play
    AllIn,           // refused: every player is in
    Failed,          // the join failed (the log says why)
    PlayerOneStays,  // B held on player 1's pad
};

inline const wchar_t* NoteText(Note note) noexcept
{
    switch (note)
    {
    case Note::Waits: return L"Joins once a party member is free";
    case Note::NoMember: return L"No party member is free to play";
    case Note::AllIn: return L"Every player is in already";
    case Note::Failed: return L"The join did not work";
    case Note::PlayerOneStays: return L"Player 1 stays in";
    case Note::None: break;
    }
    return L"";
}

/// "PLAYER 2" for seat 1.
inline std::wstring PlayerName(int seat)
{
    return L"PLAYER " + std::to_wstring(seat + 1);
}

/// Screen `screen`'s text. A screen no pad took names its player only ("PLAYER 2"): the character comes with the take.
/// An open screen (no player: a copy's) offers a join there. `note` replaces the keys' line for a while. Empty outside
/// the screens.
inline ScreenText TextFor(const rules::Claim& c, int screen, const Seats& seats, Note note = Note::None)
{
    if (screen < 0 || screen >= c.screens)
    {
        return {};
    }
    const auto   at     = static_cast<std::size_t>(screen);
    const int    seat   = c.seatOn[at];
    const auto   icon   = [](Button b) { return std::wstring(L"<button=") + MarkupName(b) + L",pad>"; };
    const auto   word   = [](Button b) { return std::wstring(PlainName(b)); };
    const auto   yellow = [](const std::wstring& text) { return L"<color=yellow>" + text + L"</color>"; };

    std::array<std::wstring, kLines> markup;
    std::array<std::wstring, kLines> plain;
    const auto finish = [&]() {
        if (note != Note::None)
        {
            markup[1] = NoteText(note);
            plain[1]  = NoteText(note);
        }
        ScreenText text;
        text.plain = plain;
        for (int line = 0; line < kLines; ++line)
        {
            text.markup += (line == 0 ? L"" : L"\r\n") + markup[static_cast<std::size_t>(line)];
        }
        return text;
    };
    markup[2] = L"Hold " + icon(Button::View) + L" to cancel";
    plain[2]  = L"Changing pads    (hold View to cancel)";
    if (seat < 0)
    {
        const Button take = kScreenButton[at];
        markup[0]         = yellow(L"NEW PLAYER");
        plain[0]          = L"NEW PLAYER";
        markup[1] = c.joiningBy[at] >= 0 ? L"Joining..." : L"Press " + icon(take) + L" to join here";
        plain[1]  = c.joiningBy[at] >= 0 ? L"Joining..." : L"Press " + std::wstring(word(take)) + L" to join here";
        return finish();
    }
    const auto& name   = seats.names[static_cast<std::size_t>(seat)];
    const auto  player = PlayerName(seat);
    if (seat == 0 && seats.startJoins)
    {
        markup[2] = L"Hold " + icon(Button::View) + L" to cancel    " + icon(Button::Start) + L" on another pad: join";
        plain[2]  = L"Changing pads    (hold View to cancel; Start on another pad joins)";
    }
    if (c.takenBy[at] < 0)
    {
        const Button take = kScreenButton[at];
        markup[0]         = yellow(player);
        plain[0]          = player;
        markup[1]         = L"Press " + icon(take) + L" to play this screen";
        plain[1]          = L"Press " + word(take) + L" to play this screen";
    }
    else if (!c.kept[at])
    {
        const bool steps = seats.canStep[static_cast<std::size_t>(seat)];
        markup[0]        = player + L"    " +
                    (steps ? icon(Button::Left) + L"  " + yellow(name) + L"  " + icon(Button::Right) : yellow(name));
        plain[0]  = player + L"    " + (steps ? L"<   " + name + L"   >" : name);
        markup[1] = icon(Button::A) + L" Keep    " + icon(Button::B) + L" Back" +
                    (seat >= 1 ? L"    Hold " + icon(Button::B) + L" Leave" : std::wstring());
        plain[1]  = (steps ? L"Left/Right: character    A: keep    B: back" : L"A: keep    B: back") +
                   std::wstring(seat >= 1 ? L"    hold B: leave" : L"");
    }
    else
    {
        markup[0] = player + L"    " + yellow(name);
        plain[0]  = player + L"    " + name;
        markup[1] = L"Kept    " + icon(Button::B) + L" Change";
        plain[1]  = L"Kept    (B: change)";
    }
    return finish();
}

}  // namespace lss::players::pad_claim_text
