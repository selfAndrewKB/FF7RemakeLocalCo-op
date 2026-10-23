// Host-side tests for render/area_chains.h, run without the game: the per-view chains of area light settings over a
// model of the game's setter (FScene__SetDirectLightControllerBlendData_RT 0x142583F10, as decompiled).

#include "render/area_chains.h"

#include <cstdio>
#include <cstring>

namespace {

namespace chains = lss::render::area_chains;
using lss::render::shadow_code::ViewRec;

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

/// FDirectLightControllerBlendData, as the reflection dump lays it out.
struct Blend
{
    std::uint8_t bytes[chains::kBlendSize]{};
    void         Flag(std::size_t at) { bytes[at] = 1; }
    void         Value(std::size_t at, float v) { std::memcpy(&bytes[at], &v, 4); }
};
Blend ShadowCutoff(float distance, float cascade0 = -1.0f)
{
    Blend b;
    b.Flag(0x08);  // bOverride_LightShadowCutoffDistance
    b.Value(0x0C, distance);
    if (cascade0 >= 0.0f)
    {
        b.Flag(0x18);  // bOverride_DirectionalLightCascadeDistance0
        b.Value(0x1C, cascade0);
    }
    return b;
}

/// The scene's 24 bytes and the sun's fields, and the game's setter on them.
struct Model
{
    std::uint8_t scene[24]{};  // +0x1608..+0x161F
    float        cascades[5]{};
    float        bias = 0.0f, sharpen = 0.0f, distance = 0.0f;
    int          sunId = 1;
    const Blend* data  = nullptr;
    int          runs  = 0;

    const void* Id() const { return this; }
    const void* Sun() const { return sunId == 0 ? nullptr : reinterpret_cast<const void*>(static_cast<std::uintptr_t>(sunId)); }
    void        Snapshot(ViewRec& r) const
    {
        r.scene = Id();
        r.sun   = Sun();
        std::memcpy(r.sceneFields, scene, sizeof(scene));
        r.sunDistance = 0;
        std::memset(r.cascades, 0, sizeof(r.cascades));
        std::memset(r.biasSharpen, 0, sizeof(r.biasSharpen));
        if (r.sun == nullptr)
        {
            return;  // no sun: the live scene's snapshot leaves the sun's fields at 0
        }
        std::memcpy(&r.sunDistance, &distance, 4);
        std::memcpy(r.cascades, cascades, sizeof(cascades));
        std::memcpy(&r.biasSharpen[0], &bias, 4);
        std::memcpy(&r.biasSharpen[1], &sharpen, 4);
    }
    void Apply(const ViewRec& r)
    {
        std::memcpy(scene, r.sceneFields, sizeof(scene));
        if (r.sun == Sun())
        {
            std::memcpy(&distance, &r.sunDistance, 4);
            std::memcpy(cascades, r.cascades, sizeof(cascades));
            std::memcpy(&bias, &r.biasSharpen[0], 4);
            std::memcpy(&sharpen, &r.biasSharpen[1], 4);
        }
    }
    void RunOriginal()
    {
        ++runs;
        const std::uint8_t* d = data->bytes;
        std::memcpy(&scene[0x00], &d[0x00], 2);  // sun off, the light cutoff's override flag
        scene[0x08] = d[0x08];
        scene[0x10] = d[0x10];
        if (scene[0x01] != 0) std::memcpy(&scene[0x04], &d[0x04], 4);
        if (scene[0x08] != 0) std::memcpy(&scene[0x0C], &d[0x0C], 4);
        if (scene[0x10] != 0) std::memcpy(&scene[0x14], &d[0x14], 4);
        for (int i = 0; i < 5; ++i)
        {
            if (d[0x18 + 8 * i] != 0) std::memcpy(&cascades[i], &d[0x1C + 8 * i], 4);  // only when overridden: persists
        }
        if (d[0x40] != 0) std::memcpy(&bias, &d[0x44], 4);
        if (d[0x48] != 0) std::memcpy(&sharpen, &d[0x4C], 4);
    }
    float SceneCutoff() const
    {
        float v;
        std::memcpy(&v, &scene[0x0C], 4);
        return v;
    }
};

float CutoffOf(const ViewRec& r)
{
    float v;
    std::memcpy(&v, &r.sceneFields[0x0C], 4);
    return v;
}
float Cascade0(const ViewRec& r)
{
    float v;
    std::memcpy(&v, &r.cascades[0], 4);
    return v;
}

struct World
{
    Model   model;
    ViewRec game;
    ViewRec records[chains::kRecords];

