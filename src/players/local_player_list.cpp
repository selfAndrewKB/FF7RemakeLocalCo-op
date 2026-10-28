#include "players/local_player_list.h"
#include "players/unreal_call.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "engine_facts.h"
#include "platform/guarded.h"
#include "players/own_controller.h"

#include <Unreal/Core/Containers/FString.hpp>

#include <format>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::players::local_player_list {

namespace {

using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

/// UGameplayStatics::CreatePlayer's parameter frame, checked against the UFunction's own size before use.
struct CreatePlayerParams
{
    UObject*     WorldContextObject;
    std::int32_t ControllerId;
    bool         bSpawnPlayerController;
    UObject*     ReturnValue;
};
static_assert(sizeof(CreatePlayerParams) == 24);

using RemoveLocalPlayerFn = bool (*)(UObject* gameInstance, UObject* localPlayer);

bool GuardedRemove(RemoveLocalPlayerFn remove, UObject* gameInstance, UObject* localPlayer) noexcept
{
    __try
    {
        remove(gameInstance, localPlayer);  // its result says nothing reliable; the list is checked instead
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

UObject* Field(const void* object, std::uint32_t offset) noexcept
{
    return static_cast<UObject*>(platform::ReadPointer(object, offset));
}

/// An object's class pointer (UObjectBase::ClassPrivate), read under a guard: a cheap check that an object read
/// earlier is still the same kind of object.
const void* ClassOf(const void* object) noexcept
{
    return Field(object, facts::kObjectClass);
}

// UE4SS's object lookups can throw (a malformed path, or running out of memory), so every function here that makes
// one catches: an exception escaping a noexcept function would end the game.

RC::Unreal::TArray<UObject*>* List(UObject* gameInstance) noexcept
try
{
    return gameInstance == nullptr
               ? nullptr
               : gameInstance->GetValuePtrByPropertyNameInChain<RC::Unreal::TArray<UObject*>>(STR("LocalPlayers"));
}
catch (...)
{
    return nullptr;
}

/// Calls a reflected function with no parameters (e.g. AActor.K2_DestroyActor) on `target`.
bool CallNoParams(UObject* target, const wchar_t* path) noexcept
try
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
    if (target == nullptr || function == nullptr || function->GetParmsSize() != 0)
    {
        return false;
    }
    return GuardedProcessEvent(target, function, nullptr);
}
catch (...)
{
    return false;
}

const wchar_t* YesNo(bool value) noexcept
{
    return value ? L"yes" : L"no";
}

}  // namespace

UObject* GameInstance() noexcept
try
{
    return UObjectGlobals::FindFirstOf(STR("GameInstance"));
}
catch (...)
{
    return nullptr;
}

std::int32_t Count(UObject* gameInstance) noexcept
{
    auto* players = List(gameInstance);
    return players == nullptr ? -1 : players->Num();
}

UObject* At(UObject* gameInstance, std::int32_t index) noexcept
{
    auto* players = List(gameInstance);
    return players != nullptr && index >= 0 && index < players->Num() ? (*players)[index] : nullptr;
}

bool Listed(UObject* gameInstance, UObject* localPlayer) noexcept
{
    auto* players = List(gameInstance);
    if (players == nullptr || localPlayer == nullptr)
    {
        return false;
    }
    for (std::int32_t i = 0; i < players->Num(); ++i)
    {
        if ((*players)[i] == localPlayer)
        {
            return true;
        }
    }
    return false;
}

std::wstring Added::Text(const wchar_t* whose) const
{
    return std::format(L"CreatePlayer {}; {} camera manager {}: owner {}, camera actor {}", called ? L"returned" : L"RAISED AN EXCEPTION",
                       whose, managerSeen ? L"seen" : L"NOT SEEN",
                       ownerFilled ? L"filled from Owner" : ownerRefused ? L"REFUSED" : L"already set",
                       renamed ? std::format(L"named EndCameraActr{}", controllerId + 1) : std::wstring(L"NOT RENAMED"));
}

Added Add(UObject* playerOneController, std::int32_t controllerId) noexcept
{
    return Add(GameInstance(), playerOneController, controllerId);
}

Added Add(UObject* gameInstance, UObject* playerOneController, std::int32_t controllerId) noexcept
try
{
    Added added;
    added.controllerId = controllerId;
    auto* players = List(gameInstance);
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics:CreatePlayer"));
    auto* statics  = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics"));
    if (players == nullptr || function == nullptr || statics == nullptr || function->GetParmsSize() != sizeof(CreatePlayerParams))
    {
        return added;
    }
    const std::int32_t before = players->Num();

    CreatePlayerParams params{};
    params.WorldContextObject     = playerOneController;
    params.ControllerId           = controllerId;
    params.bSpawnPlayerController = true;
    own_controller::Report report;
    {
        own_controller::JoinScope scope(playerOneController, controllerId);
        added.called = GuardedProcessEvent(static_cast<UObject*>(statics->GetClassDefaultObject()), function, &params);
        report       = scope.Result();
    }
    added.managerSeen  = report.cameraManager != nullptr;
    added.ownerFilled  = report.ownerFilled;
    added.ownerRefused = report.ownerRefused;
    added.renamed      = report.renamed;

    // The engine's list decides what happened: a player added before an exception must still be tracked.
    if (players->Num() != before + 1)
    {
        return added;
    }
    added.localPlayer   = (*players)[before];  // the engine appends the new player
    added.controller    = Field(added.localPlayer, facts::kLocalPlayerController);
    added.cameraManager = Field(added.controller, facts::kControllerCameraManager);
    return added;
}
catch (...)
{
    return {};
}

std::wstring Leftovers::Text() const
{
    return std::format(L"camera actor {}, battle-talk actor {}", Describe(cameraActor), Describe(battleTalk));
}

Leftovers ReadLeftovers(UObject* localPlayer) noexcept
{
    Leftovers leftovers;
    UObject*  controller   = Field(localPlayer, facts::kLocalPlayerController);
    UObject*  manager      = Field(controller, facts::kControllerCameraManager);
    leftovers.cameraActor  = Field(manager, facts::kCameraManagerMainCamera);
    leftovers.cameraClass  = ClassOf(leftovers.cameraActor);
    leftovers.battleTalk   = Field(controller, facts::kControllerBattleTalkActor);
    leftovers.talkClass    = ClassOf(leftovers.battleTalk);
    return leftovers;
}

bool Remove(UObject* gameInstance, UObject* localPlayer, std::uintptr_t removeLocalPlayer, bool& raised) noexcept
{
    if (!Listed(gameInstance, localPlayer))
    {
        raised = false;
        return true;
    }
    // The list is the evidence, whatever the call reported: it can remove the player and then fault.
    raised = !GuardedRemove(reinterpret_cast<RemoveLocalPlayerFn>(removeLocalPlayer), gameInstance, localPlayer);
    return !Listed(gameInstance, localPlayer);
}

Destroyed DestroyLeftovers(const Leftovers& leftovers) noexcept
{
    // Research says nothing destroys these two with their owners; a changed class pointer would mean the memory was
    // reused, and then they are left alone.
    Destroyed gone;
    gone.camera = leftovers.cameraClass != nullptr && ClassOf(leftovers.cameraActor) == leftovers.cameraClass &&
                  CallNoParams(leftovers.cameraActor, STR("/Script/Engine.Actor:K2_DestroyActor"));
    gone.talk = leftovers.talkClass != nullptr && ClassOf(leftovers.battleTalk) == leftovers.talkClass &&
                CallNoParams(leftovers.battleTalk, STR("/Script/Engine.Actor:K2_DestroyActor"));
    return gone;
}

bool FillView(UObject* cameraManager) noexcept
try
{
    UObject* cameraActor = Field(cameraManager, facts::kCameraManagerMainCamera);
    auto*    component   = cameraActor == nullptr
                               ? nullptr
                               : cameraActor->GetValuePtrByPropertyNameInChain<UObject*>(STR("CameraComponent"));
    auto*    setter      = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Engine.CameraComponent:SetConstraintAspectRatio"));
    if (component == nullptr || *component == nullptr || setter == nullptr)
    {
        return false;
    }
    auto* parameter =
        setter->FindProperty(RC::Unreal::FName(STR("bInConstrainAspectRatio"), RC::Unreal::FNAME_Find));
    alignas(8) std::uint8_t frame[16]{};
    if (parameter == nullptr || parameter->GetSize() != 1 || setter->GetParmsSize() > static_cast<std::int32_t>(sizeof(frame)) ||
        parameter->GetOffset_Internal() < 0 || parameter->GetOffset_Internal() >= setter->GetParmsSize())
    {
        return false;
    }
    frame[parameter->GetOffset_Internal()] = 0;  // false: fill the view, keep the camera's FOV
    return GuardedProcessEvent(*component, setter, frame);
}
catch (...)
{
    return false;
}

std::wstring Describe(UObject* object)
{
    if (object == nullptr)
    {
        return L"none";
    }
    try
    {
        return std::wstring(object->GetFullName());
    }
    catch (...)
    {
        return L"(unnamed)";
    }
}

}  // namespace lss::players::local_player_list
