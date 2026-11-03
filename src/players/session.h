#pragma once

// The further players (seats) joining, taking their party member, and leaving, through UE's and the game's own
// paths, on the game thread. Seat 1 is player 2; the seats are built one at a time (players/seats.h, plan/seats.md).
//
// Join: UGameplayStatics::CreatePlayer(ControllerId = the seat, spawn a controller) inside own_controller's join
// scope, so the player gets a real End player controller and camera manager of its own; the engine appends the
// player to its list after the players already in (the mirrors leave first), so its column is the next free one:
// player 3 joining with player 2 out takes column 1, and keeps it when player 2 joins after. The engine then lays
// out the split screen by itself. Each view is filled (the game's main cameras ask for 16:9) with the stock setter
// UCameraComponent::SetConstraintAspectRatio.
// A further player exists only with a member to play (the user, 2026-10-21, as in Rebirth): a join first asks the
// member test, and with the row's member not free nothing is created; the player waits outside the game and joins,
// seating at once, when the member is free (the field steady, player 1 on a character; Ctrl+F8 stops the wait). A
// second camera without a member would float alone over an empty view.
// Member: the seat takes the member in its row of the game's party order (players/party_member.h: seat 1 the 2nd
// row) through the game's leader funnel (players/party_seat.h), and its own field camera operator follows that
// member (players/seat_camera.h). It follows the row through the game's own funnels (players/party_events.h):
// before the game seats player 1 on its member, and when a formation rebuild drops the member, it is given back
// (through the same funnel) and the row's new member seated next tick, or, with none free, the player leaves and
// waits as at a join; after a rebuild its PLAYER actor type is set again; when the game hands its leader to its AI
// (an event, a field action, the field control lock) the member is handed to its AI and taken back when the game
// takes its leader back, once no script still holds the member (a periodic check backs both funnels up on the
// game's own leader state); a story script that takes the member (a script layer of its field AI, an animation, the
// cinema modes: players/field_hold.h) or a guide the level waits on (a lead or a run to a mark: players/lead_gate.h)
// gets it the same way the game's leader hand does, the seat and the player's camera kept on it, until the hold
// ends. A member destroyed under the player (a retry rebuilds the party without a field exit) is let go of without
// a call.
// Leave: the member given back first, then the actors the player's controller and camera manager spawned, which the
// engine never destroys, are destroyed, then UGameInstance::RemoveLocalPlayer. The engine's own list is the
// evidence of both.
// World changes: a retry, load, jump or return to the title takes every seat out first, the last seat first
// (players/field_exit.h, with a field-step check in Tick as the fallback), and each joins again in seat order (a
// seat waits while a lower seat's rejoin is pending, so each takes back its column), taking its member again, once
// the field has been steady for a moment with player 1 on a character.
//
// State ownership: the session owns each seat's record (its local player, the rejoin, the wait, the hand and hold,
// the member's weak pair, the log keys) and resets it on the leave, the release and the forget; the seat's game
// objects (its controller, operator and member) are the registry's (players/seat_objects.h: the session binds the
// controller at the join and forgets the seat at the leave), and the modules a seat drives own what they make (its
// controller's scopes, its operator, its trigger, its member's seat). Every module the session tells of a leave is
// per seat (plan/seats.md step 3).

#include "players/party_member.h"
#include "players/seat_state.h"
#include "players/seats.h"

