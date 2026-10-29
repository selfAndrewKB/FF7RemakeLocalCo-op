// Host-side tests for players/field_hold.h: the user's rule on every layer and kind that matters.

#include "players/field_hold.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

namespace fh = lss::players::field_hold;
using fh::Hold;

void LevelStagingIsFree()
{
    // Everything the level writes on layer 1 during gameplay: the stay, the stand at the end of a path, the follow,
    // the kinds that never read the player (the run to a mark asks the game, below).
    for (const fh::Kind kind : {fh::kNone, fh::kStay, fh::kFollowTarget, fh::kWandering, fh::kPatrol, fh::kAccompany,
                                fh::kInterest, fh::kUnique})
    {
        EXPECT(fh::Of(fh::kLayerLevel, kind) == Hold::Free);
        EXPECT(fh::Of(fh::kLayerStream, kind) == Hold::Free);
    }
    EXPECT(fh::Of(fh::kLayerLevel, fh::kEmpty) == Hold::Free);  // nothing asked
}

void ArrivalAsksTheGame()
{
    // LEAD on the level layer walks a member to a mark in step with the player as often as it makes a guide; the
    // kind cannot tell the two apart, so the rule hands the question on (the user, 2026-10-21). The run to a mark
    // is the same question (the user, 2026-10-27: Aerith's run ahead of Cloud stalled the level under a further player).
    // Elsewhere it is what its layer says: the stream's default never leads, the cinema copy is the actor mode's.
    EXPECT(fh::Of(fh::kLayerLevel, fh::kLead) == Hold::Arrival);
    EXPECT(fh::Of(fh::kLayerStream, fh::kLead) == Hold::Free);
    EXPECT(fh::Of(5, fh::kLead) == Hold::Layer);
    EXPECT(fh::Of(fh::kLayerLevel, fh::kFollowPosition) == Hold::Arrival);
    EXPECT(fh::Of(fh::kLayerStream, fh::kFollowPosition) == Hold::Free);
    EXPECT(fh::Of(5, fh::kFollowPosition) == Hold::Layer);
}

void TheMoveSettings()
{
    // The four Status names the game's arrival sender compares, without case; a further one ahead in a member's
    // own chain of states makes the chain a scripted path (Aerith's run, a stay, then the next run).
    for (const wchar_t* status : {L"Lead", L"MoveToTarget", L"FollowActor", L"RouteMove", L"lead", L"MOVETOTARGET"})
    {
        EXPECT(fh::MoveSetting(status));
    }
    for (const wchar_t* status : {L"Stable", L"Stay", L"Animation", L"", L"Lead ", L"Leads"})
    {
        EXPECT(!fh::MoveSetting(status));
    }
    // A path ends when the member stands (Stable); a stay or a move is still the path.
    EXPECT(fh::PathEnd(L"Stable"));
    EXPECT(fh::PathEnd(L"stable"));
    EXPECT(!fh::PathEnd(L"Stay"));
    EXPECT(!fh::PathEnd(L"Lead"));
    EXPECT(!fh::PathEnd(L""));
}

void OnlyTheArrivalKindsAskTheGame()
{
    // The gate reads a handler only for the kinds whose arrival the level can wait on; the rule and the gate share
    // the one list.
    for (std::uint8_t kind = 0; kind <= fh::kUnique; ++kind)
    {
        EXPECT(fh::SignalsArrival(kind) == (kind == fh::kLead || kind == fh::kFollowPosition));
    }
    EXPECT(!fh::SignalsArrival(0xFF));
}

void AnimationsAreWatched()
{
    // A scripted animation, the level's or a one-shot: the AI plays it and the player watches (the user,
    // 2026-10-21, after a seat mid-animation cut one short). On a script layer the layer is named first.
    EXPECT(fh::Of(fh::kLayerLevel, fh::kAnimation) == Hold::Animation);
    EXPECT(fh::Of(fh::kLayerLevel, fh::kSingleAnimation) == Hold::Animation);
    EXPECT(fh::Of(fh::kLayerStream, fh::kAnimation) == Hold::Animation);
    EXPECT(fh::Of(fh::kLayerStream, fh::kSingleAnimation) == Hold::Animation);
    EXPECT(fh::Of(6, fh::kAnimation) == Hold::Layer);
}

void EscapeNeedsTheAi()
{
    EXPECT(fh::Of(fh::kLayerLevel, fh::kEscapePosition) == Hold::Kind);
    EXPECT(fh::Of(fh::kLayerStream, fh::kEscapePosition) == Hold::Kind);
}

void ScriptLayersHold()
{
    // Whatever the kind: the act layers (5, 7), the event layer (6), a route move (3), a field action (7).
    for (std::uint8_t layer = 2; layer < 15; ++layer)
    {
        if (layer == fh::kLayerCinema)
        {
            continue;
        }
        EXPECT(fh::Of(layer, fh::kStay) == Hold::Layer);
        EXPECT(fh::Of(layer, fh::kNone) == Hold::Layer);
        EXPECT(fh::Of(layer, fh::kAccompany) == Hold::Layer);
        EXPECT(fh::Of(layer, fh::kSingleAnimation) == Hold::Layer);  // the layer is named first
    }
}

