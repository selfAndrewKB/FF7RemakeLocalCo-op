#include "hud/root_placement.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "engine_facts.h"
#include "hud/user_widgets.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "players/unreal_call.h"

#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lss::hud::root_placement {

namespace {

using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

struct Placement
{
    const wchar_t* className;
    float          anchorX;  // the corner of the band its 1920x1080 area goes to
    float          anchorY;
};
constexpr Placement kPlacements[] = {
    {L"NaviMap_C", 1.0f, 0.0f},  // top right: the minimap and the objective distance
    {L"Compass_C", 0.5f, 0.0f},  // top centre: the compass the minimap toggle swaps in for the minimap
    {L"Command_C", 0.0f, 1.0f},  // bottom left: the command menu and its prompt row
    {L"Status_C", 1.0f, 1.0f},   // bottom right: the party panel
};
constexpr std::uint32_t kScanTicks = 30;  // new roots are looked for this often; placed ones are checked every tick

/// A viewport slot: UUserWidget+0x1B0 anchors (min x, min y, max x, max y), +0x1C0 offsets (left, top, right/width,
/// bottom/height), +0x1D0 alignment.
struct ViewportLayout
{
    float anchors[4]   = {};
    float offsets[4]   = {};
    float alignment[2] = {};
    bool  operator==(const ViewportLayout&) const = default;
};
static_assert(facts::kUserWidgetOffset == facts::kUserWidgetAnchors + 16 && facts::kUserWidgetAlignment == facts::kUserWidgetOffset + 16);

struct Placed
{
    user_widgets::Widget widget;
    const Placement*     placement = nullptr;
    ViewportLayout       original;  // the game's own, set back when the split ends
    ViewportLayout       applied;   // what was set; anything else means the game has set it since
};

std::vector<Placed> g_placed;
std::uint32_t       g_ticks  = 0;
bool                g_active = false;

struct Setter
{
    UFunction*   function = nullptr;
    std::int32_t value    = -1;  // the struct parameter's offset in the frame
};
struct Setters
{
    Setter position, size, alignment, anchors;
};
std::optional<Setters> g_setters;
bool                   g_settersMissingLogged = false;

constexpr std::int32_t kFrameBytes = 64;

std::optional<Setter> FindSetter(const wchar_t* path, const wchar_t* param, std::int32_t size)
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
    if (function == nullptr || function->GetParmsSize() > kFrameBytes)
    {
        return std::nullopt;
    }
    auto* property = function->FindProperty(RC::Unreal::FName(param, RC::Unreal::FNAME_Find));
    if (property == nullptr || property->GetSize() != size || property->GetOffset_Internal() < 0 ||
        property->GetOffset_Internal() + size > function->GetParmsSize())
    {
        return std::nullopt;
    }
    return Setter{function, property->GetOffset_Internal()};
}

std::optional<Setters> FindSetters()
{
    const auto position  = FindSetter(STR("/Script/UMG.UserWidget:SetPositionInViewport"), STR("Position"), 8);
    const auto size      = FindSetter(STR("/Script/UMG.UserWidget:SetDesiredSizeInViewport"), STR("Size"), 8);
    const auto alignment = FindSetter(STR("/Script/UMG.UserWidget:SetAlignmentInViewport"), STR("Alignment"), 8);
    const auto anchors   = FindSetter(STR("/Script/UMG.UserWidget:SetAnchorsInViewport"), STR("Anchors"), 16);
    if (!position || !size || !alignment || !anchors)
    {
        return std::nullopt;
    }
    return Setters{*position, *size, *alignment, *anchors};
}

bool Call(UObject* widget, const Setter& setter, const float* values, std::size_t count) noexcept
{
    alignas(16) std::uint8_t frame[kFrameBytes]{};
    std::memcpy(frame + setter.value, values, count * sizeof(float));
    return players::GuardedProcessEvent(widget, setter.function, frame);
}

/// The whole layout through the game's setters. SetPositionInViewport (and the size setter) reset the anchors, so the
/// anchors go last.
bool Apply(UObject* widget, const ViewportLayout& layout) noexcept
{
    const float position[2] = {layout.offsets[0], layout.offsets[1]};
    const float size[2]     = {layout.offsets[2], layout.offsets[3]};
    return Call(widget, g_setters->position, position, 2) && Call(widget, g_setters->size, size, 2) &&
           Call(widget, g_setters->alignment, layout.alignment, 2) && Call(widget, g_setters->anchors, layout.anchors, 4);
}

std::optional<ViewportLayout> ReadLayout(const UObject* widget) noexcept
{
    float raw[10] = {};
    if (!platform::CopyGuarded(raw, reinterpret_cast<const std::uint8_t*>(widget) + facts::kUserWidgetAnchors, sizeof(raw)))
    {
        return std::nullopt;
    }
    ViewportLayout layout;
    std::memcpy(layout.anchors, raw, sizeof(layout.anchors));
    std::memcpy(layout.offsets, raw + 4, sizeof(layout.offsets));
    std::memcpy(layout.alignment, raw + 8, sizeof(layout.alignment));
    return layout;
}

/// The 1920x1080 area at the placement's corner.
ViewportLayout AreaAt(const Placement& placement) noexcept
{
    ViewportLayout layout;
    layout.anchors[0] = layout.anchors[2] = placement.anchorX;
    layout.anchors[1] = layout.anchors[3] = placement.anchorY;
    layout.offsets[2]                     = 1920.0f;
    layout.offsets[3]                     = 1080.0f;
    layout.alignment[0]                   = placement.anchorX;
    layout.alignment[1]                   = placement.anchorY;
    return layout;
}

