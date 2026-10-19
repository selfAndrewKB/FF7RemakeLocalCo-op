#pragma once

// Player 2 joining, taking its party member, and leaving, through UE's and the game's own paths, on the game thread.
//
// Join: UGameplayStatics::CreatePlayer(ControllerId 1, spawn a controller) inside own_controller's join scope, so
// player 2 gets a real End player controller and camera manager of its own. The engine then lays out the split
// screen by itself. Each half is filled (the game's main cameras ask for 16:9) with the stock setter
// UCameraComponent::SetConstraintAspectRatio.
// Member: at once, and again each tick while it has none, player 2 takes the member in the 2nd slot of the game's
// party order (players/party_member.h) through the game's leader funnel (players/party_seat.h), and its own field
// camera operator follows that member (players/player_two_camera.h). While the slot's member is not free player 2
// stays in and waits, logging why once. It follows the slot through the game's own funnels (players/party_events.h):
// before the game seats player 1 on its member, and when a formation rebuild drops the member, it is given back
// (through the same funnel) and chosen again; after a rebuild its PLAYER actor type is set again; at an event the
// member is handed to its AI and taken back as the game does for its leader; a story script that takes it (a field
// AI layer, the cinema modes) gets it back until it is free again. A periodic check backs the funnels up, and a
// member destroyed under player 2 (a retry rebuilds the party without a field exit) is let go of without a call.
// Leave: the member given back first, then the actors player 2's controller and camera manager spawned, which the
// engine never destroys, are destroyed, then UGameInstance::RemoveLocalPlayer. The engine's own list is the
// evidence of both.
// World changes: a retry, load, jump or return to the title takes player 2 out first (players/field_exit.h, with a
// field-step check in Tick as the fallback), and player 2 joins again, taking its member again, once the field has
// been steady for a moment with player 1 on a character.
//
// One further player today (seat 1); the member modules are seat-indexed for a third.

#include <chrono>
#include <cstdint>
#include <string>

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
    /// player 2 out at a field exit the request did not report, rejoins player 2 after a world change, takes the
    /// member once free, and follows the party order.
    void Tick() noexcept;
    /// Logs every local player: controller, camera manager, main camera actor, pawn, view shape.
    void Report() const noexcept;
    /// Report, plus each player's input/movement/camera call counts since the last one.
    void Status() const noexcept;

    bool HasPlayerTwo() const noexcept { return playerTwo_ != nullptr; }
    /// The game instance's local player `index` (0 or 1), or null. Game thread.
    RC::Unreal::UObject* LocalPlayer(int index) const noexcept;
    /// Player 2 holds its member.
    bool Seated() const noexcept;
    /// The member player 2 plays (type and row), or why it waits for one; for the `p2.member` reading.
    std::wstring MemberReading() const noexcept;
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
        bool                                  steady   = false;  // the field has been ready since `since`
        std::chrono::steady_clock::time_point since{};
        int                                   failures = 0;      // joins that did not add player 2
    };

    static void OnFieldExit(void* self, const wchar_t* caller) noexcept;
    /// Leave's work. A world change cannot wait: it takes player 2 out even if the member's release failed.
    bool TakeOut(bool worldChange) noexcept;
    void LeaveForWorldChange(const wchar_t* why) noexcept;
    void TryRejoin() noexcept;
    /// Player 2 is gone by the engine's hand: the mod's own records of it are dropped, nothing is touched.
    void Forget() noexcept;
    /// Player 2 takes its slot's member if it is free now (the wait is logged once per reason).
    void TrySeat() noexcept;
    /// Player 2's member is no longer its to play: given back, and chosen again next tick.
    void ReleaseMember(const wchar_t* why) noexcept;
    /// The party order, the member's existence and its AI's hold, checked while player 2 holds a member.
    void FollowSlot() noexcept;
    /// The game's party funnels (players/party_events.h).
    static void OnLeaderChanging(void* self, void* character) noexcept;
    static void OnStreamFinishing(void* self) noexcept;
    static void OnStreamFinished(void* self) noexcept;
    static void OnLeaderHandedToAi(void* self) noexcept;
    static void OnLeaderReturned(void* self) noexcept;
    /// Player 2's field camera operator forgotten and destroyed (the member's release or the leave).
    void DestroyOperator() noexcept;

    RC::Unreal::UObject* playerTwo_    = nullptr;  // ULocalPlayer
    RC::Unreal::UObject* gameInstance_ = nullptr;  // kept after a leave: the instance lives as long as the game
    Rejoin               rejoin_;
    unsigned long        gameThread_         = 0;  // the engine tick's thread (Tick)
    bool                 worldChangeStuck_   = false;  // a world change could not take player 2 out; not retried
    bool                 fieldExitInstalled_ = false;
    std::uintptr_t       removeLocalPlayer_ = 0;
    bool                 cameraInstalled_   = false;
    bool                 seatInstalled_     = false;
    bool                 eventsInstalled_   = false;
    bool                 handedToAi_        = false;    // player 2's member is with its AI for an event, as the leader is
    std::int32_t         memberWeak_[2]     = {-1, 0};  // the seated member, to notice it destroyed
    std::uint32_t        waitLogged_        = 0;        // the wait last logged (type, reason, ordinal), 0 none
    std::uint32_t        seatRetryAt_       = 0;        // the tick a refused seat is tried again
    std::uint32_t        ticks_             = 0;
};

}  // namespace lss::players
