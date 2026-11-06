#pragma once

// Each view's own chain of area light settings (lever A of render/shadows_per_view;
// research/RE-shadows-per-view-build.md), a pure rule tested on the host (tests/area_chains_test.cpp) over a model of
// the game's setter.
//
// Every view's camera evaluation pushes "defaults" then "this view's blend" of the rendering-settings controllers to the
// one scene (FScene::SetDirectLightControllerBlendData, run on the render thread in queue order), so the scene ends the
// frame with the last view's set. The sun's fields are written only when an area overrides them and persist until
// another area does: a view's set is its own history of pushes, not its last push. So the render-thread side keeps one
// chain per view (its own pushes, plus the untagged ones a scene capture makes, which land on every view in the game
// too) and the game's chain (every push, which is what the scene would hold without the mod). Each push runs the game's
// own setter once per chain, from that chain's state, and keeps what it leaves; the scene is left holding the game's
// chain. Between a frame's light passes and its next push the scene (and the sun's proxy) hold the last view's set the
// stubs wrote, as the game's own last push would have left them; the next push puts the game's chain back, but only
// over values it recognises as a stub's (a chain's or the game's): anything else wrote the scene, and every chain of
// it starts again from what is there. A chain also starts again when another view (another state) takes its index.
// With one view its chain equals the game's from the start; a player who stays after the others leave keeps their own.
//
// The tag: the game thread stamps each push it makes for a player's view into the blend data's padding (a magic byte,
// the view's index in the family, the view's state pointer), which the game's render command copies with the data and
// its setter never reads.
//
// The height fog (A-fog) is pushed the same way, once per view right after that view's light pushes, but its
// setter writes every field each time: a view's fog is its own last push, so each record is a snapshot, not a
// chain. Its blend data has room for the magic byte, the index and a check byte only; the view's state comes from
// that index's light chain, which the same view's light push has just updated.

#include "render/shadow_code.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::render::area_chains {

using shadow_code::FogRec;
using shadow_code::kRecords;
using shadow_code::ViewRec;

inline constexpr std::size_t  kBlendSize = 0x50;  // FDirectLightControllerBlendData
inline constexpr std::uint8_t kMagic     = 0x5B;
inline constexpr std::size_t  kMagicAt   = 0x02;
inline constexpr std::size_t  kIndexAt   = 0x03;
/// The padding bytes after the bools at +0x08, +0x10 and +0x18 (each followed by its float at +0x0C, +0x14, +0x1C).
inline constexpr std::size_t kStateAt[8] = {0x09, 0x0A, 0x0B, 0x11, 0x12, 0x13, 0x19, 0x1A};

struct Tag
{
    bool          valid = false;
    std::uint8_t  index = 0;
    const void*   state = nullptr;
};

inline void Stamp(std::uint8_t (&data)[kBlendSize], const Tag& tag) noexcept
{
    if (!tag.valid)
    {
        return;
    }
    data[kMagicAt] = kMagic;
    data[kIndexAt] = tag.index;
    std::uint8_t state[8];
    std::memcpy(state, &tag.state, sizeof(state));
    for (std::size_t i = 0; i < 8; ++i)
    {
        data[kStateAt[i]] = state[i];
    }
}

inline Tag ReadTag(const std::uint8_t* data) noexcept
{
    Tag tag;
    if (data[kMagicAt] != kMagic || data[kIndexAt] >= kRecords)
    {
        return tag;
    }
    std::uint8_t state[8];
    for (std::size_t i = 0; i < 8; ++i)
    {
        state[i] = data[kStateAt[i]];
    }
    tag.valid = true;
    tag.index = data[kIndexAt];
    std::memcpy(&tag.state, state, sizeof(state));
    return tag;
}

/// The same scene and sun values (raw: what a stub writes and Apply puts back).
inline bool SameSet(const ViewRec& a, const ViewRec& b) noexcept
{
    return a.scene == b.scene && a.sun == b.sun && std::memcmp(a.sceneFields, b.sceneFields, sizeof(a.sceneFields)) == 0 &&
           a.sunDistance == b.sunDistance && std::memcmp(a.cascades, b.cascades, sizeof(a.cascades)) == 0 &&
           std::memcmp(a.biasSharpen, b.biasSharpen, sizeof(a.biasSharpen)) == 0;
}

