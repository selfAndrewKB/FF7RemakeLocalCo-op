#pragma once

// Player 2 joining and leaving through UE's own local-player path, on the game thread.
//
// Join: UGameplayStatics::CreatePlayer(ControllerId 1, spawn a controller) inside own_controller's join
// scope, so player 2 gets a real End player controller and camera manager of its own. The engine then lays
// out the split screen by itself. Each half is filled (the game's main cameras ask for 16:9) with the stock
// setter UCameraComponent::SetConstraintAspectRatio.
// Leave: the actors player 2's controller and camera manager spawned, which the engine never destroys, are
// destroyed first, then UGameInstance::RemoveLocalPlayer. The engine's own list is the evidence of both.
//
// Player 2 comes out without a pawn: seating a party member is the next stage (Documentation/plan/topics/session.md).

#include <cstdint>

namespace RC::Unreal {
class UObject;
}

namespace lss::platform {
class GameImage;
}

namespace lss::players {

class Session
{
public:
    /// Checks and resolves everything the join and leave need. False (logged) if anything is missing.
    bool Install(const platform::GameImage& image) noexcept;

    void Join() noexcept;
    void Leave() noexcept;
    /// PROBE (Ctrl+F9): player 2 takes Aerith through the game's leader funnel, or gives her back.
    void ToggleSeat() noexcept;
    /// Logs every local player: controller, camera manager, main camera actor, pawn, view shape.
    void Report() const noexcept;
    /// Report, plus each player's input/movement/camera call counts since the last one.
    void Status() const noexcept;

    bool HasPlayerTwo() const noexcept { return playerTwo_ != nullptr; }
    bool Seated() const noexcept;

    /// An engine console command, as typed in the console (UKismetSystemLibrary::ExecuteConsoleCommand with
    /// player 1's controller). False if the call could not be made.
    static bool Console(const wchar_t* command) noexcept;

private:
    RC::Unreal::UObject* playerTwo_    = nullptr;  // ULocalPlayer
    RC::Unreal::UObject* gameInstance_ = nullptr;
    std::uintptr_t       removeLocalPlayer_ = 0;
    bool                 cameraInstalled_   = false;
    bool                 seatInstalled_     = false;
};

}  // namespace lss::players
