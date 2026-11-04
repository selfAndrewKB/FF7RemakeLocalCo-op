#pragma once

// What a player's window says (players/player_window; the rules: players/player_window_rules.h). Pure: host-tested
// (tests/player_window_test.cpp).
//
// One text, two forms. The game's form is markup for its message window (hud/view_window; research/RE-game-widgets.md
// §12): button icons by physical name (`<button=cross>` is A on an Xbox pad and ✕ on a DualShock; physical, because
// the window reads the physical buttons, never the confirm swap), each forced to a pad's art (`,pad`: the game
// otherwise draws the last device's, a keyboard's keycaps after a key press), the character in the game's yellow,
// lines joined by CR LF as the game's own strings are. The plain form is the same lines in words, for the fallback
// drawn on the engine's canvas (hud/view_prompt) when no game window can be opened. A window says, top to bottom: its
// player's number and character; then a line for each key the player can press, or a note for a while (a join that
// waits, a refusal).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lss::players::player_window_text {

inline constexpr int kLines = 5;  // the most a window says: player 2's or 3's open window in the fallback's words

struct ScreenText
{
    std::wstring                     markup;  // the game window's text
    std::array<std::wstring, kLines> plain;   // the fallback's lines, the character's first, the rest empty
};

/// A line a window shows for a while in place of its keys. On player 1's window unless it says otherwise.
enum class Note : std::uint8_t
{
    None,
    Waits,           // the new player (`about`) joins once a party member is free
    NoMember,        // refused: no party member is free for a new player
    AllIn,           // refused: every player is in already
    Failed,          // the join did not work (the log says why)
    PlayerOneStays,  // B held on player 1's pad (on player 1's own window)
};

/// The note's words; `about` the player it concerns (seat, 0 player 1's).
inline std::wstring NoteText(Note note, int about)
{
    const std::wstring player = L"Player " + std::to_wstring(about + 1);
    switch (note)
    {
    case Note::Waits: return player + L" joins once a party member is free";
    case Note::NoMember: return L"No party member is free for a new player";
    case Note::AllIn: return L"Every player is in already";
    case Note::Failed: return player + L"'s join did not work";
    case Note::PlayerOneStays: return L"Player 1 cannot be removed from the game";
    case Note::None: break;
    }
    return L"";
}

/// What the window knows of its player.
struct Window
{
    int          seat       = 0;      // 0 player 1's
    std::wstring name;                // the character in capitals ("TIFA")
    bool         open       = false;  // the player's window is open: its keys show (else only a note does)
    bool         canStep    = false;  // the d-pad steps to another party member (never player 1)
    bool         canMove    = false;  // more than one screen: LB/RB move the view
    bool         startJoins = false;  // a seat is free: Start on another pad adds a player (said on player 1's)
    Note         note       = Note::None;
    int          about      = 0;      // the note's player
};

/// "PLAYER 2" for seat 1.
inline std::wstring PlayerName(int seat)
{
    return L"PLAYER " + std::to_wstring(seat + 1);
}

inline ScreenText TextFor(const Window& w)
{
    struct Line
    {
        std::wstring markup;  // empty: the game window has no such line
        std::wstring plain;   // empty: the fallback has no such line
    };
    const auto icon   = [](const wchar_t* name) { return std::wstring(L"<button=") + name + L",pad>"; };
    const auto yellow = [](const std::wstring& text) { return L"<color=yellow>" + text + L"</color>"; };
    const auto player = PlayerName(w.seat);

    std::vector<Line> lines;
    lines.push_back({player + L"    " +
                         (w.canStep && w.open ? icon(L"left") + L"  " + yellow(w.name) + L"  " + icon(L"right") : yellow(w.name)),
                     player + L"    " + (w.canStep && w.open ? L"<   " + w.name + L"   >" : w.name)});
    if (w.note != Note::None)
    {
        lines.push_back({NoteText(w.note, w.about), NoteText(w.note, w.about)});
    }
    else if (w.open)
    {
        if (w.canStep)
        {
            lines.push_back({L"", L"Left/Right: character"});  // the game window shows the arrows beside the name
        }
        if (w.canMove)
        {
            lines.push_back({icon(L"L1") + L" " + icon(L"R1") + L" Move my view", L"LB/RB: move my view"});
        }
        lines.push_back({icon(L"cross") + L" Done", L"A: done"});
        if (w.seat >= 1)
        {
            lines.push_back({L"Hold " + icon(L"circle") + L" to leave the game", L"Hold B to leave the game"});
        }
        if (w.seat == 0 && w.startJoins)
        {
            lines.push_back({L"Press " + icon(L"options") + L" on another controller to join",
                             L"Press Start on another controller to join"});
        }
    }

    ScreenText  text;
    std::size_t plain = 0;
    for (const Line& line : lines)
    {
        if (!line.markup.empty())
        {
            text.markup += (text.markup.empty() ? L"" : L"\r\n") + line.markup;
        }
        if (!line.plain.empty() && plain < text.plain.size())
        {
            text.plain[plain++] = line.plain;
        }
    }
    return text;
}

}  // namespace lss::players::player_window_text
