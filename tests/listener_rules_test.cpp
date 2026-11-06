// Host-side tests for the listeners' decisions (audio/listener_rules.h) and the seqlock it reads its threads'
// values through (platform/seqlock.h). Run without the game.

#include "audio/listener_rules.h"
#include "platform/seqlock.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

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

bool Near(const rules::Vec3& a, const rules::Vec3& b)
{
    return rules::Distance(a, b) < 1e-5f;
}

void RotationGivesTheSeadAxes()
{
    std::puts("listener_rules: the push's front and up from an engine rotation");
    const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    EXPECT(Near(rules::SeadFront(identity), {1, 0, 0}));
    EXPECT(Near(rules::SeadUp(identity), {0, 0, -1}));
    const float s = std::sqrt(0.5f);
    const float yaw90[4] = {0.0f, 0.0f, s, s};  // a quarter turn about up: front x -> y
    EXPECT(Near(rules::SeadFront(yaw90), {0, 1, 0}));
    EXPECT(Near(rules::SeadUp(yaw90), {0, 0, -1}));
}

void NearestTakesTheClosestThenHolds()
{
    std::puts("listener_rules: a new sound takes the nearest listener; ties go to player 1");
    const float inf  = std::numeric_limits<float>::infinity();
    const float nan  = std::numeric_limits<float>::quiet_NaN();
    const float wide = 100000.0f;  // a range every listener is within
    EXPECT(rules::ChooseNearest(rules::kNoChoice, std::array{500.0f, 200.0f, 900.0f}, wide) == 1);
    EXPECT(rules::ChooseNearest(rules::kNoChoice, std::array{300.0f, 300.0f, 300.0f}, wide) == 0);
    EXPECT(rules::ChooseNearest(rules::kNoChoice, std::array{500.0f, inf, 100.0f}, wide) == 2);

    std::puts("listener_rules: a chosen listener within range is kept until another is 20 % nearer");
    EXPECT(rules::ChooseNearest(0, std::array{500.0f, 450.0f, inf}, wide) == 0);   // 450 is not below 400
    EXPECT(rules::ChooseNearest(0, std::array{500.0f, 390.0f, inf}, wide) == 1);
    EXPECT(rules::ChooseNearest(1, std::array{390.0f, 450.0f, inf}, wide) == 1);   // back to player 1 needs below 360
    EXPECT(rules::ChooseNearest(1, std::array{350.0f, 450.0f, inf}, wide) == 0);
    EXPECT(rules::ChooseNearest(2, std::array{900.0f, 100.0f, 110.0f}, wide) == 2);  // between two seats: held
    EXPECT(rules::ChooseNearest(0, std::array{500.0f, 450.0f, inf}, 0.0f) == 1);  // no range known yet: the nearest

    std::puts("listener_rules: a chosen listener out of the sound's range is left for the nearest");
    EXPECT(rules::ChooseNearest(1, std::array{900.0f, 1050.0f, inf}, 1000.0f) == 0);  // 900 is not below 840, yet 1050 > 1000
    EXPECT(rules::ChooseNearest(1, std::array{900.0f, 1000.0f, inf}, 1000.0f) == 0);  // at the range is out (strict)
    EXPECT(rules::ChooseNearest(1, std::array{900.0f, 990.0f, inf}, 1000.0f) == 1);   // within: held
    EXPECT(rules::ChooseNearest(2, std::array{3000.0f, 2500.0f, 2600.0f}, 1000.0f) == 1);  // nobody hears it: the nearest

    std::puts("listener_rules: a listener that goes off is left at once; none on is player 1's");
    EXPECT(rules::ChooseNearest(2, std::array{900.0f, 800.0f, inf}, wide) == 1);
    EXPECT(rules::ChooseNearest(1, std::array{900.0f, nan, inf}, wide) == 0);
    EXPECT(rules::ChooseNearest(1, std::array{nan, nan, nan}, wide) == 0);
    EXPECT(rules::ChooseNearest(7, std::array{900.0f, 100.0f}, wide) == 1);  // a stale index outside the list
}

void ChoicesLastFromTestToUpdateAndLapse()
{
    std::puts("listener_rules: a choice is the update's in its frame, held into the next, gone after");
    rules::ChoiceTable<4, 4> table;
    int a = 0, b = 0;
    EXPECT(table.Held(&a, 10) == rules::kNoChoice);
    EXPECT(table.Record(&a, 2, 10));
    EXPECT(table.ChosenIn(&a, 10) == 2);
    EXPECT(table.Held(&a, 11) == 2);
    EXPECT(table.ChosenIn(&a, 11) == rules::kNoChoice);  // its test has not run in frame 11
    EXPECT(table.Held(&a, 12) == rules::kNoChoice);      // not refreshed in frame 11: lapsed
    EXPECT(table.Held(&b, 10) == rules::kNoChoice);
    EXPECT(table.Record(&b, 1, 20));
    EXPECT(table.Held(&b, 21) == 1);  // a new sound at a freed sound's address inherits its hold (the range rule decides)

    std::puts("listener_rules: sounds that share a home slot keep their own choices");
    rules::ChoiceTable<1, 2> two;  // two entries, each sound's probe covers both
    int c = 0;
    EXPECT(two.Record(&a, 1, 5));
    EXPECT(two.Record(&b, 2, 5));
    EXPECT(!two.Record(&c, 0, 5));  // no free entry: refused, the others' choices kept
    EXPECT(two.ChosenIn(&a, 5) == 1 && two.ChosenIn(&b, 5) == 2);
    EXPECT(two.ChosenIn(&c, 5) == rules::kNoChoice);
    EXPECT(two.Record(&c, 2, 7));   // a's and b's lapsed: c takes one
    EXPECT(two.ChosenIn(&c, 7) == 2);
    EXPECT(two.Held(&a, 7) == rules::kNoChoice);

    rules::ChoiceTable<4, 16> many;
    std::array<int, 16> sounds{};
    bool all = true;
    for (std::size_t i = 0; i < sounds.size(); ++i)
    {
        all = many.Record(&sounds[i], static_cast<int>(i % 3), 1) && all;
    }
    EXPECT(all);
    for (std::size_t i = 0; i < sounds.size(); ++i)
    {
        EXPECT(many.ChosenIn(&sounds[i], 1) == static_cast<int>(i % 3));
    }
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
    RotationGivesTheSeadAxes();
    NearestTakesTheClosestThenHolds();
    ChoicesLastFromTestToUpdateAndLapse();
    SeqlockReadsWholePublishes();
    SeqlockNeverReadsMidPublish();
    return lss::test::Report();
}
