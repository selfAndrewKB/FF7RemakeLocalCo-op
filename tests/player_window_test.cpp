// Host-side tests for the player windows' pure rules and texts (players/player_window_rules.h,
// players/player_window_text.h): one layer of keys in a player's own window, the view moving one screen at a time, the
// player numbers traded with each person keeping their screen, a spare pad taking back a player whose pad is gone, a
// pad that presses on the title screen taking player 1, and each window's lines.

#include "players/player_window_rules.h"
#include "players/player_window_text.h"

#include <cstdio>
#include <string>
#include <utility>
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
    const pad::PadTable pads = pad::FromXInput({2, 0, 1, 3});
    EXPECT((WithPadsTraded(pads, 0, 1) == pad::FromXInput({0, 2, 1, 3})));
    EXPECT((Traded({0, 1, -1}, 1, 1) == std::array<int, kSeats>{0, 1, -1}));  // with itself: nothing
    EXPECT(TradePartner(1, 0, 0b111) == -1);
    EXPECT(!Plays(0b111, 3) && !Plays(0b111, -1) && Plays(0, 0));

    // Every person keeps their pad and their screen through any trade: three players, each pad's screen before and
    // after; once with XInput pads only, once with player 2 on a PlayStation pad.
    const std::array<int, kSeats> seatOn{2, 0, 1};
    const auto shown = [](const pad::PadTable& t, const std::array<int, kSeats>& on, int s) {
        return pad::PadOn(t, on[static_cast<std::size_t>(s)]);  // the pad whose player a screen shows
    };
    for (const pad::PadTable& before : {pad::FromXInput({1, 3, 0, 2}), WithPadOn(pad::FromXInput({1, 3, 0, 2}), 6, 1)})
    {
        for (int seat = 0; seat < kSeats; ++seat)
        {
            for (const int dir : {-1, +1})
            {
                const int other = TradePartner(seat, dir, 0b111);
                if (other < 0)
                {
                    continue;
                }
                const pad::PadTable           after   = WithPadsTraded(before, seat, other);
                const std::array<int, kSeats> screens = Traded(seatOn, seat, other);
                EXPECT(pad::IsValid(after));
                for (int screen = 0; screen < kSeats; ++screen)
                {
                    EXPECT(shown(after, screens, screen) == shown(before, seatOn, screen));
                }
            }
        }
    }
}

