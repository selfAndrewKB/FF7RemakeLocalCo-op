#include "hud/ui_targets.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/image_entries.h"
#include "hud/overlay_pass.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "render/movie_frame.h"

#include <algorithm>
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

constexpr int kMaxColumns = overlay_pass::kMaxColumns;

// Render thread only.
struct RenderState
{
    void* views[kMaxColumns]   = {};       // the views whose UI passes ran this Render (by column)
    void* targets[kMaxColumns] = {};       // column c's target; the mod holds column 0's while column 1's is in the member
    int   count                = 0;        // views listed this Render (the highest column + 1)
    void* held                 = nullptr;  // the reference the mod owns (moved out of the member)
    int   composited           = 0;        // views post-processed since the last UI pass of column 0
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

/// The view's column: 0 for the view whose ViewRect.Min is (0, 0), column 0's in both split shapes (and the only
/// view); the next ones are counted in the order the UI loop runs them (bounded by the last column).
int ColumnOf(const void* view, std::int32_t (&rect)[4]) noexcept
{
    std::memset(rect, 0, sizeof(rect));
    if (view == nullptr ||
        !platform::CopyGuarded(rect, static_cast<const std::uint8_t*>(view) + facts::kViewRectMinX, sizeof(rect)))
    {
        return 0;
    }
    if (rect[0] == 0 && rect[1] == 0)
    {
        return 0;
    }
    for (int column = 1; column < g_render.count; ++column)
    {
        if (g_render.views[column] == view)
        {
            return column;  // a view passed again in this Render keeps its column
        }
    }
    return std::clamp(g_render.count, 1, kMaxColumns - 1);
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
    const int    column = ColumnOf(view, rect);
    void* const  before = Member();
    if (column == 0)
    {
        ReleaseHeld();  // a Render whose post-process loop did not run leaves one behind
        g_render = {};
        g_render.views[0] = view;
        g_render.count    = 1;
    }
    else
    {
        g_render.views[column] = view;
        g_render.count         = std::max(g_render.count, column + 1);
        // One target is moved out, for column 1's view (a third view keeps sampling the member: step 5).
        if (column == 1 && g_enabled.load(std::memory_order_relaxed) && g_render.views[0] != nullptr &&
            g_render.held == nullptr && before != nullptr)
        {
            g_render.targets[0] = before;
            g_render.held       = before;  // the reference moves with the pointer
            SetMember(nullptr);            // the game's allocator now gives this view a target of its own
        }
    }
    image_entries::BeginUiPass(g_enabled.load(std::memory_order_relaxed) ? column : -1);
    const std::uint64_t bound = reinterpret_cast<AllocBindFn>(call.Original())(sceneTargets, commandList);
    void* const after = Member();
    if (column == 0)
    {
        g_render.targets[0] = after;
    }
    else if (column == 1 && g_render.held != nullptr)
    {
        g_render.targets[1] = after;
        g_secondSeen.store(true, std::memory_order_relaxed);
    }
    if (Logging())
    {
        platform::Log(L"HUD targets: UI pass view {} (column {}, rect {},{}-{},{}) target {} -> {}{}", view, column, rect[0],
                      rect[1], rect[2], rect[3], before, after, column == 1 && g_render.held != nullptr ? L" (moved)" : L"");
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
    int column = -1;
    for (int i = 0; view != nullptr && i < g_render.count; ++i)
    {
        column = view == g_render.views[i] ? i : column;
    }
    void* member = Member();
    if (column >= 0 && g_render.held != nullptr && g_render.targets[column] != nullptr && member != g_render.targets[column] &&
        g_render.held == g_render.targets[column])
    {
        g_render.held = member;  // the other view's target, still referenced
        SetMember(g_render.targets[column]);
    }
    if (Logging())
    {
        platform::Log(L"HUD targets: post-process view {} (column {}) samples {}", view, column, Member());
    }
    // The movie frame's release at the end of this call runs only for the family's last view (render/movie_frame.h);
    // a view the UI loop did not list releases as the game does.
    render::movie_frame::BeginView(column < 0 || view == g_render.views[g_render.count - 1]);
    reinterpret_cast<PostProcessFn>(g_postCall.Original())(postProcessing, commandList, view, velocity);
    render::movie_frame::EndView();
    if (column >= 0 && ++g_render.composited >= g_render.count)
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
              .help  = L"log every UI pass and post-process call (view, column, target) for that many Renders",
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