#include <array>
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
    Session() noexcept;  // numbers the seats

    /// Checks and resolves everything the join and leave need. False (logged) if anything is missing.
    bool Install(const platform::GameImage& image) noexcept;
    /// Puts back the field-exit calls (unload).
    void Remove() noexcept;

    /// The seat's player joins on its member; with none free, waits outside the game for one (the player's intent). A
    /// seat fully out first takes the lowest rank no other player holds (party_member::Rank): the next party member no
    /// human plays.
    void Join(int seat) noexcept;
    /// The seat's player steps its rank to the next one no other player holds, `dir` +1 right or -1 left (a player window's
    /// arrows): its member is given back and the new row's seated by the session's follow, as at a story party change.
    /// False (nothing changed) when the seat cannot step (CanStepMember).
    bool StepMember(int seat, int dir) noexcept;
    /// The seat plays its member (seated, not handed to its AI, standing in for no one), no battle is on, and another
    /// rank is free to step to.
    bool CanStepMember(int seat) const noexcept;
    /// The actor of the seat's row now (its rank's, or the stand-in's), whatever its state: a player window's name. Null
    /// without one.
    void* RowActor(int seat) const noexcept;
    /// Why the seat's last join or wait found no member (party_member::Wait::None after a seat or a leave).
    party_member::Wait WaitReason(int seat) const noexcept
    {
        return SeatBuilt(seat) ? seats_[seat].lastWait : party_member::Wait::None;
    }
    /// The seat's player leaves (the player's intent); also cancels a rejoin after a world change and a wait.
    void Leave(int seat) noexcept;
    /// Once per engine tick, after players/game_state's: lets go of a player the engine no longer lists, takes
    /// every seat out at a field exit the request did not report, rejoins each after a world change or once its
    /// member is free, seats the member, and follows the party order.
    void Tick() noexcept;
    /// Logs every local player: controller, camera manager, main camera actor, pawn, view shape.
    void Report() const noexcept;
    /// Report, plus each player's input/movement/camera call counts since the last one.
    void Status() const noexcept;

    /// The seat's player is in (its local player listed by the engine).
    bool In(int seat) const noexcept { return SeatBuilt(seat) && seats_[seat].localPlayer != nullptr; }
    /// How many further players are in (the views beyond player 1's that a seat fills).
    int Count() const noexcept;
    /// The controller ids the players hold, as bits (bit 0 player 1, bit i a seat in): what a mirror may not take.
    std::uint32_t SeatIds() const noexcept;
    /// The game instance's local player `index` (a column: 0 is player 1), or null (the instance is found within a
    /// second of the first tick). Game thread.
    RC::Unreal::UObject* LocalPlayer(int index) const noexcept;
    /// The seat's own local player while it is in (whatever its column), or null.
    RC::Unreal::UObject* SeatPlayer(int seat) const noexcept { return SeatBuilt(seat) ? seats_[seat].localPlayer : nullptr; }
    /// The seat's player holds its member.
    bool Seated(int seat) const noexcept;
    /// The seat's member is with its AI while the game's leader is (a scene, a field action, the field control
    /// lock), or for its own scripted hold; the seat is kept and comes back when the hand ends.
    bool HandedToAi(int seat) const noexcept { return SeatBuilt(seat) && seats_[seat].handedToAi; }
    /// The seat's player was asked for and is out until the row's member is free for it.
    bool Awaiting(int seat) const noexcept { return SeatBuilt(seat) && seats_[seat].awaiting; }
    /// The member the seat plays (type and row), or why it waits for one; for the `p2.member` reading.
    std::wstring MemberReading(int seat) const noexcept;
    /// Every deployed member's AI detail; for the `party.ai` reading.
    std::wstring FormationReading() const noexcept;
    /// The seat's player was taken out by a world change and joins again once the field is back.
    bool RejoinPending(int seat) const noexcept { return SeatBuilt(seat) && seats_[seat].rejoin.pending; }
    /// The seat's state in one word (players/seat_state.h), from the record's flags.
    SeatState State(int seat) const noexcept
    {
        return StateOf({.in = In(seat), .seated = Seated(seat), .handedToAi = HandedToAi(seat), .awaiting = Awaiting(seat),
                        .rejoinPending = RejoinPending(seat)});
    }

    /// An engine console command, as typed in the console (UKismetSystemLibrary::ExecuteConsoleCommand with
    /// player 1's controller). False if the call could not be made.
    static bool Console(const wchar_t* command) noexcept;

    /// The mirror player's local players (views/mirror_player.h): how many are in (they fill the columns the seats
    /// are out of, so the join and the rejoin expect them beside the players), and the call that takes them out
    /// inside the join, right before CreatePlayer, so the seat takes its column on the same tick; and the call that
    /// fills the column the tick a seat leaves for a world change, so the loading screen is drawn by every view.
    struct MirrorPlayers
    {
        std::int32_t (*count)() noexcept           = nullptr;
        void (*clear)(const wchar_t* why) noexcept = nullptr;
        /// `playersIn` and `seatIds` (bit i: controller id i is a player's) as the list stands after the leave.
        void (*fillAtLeave)(const wchar_t* why, std::int32_t playersIn, std::uint32_t seatIds) noexcept = nullptr;
    };
    void SetMirrorPlayers(MirrorPlayers mirror) noexcept { mirror_ = mirror; }