/// A spare pad's Start takes back the lowest player whose pad is gone; none gone, or gone pads nobody plays: -1. (A slot
/// no pad ever answered, a keyboard player 1's, is never in the gone set: players/pad_presence.)
void PadsGone()
{
    const pad::PadTable id = pad::kIdentity;
    EXPECT(SeatWithPadGone(id, 0b011, 0b0010) == 1);
    EXPECT(SeatWithPadGone(id, 0b111, 0b0101) == 0);   // player 1's and player 3's: the lowest
    EXPECT(SeatWithPadGone(id, 0b011, 0b1100) == -1);  // gone pads no player holds
    EXPECT(SeatWithPadGone(pad::FromXInput({3, 1, 2, 0}), 0b001, 0b1000) == 0);  // player 1 on pad 4
    EXPECT(SeatWithPadGone(id, 0b111, 0) == -1);
    // Player 2 on the first PlayStation pad: its going counts, pad 2's (muted beside it) does not.
    const pad::PadTable ps = WithPadOn(id, 4, 1);
    EXPECT(SeatWithPadGone(ps, 0b011, 0b0001'0000) == 1);
    EXPECT(SeatWithPadGone(ps, 0b011, 0b0000'0010) == -1);
}

/// The title screen's press takes player 1's slot; a join's pad takes the new player's; a pad's seat is its slot's,
/// unless it is muted there.
void PadsAndSeats()
{
    const pad::PadTable     id   = pad::kIdentity;
    constexpr std::uint32_t kAll = 0xFF;
    EXPECT((WithPlayerOne(id, 2) == pad::FromXInput({2, 1, 0, 3})));
    EXPECT((WithPlayerOne(id, 0) == pad::kIdentity));  // already player 1's
    EXPECT((WithPadOn(id, 3, 1) == pad::FromXInput({0, 3, 2, 1})));
    EXPECT(pad::IsValid(WithPadOn(pad::FromXInput({2, 0, 3, 1}), 1, 2)));
    EXPECT(SeatOf(id, 0b1, kAll, 0) == 0);
    EXPECT(SeatOf(id, 0b1, kAll, 1) == -1);  // player 2 not in: the pad plays no one
    EXPECT(SeatOf(id, 0b11, kAll, 1) == 1);
    EXPECT(SeatOf(id, 0b111, kAll, 3) == -1);  // the spare slot
    EXPECT(SeatOf(pad::FromXInput({1, 0, 2, 3}), 0b1, kAll, 1) == 0);
    // An Xbox pad and a DualSense on player 1's slot: the Xbox pad plays, the DualSense is muted and its Start joins;
    // with the Xbox pad gone, the DualSense plays player 1.
    EXPECT(SeatOf(id, 0b1, 0b0001'0001, 4) == -1 && SeatOf(id, 0b1, 0b0001'0001, 0) == 0);
    EXPECT(SeatOf(id, 0b1, 0b0001'0000, 4) == 0);
    // The DualSense pressed on the title: it plays player 1, the Xbox pad is muted.
    const pad::PadTable title = WithPlayerOne(id, 4);
    EXPECT(SeatOf(title, 0b1, 0b0001'0001, 4) == 0 && SeatOf(title, 0b1, 0b0001'0001, 0) == -1);
    // The muted DualSense joins as player 2: on record 1, live there; player 1 stays the Xbox pad's.
    const pad::PadTable joined = WithPadOn(id, 4, 1);
    EXPECT(SeatOf(joined, 0b11, 0b0001'0001, 4) == 1 && SeatOf(joined, 0b11, 0b0001'0001, 0) == 0);
}

/// A key on the title makes the keyboard player 1: every pad on player 1's slot plays no one (its Start joins); a
/// joined pad plays its own player; the keyboard is never gone; a pad's title press takes player 1 back. In a window
/// the keyboard trades numbers as a pad does: the pad takes the keyboard's number and the keyboard the pad's.
void KeyboardPlayerOne()
{
    const pad::PadTable kb = pad::WithKeyboardOn(pad::kIdentity, 0);
    EXPECT(pad::IsValid(kb) && pad::KeyboardSlot(kb) == 0 && pad::PadOn(kb, 0) == pad::kNoPad);
    EXPECT(SeatOf(kb, 0b1, 0b0001'0001, 0) == -1 && SeatOf(kb, 0b1, 0b0001'0001, 4) == -1);
    EXPECT(SeatOf(kb, 0b1, 0b0000'0000, 0) == -1);  // absent: not muted, but no player's either
    const pad::PadTable joined = WithPadOn(kb, 0, 1);  // pad 1's Start adds player 2
    EXPECT(pad::KeyboardSlot(joined) == 0 && SeatOf(joined, 0b11, 0b1, 0) == 1);
    EXPECT(SeatWithPadGone(joined, 0b11, 0xFF) == 1);  // only player 2's pad can be gone
    EXPECT(pad::KeyboardSlot(WithPlayerOne(kb, 4)) == -1 && pad::PadOn(WithPlayerOne(kb, 4), 0) == 4);

    // Player 2's pad trades with the keyboard's player 1: pad 1 plays player 1, the keyboard player 2; and back.
    EXPECT(CanTradePads(joined, 1, 0) && CanTradePads(joined, 0, 1));
    const pad::PadTable traded = WithPadsTraded(joined, 1, 0);
    EXPECT(pad::IsValid(traded) && pad::PadOn(traded, 0) == 0 && pad::KeyboardSlot(traded) == 1);
    EXPECT(SeatOf(traded, 0b11, 0b1, 0) == 0 && pad::PadOn(traded, 1) == pad::kNoPad);
    EXPECT(WithPadsTraded(traded, 0, 1) == joined);
    // A PlayStation pad on player 3 trades with the keyboard on player 2.
    const pad::PadTable three    = WithPadOn(traded, 6, 2);
    const pad::PadTable psTraded = WithPadsTraded(three, 2, 1);
    EXPECT(pad::IsValid(psTraded) && pad::PadOn(psTraded, 1) == 6 && pad::KeyboardSlot(psTraded) == 2);
    EXPECT(!CanTradePads(joined, 1, 1) && !CanTradePads(joined, 0, 3));

    // The keyboard plays one player slot at most, never the spare.
    pad::PadTable two = pad::kIdentity;
    two.live[0]       = pad::kKeyboard;
    two.live[1]       = pad::kKeyboard;
    EXPECT(!pad::IsValid(two));
    pad::PadTable spare = pad::kIdentity;
    spare.live[3]       = pad::kKeyboard;
    EXPECT(!pad::IsValid(spare) && pad::WithKeyboardOn(pad::kIdentity, 3) == pad::kIdentity);
    // Moving the keyboard frees the slot it played; Unkeyboarded puts it back beside player 1.
    const pad::PadTable moved = pad::WithKeyboardOn(kb, 2);
    EXPECT(pad::IsValid(moved) && pad::KeyboardSlot(moved) == 2 && pad::PadOn(moved, 0) == 0);
    EXPECT(pad::KeyboardSlot(pad::Unkeyboarded(moved)) == -1 && pad::Unkeyboarded(moved) == pad::kIdentity);

    // The claim: the device turning to the keyboard on the title, or the title coming up with the keyboard last used.
    EXPECT(KeyboardClaims(true, false, true, true));    // a key on the title
    EXPECT(KeyboardClaims(true, true, true, false));    // the title came up after the keyboard skipped the logos
    EXPECT(!KeyboardClaims(true, true, true, true));    // already the keyboard's: no second claim
    EXPECT(!KeyboardClaims(true, false, false, false)); // a key in the field
    EXPECT(!KeyboardClaims(false, true, true, true));   // a pad's press on the title (its own TitlePress)
}

/// The keyboard for any player: Tab held joins it on a free slot (its pads there muted, their Start joins), its device
/// slot is the one it plays, and any two of three players trade numbers whatever plays them (a pad of either reader or
/// the keyboard), each person keeping their device and screen.
void KeyboardAnyPlayer()
{
    const pad::PadTable id = pad::kIdentity;
    EXPECT(DeviceSlot(id, pad::kKeyboardPad) == -1);  // beside player 1: no slot of its own
    const pad::PadTable kb2 = WithDeviceOn(id, pad::kKeyboardPad, 1);
    EXPECT(pad::IsValid(kb2) && DeviceSlot(kb2, pad::kKeyboardPad) == 1 && DeviceSlot(kb2, 1) == -1);
    EXPECT(SeatOf(kb2, 0b11, 0b11, pad::kKeyboardPad) == 1);
    EXPECT(SeatOf(kb2, 0b01, 0b11, pad::kKeyboardPad) == -1);  // not in yet
    EXPECT(SeatOf(kb2, 0b11, 0b11, 1) == -1);                  // pad 2 muted under it: its Start joins
    EXPECT(WithDeviceOn(id, 4, 1) == WithPadOn(id, 4, 1) && DeviceSlot(WithPadOn(id, 4, 1), 4) == 1);
    EXPECT(DeviceSlot(WithPadOn(id, 4, 1), 1) == -1);  // pad 2 beside the live PlayStation pad

    // Player 1 on pad 1, player 2 on the keyboard, player 3 on the first PlayStation pad: every trade keeps each person's
    // device on their screen, and the keyboard always plays exactly one slot.
    const pad::PadTable           three  = WithPadOn(kb2, 4, 2);
    const std::array<int, kSeats> seatOn{0, 1, 2};
    const auto device = [](const pad::PadTable& t, int seat) {
        return pad::PadOn(t, seat) == pad::kNoPad ? pad::kKeyboardPad : pad::PadOn(t, seat);
    };
    for (const auto& [a, b] : {std::pair{0, 1}, std::pair{1, 2}, std::pair{0, 2}})
    {
        EXPECT(CanTradePads(three, a, b));
        const pad::PadTable           after   = WithPadsTraded(three, a, b);
        const std::array<int, kSeats> screens = Traded(seatOn, a, b);
        EXPECT(pad::IsValid(after) && pad::KeyboardSlot(after) >= 0);
        for (int screen = 0; screen < kSeats; ++screen)
        {
            EXPECT(device(after, screens[static_cast<std::size_t>(screen)]) ==
                   device(three, seatOn[static_cast<std::size_t>(screen)]));
        }
        EXPECT(WithPadsTraded(after, a, b) == three);  // and back
    }
}

/// The keyboard player's window names its keys; player 1's says Tab joins while the keyboard plays no one.
void KeyboardWindow()
{
    Window w{.seat = 2, .name = L"TIFA", .open = true, .canStep = true, .canTrade = true, .canMove = true, .keyboard = true};
    const std::vector<std::wstring> lines{
        L"<  PLAYER 3  >    ^   <color=yellow>TIFA</color>   v",
        L"Up/Down: character",
        L"Left/Right: player number",
        L"Q/E: Change View",
        L"Enter: done",
        L"Hold Backspace to leave the game",
    };
    EXPECT(LinesOf(TextFor(w).markup) == lines);
    EXPECT(TextFor(w).plain[5] == L"Hold Backspace to leave the game");
    EXPECT(!Has(TextFor(w).markup, L"<button="));

    Window one{.seat = 0, .name = L"CLOUD", .open = true, .startJoins = true, .keyboardJoins = true};
    EXPECT(LinesOf(TextFor(one).markup).back() == L"Press <button=options,pad> on another controller or hold Tab on the keyboard to join");
    EXPECT(TextFor(one).plain[2] == L"Press Start on another controller or hold Tab to join");
    one.keyboard = true;  // the keyboard is player 1: a controller's Start joins
    EXPECT(LinesOf(TextFor(one).markup).back() == L"Press Start on a controller to join");
}

/// Player 2's open window: the number between left and right, the character between up and down, then move, done,
/// leave; one key a line.
void FurtherPlayersWindow()
{
    Window w{.seat = 1, .name = L"BARRET", .open = true, .canStep = true, .canTrade = true, .canMove = true};
    const ScreenText t = TextFor(w);
    const std::vector<std::wstring> lines{
        L"<button=left,pad> PLAYER 2 <button=right,pad>    <button=up,pad>  <color=yellow>BARRET</color>  <button=down,pad>",
        L"<button=L1,pad> <button=R1,pad> Change View",
        L"<button=cross,pad> Done",
        L"Hold <button=circle,pad> to leave the game",
    };
    EXPECT(LinesOf(t.markup) == lines);
    EXPECT(t.plain[0] == L"<  PLAYER 2  >    ^   BARRET   v");
    EXPECT(t.plain[1] == L"Up/Down: character");
    EXPECT(t.plain[2] == L"Left/Right: player number");
    EXPECT(t.plain[3] == L"LB/RB: Change View");
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
    // A member in the party but with the game's AI: named; without a name, the plain wait.
    EXPECT(NoteText(Note::WaitsOnAi, 1, L"TIFA") == L"Player 2 has joined and is waiting for TIFA to be released from AI control");
    EXPECT(NoteText(Note::WaitsOnAi, 1) == NoteText(Note::Waits, 1));
    Window named{.seat = 0, .name = L"CLOUD", .note = Note::WaitsOnAi, .about = 2, .aboutName = L"BARRET"};
    EXPECT(LinesOf(TextFor(named).markup).back() == L"Player 3 has joined and is waiting for BARRET to be released from AI control");
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
    KeyboardPlayerOne();
    KeyboardAnyPlayer();
    KeyboardWindow();
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
