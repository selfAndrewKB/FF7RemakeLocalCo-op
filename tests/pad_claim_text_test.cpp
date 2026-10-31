// Host-side tests for the claim screen's texts (players/pad_claim_text.h): each screen's state says its character and
// its keys, in the game's markup and in words, and only the steps the rules allow are offered.

#include "players/pad_claim_text.h"

#include <cstdio>
#include <string>

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

const std::array<std::wstring, rules::kScreens> kNames{L"CLOUD", L"BARRET", L"TIFA"};

bool Has(const std::wstring& text, const std::wstring& part)
{
    return text.find(part) != std::wstring::npos;
}

/// Before anyone takes a screen, each says its character and its own button (A, X, Y), and how to cancel.
void UntakenScreens()
{
    const rules::Claim c = rules::Open(3, {0, 1, 2}, rules::kIdentity, true);
    const ScreenText   first  = TextFor(c, 0, kNames);
    const ScreenText   second = TextFor(c, 1, kNames);
    const ScreenText   third  = TextFor(c, 2, kNames);
    EXPECT(Has(first.markup, L"<color=yellow>CLOUD</color>"));
    EXPECT(Has(first.markup, L"Press <button=cross,pad> to play this screen"));
    EXPECT(Has(second.markup, L"<button=square,pad>"));
    EXPECT(Has(third.markup, L"<button=triangle,pad>"));
    EXPECT(Has(third.markup, L"<color=yellow>TIFA</color>"));
    EXPECT(Has(first.markup, L"Hold <button=touch,pad> to cancel"));
    EXPECT(first.plain[0] == L"CLOUD");
    EXPECT(first.plain[1] == L"Press A to play this screen");
    EXPECT(second.plain[1] == L"Press X to play this screen");
    EXPECT(Has(first.plain[2], L"hold View to cancel"));
}

/// Lines are joined as the game's strings join them: CR LF, three lines, none trailing.
void LinesJoinedLikeTheGame()
{
    const rules::Claim c    = rules::Open(2, {0, 1, -1}, rules::kIdentity, true);
    const std::wstring text = TextFor(c, 0, kNames).markup;
    std::size_t        breaks = 0;
    for (std::size_t at = text.find(L"\r\n"); at != std::wstring::npos; at = text.find(L"\r\n", at + 2))
    {
        ++breaks;
    }
    EXPECT(breaks == 2);
    EXPECT(!text.ends_with(L"\r\n"));
    EXPECT(text.find(L'\n') == text.find(L"\r\n") + 1);
}

/// A taken screen offers keep and back; the character arrows only where a step can move it.
void TakenScreens()
{
    rules::Claim c = rules::Open(3, {0, 1, 2}, rules::kIdentity, true);
    rules::Press(c, 1, rules::kX);  // pad 2 takes the second screen (BARRET)
    rules::Press(c, 0, rules::kA);  // pad 1 takes the first (CLOUD)
    const ScreenText barret = TextFor(c, 1, kNames);
    const ScreenText cloud  = TextFor(c, 0, kNames);
    EXPECT(Has(barret.markup, L"<button=left,pad>  <color=yellow>BARRET</color>  <button=right,pad>"));
    EXPECT(Has(barret.markup, L"<button=cross,pad> Keep    <button=circle,pad> Back"));
    EXPECT(barret.plain[0] == L"<   BARRET   >");
    // Player 1 cannot step while its view stays on the first screen.
    EXPECT(!Has(cloud.markup, L"<button=left,pad>"));
    EXPECT(cloud.plain[0] == L"CLOUD");
    EXPECT(cloud.plain[1] == L"A: keep    B: back");
}

/// Two screens: only one player besides player 1, so no step is offered.
void NoStepWithOneOther()
{
    rules::Claim c = rules::Open(2, {0, 1, -1}, rules::kIdentity, true);
    rules::Press(c, 1, rules::kX);
    EXPECT(!Has(TextFor(c, 1, kNames).markup, L"<button=left,pad>"));
    // With player 1 free to move, the two can trade.
    rules::Claim free = rules::Open(2, {0, 1, -1}, rules::kIdentity, false);
    rules::Press(free, 1, rules::kX);
    EXPECT(Has(TextFor(free, 1, kNames).markup, L"<button=left,pad>"));
}

/// A kept screen says so and offers B to change.
void KeptScreen()
{
    rules::Claim c = rules::Open(3, {0, 1, 2}, rules::kIdentity, true);
    rules::Press(c, 2, rules::kY);
    rules::Press(c, 2, rules::kA);
    const ScreenText tifa = TextFor(c, 2, kNames);
    EXPECT(Has(tifa.markup, L"Kept    <button=circle,pad> Change"));
    EXPECT(tifa.plain[1] == L"Kept    (B: change)");
}

/// A screen with no player, or outside the screens, has no text.
void EmptyScreens()
{
    const rules::Claim c = rules::Open(2, {0, 1, 2}, rules::kIdentity, true);
    EXPECT(TextFor(c, 2, kNames).markup.empty());
    EXPECT(TextFor(c, -1, kNames).markup.empty());
    EXPECT(TextFor(c, 3, kNames).plain[0].empty());
}

}  // namespace

int main()
{
    UntakenScreens();
    LinesJoinedLikeTheGame();
    TakenScreens();
    NoStepWithOneOther();
    KeptScreen();
    EmptyScreens();
    return lss::test::Report();
}
