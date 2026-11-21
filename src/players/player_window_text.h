#pragma once

// What a player's window says (players/player_window; the rules: players/player_window_rules.h). Pure: host-tested
// (tests/player_window_test.cpp).
//
// One text, two forms. The game's form is markup for its message window (hud/view_window, RE-game-widgets.md):
// button icons by physical name (`<button=cross>` is A on an Xbox pad and ✕ on a DualShock; physical, because
// the window reads the physical buttons, never the confirm swap), each forced to a pad's art (`,pad`: the game
// otherwise draws the last device's, a keyboard's keycaps after a key press), the character in the game's yellow,
// lines joined by CR LF as the game's own strings are. The plain form is the same lines in words, for the fallback
// drawn on the engine's canvas (hud/view_prompt) when no game window can be opened. A window says, top to bottom: its
// player's number between the d-pad's left and right (a trade of numbers) and its character between up and down (a
// step through the party list), each pair shown while it can change; then a line for each other key the player can
// press, or a note for a while (a join that waits, a refusal).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lss::players::player_window_text {

inline constexpr int kLines = 6;  // the most a window says: player 2's or 3's open window in the fallback's words

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
    WaitsOnAi,       // the new player's party member (`aboutName`) is in the party, with the game's AI until released
    NoMember,        // refused: no party member is free for a new player
    AllIn,           // refused: every player is in already
    Failed,          // the join did not work (the log says why)
    PlayerOneStays,  // B held on player 1's pad (on player 1's own window)
};

/// The note's words; `about` the player it concerns (seat, 0 player 1's), `aboutName` that player's character in capitals.
inline std::wstring NoteText(Note note, int about, const std::wstring& aboutName = {})
{
    const std::wstring player = L"Player " + std::to_wstring(about + 1);
    switch (note)
    {
    case Note::Waits: return player + L" has joined and is waiting for a free party member";
    case Note::WaitsOnAi:
        return aboutName.empty() ? NoteText(Note::Waits, about)
                                 : player + L" has joined and is waiting for " + aboutName + L" to be released from AI control";
    case Note::NoMember: return L"No available party members";
    case Note::AllIn: return L"All players are currently present";
    case Note::Failed: return player + L"'s join failed. See UE4SS.log.";
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
    bool         canStep    = false;  // the d-pad's up/down step to another party member (never player 1)
    bool         canTrade   = false;  // another player is in: the d-pad's left/right trade player numbers
    bool         canMove    = false;  // more than one screen: LB/RB move the view
    bool         startJoins = false;  // a seat is free: Start on another pad adds a player (said on player 1's)
    bool         keyboard   = false;  // the player plays on the keyboard: its keys' names, not the pad's icons
    bool         keyboardJoins = false;  // Tab held adds a player on the keyboard too (it plays no one of its own)
    Note         note       = Note::None;
    int          about      = 0;      // the note's player
    std::wstring aboutName;           // the note's player's character in capitals, when the note names it
};

/// The notice after a press on the title screen: which controller plays player 1 (`pad` the physical pad, as
/// players/pad_rules numbers them: 0..3 XInput, 4..7 PlayStation; negative: the keyboard).
inline std::wstring TitleClaimed(int pad)
{
    if (pad < 0)
    {
        return L"Keyboard is set as Player 1";
    }
    const bool playStation = pad >= 4;
    return (playStation ? L"PlayStation Controller " : L"Controller ") + std::to_wstring(pad % 4 + 1) +
           L" is set as Player 1";
}

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

    const bool trade = w.canTrade && w.open;
    const bool step  = w.canStep && w.open;
    // The keyboard's arrows have no icon of their own: the plain marks in both forms.
    const bool icons = !w.keyboard;
    std::vector<Line> lines;
    lines.push_back({(trade && icons ? icon(L"left") + L" " + player + L" " + icon(L"right") : trade ? L"<  " + player + L"  >" : player) +
                         L"    " +
                         (step && icons ? icon(L"up") + L"  " + yellow(w.name) + L"  " + icon(L"down")
                          : step        ? L"^   " + yellow(w.name) + L"   v"
                                        : yellow(w.name)),
                     (trade ? L"<  " + player + L"  >" : player) + L"    " + (step ? L"^   " + w.name + L"   v" : w.name)});
    if (w.note != Note::None)
    {
        const std::wstring note = NoteText(w.note, w.about, w.aboutName);
        lines.push_back({note, note});
    }
    else if (w.open && w.keyboard)
    {
        // The keyboard's keys by name, the same in both forms.
        const auto both = [&lines](const std::wstring& text) { lines.push_back({text, text}); };
        if (w.canStep)
        {
            both(L"Up/Down: character");
        }
        if (w.canTrade)
        {
            both(L"Left/Right: player number");
        }
        if (w.canMove)
        {
            both(L"Q/E: Change View");
        }
        both(L"Enter: done");
        if (w.seat >= 1)
        {
            both(L"Hold Backspace to leave the game");
        }
        if (w.seat == 0 && w.startJoins)
        {
            both(L"Press Start on a controller to join");
        }
    }
    else if (w.open)
    {
        // The game window shows the d-pad's pairs beside the number and the name; the fallback says them in words.
        if (w.canStep)
        {
            lines.push_back({L"", L"Up/Down: character"});
        }
        if (w.canTrade)
        {
            lines.push_back({L"", L"Left/Right: player number"});
        }
        if (w.canMove)
        {
            lines.push_back({icon(L"L1") + L" " + icon(L"R1") + L" Change View", L"LB/RB: Change View"});
        }
        lines.push_back({icon(L"cross") + L" Done", L"A: done"});
        if (w.seat >= 1)
        {
            lines.push_back({L"Hold " + icon(L"circle") + L" to leave the game", L"Hold B to leave the game"});
        }
        if (w.seat == 0 && w.startJoins)
        {
            lines.push_back({L"Press " + icon(L"options") + L" on another controller" +
                                 (w.keyboardJoins ? L" or hold Tab on the keyboard" : L"") + L" to join",
                             std::wstring(L"Press Start on another controller") +
                                 (w.keyboardJoins ? L" or hold Tab" : L"") + L" to join"});
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
