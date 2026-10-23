#pragma once

// The engine's list of local players, for the modules that add a local player of the mod's own (players/session:
// player 2; views/mirror_player: the mirror in an empty column) through UE's own paths, on the game thread.
//
// Add: UGameplayStatics::CreatePlayer(ControllerId, spawn a controller) inside the camera-manager join scope of
// players/own_controller, so the new player gets a real End player controller and camera manager, the camera
// manager's BeginPlay made safe for a further player. The engine's list is the evidence of what happened, not the
// call: the engine can add the player and then raise, and such a player must still be tracked and removed.
// Remove: UGameInstance::RemoveLocalPlayer under a guard, the list again the evidence. The actors the player's
// controller and camera manager spawned, which the engine never destroys (the End camera manager has no EndPlay or
// Destroyed override), are read before the removal and destroyed after it, if still the same objects: the main
// camera actor and the battle-talk actor.
// Fill: the stock UCameraComponent::SetConstraintAspectRatio(false) on a camera manager's main camera, so the view
// fills its share of the window (the End camera actor constrains to 16:9 by default).
// Every call must run on the game thread, outside the frame's drawing.

#include <cstdint>
#include <string>

namespace RC::Unreal {
class UObject;
}

namespace lss::players::local_player_list {

/// The game instance (UE4SS's lookup), or null.
RC::Unreal::UObject* GameInstance() noexcept;
/// How many local players the instance lists; -1 without a list.
std::int32_t Count(RC::Unreal::UObject* gameInstance) noexcept;
/// The local player at `index`, or null.
RC::Unreal::UObject* At(RC::Unreal::UObject* gameInstance, std::int32_t index) noexcept;
/// The engine still lists `localPlayer`. Reads only the list, never the player's object.
bool Listed(RC::Unreal::UObject* gameInstance, RC::Unreal::UObject* localPlayer) noexcept;

/// What an add did. `localPlayer` is set when the engine's list grew by one, whatever the call reported.
struct Added
{
    bool                 called        = false;    // CreatePlayer returned (false: it raised, or was not found)
    bool                 managerSeen   = false;    // the camera manager's BeginPlay ran under the join scope
    bool                 ownerFilled   = false;    // its PCOwner was null and was set from Owner
    bool                 ownerRefused  = false;    // its PCOwner was null but Owner was not a fitting controller
    bool                 renamed       = false;    // its camera actor got a name of its own
    RC::Unreal::UObject* localPlayer   = nullptr;  // ULocalPlayer, as the engine lists it
    RC::Unreal::UObject* controller    = nullptr;  // its End player controller
    RC::Unreal::UObject* cameraManager = nullptr;  // its End camera manager

    /// "CreatePlayer returned; <whose> camera manager seen: owner ..., camera actor ...", for the caller's log line.
    std::wstring Text(const wchar_t* whose) const;
};

/// Adds a local player with controller id `controllerId` through CreatePlayer, inside the join scope. The id must
/// be free (the engine refuses a duplicate; the caller's check), the list's new last entry is the player's. Logged
/// only on the caller's side. The first form finds the game instance (an object walk; the session caches its own),
/// the second takes the caller's.
Added Add(RC::Unreal::UObject* playerOneController, std::int32_t controllerId) noexcept;
Added Add(RC::Unreal::UObject* gameInstance, RC::Unreal::UObject* playerOneController, std::int32_t controllerId) noexcept;

/// The actors a local player's controller and camera manager spawned, read before the removal destroys their owners;
/// each with its class pointer, so a reused address is told from the actor.
struct Leftovers
{
    RC::Unreal::UObject* cameraActor = nullptr;
    const void*          cameraClass = nullptr;
    RC::Unreal::UObject* battleTalk  = nullptr;
    const void*          talkClass   = nullptr;

    std::wstring Text() const;  // both actors' names
};
Leftovers ReadLeftovers(RC::Unreal::UObject* localPlayer) noexcept;

/// RemoveLocalPlayer (`removeLocalPlayer`: its verified address) under a guard. True when the list no longer holds
/// the player, whatever the call reported; `raised` says whether the call raised.
bool Remove(RC::Unreal::UObject* gameInstance, RC::Unreal::UObject* localPlayer, std::uintptr_t removeLocalPlayer,
            bool& raised) noexcept;

/// Destroys each leftover still of its class (AActor.K2_DestroyActor). Which were destroyed.
struct Destroyed
{
    bool camera = false;
    bool talk   = false;
};
Destroyed DestroyLeftovers(const Leftovers& leftovers) noexcept;

/// SetConstraintAspectRatio(false) on the camera manager's main camera. False if anything is missing.
bool FillView(RC::Unreal::UObject* cameraManager) noexcept;

/// An object's full name, "none" for null, "(unnamed)" if the lookup raised.
std::wstring Describe(RC::Unreal::UObject* object);

}  // namespace lss::players::local_player_list