private:
    /// After a world change took the seat's player out.
    struct Rejoin
    {
        bool                                  pending  = false;
        bool                                  steady   = false;  // the field has been ready since `since`
        std::chrono::steady_clock::time_point since{};
        int                                   failures = 0;      // joins that did not add the player
    };

    static constexpr int kNoColumn = 1000;  // a seat that held no column: rejoins after every seat that did

    /// One further player: everything the session keeps per seat. `index` is the seat (the scopes' numbering,
    /// players/seats.h: 1 = player 2) and the controller id; 0 = not numbered. Its place in the engine's list (its
    /// column) is wherever the join appended it.
    struct Seat
    {
        int                  index          = 0;
        RC::Unreal::UObject* localPlayer    = nullptr;  // ULocalPlayer while in
        Rejoin               rejoin;
        bool                 awaiting       = false;    // asked for, out until the row's member is free
        bool                 handedToAi     = false;    // its member is with its AI, as the game's leader is, or for its own hold
        bool                 holdHand       = false;    // the hand is for the member's own scripted hold: it ends with the hold, not the game's return
        party_member::Wait   hold           = party_member::Wait::None;  // that hold, for the reading
        bool                 returnWaitLogged = false;  // the game took its leader back while a script still held the member
        bool                 keptThroughLock = false;   // the game's leader is with its AI for an interaction lock; the member stayed
        std::int32_t         memberWeak[2]  = {-1, 0};  // the seated member, to notice it destroyed
        std::uint32_t        waitLogged     = 0;        // the wait last logged (type, reason, ordinal), 0 none
        party_member::Wait   lastWait       = party_member::Wait::None;  // the reason of the last wait (WaitReason)
        std::uint32_t        seatRetryAt    = 0;        // the tick a refused seat is tried again
        std::uint32_t        seatGraceUntil = 0;        // after a leader change took its member: no leave to wait before this tick
        std::uint8_t         standIn        = 9;        // player 1's former character's type the seat plays since the game's leader
                                                        // change took its member (9 = facts::kPlayerTypeNone: none), until the
                                                        // game's leader goes home (the change's clear, or the tick while player
                                                        // 1 is on the field leader), a world change or a party rebuild (kept
                                                        // through a leave to wait)
        std::uint32_t        awaitSince     = 0;        // the tick the wait for a member began (0 none): the wait's length in the join line
        std::uint32_t        formationLogged = 0;       // the formation last logged under the wait (party_member::Choice::formation), 0 none
        int                  lastColumn     = kNoColumn;  // the column held when a world change took it out (the rejoin's order)

        /// "player 2" for seat 1: the name every line of the seat's uses.
        const wchar_t* Name() const noexcept { return SeatName(index); }
        /// The seat's controller id (the engine routes pad `index` to it).
        std::int32_t ControllerId() const noexcept { return index; }
        /// The member's records cleared: the weak pair, the hand and the hold (the member was given back, forgotten
        /// or destroyed).
        void ResetMember() noexcept;
    };

    static void OnFieldExit(void* self, const wchar_t* caller) noexcept;
    /// Join's work, for the player's intent and for a rejoin or a wait (which keep the seat's rank).
    void JoinNow(int index) noexcept;
    /// The ranks the other seats hold (in, waiting for a member, or to rejoin), as party_rank_rules' bits.
    std::uint32_t RanksHeldBesides(int index) const noexcept;
    /// A seat whose rank the party no longer has (a story party change shrank it) takes the lowest free one.
    void SettleRank(Seat& seat, void* playerOneController) noexcept;
    /// Player 1's controller, or null.
    RC::Unreal::UObject* PlayerOneController() const noexcept;
    /// Leave's work for one seat. A world change cannot wait: it takes the player out even if the member's release
    /// failed.
    bool TakeOut(Seat& seat, bool worldChange) noexcept;
    /// Every seat out, the last first (the engine list's order), each to join again once the field is back.
    void LeaveForWorldChange(const wchar_t* why) noexcept;
    /// The seat's player joins again: after a world change once the field has been back for a moment, and while
    /// it waits for a member once the row's member is free.
    void TryRejoin(Seat& seat) noexcept;
    /// The seat's player is in without a member and none is free: it leaves and waits outside the game for one.
    void LeaveToAwait(Seat& seat, const wchar_t* why) noexcept;
    /// The wait for a member, logged once per reason (the row, its type, the reason).
    void LogWait(Seat& seat, const party_member::Choice& choice, const wchar_t* who) noexcept;
    void LogFormation(Seat& seat, const party_member::Choice& choice) noexcept;  // at Debug, once per formation change under the wait
    /// The seat's player is gone by the engine's hand: the mod's own records of it are dropped, nothing is touched.
    void Forget(Seat& seat) noexcept;
    /// The seat takes its row's member if it is free now. False with the wait in `choice`.
    bool TrySeat(Seat& seat, party_member::Choice& choice) noexcept;
    /// The seat's member is no longer its to play: given back; the row's member is seated next tick, or the player
    /// leaves to wait for one.
    void ReleaseMember(Seat& seat, const wchar_t* why) noexcept;
    /// A scripted hold on the member (a script layer, an animation, a cutscene mode, a guide the level waits on): the
    /// member to its AI with the seat kept (the game's own hand), and back when the hold ends.
    void HandForHold(Seat& seat, void* member, party_member::Wait hold) noexcept;
    void ReturnFromHold(Seat& seat, void* member) noexcept;
    /// The party order, the member's existence and its AI's hold, checked while the seat holds a member.
    void FollowSlot(Seat& seat) noexcept;
    /// The game's party funnels (players/party_events.h), each answered for every seat.
    static void OnLeaderChanging(void* self, void* character) noexcept;
    static void OnStreamFinishing(void* self) noexcept;
    static void OnStreamFinished(void* self) noexcept;
    static void OnLeaderHandedToAi(void* self) noexcept;
    static void OnLeaderReturned(void* self) noexcept;
    void HandWithLeader(Seat& seat) noexcept;
    void ReturnWithLeader(Seat& seat) noexcept;
    /// The battle modules let go of the seat's member before it is given back, destroyed or forgotten: its lock and
    /// target end, its menu lets go of its focus layers, inside their own context while the member still exists.
    static void EndMemberModules(const Seat& seat) noexcept;
    /// The seat's field camera operator forgotten and destroyed (the member's release or the leave).
    void DestroyOperator(Seat& seat) noexcept;
    /// The seat's player controller, or null.
    RC::Unreal::UObject* Controller(const Seat& seat) const noexcept;

    /// The mirror players in now (0 without the module).
    std::int32_t Mirrors() const noexcept { return mirror_.count != nullptr ? mirror_.count() : 0; }

    std::array<Seat, scopes::kMaxPlayers> seats_;  // index = seat; [0] unused (player 1 is the game's)
    MirrorPlayers        mirror_;
    RC::Unreal::UObject* gameInstance_ = nullptr;  // kept after a leave: the instance lives as long as the game
    unsigned long        gameThread_         = 0;  // the engine tick's thread (Tick)
    bool                 worldChangeStuck_   = false;  // a world change could not take a seat out; not retried
    bool                 fieldExitInstalled_ = false;
    std::uintptr_t       removeLocalPlayer_ = 0;
    bool                 cameraInstalled_   = false;
    bool                 seatInstalled_     = false;
    bool                 eventsInstalled_   = false;
    std::uint32_t        ticks_             = 0;
};

}  // namespace lss::players
