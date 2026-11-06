#include "render/shadows_per_view.h"

#include "control/registry.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/vtable_entry.h"
#include "render/area_blend.h"
#include "render/area_chains.h"
#include "render/family_now.h"
#include "render/shadow_code.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace lss::render::shadows_per_view {

namespace {

namespace code = shadow_code;

code::State            g_state;  // read by the stubs on the render thread; switches written by the console
platform::CodeRedirect g_gather, g_decide, g_hide, g_casters, g_area, g_sun, g_fog;
bool                   g_lamps = false, g_castersIn = false, g_areas = false, g_sunIn = false, g_fogIn = false;  // installed

std::uint64_t Counter(std::uint64_t& value) noexcept
{
    return std::atomic_ref<std::uint64_t>(value).load(std::memory_order_relaxed);  // the stubs' plain increments
}

// Lever A: the tag (game thread) and the chains (the thread that runs the scene's setter: the render thread).
using StartFinalFn = void* (*)(void* view, void* location);
using SetBlendFn   = std::uint64_t (*)(void* scene, const void* data);
using SetBlendRtFn = std::uint64_t (*)(void* payload);
platform::CallRedirect g_startCall, g_rtTask, g_rtInline;
platform::VTableEntry  g_setBlend;
StartFinalFn           g_startFinal = nullptr;
SetBlendRtFn           g_setRt      = nullptr;
thread_local area_chains::Tag t_note;  // the view whose camera evaluation is pushing now (that thread's)
std::atomic<std::uint64_t> g_tagged{0}, g_untagged{0}, g_pushes{0};
// The chains' thread: the views whose settings differed from the game's at the last frame's end (read by the console).
std::array<std::atomic<bool>, code::kRecords> g_differed{};
std::uint64_t g_stubPasses = 0;   // the stubs' counters at the last push: a change means a frame was rendered since
int           g_lastIndex  = -1;  // the last tagged push's view: a lower one starts the next frame
bool          g_faultLogged = false;

// Lever A-fog: the tag (the scene's fog setter, game thread) and the records (its render-thread half).
using SetFogFn       = std::uint64_t (*)(void* scene, const void* data);
using FogConstantsFn = void (*)(void* renderer);
platform::CallRedirect g_fogRtTask, g_fogConstantsCall;
FogConstantsFn         g_fogConstants = nullptr;
platform::VTableEntry  g_setFog;
SetBlendRtFn           g_fogRt = nullptr;
std::atomic<std::uint64_t> g_fogTagged{0}, g_fogUntagged{0};
std::array<std::atomic<bool>, code::kRecords> g_fogDiffered{};  // the records' thread, as g_differed
bool g_fogFaultLogged = false;
bool g_fogStaleLogged = false;

/// The view's index in its family and its state, if it is one of the first views (a player's camera evaluation).
area_chains::Tag TagOf(const void* view) noexcept
{
    area_chains::Tag tag;
    const std::uint8_t* family = nullptr;
    void* const*        views  = nullptr;
    std::int32_t        count  = 0;
    if (!platform::Read(view, family) || family == nullptr || !platform::Read(family, views) ||
        !platform::Read(family + 8, count) || views == nullptr)
    {
        return tag;
    }
    for (std::int32_t i = 0; i < count && i < static_cast<std::int32_t>(code::kRecords); ++i)
    {
        const void* at = nullptr;
        if (platform::Read(views + i, at) && at == view)
        {
            tag.valid = platform::Read(static_cast<const std::uint8_t*>(view) + code::kViewState, tag.state);
            tag.index = static_cast<std::uint8_t>(i);
            break;
        }
    }
    return tag;
}

/// At CalcSceneView's call: notes the view for the pushes its blend makes, and gives the area controllers' fade timers
/// the view's own around it (render/area_blend.h).
void* StartFinal(void* view, void* location)
{
    t_note = TagOf(view);
    const void* state = t_note.valid ? t_note.state : nullptr;
    area_blend::Before(view, state);
    void* const result = g_startFinal(view, location);
    area_blend::After(view, state);
    t_note = {};
    return result;
}

/// FScene's setter: a noted view's push carries its tag in the data's padding (the game's command copies it whole).
std::uint64_t SetBlend(void* scene, const void* data)
{
    const auto original = reinterpret_cast<SetBlendFn>(g_setBlend.Original());
    std::uint8_t copy[area_chains::kBlendSize];
    if (t_note.valid && platform::CopyGuarded(copy, data, sizeof(copy)))
    {
        area_chains::Stamp(copy, t_note);
        g_tagged.fetch_add(1, std::memory_order_relaxed);
        return original(scene, copy);
    }
    g_untagged.fetch_add(1, std::memory_order_relaxed);
    return original(scene, data);
}

/// FScene's fog setter: a noted view's push carries its index in the data's padding (the game's command copies it whole).
std::uint64_t SetFog(void* scene, const void* data)
{
    const auto original = reinterpret_cast<SetFogFn>(g_setFog.Original());
    std::uint8_t copy[area_chains::kFogBlendSize];
    if (t_note.valid && platform::CopyGuarded(copy, data, sizeof(copy)))
    {
        area_chains::StampFog(copy, t_note);
        g_fogTagged.fetch_add(1, std::memory_order_relaxed);
        return original(scene, copy);
    }
    g_fogUntagged.fetch_add(1, std::memory_order_relaxed);
    return original(scene, data);
}

/// The scene's first fog, or null when it has none.
std::uint8_t* FirstFog(const std::uint8_t* scene) noexcept
{
    std::int32_t  count = 0;
    std::uint8_t* fog   = nullptr;
    return scene != nullptr && platform::Read(scene + code::kSceneFogCount, count) && count > 0 &&
                   platform::Read(scene + code::kSceneFogs, fog)
               ? fog
               : nullptr;
}

/// The fields the fog setter writes, and the fog's component, from `fog` into `r` (or back).
bool CopyFog(std::uint8_t* fog, code::FogRec& r, bool intoFog) noexcept
{
    bool done = intoFog || platform::Read(fog, r.component);
    for (std::size_t i = 0; done && i < 4; ++i)
    {
        std::uint8_t* const field = fog + code::kFogFieldSpans[i][0];
        done = intoFog ? platform::WriteGuarded(field, code::FogField(r, i), code::kFogFieldSpans[i][1])
                       : platform::CopyGuarded(code::FogField(r, i), field, code::kFogFieldSpans[i][1]);
    }
    return done;
}

/// InitFogConstants: the fog stub gives the scene's first fog each view's values as the loop reaches that view; what it
/// held before is the game's record for the stub's fallback, and is put back after, so everything later in the frame
/// (volumetric fog) reads the game's.
void FogConstants(void* renderer)  // the original returns a pointer no caller reads
{
    std::uint8_t* scene = nullptr;
    std::uint8_t* fog   = platform::Read(static_cast<const std::uint8_t*>(renderer) + 8, scene) ? FirstFog(scene) : nullptr;
    code::FogRec  saved;
    const bool    kept = fog != nullptr && CopyFog(fog, saved, false);
    if (kept)
    {
        saved.valid = 1;
        saved.fog   = fog;
        saved.scene = scene;
        if (area_chains::FogBeforeViews(g_state.fogGame, g_state.fogs, saved) && !g_fogStaleLogged)
        {
            g_fogStaleLogged = true;
            platform::LogDebug(L"shadows: the scene's height fog was changed outside the fog pushes (logged once); the "
                               L"views' fog records were dropped until their next push");
        }
    }
    g_fogConstants(renderer);
    if (kept && FirstFog(scene) == fog && !CopyFog(fog, saved, true) && !g_fogFaultLogged)
    {
        g_fogFaultLogged = true;
        platform::LogWarning(L"shadows: putting the scene's height fog back failed once (volumetric fog may use the last "
                             L"view's for a frame)");
    }
}

/// The fog setter's render-thread half: the game's write, then what it left in the scene's first fog kept as the game's
/// and as the pushing view's (every view's, for an untagged push).
std::uint64_t SetFogRt(void* payload)
{
    const std::uint64_t result = g_fogRt(payload);
    std::uint8_t*       scene  = nullptr;
    std::uint8_t* const fog    = platform::Read(payload, scene) ? FirstFog(scene) : nullptr;
    if (fog == nullptr)
    {
        return result;  // no fog: the game wrote nothing
    }
    code::FogRec now;
    now.valid = 1;
    now.fog   = fog;
    now.scene = scene;
    if (!CopyFog(fog, now, false))
    {
        if (!g_fogFaultLogged)
        {
            g_fogFaultLogged = true;
            platform::LogWarning(L"shadows: reading the scene's height fog failed once (the views' fog may be the last "
                                 L"camera's for a frame)");
        }
        return result;
    }
    area_chains::FogPush(g_state.fogGame, g_state.fogs, now,
                         area_chains::ReadFogTag(static_cast<const std::uint8_t*>(payload) + sizeof(void*)), g_state.records);
    return result;
}

/// The live scene for area_chains::Push: its 24 bytes, the sun's proxy fields, the game's setter on this push.
struct LiveScene
{
    std::uint8_t* scene   = nullptr;
    void*         payload = nullptr;
    mutable bool  faulted = false;

