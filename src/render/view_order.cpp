#include "render/view_order.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <variant>

namespace lss::render::view_order {

namespace {

using SubmitFn = void (*)(void* rendererModule, void* canvas, std::uint8_t* family);

// Set once at install, before the slot is live.
platform::VTableEntry g_slot;
SubmitFn              g_submit = nullptr;

std::atomic<bool>          g_swap{false};
std::atomic<std::uint64_t> g_swapped{0};  // families submitted swapped since the last change
std::atomic<bool>          g_installed{false};
bool                       g_warnedPutBack = false;  // game thread

/// The family's view list as it stood when swapped.
struct List
{
    const void** data   = nullptr;
    const void*  first  = nullptr;  // the game's Views[0]
    const void*  second = nullptr;  // the game's Views[1]
};

/// The family's list and its first two views, if it has two or more.
bool ReadList(const std::uint8_t* family, List& list) noexcept
{
    std::int32_t count = 0;
    return family != nullptr &&
           platform::CopyGuarded(&list.data, family + facts::kFamilyViews, sizeof(list.data)) &&
           platform::CopyGuarded(&count, family + facts::kFamilyViewCount, sizeof(count)) && list.data != nullptr &&
           count >= 2 && platform::CopyGuarded(&list.first, list.data, sizeof(list.first)) &&
           platform::CopyGuarded(&list.second, list.data + 1, sizeof(list.second));
}

/// Writes `a` and `b` into the list's first two entries. False if either write failed (the first is then put back to
/// `undo`).
bool WritePair(const void** data, const void* a, const void* b, const void* undo) noexcept
{
    if (!platform::WriteGuarded(data, &a, sizeof(a)))
    {
        return false;
    }
    if (!platform::WriteGuarded(data + 1, &b, sizeof(b)))
    {
        platform::WriteGuarded(data, &undo, sizeof(undo));
        return false;
    }
    return true;
}

/// Puts the game's order back, only where the list is still the one swapped and still holds the swapped pair.
void PutBack(const std::uint8_t* family, const List& swapped) noexcept
{
    List now;
    const bool same = ReadList(family, now) && now.data == swapped.data && now.first == swapped.second &&
                      now.second == swapped.first;
    if (same && WritePair(swapped.data, swapped.first, swapped.second, swapped.second))
    {
        return;
    }
    if (!g_warnedPutBack)
    {
        g_warnedPutBack = true;
        platform::LogWarning(L"view order: the family changed during the game's call, so its order was not put back "
                             L"(the view-order test should be set to stock)");
    }
}

/// The game's call for one family, with the first two views exchanged while the switch says so.
void Submit(void* rendererModule, void* canvas, std::uint8_t* family) noexcept
{
    List list;
    if (!g_swap.load(std::memory_order_relaxed) || !ReadList(family, list) ||
        !WritePair(list.data, list.second, list.first, list.first))
    {
        g_submit(rendererModule, canvas, family);
        return;
    }
    g_submit(rendererModule, canvas, family);
    PutBack(family, list);
    if (g_swapped.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        platform::LogDebug(L"view order: the first swapped family (its second view first)");
    }
}

control::Outcome Set(const control::Value& value)
try
{
    const bool swap = std::get<std::wstring>(value) == L"swap";
    if (g_swap.exchange(swap, std::memory_order_relaxed) != swap)
    {
        g_swapped.store(0, std::memory_order_relaxed);
        platform::Log(L"view order: {}", swap ? L"swap (the family's second view first, its first second)"
                                              : L"stock (the game's order)");
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
    return control::Value{std::format(L"{}; {} families swapped since the last change",
                                      g_swap.load() ? L"swap" : L"stock", g_swapped.load())};
}
catch (...)
{
    return control::Value{std::wstring{}};
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto original = image.ReadPointer(facts::kBeginRenderingViewFamily.slot);
    if (!original)
    {
        platform::LogWarning(L"view order: the renderer's slot is outside the game; the view-order test is not available");
        return false;
    }
    g_submit = reinterpret_cast<SubmitFn>(*original);  // ReplaceSlot checks it is the function the research read
    if (!platform::ReplaceSlot(image, facts::kBeginRenderingViewFamily, reinterpret_cast<std::uintptr_t>(&Submit), g_slot))
    {
        platform::LogWarning(L"view order: the renderer's slot differs; the view-order test is not available");
        return false;
    }
    g_installed.store(true);
    control::Register({.name  = L"view-order",
                       .shape = control::Shape::Word,
                       .words = {L"stock", L"swap"},
                       .help  = L"test: swap puts the second view of the family the game renders first (player 2's "
                                L"while player 2 is in) and the first second; stock leaves the game's order "
                                L"(render/view_order.h)",
                       .set   = &Set,
                       .get   = &Get});
    platform::Log(L"view order: installed (command view-order stock | swap; stock until set)");
    return true;
}

void Remove() noexcept
{
    if (!g_installed.exchange(false))
    {
        return;
    }
    g_swap.store(false, std::memory_order_relaxed);  // a slot left in place is then a pass-through
    if (!g_slot.Restore())
    {
        platform::LogWarning(L"view order: the renderer's slot was changed after the test; it is left as found");
    }
}

}  // namespace lss::render::view_order
