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

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/overlay_pass.h"
#include "hud/user_widgets.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "players/seats.h"
#include "players/unreal_call.h"
#include "views/whole_screen.h"
#include "views/window_span.h"

#include <array>
#include <atomic>
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
    bool           glow = false;  // the widget whose tick places the UI particles: their boxes follow its corner
};
constexpr Placement kPlacements[] = {
    {L"NaviMap_C", 1.0f, 0.0f},  // top right: the minimap and the objective distance
    {L"Compass_C", 0.5f, 0.0f},  // top centre: the compass the minimap toggle swaps in for the minimap
    {L"Command_C", 0.0f, 1.0f},  // bottom left: the command menu and its prompt row
    {L"Status_C", 1.0f, 1.0f},   // bottom right: the party panel
    // Bottom right, above the party panel: the summon gauges, two roots the field command controller makes at a summon's
    // first show, ordinary default-fill roots placed by their Blueprint design alone (research/RE-hud-families.md). The
    // time gauge's own tick places the glow's particles from design points (hud/ui_effect_pass.h).
    {L"SummonChantGauge_C", 1.0f, 1.0f},        // the "SUMMON" bar that fills during a fight
    {L"SummonTimeGauge_C", 1.0f, 1.0f, true},   // the summoned creature's name and its time left
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

/// A root's render transform as far as the HUD size touches it: UWidget+0x98 RenderTransform.Scale, +0xAC
/// RenderTransformPivot (a fraction of the widget's own size), and the translation the pivot accounts for (+0x90,
/// RenderTransform.Translation: the summon gauges' own setup moves them into place with it), read only.
struct RenderScale
{
    float scale[2]       = {1.0f, 1.0f};
    float pivot[2]       = {0.5f, 0.5f};
    float translation[2] = {};

    bool SameScaleAndPivot(const RenderScale& other) const noexcept
    {
        return scale[0] == other.scale[0] && scale[1] == other.scale[1] && pivot[0] == other.pivot[0] && pivot[1] == other.pivot[1];
    }
    bool SameTranslation(const RenderScale& other) const noexcept
    {
        return translation[0] == other.translation[0] && translation[1] == other.translation[1];
    }
};

struct Placed
{
    user_widgets::Widget widget;
    const Placement*     placement = nullptr;
    ViewportLayout       original;  // the game's own, set back when the split ends
    ViewportLayout       applied;   // what was set; anything else means the game has set it since
    RenderScale          gameScale;           // the game's render scale and pivot, set back when no size is held and when the split ends
    RenderScale          sized;               // what the tick set, with the translation it was set for
    float                size      = 1.0f;    // player 1's HUD size set on the root (1 also while held at scale 1)
    bool                 held      = false;   // the mod's pivot and scale on the root (else the game's transform)
    float                passScale = 1.0f;    // the scale the last pass (or the tick) set on it
    bool                 passSkipped  = false;  // this frame's passes leave it alone: the game set its scale meanwhile
    bool                 toldPassSkip = false;  // a pass's skip of this root has been logged
    bool                 sizeLeft  = false;   // a setter failed on this root: the HUD size leaves it alone
    bool                 toldOwnScale = false;  // the game's own scale on this root has been logged
    bool                 toldTranslation = false;  // the root's own translation has been logged
};

std::vector<Placed> g_placed;
std::uint32_t       g_ticks  = 0;
bool                g_active = false;

// The HUD size (the settings file's [Views] hud-scale and its players' keys; research/RE-hud-scale-option.md,
// RE-hud-size-per-player.md): one factor per player for their view's pinned roots in a split window, each root scaled
// about the corner it is anchored to, so it stays in that corner of its view. The roots are one set painted again in
// each view's pass, so the tick holds them at player 1's size and each further pass sets its own player's scale just
// before its paint (SizeForPass), player 1's back before the game's own pass. World markers are other roots, not listed
// here, and keep the game's size. Set by the console and the Co-op page; read by the placement and the glow's box.
constexpr int kSizedPlayers = players::kSeatsBuilt + 1;
constexpr std::array<const platform::settings::Option*, kSizedPlayers> kSizeOptions{
    &platform::settings::kHudScale, &platform::settings::kHudScaleTwo, &platform::settings::kHudScaleThree};
std::array<std::atomic<float>, kSizedPlayers> g_hudScale{1.0f, 1.0f, 1.0f};
std::array<float, kSizedPlayers>              g_sizeLogged{-1.0f, -1.0f, -1.0f};  // the sizes the last line reported in effect
// Game thread: the sizes in effect as this frame's tick took them; the passes use these, never a setting changed since
// (the next tick moves the roots to it), so the frame ends at the scale the tick compares with.
std::array<float, kSizedPlayers> g_tickSizes{1.0f, 1.0f, 1.0f};
// Game thread: the size each column's pass gave the roots this frame, the glow's root then (0: not held) and the size
// each further column's line last reported (0: not yet).
std::array<float, overlay_pass::kMaxColumns> g_columnSize{};
std::array<float, overlay_pass::kMaxColumns> g_columnGlow{};
std::array<float, overlay_pass::kMaxColumns> g_columnSaid{};
std::uint32_t                                g_passSets = 0;  // the per-pass setter calls this frame

/// A seat as an index of the per-player arrays (player 1's for any other).
std::size_t PlayerIndex(int seat) noexcept
{
    return static_cast<std::size_t>(seat >= 0 && seat < kSizedPlayers ? seat : 0);
}

/// The HUD size's words, "0.75" to "1.50", as factors.
std::optional<float> FactorOf(const std::wstring& word) noexcept
{
    for (std::size_t i = 0; i < platform::settings::kHudScaleWords.size(); ++i)
    {
        if (word == platform::settings::kHudScaleWords[i])
        {
            return platform::settings::HudScaleOf(i);
        }
    }
    return std::nullopt;
}

/// The HUD size step whose factor is exactly this one (as the size sets it and reads it back), if any.
std::optional<std::size_t> StepOfFactor(float factor) noexcept
{
    for (std::size_t i = 0; i < platform::settings::kHudScaleWords.size(); ++i)
    {
        if (platform::settings::HudScaleOf(i) == factor)
        {
            return i;
        }
    }
    return std::nullopt;
}

std::wstring WordOfFactor(float factor)
{
    const auto step = StepOfFactor(factor);
    return step ? std::wstring(platform::settings::kHudScaleWords[*step]) : std::format(L"{:.2f}", factor);
}

struct Setter
{
    UFunction*   function = nullptr;
    std::int32_t value    = -1;  // the struct parameter's offset in the frame
};
struct Setters
{
    Setter position, size, alignment, anchors, renderScale, renderPivot;
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
    const auto scale     = FindSetter(STR("/Script/UMG.Widget:SetRenderScale"), STR("Scale"), 8);
    const auto pivot     = FindSetter(STR("/Script/UMG.Widget:SetRenderTransformPivot"), STR("Pivot"), 8);
    if (!position || !size || !alignment || !anchors || !scale || !pivot)
    {
        return std::nullopt;
    }
    return Setters{*position, *size, *alignment, *anchors, *scale, *pivot};
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

std::optional<RenderScale> ReadScale(const UObject* widget) noexcept
{
    const auto* base = reinterpret_cast<const std::uint8_t*>(widget);
    RenderScale scale;
    if (!platform::CopyGuarded(scale.scale, base + facts::kWidgetRenderScale, sizeof(scale.scale)) ||
        !platform::CopyGuarded(scale.pivot, base + facts::kWidgetRenderPivot, sizeof(scale.pivot)) ||
        !platform::CopyGuarded(scale.translation, base + facts::kWidgetRenderTranslation, sizeof(scale.translation)))
    {
        return std::nullopt;
    }
    return scale;
}

/// The scale and pivot through the game's setters, which hand the transform to the root's Slate widget (the
/// translation stays the game's).
bool ApplyScale(UObject* widget, const RenderScale& scale) noexcept
{
    return Call(widget, g_setters->renderPivot, scale.pivot, 2) && Call(widget, g_setters->renderScale, scale.scale, 2);
}

/// After a setter's exception: the root's own transform back, and the HUD size leaves it for the rest of the split.
void LeaveToGame(Placed& placed) noexcept
{
    ApplyScale(placed.widget.object, placed.gameScale);
    placed.size     = 1.0f;
    placed.held     = false;
    placed.sizeLeft = true;
}

/// The root at `size` about its placement's corner, its translation `now` scaled with it (hud/overlay_band.h
/// CornerPivot); none for an area without a size.
std::optional<RenderScale> ScaleAt(const Placed& placed, const RenderScale& now, float size) noexcept
{
    const auto pivot = CornerPivot({placed.placement->anchorX, placed.placement->anchorY}, {now.translation[0], now.translation[1]},
                                   placed.applied.offsets[2], placed.applied.offsets[3]);
    if (!pivot)
    {
        return std::nullopt;
    }
    return RenderScale{{size, size}, {pivot->x, pivot->y}, {now.translation[0], now.translation[1]}};
}

/// Sets a placed root to player 1's HUD size: held (`hold`: some player's size is not 1) at `size` about its corner with
/// its translation, or the game's transform back. While the game scales a root itself (its render scale not 1), the
/// root is left at the game's transform and looked at again each tick; one whose scale or pivot the game set since it
/// was held counts as unheld from then on; one whose translation the game moved since gets its pivot again.
void SizeRoot(Placed& placed, float size, bool hold)
{
    if (placed.placement == nullptr || placed.sizeLeft || !user_widgets::Alive(placed.widget))
    {
        return;
    }
    const auto now = ReadScale(placed.widget.object);
    if (!now)
    {
        return;
    }
    // A held root still at the scale a view's pass gave it, with the mod's pivot: a frame whose last pass did not run
    // (an exception in the render). The mod's own scale, set back to player 1's below, never taken for the game's.
    const bool leftByPass = placed.held && now->scale[0] == placed.passScale && now->scale[1] == placed.passScale &&
                            placed.passScale != placed.sized.scale[0] && now->pivot[0] == placed.sized.pivot[0] &&
                            now->pivot[1] == placed.sized.pivot[1];
    if (leftByPass)
    {
        platform::Log(L"HUD size: {} was left at a view's scale x{:.2f} by a frame whose last pass did not run; set back to "
                      L"player 1's",
                      placed.placement->className, placed.passScale);
    }
    bool moved     = false;
    bool gameSetIt = false;  // the game set the held root's transform since: its scale is the game's, never a copy's
    if (placed.held && !leftByPass && !now->SameScaleAndPivot(placed.sized))
    {
        placed.held = false;
        placed.size = 1.0f;
        gameSetIt   = true;
        if (!placed.toldOwnScale)
        {
            placed.toldOwnScale = true;
            platform::Log(L"HUD size: {}'s render transform was set by the game since; left with the game's while it "
                          L"scales the root (logged once per root)",
                          placed.placement->className);
        }
    }
    else if (placed.held && !now->SameTranslation(placed.sized))
    {
        moved = true;
        platform::LogDebug(L"HUD size: {} was moved by the game ({:.0f},{:.0f}); its pivot follows", placed.placement->className,
                           now->translation[0], now->translation[1]);
    }
    // A root made as a copy of a sized one (each seat's summon gauges are made from player 1's) carries its render scale:
    // an unheld root's uniform scale that is exactly one of the HUD sizes is the mod's, and the root's own scale is 1.
    const bool copied = !placed.held && !gameSetIt && now->scale[0] == now->scale[1] && now->scale[0] != 1.0f &&
                        StepOfFactor(now->scale[0]).has_value();
    if (hold == placed.held && (!hold || size == placed.size) && !moved && !copied && !leftByPass)
    {
        return;
    }
    if (!placed.held)
    {
        placed.gameScale = *now;
        if (copied)
        {
            placed.gameScale.scale[0] = placed.gameScale.scale[1] = 1.0f;
            platform::Log(L"HUD size: {} came with the HUD size's scale x{:.2f} (made from a sized root); taken as the mod's, "
                          L"its own scale 1",
                          placed.placement->className, now->scale[0]);
        }
        else if (now->scale[0] != 1.0f || now->scale[1] != 1.0f)
        {
            if (!placed.toldOwnScale)
            {
                placed.toldOwnScale = true;
                platform::Log(L"HUD size: {} has a render scale of its own ({:.2f}x{:.2f}); left at the game's while it "
                              L"does (logged once per root)",
                              placed.placement->className, now->scale[0], now->scale[1]);
            }
            return;
        }
    }
    const auto wanted = !hold ? std::optional(placed.gameScale) : ScaleAt(placed, *now, size);
    if (!wanted)
    {
        return;
    }
    if (ApplyScale(placed.widget.object, *wanted))
    {
        if (hold && (now->translation[0] != 0.0f || now->translation[1] != 0.0f) && !placed.toldTranslation)
        {
            placed.toldTranslation = true;
            platform::Log(L"HUD size: {} carries a translation of its own ({:.0f},{:.0f}); its pivot ({:.3f},{:.3f}) scales it "
                          L"about the corner with the root",
                          placed.placement->className, now->translation[0], now->translation[1], wanted->pivot[0], wanted->pivot[1]);
        }
        placed.held      = hold;
        placed.size      = hold ? size : 1.0f;
        placed.sized     = *wanted;
        placed.passScale = placed.size;
        return;
    }
    LeaveToGame(placed);
    platform::Log(L"HUD size: a setter raised an exception for {}; its own transform set back", placed.placement->className);
}

std::wstring SizeReading()
{
    std::wstring text = std::format(L"settings x{:.2f}, x{:.2f}, x{:.2f}; in effect x{:.2f}, x{:.2f}, x{:.2f}; this frame's passes "
                                    L"x{:.2f}, x{:.2f}, x{:.2f} ({} setter calls); {} root(s)",
                                    g_hudScale[0].load(std::memory_order_relaxed), g_hudScale[1].load(std::memory_order_relaxed),
                                    g_hudScale[2].load(std::memory_order_relaxed), g_sizeLogged[0], g_sizeLogged[1],
                                    g_sizeLogged[2], g_columnSize[0], g_columnSize[1], g_columnSize[2], g_passSets,
                                    g_placed.size());
    for (const Placed& placed : g_placed)
    {
        if (placed.placement == nullptr)
        {
            continue;
        }
        text += std::format(L"; {} {:p}: size {:.2f}{}{}, pass scale {:.2f}", placed.placement->className,
                            static_cast<const void*>(placed.widget.object), placed.size, placed.held ? L" held" : L"",
                            placed.sizeLeft ? L" (left)" : L"", placed.passScale);
        const auto now = user_widgets::Alive(placed.widget) ? ReadScale(placed.widget.object) : std::nullopt;
        text += now ? std::format(L", scale {:.3f}x{:.3f} pivot ({:.3f},{:.3f}) translation ({:.1f},{:.1f})", now->scale[0],
                                  now->scale[1], now->pivot[0], now->pivot[1], now->translation[0], now->translation[1])
                    : std::wstring(L", not readable");
    }
    return text;
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
        SizeRoot(placed, 1.0f, false);  // the game's own scale and pivot back
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
    g_columnSize.fill(0.0f);
    g_columnGlow.fill(0.0f);
    g_columnSaid.fill(0.0f);  // the next split says each column's size again
    platform::Log(L"HUD placement: {} root(s) set back to the game's own viewport layout and size{}", restored,
                  left > 0 ? std::format(L"; {} set by the game since, left with the game's", left) : std::wstring());
}

}  // namespace

void Tick(bool bandApplies) noexcept
try
{
    ++g_ticks;
    // The HUD sizes: in a split window's band, outside a whole-screen moment; a row of monitors keeps the game's size
    // (each of its views is already as big as single player's).
    const bool                       inEffect = bandApplies && !views::window_span::In() && !views::whole_screen::Shown();
    std::array<float, kSizedPlayers> sizes{};
    bool                             hold = false;  // some player's size is not the game's: the roots held
    for (std::size_t player = 0; player < sizes.size(); ++player)
    {
        sizes[player] = inEffect ? g_hudScale[player].load(std::memory_order_relaxed) : 1.0f;
        hold          = hold || sizes[player] != 1.0f;
    }
    if (sizes != g_sizeLogged)
    {
        g_sizeLogged = sizes;
        platform::Log(L"HUD size: player 1 x{:.2f}, player 2 x{:.2f}, player 3 x{:.2f}, each in their own view, on the minimap, "
                      L"compass, command menu, party panel and summon gauges{}",
                      sizes[0], sizes[1], sizes[2],
                      !bandApplies ? L" (no split band)" : !hold ? L" (the game's size in every view)" : L"");
    }
    g_passSets  = 0;
    g_tickSizes = sizes;
    g_columnSize.fill(0.0f);  // each column's sizes set again by its pass this frame
    g_columnGlow.fill(0.0f);
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
        placed.passSkipped = false;
        SizeRoot(placed, sizes[0], hold);
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

void Install() noexcept
try
{
    namespace settings = platform::settings;
    // A file with only player 1's key gives players 2 and 3 that size once, so their HUD keeps the size it had; the file
    // holds their keys from then on.
    const settings::Reading& one = settings::Of(settings::kHudScale);
    for (int seat = 1; seat < kSizedPlayers; ++seat)
    {
        const settings::Option& option = *kSizeOptions[static_cast<std::size_t>(seat)];
        if (one.source == settings::Source::File && settings::Of(option).source == settings::Source::Default &&
            one.word != option.fallback && settings::Save(option, one.word))
        {
            platform::Log(L"HUD size: {} takes player 1's x{} from a settings file that had one size for everyone",
                          players::SeatName(seat), one.word);
        }
    }
    for (int seat = 0; seat < kSizedPlayers; ++seat)
    {
        const auto              at     = static_cast<std::size_t>(seat);
        const settings::Option& option = *kSizeOptions[at];
        g_hudScale[at].store(FactorOf(settings::Of(option).word).value_or(1.0f), std::memory_order_relaxed);
        control::RegisterOption(
            option,
            [at](const std::wstring& word) {
                const auto factor = FactorOf(word);
                if (!factor)
                {
                    return control::Outcome{false, L"not a size from 0.75 to 1.50 in steps of 0.05"};
                }
                g_hudScale[at].store(*factor, std::memory_order_relaxed);
                platform::Log(L"HUD size: {} x{:.2f} in their view in a split window (set)",
                              players::SeatName(static_cast<int>(at)), *factor);
                return control::Outcome{};
            },
            [at] { return WordOfFactor(g_hudScale[at].load(std::memory_order_relaxed)); });
    }
    platform::Log(L"HUD size: player 1 x{:.2f} ({}), player 2 x{:.2f} ({}), player 3 x{:.2f} ({}), each in their own view in a "
                  L"split window",
                  g_hudScale[0].load(std::memory_order_relaxed), settings::Why(settings::kHudScale),
                  g_hudScale[1].load(std::memory_order_relaxed), settings::Why(settings::kHudScaleTwo),
                  g_hudScale[2].load(std::memory_order_relaxed), settings::Why(settings::kHudScaleThree));
    control::Register({.name  = L"hud.size-roots",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the players' sizes, each column's this frame, and each placed HUD root: the "
                                L"size set on it, its last pass's scale, and its live render scale, pivot and translation",
                       .get   = [] {
                           try
                           {
                               return control::Value{SizeReading()};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
}
catch (...)
{
    platform::Log(L"HUD size: the setting could not be registered (an exception); the HUD keeps the game's size");
}

LayoutPoint GlowAnchor() noexcept
{
    for (const Placement& placement : kPlacements)
    {
        if (placement.glow)
        {
            return {placement.anchorX, placement.anchorY};
        }
    }
    return {};
}

float GlowSizeOf(int column) noexcept
{
    const bool known = column >= 0 && column < overlay_pass::kMaxColumns;
    return known && g_columnGlow[static_cast<std::size_t>(column)] > 0.0f ? g_columnGlow[static_cast<std::size_t>(column)] : 1.0f;
}

void SizeForPass(int column, int seat) noexcept
try
{
    if (column < 0 || column >= overlay_pass::kMaxColumns || !g_active || !g_setters)
    {
        return;
    }
    const auto  at   = static_cast<std::size_t>(column);
    const float size = g_tickSizes[PlayerIndex(seat)];
    float       glow = 0.0f;
    for (Placed& placed : g_placed)
    {
        if (placed.placement == nullptr || !placed.held || placed.sizeLeft || placed.passSkipped ||
            !user_widgets::Alive(placed.widget))
        {
            continue;
        }
        const auto now = ReadScale(placed.widget.object);
        if (!now)
        {
            continue;
        }
        if (now->scale[0] != placed.passScale || now->scale[1] != placed.passScale)
        {
            // The game set the root's scale during the frame: the passes leave it, the next tick lets it go.
            placed.passSkipped = true;
            if (!placed.toldPassSkip)
            {
                placed.toldPassSkip = true;
                platform::Log(L"HUD size: the game set {}'s render scale during the frame ({:.2f}); each view's pass leaves it "
                              L"this frame (logged once per root)",
                              placed.placement->className, now->scale[0]);
            }
            continue;
        }
        if (placed.passScale != size)
        {
            const float scale[2] = {size, size};
            if (!Call(placed.widget.object, g_setters->renderScale, scale, 2))
            {
                LeaveToGame(placed);  // at once, so no later pass draws whatever the fault left
                platform::Log(L"HUD size: column {}'s pass raised an exception setting {}'s scale; its own transform set "
                              L"back, the root left to the game for the rest of the split",
                              column + 1, placed.placement->className);
                continue;
            }
            placed.passScale = size;
            ++g_passSets;
        }
        glow = placed.placement->glow ? size : glow;
    }
    g_columnSize[at] = size;
    g_columnGlow[at] = glow;
    if (column != 0 && size != g_columnSaid[at])
    {
        g_columnSaid[at] = size;
        platform::Log(L"HUD size: column {}'s pass ({}'s view) draws the HUD at x{:.2f}, player 1's at x{:.2f}", column + 1,
                      players::SeatName(static_cast<int>(PlayerIndex(seat))), size, g_tickSizes[0]);
    }
}
catch (...)
{
}

}  // namespace lss::hud::root_placement
