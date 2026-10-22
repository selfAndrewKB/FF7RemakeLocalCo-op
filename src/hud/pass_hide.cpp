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
#include "hud/user_widgets.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "players/unreal_call.h"

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

/// Root classes whose widgets show player 1's world cursors (prefix match on the Blueprint class name).
constexpr std::wstring_view kPlayerOneCursors[] = {
    L"FieldTarget_",       // FieldTarget_01_C, FieldTarget_Block_C: talk, examine, field actions (slot 15)
    L"FieldTargetSearch",  // FieldTargetSearch_02_C, _02_Arrow_C, FieldTargetSearchIcon_C (slot 55)
    L"TurnBackAlert",      // TurnBackAlert_C: over player 1's leader, triggered by player 1 (slot 36)
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
std::unordered_map<const void*, bool> g_isCursorClass;

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

bool IsCursorClass(UObject* widget)
{
    const void* cls = widget->GetClassPrivate();
    if (const auto found = g_isCursorClass.find(cls); found != g_isCursorClass.end())
    {
        return found->second;
    }
    const std::wstring name(widget->GetClassPrivate()->GetName());
    bool cursor = false;
    for (const auto prefix : kPlayerOneCursors)
    {
        cursor = cursor || name.starts_with(prefix);
    }
    g_isCursorClass.emplace(cls, cursor);
    return cursor;
}

bool SetColor(UObject* widget, const float (&color)[4]) noexcept
{
    alignas(16) std::uint8_t frame[32]{};
    std::memcpy(frame + g_paramOffset, color, sizeof(color));
    return players::GuardedProcessEvent(widget, g_setter, frame);
}

}  // namespace

int BeginFurtherPass() noexcept
try
{
    g_hidden.clear();
    if (!LookUp())
    {
        return 0;
    }
    for (const auto& widget : user_widgets::Live())
    {
        void* canvas = nullptr;
        float color[4]{};
        // Roots only (an overlay canvas at UEndUserWidget+0x230), shown and opaque.
        if (!IsCursorClass(widget.object) ||
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

void EndFurtherPass() noexcept
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
