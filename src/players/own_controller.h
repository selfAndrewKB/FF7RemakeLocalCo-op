#pragma once

// Player 2's own PlayerController: the End camera manager's BeginPlay, made safe for a second player.
//
// When a player joins during play, the engine spawns its controller and camera manager, and the camera
// manager's BeginPlay (AEndPlayerCameraManager, vtable slot 94) runs inside that spawn. For a second
// player it fails two ways (Remake/Documentation/research/RE-players-viewport.md §1):
//   - its owning controller (PCOwner) is not set yet, and SetViewTarget reads the controller's pawn
//     through it (the crash CreatePlayer(1, true) produces);
//   - it spawns its main camera actor with the fixed name "EndCameraActor", and UE 4.18's spawn silently
//     reuses an existing object of that name: player 1's camera actor would be destroyed and rebuilt as
//     player 2's.
// The replacement for that slot, only while a join is under way and only for a camera manager whose owner
// is not player 1's controller:
//   - fills PCOwner from the actor's Owner, the controller that spawned it, which is what the engine
//     itself writes there right after the spawn;
//   - gives the camera actor a name of its own for the length of the call, by changing the name's text
//     (its only reader is this function): "EndCameraActr" and the player's number (controller id + 1), so two
//     further players (player 3, or two mirrors) never reuse each other's actor either.
// Player 1's camera manager, and every camera manager outside a join, take the engine's path.

namespace lss::platform {
class GameImage;
}

namespace lss::players::own_controller {

/// Replaces the camera manager's BeginPlay slot after checking it, the function and the name text.
bool Install(const platform::GameImage& image) noexcept;

/// Puts the engine's function back (at mod uninstall).
void Remove() noexcept;

/// True if the camera actor's name text can be changed and put back now. A join checks this first, so
/// the rename inside the spawn is not the first time the page protection is tried.
bool CanRename() noexcept;

/// True if player 1's controller and its camera manager carry the End classes' native vtables. The
/// replacement recognises player 2's controller by that vtable; Blueprint subclasses share it.
bool UsesNativeClasses(const void* playerOneController) noexcept;

/// What the replacement did for the most recent player-2 camera manager.
struct Report
{
    void* cameraManager = nullptr;
    bool  ownerFilled   = false;  // PCOwner was null and was set from Owner
    bool  ownerRefused  = false;  // PCOwner was null but Owner was not a fitting controller
    bool  renamed       = false;  // the camera actor got player 2's name
};

/// Marks a join as under way for its lifetime: only then does the replacement act. `controllerId` is the joining
/// player's (its camera actor's name).
class JoinScope
{
public:
    JoinScope(void* playerOneController, int controllerId) noexcept;
    ~JoinScope();
    JoinScope(const JoinScope&)            = delete;
    JoinScope& operator=(const JoinScope&) = delete;

    Report Result() const noexcept;
};

}  // namespace lss::players::own_controller