/// One push through every chain it belongs to. `Scene` provides: const void* Id(), const void* Sun() (the sun's proxy
/// or null), void Snapshot(ViewRec&) (the scene's fields and the sun's, and Id/Sun), void Apply(const ViewRec&) (the
/// scene's fields; the sun's only when the record's sun is the current one), void RunOriginal() (the game's setter on
/// this push's data). Returns the number of view chains the push went to.
template <class Scene>
int Push(ViewRec& game, ViewRec (&records)[kRecords], Scene& scene, const Tag& tag)
{
    const void* const id  = scene.Id();
    const void* const sun = scene.Sun();
    if (game.valid && game.scene == id)
    {
        ViewRec now;
        scene.Snapshot(now);
        bool ours = SameSet(now, game);  // a stub leaves a chain's set, or the game's
        for (std::size_t k = 0; !ours && k < kRecords; ++k)
        {
            ours = records[k].valid && SameSet(now, records[k]);
        }
        if (ours)
        {
            scene.Apply(game);
        }
        else
        {
            for (ViewRec& r : records)  // something else wrote the scene or the sun: every chain starts again from it
            {
                if (r.scene == id)
                {
                    r.valid = 0;
                }
            }
        }
    }
    ViewRec before;
    scene.Snapshot(before);  // the game's state before this push
    scene.RunOriginal();
    scene.Snapshot(game);
    game.valid = 1;
    int chains = 0;
    for (std::size_t k = 0; k < kRecords; ++k)
    {
        ViewRec&   r    = records[k];
        const bool mine = r.valid && r.scene == id && (!tag.valid || r.state == tag.state);
        if (tag.valid ? k != tag.index : !mine)
        {
            continue;  // a view's push goes to its chain; an untagged one to every chain of this scene
        }
        const void* state = tag.valid ? tag.state : r.state;
        if (!mine)
        {
            r = before;  // a new chain starts from the game's state
        }
        else if (r.sun != sun)
        {
            // The sun's proxy was made again: its fields start from the game's, as the game keeps the new proxy's.
            r.sun         = before.sun;
            r.sunDistance = before.sunDistance;
            std::memcpy(r.cascades, before.cascades, sizeof(r.cascades));
            std::memcpy(r.biasSharpen, before.biasSharpen, sizeof(r.biasSharpen));
        }
        scene.Apply(r);
        scene.RunOriginal();
        scene.Snapshot(r);
        r.state = state;
        r.valid = 1;
        ++chains;
    }
    scene.Apply(game);
    return chains;
}

/// Whether a view's chain holds other settings than the game's (for the reading and the log): the sun off flag, each
/// cutoff's override flag and, where set, its distance; the sun's fields when both hold the same sun.
inline bool Differs(const ViewRec& view, const ViewRec& game) noexcept
{
    if (!view.valid || !game.valid || view.scene != game.scene)
    {
        return false;
    }
    const std::uint8_t* a = view.sceneFields;
    const std::uint8_t* b = game.sceneFields;
    if (a[0x00] != b[0x00])
    {
        return true;
    }
    for (std::size_t flag : {std::size_t{0x01}, std::size_t{0x08}, std::size_t{0x10}})
    {
        const std::size_t value = flag == 0x01 ? 0x04 : flag + 4;
        if (a[flag] != b[flag] || (a[flag] != 0 && std::memcmp(a + value, b + value, 4) != 0))
        {
            return true;
        }
    }
    return view.sun != nullptr && view.sun == game.sun &&
           (view.sunDistance != game.sunDistance || std::memcmp(view.cascades, game.cascades, sizeof(view.cascades)) != 0 ||
            std::memcmp(view.biasSharpen, game.biasSharpen, sizeof(view.biasSharpen)) != 0);
}

// --- The height fog -------------------------------------------------------------------------------------------------

inline constexpr std::size_t kFogBlendSize = 0x38;  // FExponentialHeightFogSettingsBlendData
inline constexpr std::size_t kFogMagicAt   = 0x01;  // the padding after the override bit field at +0x00
inline constexpr std::size_t kFogIndexAt   = 0x02;
inline constexpr std::size_t kFogCheckAt   = 0x03;  // magic ^ index: two padding bytes alone could read as a tag