    int Push(const Blend& data, int view)
    {
        Blend tagged = data;
        chains::Tag tag;
        if (view >= 0)
        {
            tag.valid = true;
            tag.index = static_cast<std::uint8_t>(view);
            tag.state = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x1000 + view));
            chains::Stamp(tagged.bytes, tag);
        }
        model.data = &tagged;
        return chains::Push(game, records, model, chains::ReadTag(tagged.bytes));
    }
};

void TestTag()
{
    std::puts("tag: stamped into the padding only, read back whole; no magic or an index past the records is untagged");
    Blend       b = ShadowCutoff(1234.0f, 99.0f);
    const Blend plain = b;
    chains::Tag tag{true, 2, reinterpret_cast<const void*>(0x1122334455667788ull)};
    chains::Stamp(b.bytes, tag);
    const chains::Tag back = chains::ReadTag(b.bytes);
    EXPECT(back.valid && back.index == 2 && back.state == tag.state);
    for (std::size_t i = 0; i < chains::kBlendSize; ++i)
    {
        bool stamped = i == chains::kMagicAt || i == chains::kIndexAt;
        for (std::size_t at : chains::kStateAt)
        {
            stamped = stamped || at == i;
        }
        if (!stamped)
        {
            EXPECT(b.bytes[i] == plain.bytes[i]);  // every field the setter reads is untouched
        }
    }
    EXPECT(!chains::ReadTag(plain.bytes).valid);
    b.bytes[chains::kIndexAt] = static_cast<std::uint8_t>(chains::kRecords);
    EXPECT(!chains::ReadTag(b.bytes).valid);
}

void TestTwoViews()
{
    std::puts("two views: each keeps its own cutoff and its own history of sun overrides; the scene is left as the game's");
    World w;
    const Blend defaults;
    w.Push(defaults, 0);  // the first frame: both views in an area without overrides
    w.Push(defaults, 1);
    w.Push(defaults, 0);
    EXPECT(w.Push(ShadowCutoff(500.0f, 1000.0f), 0) == 1);  // player 1's area overrides the cascade
    w.Push(defaults, 1);
    w.Push(ShadowCutoff(3000.0f), 1);                       // player 2's area does not
    EXPECT(CutoffOf(w.game) == 3000.0f && Cascade0(w.game) == 1000.0f);  // the game's: the last view's, cascade persists
    EXPECT(CutoffOf(w.records[0]) == 500.0f && Cascade0(w.records[0]) == 1000.0f);
    EXPECT(CutoffOf(w.records[1]) == 3000.0f && Cascade0(w.records[1]) == 0.0f);  // never had player 1's cascade
    EXPECT(w.model.SceneCutoff() == 3000.0f && w.model.cascades[0] == 1000.0f);   // the scene: the game's
    EXPECT(chains::Differs(w.records[0], w.game) && chains::Differs(w.records[1], w.game));
    EXPECT(w.records[0].state == reinterpret_cast<const void*>(0x1000) && w.records[1].state == reinterpret_cast<const void*>(0x1001));
    // Next frame: player 2 walks into player 1's area: its chain follows only its own pushes.
    w.Push(defaults, 0);
    w.Push(ShadowCutoff(500.0f, 1000.0f), 0);
    w.Push(defaults, 1);
    w.Push(ShadowCutoff(500.0f, 1000.0f), 1);
    EXPECT(!chains::Differs(w.records[1], w.game) && Cascade0(w.records[1]) == 1000.0f);
}