    std::uint8_t* Proxy() const noexcept
    {
        std::uint8_t* light = nullptr;
        std::uint8_t* proxy = nullptr;
        return platform::Read(scene + code::kSceneSunLight, light) && light != nullptr &&
                       platform::Read(light + code::kLightProxy, proxy)
                   ? proxy
                   : nullptr;
    }
    const void* Id() const noexcept { return scene; }
    const void* Sun() const noexcept { return Proxy(); }
    void        Snapshot(code::ViewRec& r) const noexcept
    {
        r.scene = scene;
        r.sun   = Proxy();
        faulted |= !platform::CopyGuarded(r.sceneFields, scene + code::kSceneFields, sizeof(r.sceneFields));
        r.sunDistance = 0;
        std::memset(r.cascades, 0, sizeof(r.cascades));
        std::memset(r.biasSharpen, 0, sizeof(r.biasSharpen));
        if (const std::uint8_t* proxy = static_cast<const std::uint8_t*>(r.sun))
        {
            faulted |= !platform::CopyGuarded(&r.sunDistance, proxy + code::kSunDistance, sizeof(r.sunDistance)) ||
                       !platform::CopyGuarded(r.cascades, proxy + code::kSunCascades, sizeof(r.cascades)) ||
                       !platform::CopyGuarded(r.biasSharpen, proxy + code::kSunBiasSharpen, sizeof(r.biasSharpen));
        }
    }
    void Apply(const code::ViewRec& r) const noexcept
    {
        faulted |= !platform::WriteGuarded(scene + code::kSceneFields, r.sceneFields, sizeof(r.sceneFields));
        std::uint8_t* proxy = Proxy();
        if (proxy != nullptr && r.sun == proxy)
        {
            faulted |= !platform::WriteGuarded(proxy + code::kSunDistance, &r.sunDistance, sizeof(r.sunDistance)) ||
                       !platform::WriteGuarded(proxy + code::kSunCascades, r.cascades, sizeof(r.cascades)) ||
                       !platform::WriteGuarded(proxy + code::kSunBiasSharpen, r.biasSharpen, sizeof(r.biasSharpen));
        }
    }
    void RunOriginal() const { g_setRt(payload); }
};

/// The scene setter's render-thread half (both callers): the push through the game's chain and its view's.
std::uint64_t SetBlendRt(void* payload)
{
    std::uint8_t* scene = nullptr;
    if (!platform::Read(payload, scene) || scene == nullptr)
    {
        return g_setRt(payload);
    }
    const area_chains::Tag tag = area_chains::ReadTag(static_cast<const std::uint8_t*>(payload) + sizeof(void*));
    // A frame's end: the stubs ran since the last push, or a view earlier in the family pushes again. The chains then
    // hold what that frame's views used: compared there, once a frame, the log names a change of state only.
    const std::uint64_t passes = Counter(g_state.areaOwn) + Counter(g_state.areaGame) + Counter(g_state.sunOwn) +
                                 Counter(g_state.sunGame) + Counter(g_state.fogOwn) + Counter(g_state.fogShared);
    if (passes != g_stubPasses || (tag.valid && tag.index < g_lastIndex))
    {
        for (std::size_t k = 0; k < code::kRecords; ++k)
        {
            const bool differs = area_chains::Differs(g_state.records[k], g_state.game);
            if (differs != g_differed[k].load(std::memory_order_relaxed))
            {
                g_differed[k].store(differs, std::memory_order_relaxed);
                float own = 0.0f, shared = 0.0f;
                std::memcpy(&own, g_state.records[k].sceneFields + 0x0C, sizeof(own));
                std::memcpy(&shared, g_state.game.sceneFields + 0x0C, sizeof(shared));
                platform::LogDebug(L"shadows: view {}'s area light settings now {} the ones the last camera set for every view "
                                   L"(its shadow distance {} {:.0f}; the last camera's {} {:.0f})",
                                   k + 1, differs ? L"differ from" : L"match",
                                   g_state.records[k].sceneFields[0x08] != 0 ? L"set by its area to" : L"default,", own,
                                   g_state.game.sceneFields[0x08] != 0 ? L"set to" : L"default,", shared);
            }
            const bool fogDiffers = area_chains::FogDiffers(g_state.fogs[k], g_state.fogGame);
            if (fogDiffers != g_fogDiffered[k].load(std::memory_order_relaxed))
            {
                g_fogDiffered[k].store(fogDiffers, std::memory_order_relaxed);
                float own = 0.0f, shared = 0.0f;  // fog +0x0C: the density as the setter stores it
                std::memcpy(&own, g_state.fogs[k].heightToOpacity + 4, sizeof(own));
                std::memcpy(&shared, g_state.fogGame.heightToOpacity + 4, sizeof(shared));
                platform::LogDebug(L"shadows: view {}'s height fog now {} the one the last camera set for every view (its "
                                   L"density {:.6f}; the last camera's {:.6f})",
                                   k + 1, fogDiffers ? L"differs from" : L"matches", own, shared);
            }
        }
    }
    g_stubPasses = passes;
    g_lastIndex  = tag.valid ? tag.index : g_lastIndex;
    LiveScene live{scene, payload};
    area_chains::Push(g_state.game, g_state.records, live, tag);
    g_pushes.fetch_add(1, std::memory_order_relaxed);
    if (live.faulted && !g_faultLogged)
    {
        g_faultLogged = true;
        platform::LogWarning(L"shadows: reading or writing the scene's light settings failed once (the views' settings may be "
                             L"off for a frame)");
    }
    return 0;  // no caller reads it (RE-shadows-per-view-build.md, build notes)
}

std::wstring Reading()
{
    const auto on = [](code::Lever lever) {
        return std::atomic_ref<std::uint8_t>(g_state.on[lever]).load(std::memory_order_relaxed) != 0;
    };
    std::wstring areas = std::format(
        L"area light settings: {}, the sun's {}; settings sent by a player's view {}, by anything else {}, kept per view {}; "
        L"light passes with the view's own settings {}, the shared ones {}; sun set-ups with the view's own {}, the shared {}; "
        L"views whose settings differ from the last camera's at the last frame's end:",
        !g_areas ? L"not installed" : on(code::kAreas) ? L"per view" : L"the game's (off)",
        !g_sunIn ? L"not installed" : on(code::kSun) ? L"per view" : L"the game's (off)", g_tagged.load(), g_untagged.load(),
        g_pushes.load(), Counter(g_state.areaOwn), Counter(g_state.areaGame), Counter(g_state.sunOwn), Counter(g_state.sunGame));
    for (std::size_t k = 0; k < code::kRecords; ++k)
    {
        if (g_differed[k].load(std::memory_order_relaxed))
        {
            areas += std::format(L" {}", k + 1);
        }
    }
    areas += std::format(L"; height fog: {}; fog sent by a player's view {}, by anything else {}; fog set-ups with the view's "
                         L"own {}, the shared {}; views whose fog differs from the last camera's:",
                         !g_fogIn ? L"not installed" : on(code::kFog) ? L"per view" : L"the game's (off)", g_fogTagged.load(),
                         g_fogUntagged.load(), Counter(g_state.fogOwn), Counter(g_state.fogShared));
    for (std::size_t k = 0; k < code::kRecords; ++k)
    {
        if (g_fogDiffered[k].load(std::memory_order_relaxed))
        {
            areas += std::format(L" {}", k + 1);
        }
    }
    return areas + std::format(L"; lamp shadows: {}; views beyond a lamp's shadow cutoff {}, within it {}; lamp shadows kept "
                               L"for a near view {}, skipped in a far view {}; characters' shadows: {}; characters judged from a "
                               L"camera other than player 1's {}",
                               !g_lamps ? L"not installed" : on(code::kLamps) ? L"per view" : L"the game's (off)",
                               Counter(g_state.beyondViews), Counter(g_state.withinViews), Counter(g_state.kept),
                               Counter(g_state.hidden), !g_castersIn ? L"not installed" : on(code::kCasters) ? L"per view" : L"the game's (off)",
                               Counter(g_state.nearCasters));
}

void RegisterSwitch(const wchar_t* name, const wchar_t* help, code::Lever lever)
{
    control::Register({.name  = name,
                       .shape = control::Shape::OnOff,
                       .help  = help,
                       .set   = [name, lever](const control::Value& value) {
                           const bool on = std::get<bool>(value);
                           std::atomic_ref<std::uint8_t>(g_state.on[lever]).store(on ? 1 : 0, std::memory_order_relaxed);
                           platform::Log(L"shadows: {} {} ({})", name, on ? L"on" : L"off", Reading());
                           return control::Outcome{};
                       },
                       .get = [lever] {
                           return control::Value{std::atomic_ref<std::uint8_t>(g_state.on[lever]).load(std::memory_order_relaxed) != 0};
                       }});
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    // The stubs read this DLL's state block: the DLL must outlive them (a restore can fail and leave one in place).
    if (!platform::PinThisModule())
    {
        platform::LogWarning(L"shadows: the mod could not keep itself loaded for the shadow code; shadows stay the game's");
        return false;
    }
    std::vector<std::uint8_t> areaCode, sunCode, fogCode, decideCode, gatherCode, hideCode, casterCode;
    try
    {
        g_state.on[code::kLamps]   = 1;
        g_state.on[code::kCasters] = 1;
        g_state.on[code::kAreas]   = 1;
        g_state.on[code::kSun]     = 1;
        g_state.on[code::kFog]     = 1;
        // A: the chains (the setter's render-thread half, both callers), the tag (the scene's setter and the view's call),
        // then the two stubs that read the chains. Each step only if the ones before it are in.
        const auto startCall = image.Rebase(code::kStartFinalCall, 5);
        const auto start     = image.Rebase(code::kStartFinal);
        const auto entry     = image.Rebase(code::kSceneBlendEntry, sizeof(void*));
        const auto setter    = image.Rebase(code::kSceneBlendSetter);
        const auto rt        = image.Rebase(code::kSceneBlendRt);
        const auto rtTask    = image.Rebase(code::kSceneBlendRtTask, 5);
        const auto rtInline  = image.Rebase(code::kSceneBlendRtInline, 5);
        const auto area      = image.Rebase(code::kAreaSite, sizeof(code::kAreaAdd));
        const bool aroundA   = image.BytesMatch(code::kAreaContextStart, code::kAreaContext) &&
                             image.BytesMatch(code::kSunContextStart, code::kSunContext);
        const auto areaBack  = image.Rebase(code::kAreaResume);
        const auto sun       = image.Rebase(code::kSunSite, sizeof(code::kSunAdd));
        const auto sunBack   = image.Rebase(code::kSunResume);
        const auto fogEntry  = image.Rebase(code::kSceneFogEntry, sizeof(void*));
        const auto fogSetter = image.Rebase(code::kSceneFogSetter);
        const auto fogRt     = image.Rebase(code::kSceneFogRt);
        const auto fogRtTask = image.Rebase(code::kSceneFogRtTask, 5);
        const auto fogCall   = image.Rebase(code::kFogConstantsCall, 5);
        const auto fogInit   = image.Rebase(code::kFogConstants);
        const auto fogSite   = image.Rebase(code::kFogSite, sizeof(code::kFogLoad));
        const auto fogBack   = image.Rebase(code::kFogResume);
        const auto gather     = image.Rebase(code::kGatherSite, sizeof(code::kGatherBytes));
        const auto gatherBack = image.Rebase(code::kGatherResume);
        const auto decide     = image.Rebase(code::kDecideSite, sizeof(code::kDecideBytes));
        const auto decideBack = image.Rebase(code::kDecideResume);
        const auto hide       = image.Rebase(code::kHideSite, sizeof(code::kHideAdd));
        const auto hideBack   = image.Rebase(code::kHideResume, sizeof(code::kHideResumeBytes));
        const auto notVisible = image.Rebase(code::kHideNotVisible);
        const auto caster     = image.Rebase(code::kCasterSite, sizeof(code::kCasterLoad));
        const auto casterBack = image.Rebase(code::kCasterResume, sizeof(code::kCasterTail));
        // Every buffer before any install: a failed allocation leaves nothing half placed.
        areaCode   = code::AreaCode(g_state, family_now::IndexAddress());
        sunCode    = code::SunCode(g_state, family_now::IndexAddress());
        fogCode    = code::FogCode(g_state, family_now::IndexAddress());
        decideCode = code::DecideCode(g_state);
        gatherCode = code::GatherCode(g_state);
        hideCode   = notVisible ? code::HideCode(g_state, *notVisible) : std::vector<std::uint8_t>{};
        casterCode = code::CasterCode(g_state);

        if (startCall && start && entry && setter && rt && rtTask && rtInline && area && areaBack && sun && sunBack && aroundA &&
            !areaCode.empty() && !sunCode.empty())
        {
            g_startFinal = reinterpret_cast<StartFinalFn>(*start);
            g_setRt      = reinterpret_cast<SetBlendRtFn>(*rt);
            const auto chains = reinterpret_cast<std::uintptr_t>(&SetBlendRt);
            g_areas = g_rtTask.Install(*rtTask, *rt, chains) && g_rtInline.Install(*rtInline, *rt, chains) &&
                      g_setBlend.Replace(*entry, *setter, reinterpret_cast<std::uintptr_t>(&SetBlend)) &&
                      g_startCall.Install(*startCall, *start, reinterpret_cast<std::uintptr_t>(&StartFinal)) &&
                      g_area.Install(*area, code::kAreaAdd, areaCode, *areaBack);
            if (!g_areas)
            {
                g_area.Restore();
                g_startCall.Restore();
                g_setBlend.Restore();
                g_rtInline.Restore();
                g_rtTask.Restore();
            }
            g_sunIn = g_areas && g_sun.Install(*sun, code::kSunAdd, sunCode, *sunBack);
        }
        // A-fog: the records (the setter's render-thread half), the tag, the put-back around the loop, then the stub; it
        // names a view's fog by the state of that view's light chain, so only with A in.
        if (g_areas && fogEntry && fogSetter && fogRt && fogRtTask && fogCall && fogInit && fogSite && fogBack &&
            !fogCode.empty() && image.BytesMatch(code::kFogContextStart, code::kFogContext))
        {
            g_fogRt        = reinterpret_cast<SetBlendRtFn>(*fogRt);
            g_fogConstants = reinterpret_cast<FogConstantsFn>(*fogInit);
            g_fogIn = g_fogRtTask.Install(*fogRtTask, *fogRt, reinterpret_cast<std::uintptr_t>(&SetFogRt)) &&
                      g_setFog.Replace(*fogEntry, *fogSetter, reinterpret_cast<std::uintptr_t>(&SetFog)) &&
                      g_fogConstantsCall.Install(*fogCall, *fogInit, reinterpret_cast<std::uintptr_t>(&FogConstants)) &&
                      g_fog.Install(*fogSite, code::kFogLoad, fogCode, *fogBack);
            if (!g_fogIn)
            {
                g_fog.Restore();
                g_fogConstantsCall.Restore();
                g_setFog.Restore();
                g_fogRtTask.Restore();
            }
        }
        if (gather && gatherBack && decide && decideBack && hide && hideBack && notVisible && !decideCode.empty() &&
            !gatherCode.empty() && !hideCode.empty() && image.BytesMatch(code::kGatherSite, code::kGatherBytes) &&
            image.BytesMatch(code::kDecideSite, code::kDecideBytes) && image.BytesMatch(code::kHideResume, code::kHideResumeBytes))
        {
            // The decision first (with the game's gather it reads only 0 or 1, as the game's), the hide next (it hides
            // nothing while the gather is the game's: a view beyond leaves no shadow to hide), the gather last.
            g_lamps = g_decide.Install(*decide, code::kDecideCompare, decideCode, *decideBack) &&
                      g_hide.Install(*hide, code::kHideAdd, hideCode, *hideBack) &&
                      g_gather.Install(*gather, code::kGatherAdd, gatherCode, *gatherBack);
            if (!g_lamps)
            {
                g_gather.Restore();
                g_hide.Restore();
                g_decide.Restore();
            }
        }
        if (caster && casterBack && !casterCode.empty() && image.BytesMatch(code::kCasterFlagCompare, code::kCasterFlagBytes) &&
            image.BytesMatch(code::kCasterResume, code::kCasterTail))
        {
            g_castersIn = g_casters.Install(*caster, code::kCasterLoad, casterCode, *casterBack);
        }
    }
    catch (...)
    {
        Remove();  // nothing is installed before every buffer is built; this only covers the unexpected
    }
    if (!g_areas)
    {
        platform::LogWarning(L"shadows: the area light settings' code could not be placed (not what the research read, or no "
                             L"room for its code); every view keeps the last camera's area light settings");
    }
    else if (!g_sunIn)
    {
        platform::LogWarning(L"shadows: the sun's cascade setup could not be placed; the sun's shadow distances stay the last "
                             L"camera's");
    }
    if (g_areas && !g_fogIn)
    {
        platform::LogWarning(L"shadows: the height fog's code could not be placed (not what the research read, or no room for "
                             L"its code); every view keeps the last camera's fog");
    }
    if (!g_lamps)
    {
        platform::LogWarning(L"shadows: the lamp shadows' code could not be placed; one far camera still removes a lamp's "
                             L"shadow from every view");
    }
    if (!g_castersIn)
    {
        platform::LogWarning(L"shadows: the characters' shadow code could not be placed; characters cast lamp shadows only near "
                             L"player 1's camera");
    }
    if (g_areas)
    {
        RegisterSwitch(L"shadows-areas",
                       L"each view's light and shadow distances from the areas its own camera stands in, not the last "
                       L"camera's (render/shadows_per_view.h; on)",
                       code::kAreas);
    }
    if (g_sunIn)
    {
        RegisterSwitch(L"shadows-sun",
                       L"each view's sun shadow distances (cascades, bias) from its own areas (render/shadows_per_view.h; on)",
                       code::kSun);
    }
    if (g_fogIn)
    {
        RegisterSwitch(L"fog-areas",
                       L"each view's height fog from the areas its own camera stands in, not the last camera's "
                       L"(render/shadows_per_view.h; on)",
                       code::kFog);
    }
    if (g_lamps)
    {
        RegisterSwitch(L"shadows-lamps",
                       L"a lamp's shadow per view: kept for a player near the lamp when another player's camera is far from it, "
                       L"skipped in the far view (render/shadows_per_view.h; on)",
                       code::kLamps);
    }
    if (g_castersIn)
    {
        RegisterSwitch(L"shadows-casters",
                       L"characters cast a lamp's shadow when near any player's camera, not only player 1's "
                       L"(render/shadows_per_view.h; on)",
                       code::kCasters);
    }
    control::Register({.name  = L"shadows",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the shadow decisions made per view and their counts since the start",
                       .get   = [] { return control::Value{Reading()}; }});
    platform::Log(L"shadows: {}{}{}{}{}",
                  g_areas ? L"each view's area light settings its own" : L"area light settings left as the game decides",
                  g_sunIn ? L" (the sun's too)" : L"", g_fogIn ? L"; each view's height fog its own" : L"",
                  g_lamps ? L"; lamp shadows decided per view" : L"; lamp shadows left as the game decides",
                  g_castersIn ? L"; characters' shadows judged from the nearest camera" : L"");
    return g_areas || g_lamps || g_castersIn;
}

void Remove() noexcept
{
    // Every switch off first: a stub whose restore fails then runs the game's own path, not frozen records.
    for (std::uint8_t& on : g_state.on)
    {
        std::atomic_ref<std::uint8_t>(on).store(0, std::memory_order_relaxed);
    }
    g_fog.Restore();
    g_fogConstantsCall.Restore();
    g_setFog.Restore();
    g_fogRtTask.Restore();
    g_sun.Restore();
    g_area.Restore();
    g_startCall.Restore();
    g_setBlend.Restore();
    g_rtInline.Restore();
    g_rtTask.Restore();
    g_casters.Restore();
    // The gather first; the decision stays if the gather could not be put back (the game's decision would read its 2 and 3
    // as "beyond" and drop shadows a near view needs).
    const bool gatherBack = g_gather.Restore();
    g_hide.Restore();
    if (gatherBack)
    {
        g_decide.Restore();
    }
    g_areas = g_sunIn = g_fogIn = g_lamps = g_castersIn = false;
}

}  // namespace lss::render::shadows_per_view
