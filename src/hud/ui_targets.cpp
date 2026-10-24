#include "hud/ui_targets.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/image_entries.h"
#include "hud/overlay_pass.h"
#include "hud/target_rotation.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "render/family_now.h"
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
std::atomic<int>      g_playerColumns{overlay_pass::kMaxColumns};  // the columns with a HUD image of their own (game thread writes)

constexpr int kMaxColumns = overlay_pass::kMaxColumns;

// Render thread only.
struct RenderState
{
    void*                     views[kMaxColumns] = {};  // the views whose UI passes ran this Render (by column)
    int                       count              = 0;   // views listed this Render (the highest column + 1)
    target_rotation::Rotation rotation;                 // each column's target and which the mod holds (pure rule)
    int                       playerColumns      = overlay_pass::kMaxColumns;  // the columns with their own image, as of column 0's pass
    int                       composited         = 0;   // views post-processed since the last UI pass of column 0
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
    // A view beyond the columns built is listed nowhere: nothing moves for it, it samples the member as it stands.
    return g_render.count < kMaxColumns ? std::max(g_render.count, 1) : -1;
}

/// The HUD image a UI pass draws. In a split frame (render/family_now.h) each player's family has one view at (0, 0), so
/// ColumnOf calls every view column 0 and nothing moves: the game's own UI target is that family's. The image is then
/// the family's place: its player's column's, or column 0's for a mirror's column (it shows player 1's HUD, which it
/// draws itself, as it has no target to borrow). Otherwise the view's column.
int ImageColumn(int column, render::family_now::Family family) noexcept
{
    if (family.index < 0)
    {
        return column;
    }
    return family.index < g_render.playerColumns ? family.index : 0;
}

/// Releases one reference the mod holds (IPooledRenderTarget::Release, vt+0x30).
void ReleaseTarget(void* pooled) noexcept
{
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

/// Releases every reference the mod still holds and empties the Render's record.
void ReleaseHeld() noexcept
{
    void*     held[kMaxColumns] = {};
    const int n                 = g_render.rotation.TakeHeld(held);
    for (int i = 0; i < n; ++i)
    {
        ReleaseTarget(held[i]);
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
    const int    column  = ColumnOf(view, rect);
    void* const  before  = Member();
    const bool   enabled = g_enabled.load(std::memory_order_relaxed);  // read once: the move and the gate agree
    bool         moved   = false;
    if (column == 0)
    {
        ReleaseHeld();  // a Render whose post-process loop did not run leaves its held targets behind
        g_render = {};
        g_render.views[0]      = view;
        g_render.count         = 1;
        g_render.playerColumns = g_playerColumns.load(std::memory_order_relaxed);  // one rule for the whole Render
    }
    else if (column > 0)
    {
        g_render.views[column] = view;
        g_render.count         = std::max(g_render.count, column + 1);
        // The member's target (the previous column's) is moved out for a further column's view, the mod holding its
        // reference, so the game's allocator gives this view a target of its own (hud/target_rotation.h).
        moved = g_render.rotation.BeforeUiPass(column, before, enabled);
        if (moved)
        {
            SetMember(nullptr);
        }
    }
    image_entries::BeginUiPass(enabled ? ImageColumn(column, render::family_now::For(view)) : -1);
    const std::uint64_t bound = reinterpret_cast<AllocBindFn>(call.Original())(sceneTargets, commandList);
    void* const after = Member();
    g_render.rotation.AfterUiPass(column, after);
    if (column == 1 && moved)
    {
        g_secondSeen.store(true, std::memory_order_relaxed);
    }
    if (Logging())
    {
        const render::family_now::Family family = render::family_now::For(view);
        platform::Log(L"HUD targets: UI pass view {} (column {}, family {} of {}, rect {},{}-{},{}) target {} -> {}{}", view,
                      column, family.index, family.count, rect[0], rect[1], rect[2], rect[3], before, after,
                      moved ? L" (moved)" : L"");
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
    // The member names the target this view samples: its own column's, or column 0's for a mirror's column; the
    // one it named goes back to the held set (hud/target_rotation.h).
    if (void* wanted = g_render.rotation.BeforePostProcess(column, Member(), g_render.playerColumns); wanted != nullptr)
    {
        SetMember(wanted);
    }
    if (Logging())
    {
        platform::Log(L"HUD targets: post-process view {} (column {}) samples {}", view, column, Member());
    }
    // The movie frame's release at the end of this call runs only for the frame's last view (render/movie_frame.h): the
    // last family of a split frame, else the family's last view; a view the UI loop did not list releases as the game
    // does.
    const render::family_now::Family family = render::family_now::For(view);
    render::movie_frame::BeginView(family.index >= 0 ? family.index == family.count - 1
                                                     : column < 0 || view == g_render.views[g_render.count - 1]);
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

void SetPlayerColumns(int columns) noexcept
{
    g_playerColumns.store(std::clamp(columns, 1, kMaxColumns), std::memory_order_relaxed);
}

}  // namespace lss::hud::ui_targets
