// Host-side tests for the player windows' pure rules and texts (players/player_window_rules.h,
// players/player_window_text.h): one layer of keys in a player's own window, the view moving one screen at a time, the
// player numbers traded with each person keeping their screen, a spare pad taking back a player whose pad is gone, a
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

/// One layer: up/down step a further player's character, left/right trade anyone's number, LB/RB move anyone's view,
/// A and Start close; B is not a press.
void OneLayerOfKeys()
{
    EXPECT(OnPress(1, kDpadUp) == (Action{Act::Step, -1}));
    EXPECT(OnPress(2, kDpadDown) == (Action{Act::Step, +1}));
    EXPECT(OnPress(0, kDpadDown).act == Act::Nothing);  // player 1's character is the game's leader
    EXPECT(OnPress(0, kDpadRight) == (Action{Act::Trade, +1}));
    EXPECT(OnPress(2, kDpadLeft) == (Action{Act::Trade, -1}));
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

/// A number trade goes to the nearest player that way, stopping at the ends; the two screens trade numbers, so each
/// person keeps their screen whichever came first, the move or the trade; the two pads trade slots.
void NumbersTrade()
{
    EXPECT(TradePartner(0, +1, 0b011) == 1);
    EXPECT(TradePartner(1, -1, 0b011) == 0);
    EXPECT(TradePartner(0, -1, 0b111) == -1);  // the lowest number
    EXPECT(TradePartner(2, +1, 0b111) == -1);  // the highest
    EXPECT(TradePartner(1, +1, 0b011) == -1);  // player 3 not in
    EXPECT(TradePartner(0, +1, 0b101) == 2);   // player 2 not in: the next player that way
    EXPECT(TradePartner(2, -1, 0b101) == 0);
    EXPECT(TradePartner(0, +1, 0b001) == -1);  // alone

    // Player 2 moved their view onto the left screen, then trades with player 1: the left screen shows player 1 (them).
    const auto moved = Moved({0, 1, -1}, 2, 1, -1);
    EXPECT(moved && (*moved == std::array<int, kSeats>{1, 0, -1}));
    EXPECT((Traded(*moved, 1, 0) == std::array<int, kSeats>{0, 1, -1}));
    // Three screens with a copy's: only the two traded numbers change places.
    EXPECT((Traded({2, -1, 0}, 0, 2) == std::array<int, kSeats>{0, -1, 2}));
    EXPECT((Traded({0, 1, 2}, 1, 2) == std::array<int, kSeats>{0, 2, 1}));
    // The pads: player 2's pad onto player 1's slot, player 1's onto player 2's.
    const pad::PadTable pads{2, 0, 1, 3};
    EXPECT((WithPadOn(pads, pads[1], 0) == pad::PadTable{0, 2, 1, 3}));
    EXPECT((Traded({0, 1, -1}, 1, 1) == std::array<int, kSeats>{0, 1, -1}));  // with itself: nothing
    EXPECT(TradePartner(1, 0, 0b111) == -1);
    EXPECT(!Plays(0b111, 3) && !Plays(0b111, -1) && Plays(0, 0));

    // Every person keeps their pad and their screen through any trade: three players, each pad's screen before and after.
    const pad::PadTable         before{1, 3, 0, 2};
    const std::array<int, kSeats> seatOn{2, 0, 1};
    for (int seat = 0; seat < kSeats; ++seat)
    {
        for (const int dir : {-1, +1})
        {
            const int other = TradePartner(seat, dir, 0b111);
            if (other < 0)
            {
                continue;
            }
            const pad::PadTable           after    = WithPadOn(before, before[static_cast<std::size_t>(seat)], other);
            const std::array<int, kSeats> screens  = Traded(seatOn, seat, other);
            for (int screen = 0; screen < kSeats; ++screen)
            {
                const auto shown = [](const pad::PadTable& t, const std::array<int, kSeats>& on, int s) {
                    return t[static_cast<std::size_t>(on[static_cast<std::size_t>(s)])];  // the pad whose player a screen shows
                };
                EXPECT(shown(after, screens, screen) == shown(before, seatOn, screen));
            }
        }
    }
}

/// A spare pad's Start takes back the lowest player whose pad is gone; none gone, or gone pads nobody plays: -1. (A slot
/// no pad ever answered, a keyboard player 1's, is never in the gone set: players/pad_presence.)
void PadsGone()
{
    EXPECT(SeatWithPadGone({0, 1, 2, 3}, 0b011, 0b0010) == 1);
    EXPECT(SeatWithPadGone({0, 1, 2, 3}, 0b111, 0b0101) == 0);  // player 1's and player 3's: the lowest
    EXPECT(SeatWithPadGone({0, 1, 2, 3}, 0b011, 0b1100) == -1); // gone pads no player holds
    EXPECT(SeatWithPadGone({3, 1, 2, 0}, 0b001, 0b1000) == 0);  // player 1 on pad 4
    EXPECT(SeatWithPadGone({0, 1, 2, 3}, 0b111, 0) == -1);
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

/// Player 2's open window: the number between left and right, the character between up and down, then move, done,
/// leave; one key a line.
void FurtherPlayersWindow()
{
    Window w{.seat = 1, .name = L"BARRET", .open = true, .canStep = true, .canTrade = true, .canMove = true};
    const ScreenText t = TextFor(w);
    const std::vector<std::wstring> lines{
        L"<button=left,pad> PLAYER 2 <button=right,pad>    <button=up,pad>  <color=yellow>BARRET</color>  <button=down,pad>",
        L"<button=L1,pad> <button=R1,pad> Move my view",
        L"<button=cross,pad> Done",
        L"Hold <button=circle,pad> to leave the game",
    };
    EXPECT(LinesOf(t.markup) == lines);
    EXPECT(t.plain[0] == L"<  PLAYER 2  >    ^   BARRET   v");
    EXPECT(t.plain[1] == L"Up/Down: character");
    EXPECT(t.plain[2] == L"Left/Right: player number");
    EXPECT(t.plain[3] == L"LB/RB: move my view");
    EXPECT(t.plain[4] == L"A: done");
    EXPECT(t.plain[5] == L"Hold B to leave the game");
    EXPECT(t.plain.size() == 6);  // the longest window fills the fallback's lines exactly
}

/// Player 1's: no character arrows, no leave; the number's arrows while another player is in; the Start hint while a
/// seat is free; no move line with one screen.
void PlayerOnesWindow()
{
    Window w{.seat = 0, .name = L"CLOUD", .open = true, .canMove = false, .startJoins = true};
    const std::vector<std::wstring> lines{
        L"PLAYER 1    <color=yellow>CLOUD</color>",
        L"<button=cross,pad> Done",
        L"Press <button=options,pad> on another controller to join",
    };
    EXPECT(LinesOf(TextFor(w).markup) == lines);
    w.canTrade = true;
    EXPECT(LinesOf(TextFor(w).markup).front() == L"<button=left,pad> PLAYER 1 <button=right,pad>    <color=yellow>CLOUD</color>");
    EXPECT(TextFor(w).plain[1] == L"Left/Right: player number");
    w.open = false;  // a shut window shows no arrows
    EXPECT(TextFor(w).plain[0] == L"PLAYER 1    CLOUD");
    w.open       = true;
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
        L"Player 2 has joined and is waiting for a free party member",
    };
    EXPECT(LinesOf(TextFor(w).markup) == lines);
    w.open = true;
    w.note = Note::PlayerOneStays;
    EXPECT(LinesOf(TextFor(w).markup).back() == L"Player 1 cannot be removed from the game");
    EXPECT(!Has(TextFor(w).markup, L"Done"));
    EXPECT(NoteText(Note::NoMember, 2) == L"No available party members");
    EXPECT(NoteText(Note::Failed, 2) == L"Player 3's join failed. See UE4SS.log.");
}

}  // namespace

int main()
{
    OneLayerOfKeys();
    TheB();
    TheViewMoves();
    NumbersTrade();
    PadsGone();
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
