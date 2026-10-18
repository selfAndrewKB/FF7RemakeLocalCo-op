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
// World changes: a retry, load, jump or return to the title takes player 2 out first (players/field_exit.h, with a
// field-step check in Tick as the fallback), and player 2 joins again, holding Aerith again if it did, once the
// field has been steady for a moment with player 1 on a character.

#include <chrono>
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
    /// Puts back the field-exit calls (unload).
    void Remove() noexcept;

    void Join() noexcept;
    /// Player 2 leaves (the player's intent); also cancels a rejoin after a world change.
    void Leave() noexcept;
    /// Once per engine tick, after players/game_state's: lets go of a player 2 the engine no longer lists, takes
    /// player 2 out at a field exit the request did not report, and rejoins player 2 after a world change.
    void Tick() noexcept;
    /// PROBE (Ctrl+F9): player 2 takes Aerith through the game's leader funnel, or gives her back.
    void ToggleSeat() noexcept;
    /// Logs every local player: controller, camera manager, main camera actor, pawn, view shape.
    void Report() const noexcept;
    /// Report, plus each player's input/movement/camera call counts since the last one.
    void Status() const noexcept;

    bool HasPlayerTwo() const noexcept { return playerTwo_ != nullptr; }
    bool Seated() const noexcept;
    /// Player 2 was taken out by a world change and joins again once the field is back.
    bool RejoinPending() const noexcept { return rejoin_.pending; }

    /// An engine console command, as typed in the console (UKismetSystemLibrary::ExecuteConsoleCommand with
    /// player 1's controller). False if the call could not be made.
    static bool Console(const wchar_t* command) noexcept;

private:
    /// After a world change took player 2 out.
    struct Rejoin
    {
        bool                                  pending  = false;
        bool                                  reseat   = false;  // player 2 held Aerith when it was taken out
        bool                                  steady   = false;  // the field has been ready since `since`
        std::chrono::steady_clock::time_point since{};
        int                                   failures = 0;      // joins that did not add player 2
    };

    static void OnFieldExit(void* self, const wchar_t* caller) noexcept;
    /// Leave's work. A world change cannot wait: it takes player 2 out even if the seat release failed.
    bool TakeOut(bool worldChange) noexcept;
    void LeaveForWorldChange(const wchar_t* why) noexcept;
    void TryRejoin() noexcept;
    /// Player 2 is gone by the engine's hand: the mod's own records of it are dropped, nothing is touched.
    void Forget() noexcept;

    RC::Unreal::UObject* playerTwo_    = nullptr;  // ULocalPlayer
    RC::Unreal::UObject* gameInstance_ = nullptr;  // kept after a leave: the instance lives as long as the game
    Rejoin               rejoin_;
    unsigned long        gameThread_         = 0;  // the engine tick's thread (Tick)
    bool                 worldChangeStuck_   = false;  // a world change could not take player 2 out; not retried
    bool                 fieldExitInstalled_ = false;
    std::uintptr_t       removeLocalPlayer_ = 0;
    bool                 cameraInstalled_   = false;
    bool                 seatInstalled_     = false;
};

}  // namespace lss::players