inline void StampFog(std::uint8_t (&data)[kFogBlendSize], const Tag& tag) noexcept
{
    if (tag.valid)
    {
        data[kFogMagicAt] = kMagic;
        data[kFogIndexAt] = tag.index;
        data[kFogCheckAt] = static_cast<std::uint8_t>(kMagic ^ tag.index);
    }
}

/// The view's index only (state null).
inline Tag ReadFogTag(const std::uint8_t* data) noexcept
{
    Tag tag;
    if (data[kFogMagicAt] == kMagic && data[kFogIndexAt] < kRecords &&
        data[kFogCheckAt] == static_cast<std::uint8_t>(kMagic ^ data[kFogIndexAt]))
    {
        tag.valid = true;
        tag.index = data[kFogIndexAt];
    }
    return tag;
}

/// One fog push, after the game's setter ran: `now` is the scene's first fog as the push left it (valid, its fog,
/// its component, its scene and the fields). The game's record becomes it; a view's push replaces that view's record, named by the state of
/// the view's light chain (none: the view gets the game's); an untagged push lands on every view's record of the same fog,
/// as it does in the game. Returns the number of view records the push went to.
inline int FogPush(FogRec& game, FogRec (&fogs)[kRecords], const FogRec& now, const Tag& tag,
                   const ViewRec (&views)[kRecords]) noexcept
{
    game       = now;
    game.state = nullptr;
    game.valid = 1;
    int records = 0;
    for (std::size_t k = 0; k < kRecords; ++k)
    {
        FogRec& r = fogs[k];
        if (tag.valid)
        {
            if (k != tag.index)
            {
                continue;
            }
            const ViewRec& light = views[k];
            if (!light.valid || light.scene != now.scene || light.state == nullptr)
            {
                r.valid = 0;
                continue;
            }
            r       = now;
            r.state = light.state;
        }
        else if (r.valid && r.fog == now.fog && r.component == now.component)
        {
            const void* const state = r.state;
            r       = now;
            r.state = state;
        }
        else
        {
            continue;
        }
        r.valid = 1;
        ++records;
    }
    return records;
}

/// The same fog fields (ignoring the record's state).
inline bool SameFog(const FogRec& a, const FogRec& b) noexcept
{
    return a.fog == b.fog && a.component == b.component &&
           std::memcmp(a.heightToOpacity, b.heightToOpacity, sizeof(a.heightToOpacity)) == 0 &&
           std::memcmp(a.inscattering, b.inscattering, sizeof(a.inscattering)) == 0 &&
           std::memcmp(a.appendix, b.appendix, sizeof(a.appendix)) == 0 && a.lastField == b.lastField;
}

/// Right before the per-view loop: `held` is what the scene's first fog holds now (valid, fog, component, scene,
/// fields), which is by definition the game's. If the game's record is of this fog but holds other values, something
/// other than a push wrote the fog (the unthreaded inline setter, a world-origin shift, a fog re-made at the same
/// address): every view's record of it is stale and dropped. Either way the game's record becomes `held`. Returns
/// whether records were dropped.
inline bool FogBeforeViews(FogRec& game, FogRec (&fogs)[kRecords], const FogRec& held) noexcept
{
    const bool stale = game.valid && game.fog == held.fog && !SameFog(game, held);
    for (FogRec& r : fogs)
    {
        if (stale && r.fog == held.fog)
        {
            r.valid = 0;
        }
    }
    game       = held;
    game.state = nullptr;
    game.valid = 1;
    return stale;
}

/// Whether a view's fog differs from the game's (the reading and the log): the height, density, falloff, opacity
/// and both colours of the same fog.
inline bool FogDiffers(const FogRec& view, const FogRec& game) noexcept
{
    return view.valid && game.valid && view.fog == game.fog && view.component == game.component &&
           (std::memcmp(view.heightToOpacity, game.heightToOpacity, sizeof(view.heightToOpacity)) != 0 ||
            std::memcmp(view.inscattering, game.inscattering, sizeof(view.inscattering)) != 0 ||
            std::memcmp(view.appendix, game.appendix, sizeof(view.appendix)) != 0);
}

}  // namespace lss::render::area_chains
