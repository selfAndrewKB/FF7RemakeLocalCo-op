#include "menu/coop_slider.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "engine_facts.h"
#include "menu/coop_menu_rules.h"
#include "menu/coop_page.h"
#include "menu/game_ui.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/unreal_call.h"

#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>

namespace lss::menu::coop_slider {

namespace {

namespace rules    = coop_menu_rules;
namespace settings = platform::settings;

using game_ui::At;
using platform::Read;
using platform::ReadPointer;
using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;

using MarkerFn     = void (*)(void* page, void* gauge, const std::uint8_t* info);
using FromIntFn    = void* (*)(void* out, std::int32_t value);
using ActionDownFn = std::uintptr_t (*)(void* page, void* a, void* b, void* event);

/// The reflected calls the slider makes, found at install.
struct Calls
{
    UFunction* setDesiredCount = nullptr;
    UFunction* setScrollOffset = nullptr;
    UFunction* getScrollOffset = nullptr;
    UFunction* getMaxOffset    = nullptr;
    UFunction* setPosition     = nullptr;
    UFunction* setText         = nullptr;
    UClass*    canvasSlot      = nullptr;  // the marker's slot class, checked before its setter runs
};

// Set at install and on the first tick, then only read.
Calls                  g_calls;
std::uintptr_t         g_marker = 0, g_fromInt = 0, g_openSite = 0, g_stepSite = 0, g_rowSite = 0;
std::uintptr_t         g_keyLeft = 0, g_keyRight = 0;  // the d-pad keys' FName globals (live addresses)
platform::CallRedirect g_open, g_step, g_row;
platform::VTableEntry  g_hold;
bool                   g_ready = false;

// Game thread.
int  g_readings = 0;      // the strip's readings still to log for this open (the open and its first step)
bool g_failSaid = false;  // a failed marker or label write logged for this open

/// An FString parameter as ProcessEvent's frame holds it: a view over the mod's text (the call copies it; nothing here
/// is freed by the game).
struct StringView
{
    const wchar_t* data;
    std::int32_t   num;
    std::int32_t   max;
};

bool Call(void* object, UFunction* function, void* params) noexcept
{
    return object != nullptr && function != nullptr &&
           players::GuardedProcessEvent(static_cast<UObject*>(object), function, params);
}

bool SetText(void* block, const wchar_t* text) noexcept
{
    const auto length = static_cast<std::int32_t>(std::wcslen(text)) + 1;
    StringView params{text, length, length};
    return Call(block, g_calls.setText, &params);
}

void IsCanvasSlot(void* object, bool* out) noexcept
try
{
    *out = static_cast<UObject*>(object)->IsA(g_calls.canvasSlot);
}
catch (...)
{
    *out = false;
}

void ClassText(void* object, std::wstring* out) noexcept
try
{
    *out = static_cast<UObject*>(object)->GetClassPrivate()->GetName();
}
catch (...)
{
    out->clear();
}

std::wstring ClassName(void* object) noexcept
{
    std::wstring text;
    return object != nullptr && platform::CallGuarded(&ClassText, object, &text) && !text.empty() ? text : L"(none)";
}

/// The strip's readings the build rests on (research/RE-options-gauge-scroll.md §6 check 1): its clipping, its
/// desired count, its scroll bar, its slot, the size it was last given and its scroll range.
void LogReadings(void* strip, const settings::Option& row) noexcept
{
    std::uint8_t clipping = 0xFF, bar = 0xFF, alwaysBar = 0xFF;
    std::int32_t desired  = -1;
    float        size[2]{-1.0f, -1.0f};
    float        maxOffset = -1.0f;
    Read(At(strip, facts::kWidgetClipping), clipping);
    Read(At(strip, facts::kScrollBoxDesired), desired);
    Read(At(strip, facts::kScrollBoxBar), bar);
    Read(At(strip, facts::kScrollBoxAlwaysBar), alwaysBar);
    if (void* const slate = ReadPointer(strip, facts::kScrollBoxSlate))
    {
        platform::CopyGuarded(size, At(slate, facts::kSlateScrollSize), sizeof size);
    }
    Call(strip, g_calls.getMaxOffset, &maxOffset);
    platform::LogDebug(L"Co-op slider: {} strip: clipping {}, desired count {}, scroll bar {} (always {}), slot {}, last "
                       L"given {:.1f} x {:.1f}, scroll range {:.1f}",
                       row.key, clipping, desired, bar, alwaysBar, ClassName(ReadPointer(strip, facts::kWidgetSlot)), size[0],
                       size[1], maxOffset);
}

/// The window, the marker and the label of the gauge the game has just updated: the mod's sliders only.
void Fit(void* page, void* gauge, const std::uint8_t* info, bool opened) noexcept
{
    std::uint8_t category = 0;
    std::int32_t min = 0, max = 0, value = 0;
    if (!coop_page::IsPage(page) || gauge == nullptr || gauge != ReadPointer(page, facts::kOptionsGauge) || info == nullptr ||
        !Read(info + facts::kOptionInfoCategory, category) || !Read(info + facts::kOptionInfoMin, min) ||
        !Read(info + facts::kOptionInfoMax, max) || !Read(At(page, facts::kOptionsEditValue), value))
    {
        return;
    }
    const auto option = rules::OptionOf(category);
    if (!option)
    {
        return;
    }
    const settings::Option& row = *settings::kOptions[static_cast<std::size_t>(*option)];
    void* const strip = ReadPointer(page, facts::kOptionsGaugeStrip);
    const int cells = max - min, marker = value - min;
    if (!rules::IsSlider(row) || strip == nullptr || cells <= 0 || marker < 0 || marker > cells || value < 0 ||
        value >= static_cast<int>(row.wordLabels.size()))
    {
        return;
    }
    if (opened)
    {
        g_readings = 2;
        g_failSaid = false;
    }
    // The window: the stock 10 cells, the call the game's shop list makes; then scrolled to keep the marker in view.
    std::int32_t desired = cells > rules::kSliderWindow ? rules::kSliderWindow : 0;
    Call(strip, g_calls.setDesiredCount, &desired);
    float offset = 0.0f;
    Call(strip, g_calls.getScrollOffset, &offset);
    const int was   = static_cast<int>(std::lround(offset / facts::kGaugePitch));
    const int first = rules::FirstVisible(marker, was, cells, rules::kSliderWindow);
    float     to    = static_cast<float>(first) * facts::kGaugePitch;
    Call(strip, g_calls.setScrollOffset, &to);
    // The marker, by the setter the game placed it with: its x within the window, its y as the game left it.
    void* const head = ReadPointer(gauge, facts::kGaugeHead);
    void* const slot = head != nullptr ? ReadPointer(head, facts::kWidgetSlot) : nullptr;
    bool        canvas = false;
    struct
    {
        float x, y;
    } position{static_cast<float>(marker - first) * facts::kGaugePitch + facts::kGaugeHeadX, 0.0f};
    const bool placed = slot != nullptr && platform::CallGuarded(&IsCanvasSlot, slot, &canvas) && canvas &&
                        Read(At(slot, facts::kCanvasSlotTop), position.y) && Call(slot, g_calls.setPosition, &position);
    const wchar_t* const word    = row.wordLabels[static_cast<std::size_t>(value)];
    const bool           labeled = SetText(ReadPointer(gauge, facts::kGaugeText), word);
    const bool failed = !placed || !labeled;
    if (opened || (failed && !g_failSaid))
    {
        g_failSaid = g_failSaid || failed;
        platform::LogDebug(L"Co-op slider: {} {} at {} (cell {} of {}), its window from cell {}{}{}", row.key,
                           opened ? L"opened" : L"stepped", word, marker, cells, first, placed ? L"" : L"; the marker not moved",
                           labeled ? L"" : L"; the label not written");
    }
    else
    {
        platform::LogTrace(L"Co-op slider: {} at {} (cell {} of {}), its window from cell {}", row.key, word, marker, cells, first);
    }
    if (g_readings > 0)
    {
        --g_readings;
        LogReadings(strip, row);
    }
}

/// The game's gauge update at the press (every row kind; the open of a Range row).
void Opened(void* page, void* gauge, const std::uint8_t* info)
{
    reinterpret_cast<MarkerFn>(g_open.Original())(page, gauge, info);
    Fit(page, gauge, info, true);
}

/// The game's gauge update after each Left/Right step.
void Stepped(void* page, void* gauge, const std::uint8_t* info)
{
    reinterpret_cast<MarkerFn>(g_step.Original())(page, gauge, info);
    Fit(page, gauge, info, false);
}

/// A Range row's value text: the value's word for the mod's sliders (the row's value read just answered by the page),
/// else the game's number.
void* RowValueText(void* out, std::int32_t value)
{
    const auto option = coop_page::TakeAnswered(value);
    if (option && out != nullptr)
    {
        const settings::Option& row = *settings::kOptions[static_cast<std::size_t>(*option)];
        if (rules::IsSlider(row) && value >= 0 && value < static_cast<std::int32_t>(row.wordLabels.size()) &&
            game_ui::AssignString(out, row.wordLabels[static_cast<std::size_t>(value)]))
        {
            return out;
        }
    }
    return reinterpret_cast<FromIntFn>(g_row.Original())(out, value);
}

/// The repeat flag of a held d-pad Left/Right on the mod's page while one of its sliders is open; null otherwise.
std::uint8_t* HeldStep(void* page, void* event) noexcept
{
    std::uint8_t  state = 0, repeat = 0;
    std::uint64_t key = 0, left = 0, right = 0;
    if (!g_ready || event == nullptr || !coop_page::IsPage(page) || !Read(At(page, facts::kOptionsState), state) ||
        state != facts::kOptionsStateRangeEdit || !Read(At(event, facts::kActionEventRepeat), repeat) || repeat != 1 ||
        !Read(At(event, facts::kActionEventKey), key) || !Read(reinterpret_cast<const void*>(g_keyLeft), left) ||
        !Read(reinterpret_cast<const void*>(g_keyRight), right) || (key != left && key != right))
    {
        return nullptr;
    }
    return static_cast<std::uint8_t*>(At(event, facts::kActionEventRepeat));
}

/// The page's action handler: a held Left/Right on the mod's open slider is passed as a press, so the game steps it at
/// the input's own repeat rate (0.2 s, then every 0.1 s), as its handler does every press. The event is put back after.
std::uintptr_t ActionDown(void* page, void* a, void* b, void* event)
{
    const auto          original = reinterpret_cast<ActionDownFn>(g_hold.Original());
    std::uint8_t* const repeat   = HeldStep(page, event);
    constexpr std::uint8_t kPress = 0, kRepeat = 1;
    if (repeat == nullptr || !platform::WriteGuarded(repeat, &kPress, sizeof kPress))
    {
        return original(page, a, b, event);
    }
    platform::LogTrace(L"Co-op slider: a held step");
    const std::uintptr_t reply = original(page, a, b, event);
    platform::WriteGuarded(repeat, &kRepeat, sizeof kRepeat);
    return reply;
}

template <typename T>
T* Find(const wchar_t* path)
{
    return RC::Unreal::UObjectGlobals::StaticFindObject<T*>(nullptr, nullptr, path);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    namespace f          = facts;
    const auto marker    = platform::FindFunction(image, f::kGaugeMarker);
    const auto fromInt   = platform::FindFunction(image, f::kFromInt);
    const auto openSite  = image.Rebase(f::kGaugeMarkerOpenCall, 5);
    const auto stepSite  = image.Rebase(f::kGaugeMarkerStepCall, 5);
    const auto rowSite   = image.Rebase(f::kRowValueTextCall, 5);
    g_calls.setDesiredCount = Find<UFunction>(f::kScrollBoxSetDesiredCount);
    g_calls.setScrollOffset = Find<UFunction>(f::kScrollBoxSetScrollOffset);
    g_calls.getScrollOffset = Find<UFunction>(f::kScrollBoxGetScrollOffset);
    g_calls.getMaxOffset    = Find<UFunction>(f::kScrollBoxGetMaxOffset);
    g_calls.setPosition     = Find<UFunction>(f::kCanvasSlotSetPosition);
    g_calls.setText         = Find<UFunction>(f::kTextBlockSetText);
    g_calls.canvasSlot      = Find<UClass>(f::kCanvasSlotClassPath);
    // Each call's frame is exactly its parameters: a function of another shape would read past it.
    const auto shaped = [](UFunction* function, std::int32_t bytes) {
        return function != nullptr && function->GetParmsSize() == bytes;
    };
    if (!marker || !fromInt || !openSite || !stepSite || !rowSite || !shaped(g_calls.setDesiredCount, 4) ||
        !shaped(g_calls.setScrollOffset, 4) || !shaped(g_calls.getScrollOffset, 4) || !shaped(g_calls.getMaxOffset, 4) ||
        !shaped(g_calls.setPosition, 8) || !shaped(g_calls.setText, sizeof(StringView)) || g_calls.canvasSlot == nullptr)
    {
        platform::LogWarning(L"Co-op slider: an engine check failed; the long rows stay lists");
        return false;
    }
    g_marker   = *marker;
    g_fromInt  = *fromInt;
    g_openSite = *openSite;
    g_stepSite = *stepSite;
    g_rowSite  = *rowSite;
    g_keyLeft  = image.Rebase(f::kKeyDPadLeft, 8).value_or(0);
    g_keyRight = image.Rebase(f::kKeyDPadRight, 8).value_or(0);
    return true;
}
catch (...)
{
    return false;
}

bool Place(const platform::GameImage& image) noexcept
{
    g_ready = g_marker != 0 && g_open.Install(g_openSite, g_marker, reinterpret_cast<std::uintptr_t>(&Opened)) &&
              g_step.Install(g_stepSite, g_marker, reinterpret_cast<std::uintptr_t>(&Stepped)) &&
              g_row.Install(g_rowSite, g_fromInt, reinterpret_cast<std::uintptr_t>(&RowValueText));
    if (!g_ready)
    {
        Unplace();
        platform::LogWarning(L"Co-op slider: a call could not be redirected; the long rows stay lists");
        return false;
    }
    // Holding Left/Right is its own: without it each press still steps once.
    const bool held = g_keyLeft != 0 && g_keyRight != 0 &&
                      platform::ReplaceSlot(image, facts::kOptionsActionDown, reinterpret_cast<std::uintptr_t>(&ActionDown),
                                            g_hold);
    platform::Log(L"Co-op slider: the long rows are the game's sliders, a window of {} cells scrolled with the value; {}",
                  rules::kSliderWindow,
                  held ? L"a held Left/Right steps on at the input's repeat rate"
                       : L"the page's action handler is not what the research read: one step per press");
    return true;
}

void Unplace() noexcept
{
    g_ready = false;
    g_hold.Restore();
    g_row.Restore();
    g_step.Restore();
    g_open.Restore();
}

bool Ready() noexcept
{
    return g_ready;
}

}  // namespace lss::menu::coop_slider
