// Host-side tests for the sound watch's decisions (audio/listener_rules.h) and the seqlock it reads its threads'
// values through (platform/seqlock.h, taken from Rebirth with its tests). Run without the game.

#include "audio/listener_rules.h"
#include "platform/seqlock.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "listener_rules: all passed\n" : "listener_rules: %d failure(s)\n", g_failures);
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

namespace rules = lss::audio::listener_rules;
using lss::platform::SeqFloats;

void WithinIsTheGamesStrictCompare()
{
    std::puts("listener_rules: a sound is heard closer than its range, never at it, never with no range");
    EXPECT(rules::Within(1000.0f, 999.0f));
    EXPECT(!rules::Within(1000.0f, 1000.0f));
    EXPECT(!rules::Within(1000.0f, 1500.0f));
    EXPECT(!rules::Within(0.0f, 0.0f));
    EXPECT(!rules::Within(-5.0f, 0.0f));
}

void DistanceIsEuclidean()
{
    std::puts("listener_rules: the distance between two points");
    EXPECT(rules::Distance({0, 0, 0}, {300, 400, 0}) == 500.0f);
    EXPECT(rules::Distance({1, 2, 3}, {1, 2, 3}) == 0.0f);
}

void AutoSeSpaceSwapsYAndZInMetres()
{
    std::puts("listener_rules: AutoSe's space is metres with y and z swapped, as the game's converter");
    const rules::Vec3 at = rules::UeCmToAutoSe({100.0f, 200.0f, 300.0f});
    EXPECT(at[0] == 1.0f && at[1] == 3.0f && at[2] == 2.0f);
}

void ClipIsBeyondTheDistance()
{
    std::puts("listener_rules: the clip cuts beyond its distance, keeps at or within it");
    EXPECT(rules::BeyondClip({10, 0, 0}, {0, 0, 0}, 5.0f));
    EXPECT(!rules::BeyondClip({5, 0, 0}, {0, 0, 0}, 5.0f));
    EXPECT(!rules::BeyondClip({3, 4, 0}, {0, 0, 0}, 5.0f));
}

void SeqlockReadsWholePublishes()
{
    std::puts("seqlock: nothing before the first publish; then the latest values, whole");
    SeqFloats<3>         floats;
    std::array<float, 3> out{9, 9, 9};
    EXPECT(!floats.Read(out));
    floats.Publish({1, 2, 3});
    EXPECT(floats.Read(out));
    EXPECT(out[0] == 1 && out[1] == 2 && out[2] == 3);
    floats.Publish({4, 5, 6});
    EXPECT(floats.Read(out));
    EXPECT(out[0] == 4 && out[1] == 5 && out[2] == 6);
    EXPECT(floats.version.load() == 4);
}

void SeqlockNeverReadsMidPublish()
{
    std::puts("seqlock: an odd version (the writer mid-publish) is never read");
    SeqFloats<2> floats;
    floats.Publish({1, 2});
    floats.version.store(3);
    std::array<float, 2> out{};
    EXPECT(!floats.Read(out));
    floats.version.store(4);
    EXPECT(floats.Read(out));
    EXPECT(out[0] == 1 && out[1] == 2);
}

}  // namespace

int main()
{
    WithinIsTheGamesStrictCompare();
    DistanceIsEuclidean();
    AutoSeSpaceSwapsYAndZInMetres();
    ClipIsBeyondTheDistance();
    SeqlockReadsWholePublishes();
    SeqlockNeverReadsMidPublish();
    return lss::test::Report();
}
