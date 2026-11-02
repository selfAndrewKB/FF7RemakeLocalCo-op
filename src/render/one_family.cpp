#include "render/one_family.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "render/family_boundary.h"
#include "render/family_split.h"
#include "render/motion_history.h"
#include "render/view_places.h"

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <variant>

namespace lss::render::one_family {

namespace {

using SubmitFn = void (*)(void* rendererModule, void* canvas, std::uint8_t* family);

// Set once at install, before the slot is live; then only read (game thread: Draw and the menu's viewport submit there).
platform::VTableEntry g_slot;
SubmitFn              g_submit     = nullptr;
std::uintptr_t        g_drawReturn = 0;

std::atomic<bool>          g_on{true};
std::atomic<std::uint64_t> g_frames{0};      // game viewport frames split since the start
std::atomic<std::int32_t>  g_lastViews{0};   // the views of the last game viewport frame (split or not)
std::atomic<bool>          g_installed{false};

/// Same signature as FRendererModule::BeginRenderingViewFamily. Not inlined: the return address it reads is its caller's.
__declspec(noinline) void Submit(void* rendererModule, void* canvas, std::uint8_t* family) noexcept
{
    if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != g_drawReturn)
    {
        g_submit(rendererModule, canvas, family);  // the menu's viewport: untouched
        return;
    }
    family_boundary::Poll();
    view_places::Publish(family);  // every game viewport frame, split or not: each view's place by its state
    if (!g_on.load(std::memory_order_relaxed) || !family_boundary::Ready())
    {
        g_submit(rendererModule, canvas, family);
        return;
    }
    // Before each family: its place and its view's state for the render thread, the boundary reset before every family
    // after the first, and the motion table's step kept to the first family (render/motion_history.h). A failed queue
    // stops the split (family_split.h).
    const int made = family_split::SubmitOnePerView(
        family, [&](std::uint8_t* one) { g_submit(rendererModule, canvas, one); },
        [family](int k, int n, const void* view) {
            const void* state = nullptr;
            platform::CopyGuarded(&state, static_cast<const std::uint8_t*>(view) + facts::kViewState, sizeof(state));
            const motion_history::Step step = motion_history::For(family, k);
            if (!family_boundary::Queue({k, n, state}, k > 0, step.render))
            {
                return false;
            }
            motion_history::Arm(step);  // the submission follows at once
            return true;
        });
    if (made > 1)
    {
        // After the last family: no place, until the next split frame; the motion table's paused byte put back.
        family_boundary::Queue({}, false, motion_history::For(family, -1).render);
        g_frames.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_lastViews.exchange(made, std::memory_order_relaxed) != made)
    {
        platform::LogDebug(L"one family per player: the game viewport's frame is drawn as {} famil{}", made,
                           made == 1 ? L"y" : L"ies");
    }
}

control::Outcome Set(const control::Value& value)
try
{
    const bool on = std::get<bool>(value);
    if (g_on.exchange(on, std::memory_order_relaxed) != on)
    {
        platform::Log(L"one family per player: {}", on ? L"on (each player drawn as the first view of its own family)"
                                                       : L"off (the game's one family of every view)");
    }
    return {};
}
catch (...)
{
    return {false, L"the switch could not be set"};
}

control::Value Get()
try
{
    const std::int32_t last = g_lastViews.load();
    return control::Value{std::format(L"{}; {} frames split, the last frame {} famil{}; boundary: {}",
                                      g_on.load() ? L"on" : L"off", g_frames.load(), last, last == 1 ? L"y" : L"ies",
                                      family_boundary::Describe())};
}
catch (...)
{
    return control::Value{std::wstring{}};
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto original   = image.ReadPointer(facts::kBeginRenderingViewFamily.slot);
    const auto drawReturn = image.Rebase(facts::kDrawSubmitReturn);
    if (!original || !drawReturn || !image.BytesMatch(facts::kDrawSubmitCall, facts::kDrawSubmitCallBytes))
    {
        platform::LogWarning(L"one family per player: the game viewport's call differs; every player stays in one family");
        return false;
    }
    if (!family_boundary::Ready())
    {
        platform::LogWarning(L"one family per player: the family boundary is not installed; every player stays in one family");
        return false;
    }
    // Both set before the slot is live, so the replacement never runs without them; ReplaceSlot checks the original is
    // the function the research read.
    g_submit     = reinterpret_cast<SubmitFn>(*original);
    g_drawReturn = *drawReturn;
    if (!platform::ReplaceSlot(image, facts::kBeginRenderingViewFamily, reinterpret_cast<std::uintptr_t>(&Submit), g_slot))
    {
        platform::LogWarning(L"one family per player: the renderer's slot differs; every player stays in one family");
        return false;
    }
    g_installed.store(true);
    control::Register({.name  = L"one-family",
                       .shape = control::Shape::OnOff,
                       .help  = L"each player drawn as the first view of its own view family (on, at start), or every "
                                L"view in the game's one family (off, for an A/B) (render/one_family.h)",
                       .set   = &Set,
                       .get   = &Get});
    platform::Log(L"one family per player: installed (switch one-family on | off; on)");
    return true;
}

void Remove() noexcept
{
    if (!g_installed.exchange(false))
    {
        return;
    }
    g_on.store(false, std::memory_order_relaxed);  // a slot left in place is then a pass-through
    if (!g_slot.Restore())
    {
        platform::LogWarning(L"one family per player: the renderer's slot was changed after us; it is left as found");
    }
}

}  // namespace lss::render::one_family
