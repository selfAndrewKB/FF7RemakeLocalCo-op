// Host-side tests for render/family_split.h: one view family per player, and the family put back exactly.

#include "render/family_split.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

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

namespace fs    = lss::render::family_split;
namespace facts = lss::facts;

/// A family's first 0x100 bytes: the view array pointer, the count, the skip flag and the frame number beside it.
struct Family
{
    std::array<std::uint8_t, 0x100> bytes{};

    template <class T>
    T Get(std::uint32_t offset) const
    {
        T value{};
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }
    template <class T>
    void Put(std::uint32_t offset, T value)
    {
        std::memcpy(bytes.data() + offset, &value, sizeof(T));
    }
};

/// What one submission saw.
struct Seen
{
    const void*  first = nullptr;
    std::int32_t count = 0;
    std::int32_t skip  = 0;
};

int g_a = 0, g_b = 0, g_c = 0;

Family Make(const void** views, std::int32_t count, std::int32_t skip)
{
    Family f;
    f.Put(facts::kFamilyViews, views);
    f.Put(facts::kFamilyViewCount, count);
    f.Put(facts::kFamilySkipFrameStep, skip);
    f.Put<std::int32_t>(0x74, 1234);  // the frame number: never written by the split
    return f;
}

void TestThreeViews()
{
    const void* views[3] = {&g_a, &g_b, &g_c};
    Family      f        = Make(views, 3, 0);
    std::vector<Seen> seen;
    std::vector<int>  betweens;
    const int made = fs::SubmitOnePerView(
        f.bytes.data(),
        [&](std::uint8_t* family) {
            Family view;
            std::memcpy(view.bytes.data(), family, view.bytes.size());
            seen.push_back({views[0], view.Get<std::int32_t>(facts::kFamilyViewCount),
                            view.Get<std::int32_t>(facts::kFamilySkipFrameStep)});
            EXPECT(view.Get<std::int32_t>(0x74) == 1234);
        },
        [&](int k, int n, const void* view) {
            EXPECT(n == 3);
            EXPECT(view == (k == 0 ? &g_a : k == 1 ? &g_b : &g_c));  // the game's view k, before it is brought forward
            betweens.push_back(k);
            return true;
        });

    EXPECT(made == 3);
    EXPECT(seen.size() == 3);
    EXPECT(seen[0].first == &g_a && seen[0].count == 1 && seen[0].skip == 0);
    EXPECT(seen[1].first == &g_b && seen[1].count == 1 && seen[1].skip == 1);
    EXPECT(seen[2].first == &g_c && seen[2].count == 1 && seen[2].skip == 1);
    EXPECT((betweens == std::vector<int>{0, 1, 2}));
    // Put back exactly.
    EXPECT(views[0] == &g_a && views[1] == &g_b && views[2] == &g_c);
    EXPECT(f.Get<std::int32_t>(facts::kFamilyViewCount) == 3);
    EXPECT(f.Get<std::int32_t>(facts::kFamilySkipFrameStep) == 0);
    EXPECT(f.Get<std::int32_t>(0x74) == 1234);
}

void TestSkipKept()
{
    // A family that already skips the step (Square Enix's own second submit sets it) keeps its value.
    const void* views[2] = {&g_a, &g_b};
    Family      f        = Make(views, 2, 7);
    std::vector<std::int32_t> skips;
    fs::SubmitOnePerView(
        f.bytes.data(), [&](std::uint8_t*) { skips.push_back(f.Get<std::int32_t>(facts::kFamilySkipFrameStep)); },
        [](int, int, const void*) { return true; });
    EXPECT((skips == std::vector<std::int32_t>{7, 1}));
    EXPECT(f.Get<std::int32_t>(facts::kFamilySkipFrameStep) == 7);
}

void TestWholeFamilies()
{
    // One view, no views, no array, or more than a family holds: one ordinary submission, nothing changed.
    const void* one[1] = {&g_a};
    for (const std::int32_t count : {1, 0, -1, fs::kViewsMax + 1})
    {
        Family f      = Make(one, count, 0);
        int    calls  = 0;
        int    betw   = 0;
        const int made = fs::SubmitOnePerView(
            f.bytes.data(),
            [&](std::uint8_t*) {
                ++calls;
                EXPECT(f.Get<std::int32_t>(facts::kFamilyViewCount) == count);
            },
            [&](int, int, const void*) {
                ++betw;
                return true;
            });
        EXPECT(made == 1 && calls == 1 && betw == 0);
        EXPECT(f.Get<std::int32_t>(facts::kFamilyViewCount) == count);
    }
    Family f = Make(nullptr, 3, 0);
    int    calls = 0;
    EXPECT(fs::SubmitOnePerView(f.bytes.data(), [&](std::uint8_t*) { ++calls; }, [](int, int, const void*) { return true; }) ==
               1 &&
           calls == 1);
}

void TestStopped()
{
    // The boundary fails before view 0: the game's own family, whole. Before view 2: views 0 and 1 drawn, 2 not.
    const void* views[3] = {&g_a, &g_b, &g_c};
    Family      f        = Make(views, 3, 0);
    std::vector<std::int32_t> counts;
    const auto record = [&](std::uint8_t*) { counts.push_back(f.Get<std::int32_t>(facts::kFamilyViewCount)); };
    EXPECT(fs::SubmitOnePerView(f.bytes.data(), record, [](int, int, const void*) { return false; }) == 1);
    EXPECT((counts == std::vector<std::int32_t>{3}));
    counts.clear();
    EXPECT(fs::SubmitOnePerView(f.bytes.data(), record, [](int k, int, const void*) { return k < 2; }) == 2);
    EXPECT((counts == std::vector<std::int32_t>{1, 1}));
    EXPECT(views[0] == &g_a && views[1] == &g_b && views[2] == &g_c);
    EXPECT(f.Get<std::int32_t>(facts::kFamilyViewCount) == 3 && f.Get<std::int32_t>(facts::kFamilySkipFrameStep) == 0);
}

}  // namespace

int main()
{
    TestThreeViews();
    TestSkipKept();
    TestWholeFamilies();
    TestStopped();
    if (g_failures != 0)
    {
        std::printf("family_split: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("family_split: all passed\n");
    return 0;
}
