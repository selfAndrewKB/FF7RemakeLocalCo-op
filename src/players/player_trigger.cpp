#include "players/player_trigger.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/game_state.h"
#include "players/player_two_camera.h"
#include "players/unreal_call.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace lss::players::player_trigger {

namespace {

using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

using SpawnActorFn = void* (*)(void* world, void* type, const void* transform, const void* parameters);

constexpr std::uint32_t kTrigger = 0x6E0;  // AEndPlayerController::PlayerTrigger

SpawnActorFn  g_spawnActor = nullptr;
std::uint32_t* g_nameNone  = nullptr;

// Game thread.
std::int32_t g_trigger[2]{-1, 0};  // player 2's trigger, a weak pair

template <class T>
bool Read(const void* address, T& out) noexcept
{
    return address != nullptr && platform::CopyGuarded(&out, address, sizeof(T));
}

/// AActor.GetComponentByClass(class) on `actor`, through the game's own reflected call. Null if none or unknown.
UObject* ComponentByClass(UObject* actor, const wchar_t* classPath) noexcept
try
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.Actor:GetComponentByClass"));
    auto* type     = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, classPath);
    struct Frame
    {
        UClass*  ComponentClass;
        UObject* ReturnValue;
    } frame{type, nullptr};
    if (function == nullptr || type == nullptr || function->GetParmsSize() != sizeof(Frame) || !GuardedProcessEvent(actor, function, &frame))
    {
        return nullptr;
    }
    return frame.ReturnValue;
}
catch (...)
{
    return nullptr;
}

/// ActorComponent.GetOwner() on `component`.
UObject* OwnerOf(UObject* component) noexcept
try
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.ActorComponent:GetOwner"));
    struct Frame
    {
        UObject* ReturnValue;
    } frame{nullptr};
    if (function == nullptr || function->GetParmsSize() != sizeof(Frame) || !GuardedProcessEvent(component, function, &frame))
    {
        return nullptr;
    }
    return frame.ReturnValue;
}
catch (...)
{
    return nullptr;
}

bool DestroyActor(UObject* actor) noexcept
try
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.Actor:K2_DestroyActor"));
    return function != nullptr && function->GetParmsSize() == 0 && GuardedProcessEvent(actor, function, nullptr);
}
catch (...)
{
    return false;
}

/// The copy carries its own root, capsule and overlap component (not the template's), and the template's first tag.
bool OwnComponents(UObject* copy, const std::uint8_t* original) noexcept
{
    void* root     = nullptr;
    void* rootOrig = nullptr;
    if (!Read(reinterpret_cast<const std::uint8_t*>(copy) + facts::kActorRootComponent, root) || root == nullptr ||
        !Read(original + facts::kActorRootComponent, rootOrig) || root == rootOrig)
    {
        platform::Log(L"player 2's trigger: the copy has no root of its own (the spawn kept the placed actor's)");
        return false;
    }
    for (const wchar_t* path : {L"/Script/Engine.CapsuleComponent", L"/Script/EndGame.EndCapsuleOverlapComponent"})
    {
        UObject* component = ComponentByClass(copy, path);
        if (component == nullptr || OwnerOf(component) != copy)
        {
            platform::Log(L"player 2's trigger: the copy's {} is {}", path, component == nullptr ? L"missing" : L"not its own");
            return false;
        }
    }
    std::int32_t tags = 0, tagsOrig = 0;
    std::uint64_t tag = 0, tagOrig = 1;
    std::uint64_t* names = nullptr;
    std::uint64_t* namesOrig = nullptr;
    Read(reinterpret_cast<const std::uint8_t*>(copy) + facts::kActorTags + 8, tags);
    Read(original + facts::kActorTags + 8, tagsOrig);
    Read(reinterpret_cast<const std::uint8_t*>(copy) + facts::kActorTags, names);
    Read(original + facts::kActorTags, namesOrig);
    if (tags < 1 || tagsOrig < 1 || !Read(names, tag) || !Read(namesOrig, tagOrig) || tag != tagOrig)
    {
        platform::Log(L"player 2's trigger: the copy's tag differs from the placed actor's ({} tags, {} on the original)", tags, tagsOrig);
        return false;
    }
    return true;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto spawn = platform::FindFunction(image, facts::kSpawnActor);
    const auto none  = image.Rebase(facts::kNameNoneIndex, sizeof(std::uint32_t));
    if (!spawn || !none)
    {
        return false;
    }
    g_spawnActor = reinterpret_cast<SpawnActorFn>(*spawn);
    g_nameNone   = reinterpret_cast<std::uint32_t*>(*none);
    return true;
}

void* Ensure(void* playerOneController) noexcept
{
    if (void* existing = player_two_camera::ObjectOf(g_trigger); existing != nullptr)
    {
        return existing;
    }
    auto* original = static_cast<std::uint8_t*>(nullptr);
    void* world    = game_state::World();
    void* type     = nullptr;  // the placed actor's class (a plain Actor)
    std::uint32_t none = 0;
    if (!Read(static_cast<const std::uint8_t*>(playerOneController) + kTrigger, original) || original == nullptr || world == nullptr ||
        !Read(original + facts::kObjectClass, type) || type == nullptr || !Read(g_nameNone, none) || g_spawnActor == nullptr)
    {
        platform::Log(L"player 2's trigger: player 1's trigger, the world or its class could not be read; none made");
        return nullptr;
    }
    // The engine's own spawn with the placed actor as the template: no name (a unique one is made), the game's
    // object flags for its spawned actors.
    alignas(8) std::array<std::uint8_t, 0x30> parameters{};
    std::memcpy(parameters.data(), &none, sizeof(none));
    std::memcpy(parameters.data() + facts::kSpawnParamsTemplate, &original, sizeof(original));
    const std::uint32_t flags = facts::kOperatorObjectFlags;
    std::memcpy(parameters.data() + facts::kSpawnParamsObjectFlags, &flags, sizeof(flags));
    void* spawned = nullptr;
    if (!platform::CallGuarded(g_spawnActor, spawned, world, type, static_cast<const void*>(nullptr),
                               static_cast<const void*>(parameters.data())) ||
        spawned == nullptr)
    {
        platform::Log(L"player 2's trigger: the spawn {}; none made", spawned == nullptr ? L"returned nothing" : L"RAISED AN EXCEPTION");
        return nullptr;
    }
    auto* copy = static_cast<UObject*>(spawned);
    if (!OwnComponents(copy, original))
    {
        DestroyActor(copy);
        return nullptr;
    }
    if (!player_two_camera::WeakPairOf(spawned, g_trigger))
    {
        DestroyActor(copy);
        platform::Log(L"player 2's trigger: no weak pair for the copy; destroyed");
        return nullptr;
    }
    platform::Log(L"player 2's trigger: a copy of the placed PlayerTrigger spawned with its own components ({:#x})",
                  reinterpret_cast<std::uintptr_t>(spawned));
    return spawned;
}

void Destroy() noexcept
{
    void* trigger = player_two_camera::ObjectOf(g_trigger);
    g_trigger[0] = -1;
    g_trigger[1] = 0;
    if (trigger != nullptr)
    {
        platform::Log(L"player 2's trigger: {}", DestroyActor(static_cast<UObject*>(trigger)) ? L"destroyed" : L"NOT destroyed");
    }
}

bool Owns(const void* actor) noexcept
{
    return actor != nullptr && actor == player_two_camera::ObjectOf(g_trigger);
}

}  // namespace lss::players::player_trigger