void CinemaCopyFollowsItsKind()
{
    // A cutscene's preparation copies the active record into layer 13: a copy of a free kind stays free, a copy
    // of an animation is watched, a copy of the escape holds; the scene itself is the actor mode, the caller's test.
    EXPECT(fh::Of(fh::kLayerCinema, fh::kAccompany) == Hold::Free);
    EXPECT(fh::Of(fh::kLayerCinema, fh::kNone) == Hold::Free);
    EXPECT(fh::Of(fh::kLayerCinema, fh::kFollowPosition) == Hold::Free);
    EXPECT(fh::Of(fh::kLayerCinema, fh::kLead) == Hold::Free);
    EXPECT(fh::Of(fh::kLayerCinema, fh::kAnimation) == Hold::Animation);
    EXPECT(fh::Of(fh::kLayerCinema, fh::kSingleAnimation) == Hold::Animation);
    EXPECT(fh::Of(fh::kLayerCinema, fh::kEscapePosition) == Hold::Kind);
}

void NoStateWaits()
{
    EXPECT(fh::Of(fh::kNoLayer, fh::kNone) == Hold::NoState);
    EXPECT(fh::Of(0xFF, fh::kAccompany) == Hold::NoState);
}

void AnInterestIsJudgedBeneathIt()
{
    // Live 2026-10-29: Aerith's layers [1 11 0 0 0 0 0 0 0 12 0 0 0 0 0], active layer 9 kind 12: the follow beneath.
    const std::uint8_t aerith[fh::kLayers] = {1, 11, 0, 0, 0, 0, 0, 0, 0, 12, 0, 0, 0, 0, 0};
    const fh::Judged   judged              = fh::JudgedState(9, fh::kInterest, aerith);
    EXPECT(judged.layer == fh::kLayerLevel && judged.kind == fh::kAccompany);
    EXPECT(fh::Of(judged.layer, judged.kind) == Hold::Free);
    // A lead written on the level layer meanwhile is still the gate's to judge.
    const std::uint8_t lead[fh::kLayers] = {1, 7, 0, 0, 0, 0, 0, 0, 0, 12, 0, 0, 0, 0, 0};
    const fh::Judged   leadJudged        = fh::JudgedState(9, fh::kInterest, lead);
    EXPECT(fh::Of(leadJudged.layer, leadJudged.kind) == Hold::Arrival);
    // A script on a lower script layer still holds.
    const std::uint8_t script[fh::kLayers] = {1, 11, 0, 0, 0, 2, 0, 0, 0, 12, 0, 0, 0, 0, 0};
    const fh::Judged   scriptJudged        = fh::JudgedState(9, fh::kInterest, script);
    EXPECT(scriptJudged.layer == 5 && fh::Of(scriptJudged.layer, scriptJudged.kind) == Hold::Layer);
    // An INTEREST on layer 2 under one on layer 9 is skipped too.
    const std::uint8_t twice[fh::kLayers] = {1, 11, 12, 0, 0, 0, 0, 0, 0, 12, 0, 0, 0, 0, 0};
    EXPECT(fh::JudgedState(9, fh::kInterest, twice).kind == fh::kAccompany);
    // Nothing beneath it: no state yet.
    const std::uint8_t alone[fh::kLayers] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 12, 0, 0, 0, 0, 0};
    EXPECT(fh::Of(fh::JudgedState(9, fh::kInterest, alone).layer, 0) == Hold::NoState);
    // Any other active kind is judged as it is.
    EXPECT(fh::JudgedState(9, fh::kStay, aerith).layer == 9);
    EXPECT(fh::JudgedState(fh::kNoLayer, fh::kInterest, aerith).layer == fh::kNoLayer);
}

void NamesCoverTheEnum()
{
    for (std::uint8_t kind = 0; kind <= fh::kUnique; ++kind)
    {
        EXPECT(fh::KindName(kind)[0] != L'?');
    }
    EXPECT(fh::KindName(14)[0] == L'?');
}

}  // namespace

void InteractionLocksFreezeTheInteractor()
{
    // A treasure, a pickup, a switch or a terminal: the interactor's lock alone (the bits seen live 2026-10-27:
    // 0x100000 at a switch, 0x4 at a chest).
    EXPECT(fh::InteractionLock(0x4));
    EXPECT(fh::InteractionLock(0x100000));
    EXPECT(fh::InteractionLock(0x100004));
    // No lock, or any other bit with them: a scene, a story beat, a minigame, a location jump holds everyone.
    EXPECT(!fh::InteractionLock(0));
    EXPECT(!fh::InteractionLock(0x1));
    EXPECT(!fh::InteractionLock(0x2));
    EXPECT(!fh::InteractionLock(0x100001));
    EXPECT(!fh::InteractionLock(0x40000));
    EXPECT(!fh::InteractionLock(0x200000));
    EXPECT(!fh::InteractionLock(0x80004));
}

int main()
{
    LevelStagingIsFree();
    ArrivalAsksTheGame();
    OnlyTheArrivalKindsAskTheGame();
    TheMoveSettings();
    AnimationsAreWatched();
    EscapeNeedsTheAi();
    ScriptLayersHold();
    CinemaCopyFollowsItsKind();
    NoStateWaits();
    AnInterestIsJudgedBeneathIt();
    NamesCoverTheEnum();
    InteractionLocksFreezeTheInteractor();
    if (g_failures == 0)
    {
        std::printf("field_hold: all passed\n");
        return 0;
    }
    std::printf("field_hold: %d failure(s)\n", g_failures);
    return 1;
}