/// The single-player fill these roots carry (full anchors, zero offsets): only that layout is replaced.
bool IsFill(const ViewportLayout& layout) noexcept
{
    return layout.anchors[0] == 0.0f && layout.anchors[1] == 0.0f && layout.anchors[2] == 1.0f && layout.anchors[3] == 1.0f &&
           layout.offsets[0] == 0.0f && layout.offsets[1] == 0.0f && layout.offsets[2] == 0.0f && layout.offsets[3] == 0.0f;
}

// Keyed by the class pointer for the process: the HUD's widget classes load with the game and stay [ASSUMPTION: never
// unloaded in play]; a class freed and its address reused would be judged by the old class's name (IsFill still gates).
std::unordered_map<const void*, const Placement*> g_placementOfClass;

const Placement* PlacementOf(UObject* widget)
{
    const void* cls = widget->GetClassPrivate();
    if (const auto found = g_placementOfClass.find(cls); found != g_placementOfClass.end())
    {
        return found->second;
    }
    const std::wstring name(widget->GetClassPrivate()->GetName());
    const Placement*   match = nullptr;
    for (const auto& placement : kPlacements)
    {
        if (name == placement.className)
        {
            match = &placement;
        }
    }
    g_placementOfClass.emplace(cls, match);
    return match;
}

bool Listed(const UObject* widget) noexcept
{
    for (const auto& placed : g_placed)
    {
        if (placed.widget.object == widget)
        {
            return true;
        }
    }
    return false;
}

void PlaceNew()
{
    for (const auto& widget : user_widgets::Live())
    {
        const Placement* placement = PlacementOf(widget.object);
        if (placement == nullptr || Listed(widget.object))
        {
            continue;
        }
        const auto layout = ReadLayout(widget.object);
        if (!layout || !IsFill(*layout))
        {
            platform::Log(L"HUD placement: {} has a viewport layout this placement does not expect; left alone",
                          placement->className);
            g_placed.push_back({widget, nullptr, {}, {}});  // remembered, so it is not looked at again
            continue;
        }
        const ViewportLayout area = AreaAt(*placement);
        if (!Apply(widget.object, area))
        {
            Apply(widget.object, *layout);
            platform::Log(L"HUD placement: a setter raised an exception for {}; its own layout set back", placement->className);
            g_placed.push_back({widget, nullptr, {}, {}});
            continue;
        }
        const auto applied = ReadLayout(widget.object);
        g_placed.push_back({widget, placement, *layout, applied.value_or(area)});
        platform::LogDebug(L"HUD placement: {} gets a 1920x1080 area at ({:.0f},{:.0f}) of the half", placement->className,
                           placement->anchorX, placement->anchorY);
    }
}

void RestoreAll()
{
    int restored = 0, left = 0;
    for (auto& placed : g_placed)
    {
        if (placed.placement == nullptr || !user_widgets::Alive(placed.widget))
        {
            continue;
        }
        const auto now = ReadLayout(placed.widget.object);
        if (now && *now == placed.applied && Apply(placed.widget.object, placed.original))
        {
            ++restored;
        }
        else
        {
            ++left;
        }
    }
    g_placed.clear();
    platform::Log(L"HUD placement: {} root(s) set back to the game's own viewport layout{}", restored,
                  left > 0 ? std::format(L"; {} set by the game since, left with the game's", left) : std::wstring());
}

}  // namespace

void Tick(bool bandApplies) noexcept
try
{
    ++g_ticks;
    if (!bandApplies)
    {
        if (g_active)
        {
            g_active = false;
            RestoreAll();
        }
        return;
    }
    if (!g_setters)
    {
        g_setters = FindSetters();
        if (!g_setters)
        {
            if (!g_settersMissingLogged)
            {
                g_settersMissingLogged = true;
                platform::Log(L"HUD placement: UUserWidget's viewport setters not found; the roots keep the centred box");
            }
            return;
        }
    }
    if (!g_active || g_ticks % kScanTicks == 0)
    {
        g_active = true;
        PlaceNew();
    }
    // A root the game laid out again is placed again from the game's new layout, if it is still the fill.
    for (auto& placed : g_placed)
    {
        if (placed.placement == nullptr || !user_widgets::Alive(placed.widget))
        {
            continue;
        }
        const auto now = ReadLayout(placed.widget.object);
        if (now && !(*now == placed.applied) && IsFill(*now))
        {
            const ViewportLayout area = AreaAt(*placed.placement);
            if (Apply(placed.widget.object, area))
            {
                placed.original = *now;
                placed.applied  = ReadLayout(placed.widget.object).value_or(area);
                platform::LogDebug(L"HUD placement: {} was laid out again by the game; placed again", placed.placement->className);
            }
        }
    }
    std::erase_if(g_placed, [](const Placed& placed) { return !user_widgets::Alive(placed.widget); });
}
catch (...)
{
    platform::Log(L"HUD placement: failed (an exception); left as it is");
}

void Remove() noexcept
try
{
    if (g_active)
    {
        g_active = false;
        RestoreAll();
    }
}
catch (...)
{
}

}  // namespace lss::hud::root_placement