void TestUntaggedAndOneView()
{
    std::puts("an untagged push (a capture) lands on every chain of the scene; with one view its chain equals the game's");
    World w;
    w.Push(ShadowCutoff(800.0f), 0);
    w.Push(ShadowCutoff(900.0f), 1);
    Blend capture;
    capture.Flag(0x20);  // cascade 1
    capture.Value(0x24, 77.0f);
    EXPECT(w.Push(capture, -1) == 2);
    EXPECT(w.records[0].cascades[1] != 0 && w.records[1].cascades[1] != 0 && w.game.cascades[1] != 0);
    float c1;
    std::memcpy(&c1, &w.records[1].cascades[1], 4);
    EXPECT(c1 == 77.0f);

    World one;
    one.Push(Blend{}, 0);
    one.Push(ShadowCutoff(250.0f, 4000.0f), 0);
    one.Push(Blend{}, 0);
    one.Push(ShadowCutoff(600.0f), 0);
    EXPECT(std::memcmp(one.records[0].sceneFields, one.game.sceneFields, 24) == 0);
    EXPECT(std::memcmp(one.records[0].cascades, one.game.cascades, sizeof(one.game.cascades)) == 0);
    EXPECT(!chains::Differs(one.records[0], one.game));
}

void TestStubLeftoversAndOutsideWriters()
{
    std::puts("a stub's leftovers (a view's set) are undone at the next push; anything else in the scene restarts the chains; "
              "another view taking an index starts a new chain; a scene without a sun");
    World w;
    const Blend defaults;
    w.Push(defaults, 0);
    w.Push(defaults, 1);
    w.Push(ShadowCutoff(500.0f, 1000.0f), 0);
    w.Push(ShadowCutoff(3000.0f), 1);
    // The light passes leave view 0's set in the scene (as the A and sun stubs do).
    w.model.Apply(w.records[0]);
    w.Push(defaults, 0);
    EXPECT(w.records[1].valid && CutoffOf(w.records[1]) == 3000.0f);  // undone, the chains kept
    w.Push(ShadowCutoff(3000.0f), 1);
    // Something else writes the sun: every chain starts again from what is there.
    w.model.cascades[0] = 42.0f;
    w.Push(defaults, 0);
    EXPECT(Cascade0(w.records[0]) == 42.0f && !w.records[1].valid);
    // Another view (another state) at index 0: a new chain from the game's state, not the old one's history.
    Blend b = ShadowCutoff(700.0f);
    chains::Tag other{true, 0, reinterpret_cast<const void*>(0x9999)};
    chains::Stamp(b.bytes, other);
    w.model.data = &b;
    w.records[0].cascades[2] = 0x12345678;  // history only the old view had
    chains::Push(w.game, w.records, w.model, chains::ReadTag(b.bytes));
    EXPECT(w.records[0].state == other.state && w.records[0].cascades[2] != 0x12345678);
    // Another scene: its pushes start chains of their own.
    World second;
    second.Push(ShadowCutoff(100.0f), 0);
    EXPECT(second.records[0].scene == second.model.Id() && second.records[0].scene != w.model.Id());
    // No sun: the sun's fields stay 0 and no difference is read from them.
    World dark;
    dark.model.sunId = 0;
    dark.Push(defaults, 0);
    dark.Push(defaults, 1);
    EXPECT(dark.records[0].sun == nullptr && !chains::Differs(dark.records[0], dark.game) && dark.records[0].cascades[0] == 0);
}

void TestNewSunProxy()
{
    std::puts("a new sun proxy: a chain's sun fields start again from the game's");
    World w;
    w.Push(ShadowCutoff(500.0f, 1000.0f), 0);
    w.model.sunId = 2;  // the sun was registered again: the new proxy holds its own values
    w.model.cascades[0] = 5.0f;
    w.Push(ShadowCutoff(500.0f), 0);
    EXPECT(Cascade0(w.records[0]) == 5.0f && w.records[0].sun == w.model.Sun());
}

}  // namespace

int main()
{
    TestTag();
    TestTwoViews();
    TestUntaggedAndOneView();
    TestNewSunProxy();
    TestStubLeftoversAndOutsideWriters();
    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("all passed");
    return 0;
}
