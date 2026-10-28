#include "hud/user_widgets.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "platform/log.h"

#include <algorithm>
#include <mutex>

namespace lss::hud::user_widgets {

namespace {

using RC::Unreal::UClass;
using RC::Unreal::UObject;
namespace Hook = RC::Unreal::Hook;

UClass*                 g_userWidgetClass = nullptr;
Hook::GlobalCallbackId  g_callback        = Hook::ERROR_ID;
std::mutex              g_lock;
std::vector<Widget>     g_constructed;  // under g_lock
std::vector<Widget>     g_live;         // game thread
bool                    g_seeded = false;

Widget Record(UObject* object) noexcept
{
    return {object, static_cast<std::int32_t>(object->GetInternalIndex()), object->GetClassPrivate()};
}

bool g_sorted = true;  // g_live is sorted by index with no duplicates

void Seed()
{
    g_seeded = true;
    std::vector<UObject*> found;
    RC::Unreal::UObjectGlobals::FindAllOf(STR("UserWidget"), found);
    g_live.clear();
    for (UObject* object : found)
    {
        g_live.push_back(Record(object));
    }
    g_sorted = false;
}

}  // namespace

bool Install() noexcept
{
    try
    {
        g_userWidgetClass = RC::Unreal::UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.UserWidget"));
        if (g_userWidgetClass == nullptr)
        {
            platform::Log(L"user widgets: /Script/UMG.UserWidget not found; HUD widgets are found by scanning instead");
            return false;
        }
        const Hook::FCallbackOptions options{false, false, STR("LocalSplitScreen"), STR("user widget constructed")};
        g_callback = Hook::RegisterStaticConstructObjectPostCallback(
            [](auto& info, const RC::Unreal::FStaticConstructObjectParameters&) {
                UObject* object = info.GetCurrentResolvedReturnValue();
                if (object != nullptr && object->IsA(g_userWidgetClass))
                {
                    try  // inside the game's construction: a record lost to no memory is found by the next walk instead
                    {
                        const std::lock_guard lock(g_lock);
                        g_constructed.push_back(Record(object));
                    }
                    catch (...)
                    {
                    }
                }
            },
            options);
        if (g_callback == Hook::ERROR_ID)
        {
            return false;
        }
        // The widgets made before the callback: one walk now, while the game loads, not at the
        // first join (a walk costs ~20-25 ms in play).
        Seed();
        return true;
    }
    catch (...)
    {
        platform::Log(L"user widgets: the construction callback could not be registered");
        return false;
    }
}

void Remove() noexcept
{
    if (g_callback != Hook::ERROR_ID)
    {
        Hook::UnregisterCallback(g_callback);
        g_callback = Hook::ERROR_ID;
    }
}

bool Alive(const Widget& widget) noexcept
{
    auto* item = RC::Unreal::UObjectArray::IndexToObject(widget.index);
    return item != nullptr && item->GetUObject() == widget.object && !item->IsUnreachable() && !item->IsPendingKill() &&
           widget.object->GetClassPrivate() == widget.cls;
}

const std::vector<Widget>& Live() noexcept
{
    try
    {
        if (!g_seeded || g_callback == Hook::ERROR_ID)
        {
            Seed();  // once (or every time, without the callback): the widgets constructed before
        }
        {
            const std::lock_guard lock(g_lock);
            if (!g_constructed.empty())
            {
                g_live.insert(g_live.end(), g_constructed.begin(), g_constructed.end());
                g_constructed.clear();
                g_sorted = false;
            }
        }
        std::erase_if(g_live, [](const Widget& widget) { return !Alive(widget); });
        if (!g_sorted)
        {
            // A widget recorded both by the walk and by the callback appears once.
            std::sort(g_live.begin(), g_live.end(), [](const Widget& a, const Widget& b) { return a.index < b.index; });
            g_live.erase(std::unique(g_live.begin(), g_live.end(),
                                     [](const Widget& a, const Widget& b) { return a.object == b.object; }),
                         g_live.end());
            g_sorted = true;
        }
    }
    catch (...)
    {
        platform::Log(L"user widgets: failed to update the list (an exception); kept as it was");
    }
    return g_live;
}

}  // namespace lss::hud::user_widgets
