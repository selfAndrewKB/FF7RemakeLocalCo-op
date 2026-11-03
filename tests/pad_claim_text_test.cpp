// Host-side tests for the claim screen's texts (players/pad_claim_text.h): a screen no pad took names its player only,
// a taken one names the character and its keys, one key a line, in the game's markup and in words, and the arrows only
// where the player can step.

#include "players/pad_claim_text.h"

#include <cstdio>
#include <string>
#include <vector>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "pad_claim_text: all passed\n" : "pad_claim_text: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
}  // namespace lss::test

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++lss::test::g_failures;                                         \
        }                                                                    \
    } while (false)

namespace {

using namespace lss::players::pad_claim_text;
namespace rules = lss::players::pad_claim_rules;

/// Player 2 can step (a third member is free), player 3 cannot; player 1 never does.
const Seats kSeats{{L"CLOUD", L"BARRET", L"TIFA"}, {false, true, false}};

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

/// Before anyone takes a screen, each names its player (no character) and its own button (A, X, Y), and how to cancel.
void UntakenScreens()
{
    const rules::Claim c = rules::Open(3, {0, 1, 2}, rules::kIdentity);
    const ScreenText   first  = TextFor(c, 0, kSeats);
    const ScreenText   second = TextFor(c, 1, kSeats);
    const ScreenText   third  = TextFor(c, 2, kSeats);
    EXPECT(Has(first.markup, L"<color=yellow>PLAYER 1</color>"));
    EXPECT(!Has(first.markup, L"CLOUD"));
    EXPECT(Has(first.markup, L"Press <button=cross,pad> to play this screen"));
    EXPECT(Has(second.markup, L"<button=square,pad>"));
    EXPECT(Has(second.markup, L"PLAYER 2") && !Has(second.markup, L"BARRET"));
    EXPECT(Has(third.markup, L"<button=triangle,pad>"));
    EXPECT(Has(third.markup, L"<color=yellow>PLAYER 3</color>"));
    EXPECT(Has(first.markup, L"Hold <button=touch,pad> to cancel"));
    EXPECT(Has(second.markup, L"Hold <button=circle,pad> to leave the game"));  // a joined player can leave from here
    EXPECT(!Has(first.markup, L"leave"));
    EXPECT(first.plain[0] == L"PLAYER 1");
    EXPECT(first.plain[1] == L"Press A to play this screen");
    EXPECT(second.plain[1] == L"Press X to play this screen");
    EXPECT(Has(first.plain[2], L"hold View to cancel"));
}

/// The screens follow the players, wherever each is: player 3 on the first screen takes A.
void TheButtonIsTheScreens()
{
    const rules::Claim c = rules::Open(3, {2, 0, 1}, rules::kIdentity);
    EXPECT(TextFor(c, 0, kSeats).plain[0] == L"PLAYER 3");
    EXPECT(TextFor(c, 0, kSeats).plain[1] == L"Press A to play this screen");
    EXPECT(TextFor(c, 1, kSeats).plain[0] == L"PLAYER 1");
}

/// Lines are joined as the game's strings join them: CR LF, three lines, none trailing.
void LinesJoinedLikeTheGame()
{
    const rules::Claim c    = rules::Open(2, {0, 1, -1}, rules::kIdentity);
    const std::wstring text = TextFor(c, 0, kSeats).markup;
    std::size_t        breaks = 0;
    for (std::size_t at = text.find(L"\r\n"); at != std::wstring::npos; at = text.find(L"\r\n", at + 2))
    {
        ++breaks;
    }
    EXPECT(breaks == 2);
    EXPECT(!text.ends_with(L"\r\n"));
    EXPECT(text.find(L'\n') == text.find(L"\r\n") + 1);
}

/// A taken screen names the character and offers select and back; the arrows only where that player can step.
void TakenScreens()
{
    rules::Claim c = rules::Open(3, {0, 1, 2}, rules::kIdentity);
    rules::Press(c, 1, rules::kX);  // pad 2 takes the second screen (player 2, BARRET)
    rules::Press(c, 0, rules::kA);  // pad 1 takes the first (player 1, CLOUD)
    rules::Press(c, 2, rules::kY);  // pad 3 takes the third (player 3, TIFA)
    const ScreenText barret = TextFor(c, 1, kSeats);
    const ScreenText cloud  = TextFor(c, 0, kSeats);
    const ScreenText tifa   = TextFor(c, 2, kSeats);
    EXPECT(Has(barret.markup, L"PLAYER 2    <button=left,pad>  <color=yellow>BARRET</color>  <button=right,pad>"));
    EXPECT(barret.plain[0] == L"PLAYER 2    <   BARRET   >");
    EXPECT(barret.plain[1] == L"Left/Right: character");
    EXPECT(barret.plain[2] == L"A: select");
    EXPECT(barret.plain[3] == L"B: back");
    EXPECT(barret.plain[4] == L"Hold B to leave the game");
    EXPECT(barret.plain[5] == L"Changing pads    (hold View to cancel)");
    // Player 1's character is the game's leader: named, no arrows.
    EXPECT(Has(cloud.markup, L"PLAYER 1    <color=yellow>CLOUD</color>"));
    EXPECT(!Has(cloud.markup, L"<button=left,pad>"));
    EXPECT(cloud.plain[0] == L"PLAYER 1    CLOUD");
    EXPECT(cloud.plain[1] == L"A: select");
    EXPECT(cloud.plain[2] == L"B: back");
    EXPECT(Has(cloud.plain[3], L"hold View to cancel") && cloud.plain[4].empty());
    // Player 3 has no other member free.
    EXPECT(!Has(tifa.markup, L"<button=left,pad>") && Has(tifa.markup, L"<color=yellow>TIFA</color>"));
}

/// A kept screen offers B to change, and player 3 the leave.
void KeptScreen()
{
    rules::Claim c = rules::Open(3, {0, 1, 2}, rules::kIdentity);
    rules::Press(c, 2, rules::kY);
    rules::Press(c, 2, rules::kA);
    const ScreenText tifa = TextFor(c, 2, kSeats);
    EXPECT(Has(tifa.markup, L"PLAYER 3    <color=yellow>TIFA</color>"));
    EXPECT(!Has(tifa.markup, L"Kept"));
    EXPECT(tifa.plain[1] == L"B: change");
    EXPECT(tifa.plain[2] == L"Hold B to leave the game");
}

/// One key a line, in the game's window: player 2 choosing, player 2 kept, player 1 choosing (no leave).
void OneKeyALine()
{
    rules::Claim c = rules::Open(2, {0, 1, -1}, rules::kIdentity);
    rules::Press(c, 1, rules::kX);
    rules::Press(c, 0, rules::kA);
    const std::vector<std::wstring> choosing{
        L"PLAYER 2    <button=left,pad>  <color=yellow>BARRET</color>  <button=right,pad>",
        L"<button=cross,pad> Select",
        L"<button=circle,pad> Back",
        L"Hold <button=circle,pad> to leave the game",
        L"Hold <button=touch,pad> to cancel",
    };
    EXPECT(LinesOf(TextFor(c, 1, kSeats).markup) == choosing);
    const std::vector<std::wstring> playerOne{
        L"PLAYER 1    <color=yellow>CLOUD</color>",
        L"<button=cross,pad> Select",
        L"<button=circle,pad> Back",
        L"Hold <button=touch,pad> to cancel",
    };
    EXPECT(LinesOf(TextFor(c, 0, kSeats).markup) == playerOne);
    rules::Press(c, 1, rules::kA);
    const std::vector<std::wstring> kept{
        L"PLAYER 2    <color=yellow>BARRET</color>",
        L"<button=circle,pad> Change",
        L"Hold <button=circle,pad> to leave the game",
        L"Hold <button=touch,pad> to cancel",
    };
    EXPECT(LinesOf(TextFor(c, 1, kSeats).markup) == kept);
}

/// A copy's screen is open: it offers a join with its own button, says a join under way, and never names player 1.
void OpenScreens()
{
    rules::Claim c = rules::Open(3, {-1, 0, -1}, rules::kIdentity, 0b1);
    const ScreenText left = TextFor(c, 0, kSeats);
    EXPECT(Has(left.markup, L"<color=yellow>NEW PLAYER</color>"));
    EXPECT(Has(left.markup, L"Press <button=cross,pad> to join here"));
    EXPECT(!Has(left.markup, L"CLOUD") && !Has(left.markup, L"PLAYER 1"));
    EXPECT(TextFor(c, 2, kSeats).plain[1] == L"Press Y to join here");
    rules::Press(c, 1, rules::kA);
    EXPECT(TextFor(c, 0, kSeats).plain[1] == L"Joining...");
}

/// A note replaces the keys' lines for a while: a refusal on an open screen, player 1 staying in.
void Notes()
{
    rules::Claim c = rules::Open(3, {-1, 0, -1}, rules::kIdentity, 0b1);
    EXPECT(TextFor(c, 0, kSeats, Note::NoMember).plain[1] == L"No party member is free to play");
    rules::Press(c, 0, rules::kX);  // player 1's pad takes its screen: Select and Back give way to the note
    const ScreenText stays = TextFor(c, 1, kSeats, Note::PlayerOneStays);
    EXPECT(stays.plain[1] == L"Player 1 cannot be removed from the game");
    EXPECT(Has(stays.plain[2], L"hold View to cancel") && stays.plain[3].empty());
    EXPECT(!Has(stays.markup, L"Select"));
    EXPECT(Has(TextFor(c, 0, kSeats, Note::Waits).markup, L"Joins once a party member is free"));
}

/// A further player can leave by holding B; player 1's screen never offers it. In one window with no screen open,
/// player 1's screen tells another pad to press Start.
void LeaveAndStartHints()
{
    rules::Claim c = rules::Open(2, {0, 1, -1}, rules::kIdentity, 0b11);
    rules::Press(c, 1, rules::kX);
    rules::Press(c, 0, rules::kA);
    EXPECT(Has(TextFor(c, 1, kSeats).markup, L"Hold <button=circle,pad> to leave the game"));
    EXPECT(!Has(TextFor(c, 0, kSeats).markup, L"leave"));
    Seats one = kSeats;
    one.startJoins = true;
    EXPECT(LinesOf(TextFor(c, 0, one).markup).back() == L"Press <button=options,pad> on another pad to join");
    EXPECT(!Has(TextFor(c, 1, one).markup, L"<button=options,pad>"));  // player 1's screen only
}

/// Outside the screens, there is no text.
void EmptyScreens()
{
    const rules::Claim c = rules::Open(2, {0, 1, 2}, rules::kIdentity);
    EXPECT(TextFor(c, 2, kSeats).markup.empty());  // beyond the two screens
    EXPECT(TextFor(c, -1, kSeats).markup.empty());
    EXPECT(TextFor(c, 3, kSeats).plain[0].empty());
}

}  // namespace

int main()
{
    UntakenScreens();
    TheButtonIsTheScreens();
    LinesJoinedLikeTheGame();
    TakenScreens();
    KeptScreen();
    OneKeyALine();
    OpenScreens();
    Notes();
    LeaveAndStartHints();
    EmptyScreens();
    return lss::test::Report();
}
