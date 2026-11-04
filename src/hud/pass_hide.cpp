#include "hud/pass_hide.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "engine_facts.h"
#include "hud/overlay_pass.h"
#include "hud/user_widgets.h"
#include "hud/view_window.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "players/unreal_call.h"
#include "world/search_arrows.h"
#include "world/target_icons.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lss::hud::pass_hide {

namespace {

using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

/// Root classes of the world cursors (prefix match on the Blueprint class name).
constexpr std::wstring_view kTargetIcons  = L"FieldTarget_";        // FieldTarget_01_C, _Block_C: each player's own (slot 15)
constexpr std::wstring_view kSearchArrows = L"FieldTargetSearch";   // FieldTargetSearch_02_C, _02_Arrow_C, FieldTargetSearchIcon_C (slot 55)
constexpr std::wstring_view kPlayerOneCursors[] = {
    L"TurnBackAlert",  // TurnBackAlert_C: over player 1's leader, triggered by player 1 (slot 36)
};

enum class Cursor : std::uint8_t
{
    None,
    TargetIcon,  // a field target icon: its owner's (world/target_icons.h OwnerSeat)
    SearchArrow, // a search compass or arrow: its owner's (world/search_arrows.h OwnerSeat)
    PlayerOne,   // player 1's alone
};

struct Hidden
{
    user_widgets::Widget widget;
    float                color[4] = {};
};
std::vector<Hidden> g_hidden;

UFunction*   g_setter      = nullptr;  // /Script/UMG.UserWidget:SetColorAndOpacity
std::int32_t g_paramOffset = -1;
bool         g_lookedUp    = false;
// Keyed by the class pointer for the process, as root_placement's: the cursor classes load with the game and stay
// [ASSUMPTION: never unloaded in play].
std::unordered_map<const void*, Cursor> g_cursorClass;

bool LookUp()
{
    if (g_lookedUp)
    {
        return g_setter != nullptr;
    }
    g_lookedUp = true;
    g_setter   = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/UMG.UserWidget:SetColorAndOpacity"));
    auto* property = g_setter == nullptr ? nullptr
                                         : g_setter->FindProperty(RC::Unreal::FName(STR("InColorAndOpacity"), RC::Unreal::FNAME_Find));
    if (property == nullptr || property->GetSize() != 16 || g_setter->GetParmsSize() > 32)
    {
        g_setter = nullptr;
        platform::Log(L"HUD image: UUserWidget::SetColorAndOpacity not found; player 1's cursors stay in the other columns");
        return false;
    }
    g_paramOffset = property->GetOffset_Internal();
    return true;
}

Cursor CursorOf(UObject* widget)
{
    const void* cls = widget->GetClassPrivate();
    if (const auto found = g_cursorClass.find(cls); found != g_cursorClass.end())
    {
        return found->second;
    }
    const std::wstring name(widget->GetClassPrivate()->GetName());
    Cursor cursor = name.starts_with(kTargetIcons) ? Cursor::TargetIcon : name.starts_with(kSearchArrows) ? Cursor::SearchArrow : Cursor::None;
    for (const auto prefix : kPlayerOneCursors)
    {
        cursor = name.starts_with(prefix) ? Cursor::PlayerOne : cursor;
    }
    g_cursorClass.emplace(cls, cursor);
    return cursor;
}

bool SetColor(UObject* widget, const float (&color)[4]) noexcept
{
    alignas(16) std::uint8_t frame[32]{};
    std::memcpy(frame + g_paramOffset, color, sizeof(color));
    return players::GuardedProcessEvent(widget, g_setter, frame);
}

/// The pass of a copy of player 1's picture shows no player window.
constexpr int kNoWindow = -1;

/// Every cursor root on the overlay, shown and opaque, that the pass of `seat` (0 player 1's) does not show, made
/// transparent. `playerOneCursors`: player 1's own cursors too (a further pass). `view`: the player window the pass
/// shows (hud/view_window.h: the seat's, or kNoWindow); every other window is hidden.
int Begin(int seat, bool playerOneCursors, int view) noexcept
try
{
    g_hidden.clear();
    if (!LookUp())
    {
        return 0;
    }
    for (const auto& widget : user_widgets::Live())
    {
        const Cursor cursor = CursorOf(widget.object);
        const int    window = cursor == Cursor::None ? view_window::ViewOf(widget.object) : -1;  // a player window's view
        const bool   hide   = cursor == Cursor::PlayerOne ? playerOneCursors
                              : cursor == Cursor::TargetIcon  ? world::target_icons::OwnerSeat(widget.object) != seat
                              : cursor == Cursor::SearchArrow ? world::search_arrows::OwnerSeat(widget.object) != seat
                              : window >= 0                   ? window != view
                                                              : false;
        void* canvas = nullptr;
        float color[4]{};
        // Roots only (an overlay canvas at UEndUserWidget+0x230), shown and opaque.
        if (!hide ||
            !platform::CopyGuarded(&canvas, reinterpret_cast<const std::uint8_t*>(widget.object) + facts::kEndWidgetRootCanvas,
                                   sizeof(canvas)) ||
            canvas == nullptr ||
            !platform::CopyGuarded(color, reinterpret_cast<const std::uint8_t*>(widget.object) + facts::kUserWidgetColor,
                                   sizeof(color)) ||
            color[3] <= 0.0f)
        {
            continue;
        }
        const float clear[4] = {color[0], color[1], color[2], 0.0f};
        if (SetColor(widget.object, clear))
        {
            g_hidden.push_back({widget, {color[0], color[1], color[2], color[3]}});
        }
    }
    return static_cast<int>(g_hidden.size());
}
catch (...)
{
    return static_cast<int>(g_hidden.size());
}

}  // namespace

int BeginFurtherPass(int column) noexcept
{
    const int seat = overlay_pass::SeatOf(column);
    if (seat >= 1)
    {
        return Begin(seat, true, seat);
    }
    // A copy of player 1's picture: player 1's hides, and no window (player 1's is not drawn there).
    g_hidden.clear();
    return world::target_icons::AnySeatIcons() || world::search_arrows::AnySeatArrows() || view_window::Any()
               ? Begin(0, false, kNoWindow)
               : 0;
}

int BeginOwnPass() noexcept
{
    g_hidden.clear();
    return world::target_icons::AnySeatIcons() || world::search_arrows::AnySeatArrows() || view_window::Any()
               ? Begin(0, false, 0)
               : 0;
}

void EndPass() noexcept
{
    for (auto& hidden : g_hidden)
    {
        if (user_widgets::Alive(hidden.widget))
        {
            SetColor(hidden.widget.object, hidden.color);
        }
    }
    g_hidden.clear();
}

}  // namespace lss::hud::pass_hide
