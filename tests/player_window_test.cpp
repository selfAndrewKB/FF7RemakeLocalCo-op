// Host-side tests for the player windows' pure rules and texts (players/player_window_rules.h,
// players/player_window_text.h): one layer of keys in a player's own window, the view moving one screen at a time, a
// pad that presses on the title screen taking player 1, and each window's lines.

#include "players/player_window_rules.h"
#include "players/player_window_text.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace lss::players::player_window_rules;
using namespace lss::players::player_window_text;
namespace pad = lss::players::pad_rules;

int g_failures = 0;

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

bool Has(const std::wstring& text, const std::wstring& part)
{
    return text.find(part) != std::wstring::npos;
}

/// The markup's lines, split at the game's CR LF.
std::vector<std::wstring> LinesOf(const std::wstring& markup)
{
    std::vector<std::wstring> lines;
    std::size_t               from = 0;
    for (std::size_t at = markup.find(L"\r\n"); at != std::wstring::npos; at = markup.find(L"\r\n", from))
    {
        lines.push_back(markup.substr(from, at - from));
        from = at + 2;
    }
    lines.push_back(markup.substr(from));
    return lines;
}

/// One layer: the d-pad steps a further player only, LB/RB move anyone's view, A and Start close; B is not a press.
void OneLayerOfKeys()
{
    EXPECT(OnPress(1, kDpadLeft) == (Action{Act::Step, -1}));
    EXPECT(OnPress(2, kDpadRight) == (Action{Act::Step, +1}));
    EXPECT(OnPress(0, kDpadRight).act == Act::Nothing);  // player 1's character is the game's leader
    EXPECT(OnPress(0, kLB) == (Action{Act::Move, -1}));
    EXPECT(OnPress(2, kRB) == (Action{Act::Move, +1}));
    EXPECT(OnPress(1, pad::kA).act == Act::Close);
    EXPECT(OnPress(0, pad::kStart).act == Act::Close);
    EXPECT(OnPress(1, pad::kB).act == Act::Nothing);
    EXPECT(OnPress(1, pad::kX).act == Act::Nothing);
}

/// B: a tap closes, a hold leaves (player 1 refused), nothing yet does nothing.
void TheB()
{
    EXPECT(OnB(1, true, false).act == Act::Close);
    EXPECT(OnB(0, true, false).act == Act::Close);
    EXPECT(OnB(2, false, true).act == Act::Leave);
    EXPECT(OnB(0, false, true).act == Act::Refused);
    EXPECT(OnB(1, false, false).act == Act::Nothing);
}

/// A view moves one screen, swapped with what is there (a player or a copy); never past the row's edge.
void TheViewMoves()
{
    const auto right = Moved({0, 1, -1}, 3, 0, +1);
    EXPECT(right && (*right == std::array<int, kSeats>{1, 0, -1}));
    const auto ontoCopy = Moved({0, 1, -1}, 3, 1, +1);
    EXPECT(ontoCopy && (*ontoCopy == std::array<int, kSeats>{0, -1, 1}));
    EXPECT(!Moved({0, 1, -1}, 3, 0, -1));  // the left edge
    EXPECT(!Moved({0, 1, 2}, 3, 2, +1));   // the right edge
    EXPECT(!Moved({0, -1, -1}, 1, 0, +1)); // one screen
    EXPECT(!Moved({0, 1, -1}, 2, 2, -1));  // a seat on no screen
    const auto two = Moved({1, 0, -1}, 2, 0, -1);
    EXPECT(two && (*two == std::array<int, kSeats>{0, 1, -1}));
    EXPECT(OrderWord({2, 1, 3}) == L"213");
}

/// The title screen's press takes player 1's slot; a join's pad takes the new player's; a pad's seat is its slot's.
void PadsAndSeats()
{
    EXPECT((WithPlayerOne({0, 1, 2, 3}, 2) == pad::PadTable{2, 1, 0, 3}));
    EXPECT((WithPlayerOne({0, 1, 2, 3}, 0) == pad::kIdentity));  // already player 1's
    EXPECT((WithPadOn({0, 1, 2, 3}, 3, 1) == pad::PadTable{0, 3, 2, 1}));
    EXPECT(pad::IsPermutation(WithPadOn({2, 0, 3, 1}, 1, 2)));
    EXPECT(SeatOf({0, 1, 2, 3}, 0b1, 0) == 0);
    EXPECT(SeatOf({0, 1, 2, 3}, 0b1, 1) == -1);  // player 2 not in: the pad plays no one
    EXPECT(SeatOf({0, 1, 2, 3}, 0b11, 1) == 1);
    EXPECT(SeatOf({0, 1, 2, 3}, 0b111, 3) == -1);  // the spare slot
    EXPECT(SeatOf({1, 0, 2, 3}, 0b1, 1) == 0);
}

/// Player 2's open window: the character with arrows, move, done, leave; one key a line.
void FurtherPlayersWindow()
{
    Window w{.seat = 1, .name = L"BARRET", .open = true, .canStep = true, .canMove = true};
    const ScreenText t = TextFor(w);
    const std::vector<std::wstring> lines{
        L"PLAYER 2    <button=left,pad>  <color=yellow>BARRET</color>  <button=right,pad>",
        L"<button=L1,pad> <button=R1,pad> Move my view",
        L"<button=cross,pad> Done",
        L"Hold <button=circle,pad> to leave the game",
    };
    EXPECT(LinesOf(t.markup) == lines);
    EXPECT(t.plain[0] == L"PLAYER 2    <   BARRET   >");
    EXPECT(t.plain[1] == L"Left/Right: character");
    EXPECT(t.plain[2] == L"LB/RB: move my view");
    EXPECT(t.plain[3] == L"A: done");
    EXPECT(t.plain[4] == L"Hold B to leave the game");
    EXPECT(t.plain[5].empty());
}

/// Player 1's: no arrows, no leave; the Start hint while a seat is free; no move line with one screen.
void PlayerOnesWindow()
{
    Window w{.seat = 0, .name = L"CLOUD", .open = true, .canMove = false, .startJoins = true};
    const std::vector<std::wstring> lines{
        L"PLAYER 1    <color=yellow>CLOUD</color>",
        L"<button=cross,pad> Done",
        L"Press <button=options,pad> on another controller to join",
    };
    EXPECT(LinesOf(TextFor(w).markup) == lines);
    w.startJoins = false;
    EXPECT(!Has(TextFor(w).markup, L"options"));
    EXPECT(!Has(TextFor(w).markup, L"leave"));
}

/// A note replaces the keys; with the window shut it shows alone under the name.
void Notes()
{
    Window w{.seat = 0, .name = L"CLOUD", .open = false, .note = Note::Waits, .about = 1};
    const std::vector<std::wstring> lines{
        L"PLAYER 1    <color=yellow>CLOUD</color>",
        L"Player 2 joins once a party member is free",
    };
    EXPECT(LinesOf(TextFor(w).markup) == lines);
    w.open = true;
    w.note = Note::PlayerOneStays;
    EXPECT(LinesOf(TextFor(w).markup).back() == L"Player 1 cannot be removed from the game");
    EXPECT(!Has(TextFor(w).markup, L"Done"));
    EXPECT(NoteText(Note::NoMember, 2) == L"No party member is free for a new player");
}

}  // namespace

int main()
{
    OneLayerOfKeys();
    TheB();
    TheViewMoves();
    PadsAndSeats();
    FurtherPlayersWindow();
    PlayerOnesWindow();
    Notes();
    if (g_failures != 0)
    {
        std::printf("player_window: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("player_window: all passed\n");
    return 0;
}
