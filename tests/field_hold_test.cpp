// Host-side tests for players/field_hold.h: the user's rule on every layer and kind that matters.

#include "players/field_hold.h"

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
    // Everything the level writes on layer 1 during gameplay but LEAD: the stay, the run to a mark, the stand at
    // the end of a path, the follow, and the kinds that never read the player.
    for (const fh::Kind kind : {fh::kNone, fh::kStay, fh::kFollowTarget, fh::kFollowPosition, fh::kAnimation,
                                fh::kWandering, fh::kPatrol, fh::kAccompany, fh::kInterest, fh::kUnique})
    {
        EXPECT(fh::Of(fh::kLayerLevel, kind) == Hold::Free);
        EXPECT(fh::Of(fh::kLayerStream, kind) == Hold::Free);
    }
    EXPECT(fh::Of(fh::kLayerLevel, fh::kEmpty) == Hold::Free);  // nothing asked
}

void LeadAndOneShotsNeedTheAi()
{
    EXPECT(fh::Of(fh::kLayerLevel, fh::kLead) == Hold::Kind);
    EXPECT(fh::Of(fh::kLayerLevel, fh::kSingleAnimation) == Hold::Kind);
    EXPECT(fh::Of(fh::kLayerLevel, fh::kEscapePosition) == Hold::Kind);
    EXPECT(fh::Of(fh::kLayerStream, fh::kLead) == Hold::Kind);
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
    // of a LEAD holds; the scene itself is the actor mode, the caller's test.
    EXPECT(fh::Of(fh::kLayerCinema, fh::kAccompany) == Hold::Free);
    EXPECT(fh::Of(fh::kLayerCinema, fh::kNone) == Hold::Free);
    EXPECT(fh::Of(fh::kLayerCinema, fh::kFollowPosition) == Hold::Free);
    EXPECT(fh::Of(fh::kLayerCinema, fh::kLead) == Hold::Kind);
    EXPECT(fh::Of(fh::kLayerCinema, fh::kSingleAnimation) == Hold::Kind);
}

void NoStateWaits()
{
    EXPECT(fh::Of(fh::kNoLayer, fh::kNone) == Hold::NoState);
    EXPECT(fh::Of(0xFF, fh::kAccompany) == Hold::NoState);
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

int main()
{
    LevelStagingIsFree();
    LeadAndOneShotsNeedTheAi();
    ScriptLayersHold();
    CinemaCopyFollowsItsKind();
    NoStateWaits();
    NamesCoverTheEnum();
    if (g_failures == 0)
    {
        std::printf("field_hold: all passed\n");
        return 0;
    }
    std::printf("field_hold: %d failure(s)\n", g_failures);
    return 1;
}
