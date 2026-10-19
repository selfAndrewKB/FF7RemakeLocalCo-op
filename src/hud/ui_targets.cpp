#include "hud/ui_targets.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/image_entries.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace lss::hud::ui_targets {

namespace {

// Returns whether the target is bound: Render tests al and skips the view's UI draw when it is 0.
using AllocBindFn   = std::uint64_t (*)(void* sceneTargets, void* commandList);
using PostProcessFn = void (*)(void* postProcessing, void* commandList, void* view, void* velocity);
using ReleaseFn     = std::uint32_t (*)(void* pooled);

platform::CallRedirect g_allocCall;       // the UI loop's AllocBind (view in r8)
platform::CallRedirect g_allocClearCall;  // the clear-only path's AllocBind (view in r8)
platform::CallRedirect g_postCall;        // FPostProcessing::Process (view in r8)
std::uintptr_t         g_member = 0;      // &FSceneRenderTargets+0x110 (TRefCountPtr<IPooledRenderTarget>)
bool                   g_installed = false;

std::atomic<bool>     g_enabled{false};
std::atomic<int>      g_logFrames{0};
std::atomic<bool>     g_secondSeen{false};

// Render thread only.
struct RenderState
{
    void* views[2]   = {};       // the views whose UI passes ran this Render (by player)
    void* targets[2] = {};       // each view's target (one in the member, the other held by the mod)
    void* held       = nullptr;  // the reference the mod owns (moved out of the member)
    int   composited = 0;        // views post-processed since the last UI pass of player 1
};
RenderState g_render;

void* Member() noexcept
{
    void* value = nullptr;
    platform::CopyGuarded(&value, reinterpret_cast<const void*>(g_member), sizeof(value));
    return value;
}

void SetMember(void* value) noexcept
{
    platform::WriteGuarded(reinterpret_cast<void*>(g_member), &value, sizeof(value));
}

/// The view's ViewRect.Min is (0, 0) for player 1's view in both split shapes (and for the only view).
int PlayerOf(const void* view, std::int32_t (&rect)[4]) noexcept
{
    std::memset(rect, 0, sizeof(rect));
    if (view == nullptr ||
        !platform::CopyGuarded(rect, static_cast<const std::uint8_t*>(view) + facts::kViewRectMinX, sizeof(rect)))
    {
        return 0;
    }
    return rect[0] != 0 || rect[1] != 0 ? 1 : 0;
}

/// Releases the reference the mod holds (IPooledRenderTarget::Release, vt+0x30), if any.
void ReleaseHeld() noexcept
{
    void* pooled = g_render.held;
    g_render.held = nullptr;
    if (pooled == nullptr)
    {
        return;
    }
    void** vtable = nullptr;
    std::uintptr_t release = 0;
    if (!platform::CopyGuarded(&vtable, pooled, sizeof(vtable)) ||
        !platform::CopyGuarded(&release, reinterpret_cast<const std::uint8_t*>(vtable) + 0x30, sizeof(release)))
    {
        platform::LogWarning(L"HUD targets: could not read the held target's Release; the reference is dropped");
        return;
    }
    std::uint32_t left = 0;
    if (!platform::CallGuarded(reinterpret_cast<ReleaseFn>(release), left, pooled))
    {
        platform::LogWarning(L"HUD targets: the held target's Release faulted");
    }
}

bool Logging() noexcept
{
    return g_logFrames.load(std::memory_order_relaxed) > 0;
}

/// H1, both AllocBind sites.
std::uint64_t UiPass(void* sceneTargets, void* commandList, void* view, platform::CallRedirect& call)
{
    std::int32_t rect[4];
    const int    player = PlayerOf(view, rect);
    void* const  before = Member();
    if (player == 0)
    {
        ReleaseHeld();  // a Render whose post-process loop did not run leaves one behind
        g_render = {};
        g_render.views[0] = view;
    }
    else if (g_render.views[1] = view; g_enabled.load(std::memory_order_relaxed) && g_render.views[0] != nullptr &&
                                       g_render.held == nullptr && before != nullptr)
    {
        g_render.targets[0] = before;
        g_render.held       = before;  // the reference moves with the pointer
        SetMember(nullptr);            // the game's allocator now gives this view a target of its own
    }
    image_entries::BeginUiPass(g_enabled.load(std::memory_order_relaxed) ? player : -1);
    const std::uint64_t bound = reinterpret_cast<AllocBindFn>(call.Original())(sceneTargets, commandList);
    void* const after = Member();
    if (player == 0)
    {
        g_render.targets[0] = after;
    }
    else if (g_render.held != nullptr)
    {
        g_render.targets[1] = after;
        g_secondSeen.store(true, std::memory_order_relaxed);
    }
    if (Logging())
    {
        platform::Log(L"HUD targets: UI pass view {} (player {}, rect {},{}-{},{}) target {} -> {}{}", view, player + 1,
                      rect[0], rect[1], rect[2], rect[3], before, after, player == 1 && g_render.held != nullptr ? L" (moved)" : L"");
    }
    return bound;
}

std::uint64_t UiPassMain(void* sceneTargets, void* commandList, void* view)
{
    return UiPass(sceneTargets, commandList, view, g_allocCall);
}

std::uint64_t UiPassClearOnly(void* sceneTargets, void* commandList, void* view)
{
    return UiPass(sceneTargets, commandList, view, g_allocClearCall);
}

/// H4.
void PostProcess(void* postProcessing, void* commandList, void* view, void* velocity)
{
    image_entries::EndUiPasses();
    const int player = view == g_render.views[0] ? 0 : view == g_render.views[1] ? 1 : -1;
    void*     member = Member();
    if (player >= 0 && g_render.held != nullptr && g_render.targets[player] != nullptr && member != g_render.targets[player] &&
        g_render.held == g_render.targets[player])
    {
        g_render.held = member;  // the other view's target, still referenced
        SetMember(g_render.targets[player]);
    }
    if (Logging())
    {
        platform::Log(L"HUD targets: post-process view {} (player {}) samples {}", view, player + 1, Member());
    }
    reinterpret_cast<PostProcessFn>(g_postCall.Original())(postProcessing, commandList, view, velocity);
    if (player >= 0 && ++g_render.composited >= (g_render.views[1] != nullptr ? 2 : 1))
    {
        ReleaseHeld();  // the member keeps its own reference, which the game releases at the next Render start
        if (Logging() && g_logFrames.fetch_sub(1, std::memory_order_relaxed) == 1)
        {
            platform::Log(L"HUD targets: logging ends");
        }
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto alloc      = image.Rebase(facts::kUiAllocBindCall, 5);
    const auto allocClear = image.Rebase(facts::kUiAllocBindClearCall, 5);
    const auto allocFn    = image.Rebase(facts::kUiAllocBind);
    const auto post       = image.Rebase(facts::kPostProcessCall, 5);
    const auto postFn     = image.Rebase(facts::kPostProcessProcess);
    const auto targets    = image.Rebase(facts::kSceneRenderTargetsGlobal + facts::kSceneTargetsInGameUI, sizeof(void*));
    if (!alloc || !allocClear || !allocFn || !post || !postFn || !targets)
    {
        platform::Log(L"HUD targets: an address is outside the image; both halves keep one UI image");
        return false;
    }
    g_member = *targets;
    // The post-process call first: alone it only passes the view through.
    if (!g_postCall.Install(*post, *postFn, reinterpret_cast<std::uintptr_t>(&PostProcess)) ||
        !g_allocCall.Install(*alloc, *allocFn, reinterpret_cast<std::uintptr_t>(&UiPassMain)) ||
        !g_allocClearCall.Install(*allocClear, *allocFn, reinterpret_cast<std::uintptr_t>(&UiPassClearOnly)))
    {
        g_allocCall.Restore();
        g_postCall.Restore();
        platform::Log(L"HUD targets: Render's UI calls are not what the research read; both halves keep one UI image");
        return false;
    }
    g_installed = true;

    using control::Outcome;
    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name  = L"hud-targets",
              .shape = Shape::OnOff,
              .help  = L"each view samples its own UI target (render thread; the content follows the HUD image)",
              .set   = [](const Value& value) {
                  SetEnabled(std::get<bool>(value));
                  return Outcome{};
              },
              .get = [] { return Value{Enabled()}; }});
    Register({.name  = L"hud-targets-log",
              .shape = Shape::Integer,
              .min   = 0,
              .max   = 600,
              .help  = L"log every UI pass and post-process call (view, player, target) for that many Renders",
              .set   = [](const Value& value) {
                  g_logFrames.store(static_cast<int>(std::get<std::int64_t>(value)), std::memory_order_relaxed);
                  return Outcome{};
              },
              .get = [] { return Value{static_cast<std::int64_t>(g_logFrames.load(std::memory_order_relaxed))}; }});
    platform::Log(L"HUD targets: installed (hud-targets {})", Enabled() ? L"on" : L"off");
    return true;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_enabled.store(false, std::memory_order_relaxed);
    const bool ok = g_allocClearCall.Restore() & g_allocCall.Restore() & g_postCall.Restore();
    if (!ok)
    {
        platform::Log(L"HUD targets: a call was changed after us; left in place (it moves nothing while off)");
    }
    g_installed = !ok;
}

void SetEnabled(bool on) noexcept
{
    g_enabled.store(on, std::memory_order_relaxed);
}

bool Enabled() noexcept
{
    return g_enabled.load(std::memory_order_relaxed);
}

bool TakeSecondTargetSeen() noexcept
{
    return g_secondSeen.exchange(false, std::memory_order_relaxed);
}

}  // namespace lss::hud::ui_targets
