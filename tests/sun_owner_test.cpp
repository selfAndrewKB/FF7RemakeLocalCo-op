// Host-side tests for render/sun_owner.h: whose sun cascade a view may use.

#include "render/sun_owner.h"

#include <cstdint>
#include <cstdio>
#include <map>
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

namespace so = lss::render::sun_owner;

constexpr std::uintptr_t kView0 = 0x1000, kView1 = 0x1000 + 0x28B0, kView2 = 0x1000 + 2 * 0x28B0;

void TestOtherView()
{
    EXPECT(!so::OtherView(0, kView0));       // no owner (a lamp's shadow): every view uses it
    EXPECT(!so::OtherView(kView0, kView0));  // its own cascade
    EXPECT(so::OtherView(kView1, kView0));
    EXPECT(so::OtherView(kView0, kView2));
}

void TestKeep()
{
    // Shadows 0xA..0xC: the sun's cascades of views 0, 1, 2; 0xD: a lamp's shadow (no owner); 0xE: unreadable.
    const std::map<std::uintptr_t, std::uintptr_t> owners{{0xA, kView0}, {0xB, kView1}, {0xC, kView2}, {0xD, 0}};
    const auto ownerOf = [&](std::uintptr_t shadow) {
        const auto it = owners.find(shadow);
        return it == owners.end() ? std::uintptr_t{0} : it->second;
    };
    const std::vector<so::Injection> in{
        {0x10, 0, 0, 0},     // an unshadowed light
        {0x20, 0xA, 1, 0x99},
        {0x20, 0xB, 1, 0x99},
        {0x20, 0xC, 1, 0x99},
        {0x30, 0xD, 0, 0},
        {0x40, 0xE, 0, 0},
    };
    std::vector<so::Injection> out;

    EXPECT(so::Keep(in, kView0, ownerOf, out) == 2);
    EXPECT(out.size() == 4);
    EXPECT(out[0].light == 0x10 && out[1].shadow == 0xA && out[2].shadow == 0xD && out[3].shadow == 0xE);
    EXPECT(out[1].applyLightFunction == 1 && out[1].material == 0x99);  // entries copied whole
    EXPECT(so::AnyOther(in, kView0, ownerOf));

    EXPECT(so::Keep(in, kView2, ownerOf, out) == 2);
    EXPECT(out.size() == 4 && out[1].shadow == 0xC);

    // Single player: only view 0's cascades exist, nothing is dropped.
    const std::vector<so::Injection> single{{0x10, 0, 0, 0}, {0x20, 0xA, 0, 0}, {0x30, 0xD, 0, 0}};
    EXPECT(!so::AnyOther(single, kView0, ownerOf));
    EXPECT(so::Keep(single, kView0, ownerOf, out) == 0);
    EXPECT(out.size() == 3);

    // Empty list.
    EXPECT(so::Keep({}, kView0, ownerOf, out) == 0 && out.empty());
    EXPECT(!so::AnyOther({}, kView0, ownerOf));
}

}  // namespace

int main()
{
    TestOtherView();
    TestKeep();
    if (g_failures != 0)
    {
        std::printf("sun_owner: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sun_owner: all passed\n");
    return 0;
}
