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
// screen (a copy's), that a new player can join there; what that screen's person can press, a line for each key, or
// a note for a while (a refusal, a join that waits); and how to cancel the claim.

#include "players/pad_claim_rules.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lss::players::pad_claim_text {

namespace rules = pad_claim_rules;

inline constexpr int kLines = 6;  // the most a screen says (the fallback's; the game window's is five)

struct ScreenText
{
    std::wstring                         markup;  // the game window's text
    std::array<std::wstring, kLines>     plain;   // the fallback's lines, the character's first, the rest empty
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
    case Note::PlayerOneStays: return L"Player 1 cannot be removed from the game";
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
/// An open screen (no player: a copy's) offers a join there. Each key the screen's person can press has a line of its
/// own; `note` replaces those lines for a while. Player 2 and 3 can always leave (B held); player 1 is never offered it.
/// Empty outside the screens.
inline ScreenText TextFor(const rules::Claim& c, int screen, const Seats& seats, Note note = Note::None)
{
    if (screen < 0 || screen >= c.screens)
    {
        return {};
    }
    struct Line
    {
        std::wstring markup;  // empty: the game window has no such line
        std::wstring plain;   // empty: the fallback has no such line
    };
    const auto   at     = static_cast<std::size_t>(screen);
    const int    seat   = c.seatOn[at];
    const auto   icon   = [](Button b) { return std::wstring(L"<button=") + MarkupName(b) + L",pad>"; };
    const auto   word   = [](Button b) { return std::wstring(PlainName(b)); };
    const auto   yellow = [](const std::wstring& text) { return L"<color=yellow>" + text + L"</color>"; };

    Line              title;
    std::vector<Line> keys;
    std::vector<Line> foot{{L"Hold " + icon(Button::View) + L" to cancel", L"Changing pads    (hold View to cancel)"}};
    if (seat < 0)
    {
        const Button take = kScreenButton[at];
        title             = {yellow(L"NEW PLAYER"), L"NEW PLAYER"};
        keys.push_back(c.joiningBy[at] >= 0
                           ? Line{L"Joining...", L"Joining..."}
                           : Line{L"Press " + icon(take) + L" to join here", L"Press " + word(take) + L" to join here"});
    }
    else
    {
        const auto& name   = seats.names[static_cast<std::size_t>(seat)];
        const auto  player = PlayerName(seat);
        if (c.takenBy[at] < 0)
        {
            const Button take = kScreenButton[at];
            title             = {yellow(player), player};
            keys.push_back(
                {L"Press " + icon(take) + L" to play this screen", L"Press " + word(take) + L" to play this screen"});
        }
        else if (!c.kept[at])
        {
            const bool steps = seats.canStep[static_cast<std::size_t>(seat)];
            title            = {
                player + L"    " +
                    (steps ? icon(Button::Left) + L"  " + yellow(name) + L"  " + icon(Button::Right) : yellow(name)),
                player + L"    " + (steps ? L"<   " + name + L"   >" : name)};
            if (steps)
            {
                keys.push_back({L"", L"Left/Right: character"});  // the game window shows the arrows beside the name
            }
            keys.push_back({icon(Button::A) + L" Select", L"A: select"});
            keys.push_back({icon(Button::B) + L" Back", L"B: back"});
        }
        else
        {
            title = {player + L"    " + yellow(name), player + L"    " + name};
            keys.push_back({icon(Button::B) + L" Change", L"B: change"});
        }
        if (seat >= 1)
        {
            keys.push_back({L"Hold " + icon(Button::B) + L" to leave the game", L"Hold B to leave the game"});
        }
        if (seat == 0 && seats.startJoins)
        {
            foot.push_back(
                {L"Press " + icon(Button::Start) + L" on another pad to join", L"Press Start on another pad to join"});
        }
    }
    if (note != Note::None)
    {
        keys = {{NoteText(note), NoteText(note)}};
    }

    ScreenText  text;
    std::size_t plain = 0;
    const auto  add   = [&](const Line& line) {
        if (!line.markup.empty())
        {
            text.markup += (text.markup.empty() ? L"" : L"\r\n") + line.markup;
        }
        if (!line.plain.empty() && plain < text.plain.size())
        {
            text.plain[plain++] = line.plain;
        }
    };
    add(title);
    std::for_each(keys.begin(), keys.end(), add);
    std::for_each(foot.begin(), foot.end(), add);
    return text;
}

}  // namespace lss::players::pad_claim_text
