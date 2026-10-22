#include "players/session.h"
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
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_exit.h"
#include "players/game_state.h"
#include "players/lead_gate.h"
#include "players/local_player_list.h"
#include "players/own_controller.h"
#include "players/party_events.h"
#include "players/party_member.h"
#include "players/party_seat.h"
#include "players/player_two_camera.h"
#include "battle/command_menu_two.h"
#include "battle/targeting_two.h"
#include "players/pad_buttons.h"
#include "players/scopes.h"
#include "players/split_render.h"

#include <Unreal/Core/Containers/FString.hpp>

#include <cstdint>
#include <format>
#include <new>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::players {

namespace {

using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

constexpr std::int32_t kPlayerTwoControllerId = 1;
constexpr int          kSeat                  = 1;  // player 2's seat (players/scopes.h, party_member.h)

/// The field is back after a world change (RE-lifecycle-world.md §8): play state, the steady step, running. The
/// entry's step 12 re-reconciles every non-leader controller, so a join before step 13 would be undone.
constexpr std::uint8_t kStatePlay  = 6;
constexpr std::int32_t kStepSteady = 13;

/// How long the field must stay back, with player 1 on a character, before player 2 rejoins; and how many joins
/// may fail before the rejoin is left to Ctrl+F7. Timed, not counted in ticks: loads tick uncapped.
constexpr std::chrono::seconds kRejoinSteady{2};
constexpr int                  kRejoinFailures = 3;
/// A refused seat (the funnel did not take the member) is tried again after this many ticks; the party order is
/// checked under a held member every this many ticks (cheap guarded reads).
constexpr std::uint32_t kSeatRetryTicks  = 60;
constexpr std::uint32_t kFollowSlotTicks = 10;

UObject* Field(const void* object, std::uint32_t offset) noexcept
{
    UObject* value = nullptr;
    return object != nullptr && platform::CopyGuarded(&value, static_cast<const std::uint8_t*>(object) + offset,
                                                      sizeof(value))
               ? value
               : nullptr;
}

using local_player_list::Describe;
using local_player_list::GameInstance;

// UE4SS's object lookups can throw (a malformed path, or running out of memory), so every function here
// that makes one catches: an exception escaping a noexcept function would end the game.

RC::Unreal::TArray<UObject*>* LocalPlayers(UObject* gameInstance) noexcept
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

bool Contains(RC::Unreal::TArray<UObject*>& players, UObject* player) noexcept
{
    for (std::int32_t i = 0; i < players.Num(); ++i)
    {
        if (players[i] == player)
        {
            return true;
        }
    }
    return false;
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

}  // namespace

bool Session::Install(const platform::GameImage& image) noexcept
{
    const auto remove = platform::FindFunction(image, facts::kRemoveLocalPlayer);
    if (!remove || !own_controller::Install(image))
    {
        return false;
    }
    removeLocalPlayer_ = *remove;
    cameraInstalled_   = player_two_camera::Install(image);  // optional: without it player 2's camera stays still
    seatInstalled_     = party_seat::Install(image) && party_member::Install(image);  // optional: without it player 2 has no member
    split_render::Install(image);                            // optional: without it the halves keep the split rule's reduced effects
    fieldExitInstalled_ = field_exit::Install(image, &Session::OnFieldExit, this);  // without it, Tick's step check alone
    if (seatInstalled_)
    {
        // Without it the periodic check alone follows the party (one tick late at a leader change).
        eventsInstalled_ = party_events::Install(image, {.leaderChanging   = &Session::OnLeaderChanging,
                                                         .streamFinishing  = &Session::OnStreamFinishing,
                                                         .streamFinished   = &Session::OnStreamFinished,
                                                         .leaderHandedToAi = &Session::OnLeaderHandedToAi,
                                                         .leaderReturned   = &Session::OnLeaderReturned,
                                                         .context          = this});
    }
    return true;
}

void Session::Remove() noexcept
{
    if (fieldExitInstalled_)
    {
        field_exit::Remove();
        fieldExitInstalled_ = false;
    }
    if (eventsInstalled_)
    {
        party_events::Remove();
        eventsInstalled_ = false;
    }
}

void Session::OnFieldExit(void* self, const wchar_t* caller) noexcept
{
    auto* session = static_cast<Session*>(self);
    if (!session->HasPlayerTwo())
    {
        return;
    }
    if (GetCurrentThreadId() != session->gameThread_)
    {
        platform::Log(L"field exit: requested off the game thread ({}); player 2 is left to the field-step check", caller);
        return;
    }
    session->LeaveForWorldChange(caller);
}

void Session::LeaveForWorldChange(const wchar_t* why) noexcept
{
    platform::Log(L"world change ({}): player 2 leaves first{}", why, Seated() ? L" and gives its member back" : L"");
    if (!TakeOut(true))
    {
        worldChangeStuck_ = true;
        platform::Log(L"world change: player 2 could NOT be taken out; the game tears the field down with player 2 in");
        return;
    }
    rejoin_ = Rejoin{.pending = true};
    platform::Log(L"world change: player 2 joins again once the field is back (Ctrl+F8 cancels)");
}

void Session::Tick() noexcept
try
{
    gameThread_                     = GetCurrentThreadId();
    ++ticks_;
    // The instance lives as long as the game; found once (a UE4SS object walk, never per tick: one try a second until
    // it exists), so LocalPlayer() answers before any join: the HUD's two-view layout under the mirror player read
    // every view through it and saw none in a session without a join (the user, 2026-10-22 10:13: markers drifting).
    if (gameInstance_ == nullptr && ticks_ % 60 == 1)
    {
        gameInstance_ = GameInstance();
    }
    if (eventsInstalled_)
    {
        party_events::Tick();  // the entry redirects, placed from here (outside the functions, on their thread)
    }
    const game_state::Snapshot game = game_state::Current();
    const bool steady = game.valid && game.hasRoot && game.running && game.step == kStepSteady;
    if (steady)
    {
        worldChangeStuck_ = false;
    }
    if (!HasPlayerTwo())
    {
        TryRejoin();  // after a world change, or for the member player 2 waits on
        return;
    }
    auto* players = LocalPlayers(gameInstance_);
    if (players != nullptr && !Contains(*players, playerTwo_))
    {
        platform::Log(L"player 2 was removed by the engine; the mod lets go of it");
        Forget();
        return;
    }
    // The fallback: the field is leaving (running cleared, or an exit step) and the request was not seen.
    if (!worldChangeStuck_ && game.valid && game.hasRoot && (!game.running || game.step > kStepSteady))
    {
        platform::Log(L"field exit not seen at the request (state {}, step {}, running {})", game_state::StateName(game.state),
                      game.step, game.running ? L"on" : L"off");
        LeaveForWorldChange(L"the field-step check");
        return;
    }
    if (!seatInstalled_ || !steady || game.state != kStatePlay)
    {
        return;
    }
    if (!Seated())
    {
        // After a release (the row changed, or its member left): the row's member now, or out until one is free. An
        // unreadable party is a tick's thing, not a wait.
        party_member::Choice choice;
        if (!TrySeat(choice) && !Seated() && ticks_ >= seatRetryAt_ && choice.wait != party_member::Wait::Unread)
        {
            LogWait(choice, L"player 2 is in without a member");
            LeaveToAwait(party_member::Describe(choice.wait));
        }
    }
    else if (ticks_ % kFollowSlotTicks == 0)
    {
        FollowSlot();
    }
}
catch (...)
{
    platform::Log(L"session tick: an engine lookup raised an exception");
}

void Session::TryRejoin() noexcept
try
{
    if (!rejoin_.pending && !awaiting_)
    {
        return;
    }
    const game_state::Snapshot game = game_state::Current();
    auto*                      players = LocalPlayers(gameInstance_);
    // Player 1 on a character also keeps the title screen out (a field in the play state too, with no character).
    const bool ready = game.valid && game.state == kStatePlay && game.hasRoot && game.running && game.step == kStepSteady &&
                       players != nullptr && players->Num() == 1 + Mirrors() &&
                       Field(Field((*players)[0], facts::kLocalPlayerController), facts::kControllerPawn) != nullptr;
    const auto now = std::chrono::steady_clock::now();
    if (!ready)
    {
        rejoin_.steady = false;
        if (awaiting_ && !rejoin_.pending)
        {
            rejoin_.pending = true;  // the field left under the wait: the join after it settles as after any world change
        }
        return;
    }
    if (rejoin_.pending)
    {
        if (!rejoin_.steady)
        {
            rejoin_.steady = true;
            rejoin_.since  = now;
            platform::Log(L"rejoin: the field is back ({}); player 2 joins in {} s if it stays", game.location,
                          kRejoinSteady.count());
            return;
        }
        if (now - rejoin_.since < kRejoinSteady)
        {
            return;
        }
    }
    const bool wasAwaiting = awaiting_;
    if (awaiting_)
    {
        // The member first: nothing is created for player 2 until the row's member is free (logged once per reason).
        if (ticks_ < seatRetryAt_)
        {
            return;  // the seat funnel refused the member last time; not tried again before its retry tick
        }
        const party_member::Choice choice = party_member::Choose(kSeat, Field((*players)[0], facts::kLocalPlayerController), nullptr, false);
        if (choice.member == nullptr)
        {
            LogWait(choice, L"player 2 joins once its member is free");
            rejoin_ = Rejoin{};  // the field has settled; the wait goes on without the rejoin's delay
            return;
        }
        awaiting_ = false;  // Join sets it again if the member is gone by then
    }
    const bool afterWorldChange = rejoin_.pending;
    Join();
    if (!HasPlayerTwo())
    {
        if (awaiting_)
        {
            rejoin_ = Rejoin{};  // the join found the member not free after all: the wait goes on
            return;
        }
        // The join itself failed (logged above): a few more tries, each tick for a wait, after the settle for a rejoin.
        rejoin_.steady = false;
        awaiting_      = wasAwaiting;
        if (++rejoin_.failures >= kRejoinFailures)
        {
            rejoin_   = Rejoin{};
            awaiting_ = false;
            platform::Log(L"{}: player 2 did not join in {} tries (see above); press Ctrl+F7 to join",
                          afterWorldChange ? L"rejoin" : L"the wait for a member", kRejoinFailures);
        }
        return;
    }
    rejoin_ = Rejoin{};
    if (afterWorldChange)
    {
        platform::Log(L"rejoin: player 2 is back");
    }
}
catch (...)
{
    platform::Log(L"rejoin: an engine lookup raised an exception");
}

void Session::LeaveToAwait(const wchar_t* why) noexcept
{
    platform::Log(L"player 2 leaves: {}; it joins again once its member is free (Ctrl+F8 stops the wait)", why);
    if (!TakeOut(false))
    {
        platform::Log(L"player 2 could not leave (see above); it stays in without a member");
        return;
    }
    awaiting_ = true;  // the wait's reason stays logged: the join's wait line names it again only when it changes
}

void Session::LogWait(const party_member::Choice& choice, const wchar_t* who) noexcept
{
    const std::uint32_t key = 1u | static_cast<std::uint32_t>(choice.type) << 8 | static_cast<std::uint32_t>(choice.wait) << 16 |
                              static_cast<std::uint32_t>(choice.ordinal + 1) << 24;
    if (key == waitLogged_)
    {
        return;
    }
    if (waitLogged_ == 0)
    {
        awaitSince_ = ticks_;
    }
    waitLogged_ = key;
    platform::Log(L"{} (row {} of the party order, type {}): {}; the game's party order chooses it, so a party change ends the wait",
                  who, choice.ordinal, choice.type, party_member::Describe(choice.wait));
    if (party_member::ScriptHold(choice.wait) || choice.wait == party_member::Wait::NoFieldAi ||
        choice.wait == party_member::Wait::LeaderHanded || choice.wait == party_member::Wait::EventLock)
    {
        // The member's AI and the lock mask as the take test saw them (the actor of the row, read again).
        auto* players = LocalPlayers(gameInstance_);
        if (players != nullptr && players->Num() >= 1)
        {
            platform::Log(L"  the row's member: {}",
                          party_member::AiDetail(party_member::RowActor(kSeat, Field((*players)[0], facts::kLocalPlayerController))));
        }
    }
}

void Session::Forget() noexcept
{
    playerTwo_ = nullptr;
    battle::targeting_two::OnLeave();
    battle::command_menu_two::OnLeave();
    scopes::Unbind(1);
    split_render::OnLeave();
    pad_buttons::SetPlayers(1);
    if (cameraInstalled_)
    {
        player_two_camera::Take();  // forgotten, not destroyed: it belonged to the old world
    }
    if (seatInstalled_)
    {
        party_seat::Forget(kSeat);
    }
    memberWeak_[0] = -1;
    memberWeak_[1] = 0;
    waitLogged_    = 0;
    handedToAi_    = false;
    holdHand_      = false;
    hold_          = party_member::Wait::None;
}

void Session::Join() noexcept
try
{
    if (playerTwo_ != nullptr)
    {
        platform::Log(L"player 2 is already in");
        return;
    }
    if (awaiting_)
    {
        platform::Log(L"player 2 already waits for its member to be free (see above; Ctrl+F8 stops the wait)");
        return;
    }
    if (!own_controller::CanRename())
    {
        platform::Log(L"join refused: the camera actor's name text cannot be changed (page protection refused)");
        return;
    }
    UObject* gameInstance = GameInstance();
    auto*    players      = LocalPlayers(gameInstance);
    const std::int32_t mirrors = Mirrors();  // the mirror player holds the column player 2 takes (views/mirror_player.h)
    if (players == nullptr || players->Num() != 1 + mirrors)
    {
        platform::Log(L"join refused: expected exactly one local player{}, found {}", mirrors != 0 ? L" plus the mirror" : L"",
                      players == nullptr ? -1 : players->Num());
        return;
    }
    UObject* playerOneController = Field((*players)[0], facts::kLocalPlayerController);
    if (Field(playerOneController, facts::kControllerPawn) == nullptr)
    {
        platform::Log(L"join refused: player 1 controls no character (title screen or a load); join in the field");
        return;
    }
    if (!own_controller::UsesNativeClasses(playerOneController))
    {
        platform::Log(L"join refused: player 1's controller or camera manager is not the End class the research read ({})",
                      Describe(playerOneController));
        return;
    }
    // The member first (the user, 2026-10-21): no controller and camera for player 2 until the row's member is free.
    // The instance is kept for the wait: found once, it lives as long as the game.
    gameInstance_ = gameInstance;
    if (seatInstalled_)
    {
        const party_member::Choice choice = party_member::Choose(kSeat, playerOneController, nullptr, false);
        if (choice.member == nullptr)
        {
            awaiting_ = true;
            LogWait(choice, L"player 2 joins once its member is free");
            return;
        }
    }
    // The mirror player leaves its column inside the join, so player 2 takes it on the same tick.
    if (mirrors != 0)
    {
        mirror_.clear(L"player 2 joins");
        if (players->Num() != 1)
        {
            platform::Log(L"join refused: the mirror player did not leave its column ({} local players)", players->Num());
            return;
        }
    }
    const local_player_list::Added added = local_player_list::Add(playerOneController, kPlayerTwoControllerId);
    if (!added.called && added.localPlayer == nullptr && !added.managerSeen)
    {
        platform::Log(L"join refused: UGameplayStatics.CreatePlayer is not the function the mod expects, or raised at once");
    }
    platform::Log(L"join: {}", added.Text(L"player 2's"));

    // The engine's list decides what happened: a player added before an exception must still be tracked,
    // so it can be removed before anything logs it in again.
    if (players->Num() != 2)
    {
        platform::Log(L"join failed: the engine did not add a local player (count now {})", players->Num());
        return;
    }
    playerTwo_ = (*players)[1];
    awaiting_  = false;

    UObject* playerTwoController = Field(playerTwo_, facts::kLocalPlayerController);
    UObject* managerOne          = Field(playerOneController, facts::kControllerCameraManager);
    UObject* managerTwo          = Field(playerTwoController, facts::kControllerCameraManager);
    const bool filledOne         = local_player_list::FillView(managerOne);
    const bool filledTwo         = local_player_list::FillView(managerTwo);
    platform::Log(L"join: player 2 is in; halves filled: player 1 {}, player 2 {}", filledOne ? L"yes" : L"NO",
                  filledTwo ? L"yes" : L"NO");
    split_render::OnJoin();
    pad_buttons::SetPlayers(2);  // from here pad 2's buttons are player 2's
    scopes::BindController(kSeat, playerTwoController);
    seatRetryAt_ = 0;
    if (seatInstalled_)
    {
        party_member::Choice choice;
        if (!TrySeat(choice) && !Seated() && ticks_ >= seatRetryAt_ && choice.wait != party_member::Wait::Unread)
        {
            // The member checked free a moment ago is not any more (a funnel ran in between): out, and the wait. A
            // seat the funnel refused is not this: player 2 stays in and the seat is tried again at its retry tick.
            LogWait(choice, L"player 2 joined but its member is not free any more");
            LeaveToAwait(party_member::Describe(choice.wait));
        }
    }
    else
    {
        Report();
    }
}
catch (...)
{
    platform::Log(L"join: an engine lookup raised an exception; player 2 {}", playerTwo_ != nullptr ? L"is tracked" : L"is not in");
}

void Session::Leave() noexcept
{
    if (rejoin_.pending)
    {
        rejoin_ = Rejoin{};
        platform::Log(L"player 2 no longer rejoins after the world change");
    }
    if (awaiting_)
    {
        awaiting_   = false;
        waitLogged_ = 0;
        platform::Log(L"player 2 no longer waits for a party member");
    }
    if (playerTwo_ == nullptr)
    {
        platform::Log(L"player 2 is not in");
        return;
    }
    TakeOut(false);
}

bool Session::TakeOut(bool worldChange) noexcept
try
{
    auto* players = LocalPlayers(gameInstance_);
    if (players == nullptr || !Contains(*players, playerTwo_))
    {
        platform::Log(L"leave: the engine no longer lists player 2; letting go without touching it");
        Forget();
        return true;
    }
    // The actors player 2's controller and camera manager spawned, which nothing destroys (the End camera
    // manager has no EndPlay or Destroyed override). Read before the removal destroys their owners.
    UObject*   controller = Field(playerTwo_, facts::kLocalPlayerController);
    const auto leftovers  = local_player_list::ReadLeftovers(playerTwo_);
    // Player 2's lock and target end, and its menu lets go of its focus layers inside its own context, while its
    // controller and member still exist.
    battle::targeting_two::OnLeave();
    battle::command_menu_two::OnLeave();
    // Player 2's member goes back to its AI first, through the game's leader funnel.
    if (seatInstalled_ && !party_seat::Unseat(kSeat, Field((*players)[0], facts::kLocalPlayerController), controller))
    {
        if (!worldChange)
        {
            platform::Log(L"leave refused: player 2's controller still holds its member; press Ctrl+F8 again to retry");
            return false;
        }
        // The game destroys and rebuilds the party next; removing the controller unpossesses the member on the way.
        platform::Log(L"leave: player 2's controller still holds its member; the world change takes player 2 out regardless");
    }
    // Player 2's field camera operator next: its cleanup unbinds the delegates it bound on player 2's
    // controller, which must still exist then.
    DestroyOperator();

    platform::Log(L"leave: player 2's {}", leftovers.Text());

    bool       raised  = false;
    const bool removed = local_player_list::Remove(gameInstance_, playerTwo_, removeLocalPlayer_, raised);
    if (!removed)
    {
        platform::Log(L"leave failed: the engine did not remove player 2{}", raised ? L" (the call raised an exception)" : L"");
        return false;
    }
    playerTwo_ = nullptr;
    split_render::OnLeave();
    pad_buttons::SetPlayers(1);
    if (seatInstalled_)
    {
        party_seat::Forget(kSeat);  // a later player 2 is a new controller, set up afresh
    }
    memberWeak_[0] = -1;
    memberWeak_[1] = 0;
    handedToAi_    = false;
    holdHand_      = false;
    hold_          = party_member::Wait::None;
    scopes::Unbind(kSeat);
    const auto gone = local_player_list::DestroyLeftovers(leftovers);
    platform::Log(L"leave: player 2 removed{}; leftovers destroyed: camera actor {}, battle-talk actor {}",
                  raised ? L" (the call raised an exception after removing it)" : L"", gone.camera ? L"yes" : L"no",
                  gone.talk ? L"yes" : L"no");
    return true;
}
catch (...)
{
    platform::Log(L"leave: an engine lookup raised an exception");
    return playerTwo_ == nullptr;
}

bool Session::TrySeat(party_member::Choice& choice) noexcept
try
{
    auto* players = LocalPlayers(gameInstance_);
    if (playerTwo_ == nullptr || !seatInstalled_ || players == nullptr || !Contains(*players, playerTwo_) || ticks_ < seatRetryAt_)
    {
        choice.wait = party_member::Wait::Unread;
        return false;
    }
    UObject* playerOne = Field((*players)[0], facts::kLocalPlayerController);
    UObject* playerTwo = Field(playerTwo_, facts::kLocalPlayerController);
    choice             = party_member::Choose(kSeat, playerOne, playerTwo, false);
    if (choice.member == nullptr)
    {
        return false;
    }
    if (!party_seat::Seat(kSeat, playerOne, playerTwo, choice.member, choice.type))
    {
        seatRetryAt_ = ticks_ + kSeatRetryTicks;  // the refusal is logged by the seat; tried again later, player 2 in
        return false;
    }
    if (!player_two_camera::WeakPairOf(choice.member, memberWeak_))
    {
        memberWeak_[0] = -1;
        memberWeak_[1] = 0;
    }
    // After a wait: how long player 2 was without a member (the single view's length).
    platform::Log(L"player 2 plays member type {} (row {} of the party order){}", choice.type, choice.ordinal,
                  waitLogged_ == 0 ? L"" : std::format(L", {} ticks after it began to wait for one", ticks_ - awaitSince_));
    waitLogged_ = 0;
    if (cameraInstalled_ && !player_two_camera::Start(playerOne, playerTwo, choice.member))
    {
        platform::Log(L"player 2 keeps a still camera; each player's sticks are still their own pad's");
    }
    Report();
    return true;
}
catch (...)
{
    platform::Log(L"seat: an engine lookup raised an exception");
    choice.wait = party_member::Wait::Unread;
    return false;
}

void Session::DestroyOperator() noexcept
{
    UObject*   operatorTwo  = cameraInstalled_ ? static_cast<UObject*>(player_two_camera::Take()) : nullptr;
    const bool operatorGone = operatorTwo != nullptr && CallNoParams(operatorTwo, STR("/Script/Engine.Actor:K2_DestroyActor"));
    platform::Log(L"player 2's camera operator {}", operatorTwo == nullptr ? L"was not running" : operatorGone ? L"destroyed" : L"NOT DESTROYED");
}

void Session::ReleaseMember(const wchar_t* why) noexcept
try
{
    auto* players = LocalPlayers(gameInstance_);
    if (playerTwo_ == nullptr || players == nullptr || !Contains(*players, playerTwo_))
    {
        return;
    }
    platform::Log(L"player 2's member (type {}) is {}: given back; the row's member next, or player 2 leaves to wait for one",
                  party_seat::MemberType(kSeat), why);
    // Player 2's lock and target end, and its menu lets go of its focus layers inside its own context, while its
    // member still exists; the menu is set up again for the next member by its own tick.
    battle::targeting_two::OnLeave();
    battle::command_menu_two::OnLeave();
    DestroyOperator();
    party_seat::Unseat(kSeat, Field((*players)[0], facts::kLocalPlayerController), Field(playerTwo_, facts::kLocalPlayerController));
    memberWeak_[0] = -1;
    memberWeak_[1] = 0;
    handedToAi_    = false;
    holdHand_      = false;
    hold_          = party_member::Wait::None;
}
catch (...)
{
    platform::Log(L"release: an engine lookup raised an exception");
}

void Session::HandForHold(void* member, party_member::Wait hold) noexcept
{
    // The game's own hand (what it does for its leader at an event's start), the seat and player 2's camera operator
    // kept: the member's AI runs the script, player 2 watches it, and the seat takes it back when the hold ends.
    handedToAi_ = party_member::HandToAi(member);
    holdHand_   = handedToAi_;
    hold_       = hold;
    platform::Log(L"player 2's member (type {}) handed to its AI: {}: {}; player 2's view stays on it, its controller takes it back "
                  L"when the hold ends{}",
                  party_seat::MemberType(kSeat), party_member::Describe(hold), handedToAi_ ? L"yes" : L"NO (the hand faulted; tried again)",
                  hold == party_member::Wait::Guide ? std::format(L" (lead gate: {})", lead_gate::Reading()) : L"");
    platform::LogDebug(L"  the member: {}", party_member::AiDetail(member));
}

void Session::ReturnFromHold(void* member) noexcept
{
    handedToAi_ = false;
    holdHand_   = false;
    platform::Log(L"player 2's member (type {}) back on its controller: its hold ended ({}): {}; the member: {}", party_seat::MemberType(kSeat),
                  party_member::Describe(hold_), party_member::ReturnTo(member, Field(playerTwo_, facts::kLocalPlayerController)) ? L"yes" : L"NO",
                  party_member::AiDetail(member));
    hold_ = party_member::Wait::None;
}

void Session::FollowSlot() noexcept
try
{
    void* member = party_seat::Member(kSeat);
    if (member == nullptr)
    {
        return;
    }
    // Destroyed under player 2 (a retry rebuilds the party without a field exit): nothing is called on it.
    if (memberWeak_[1] != 0 && player_two_camera::ObjectOf(memberWeak_) != member)
    {
        platform::Log(L"player 2's member was destroyed under it (the party rebuilt without a field exit); player 2 lets go and chooses again");
        battle::targeting_two::OnLeave();
        battle::command_menu_two::OnLeave();
        DestroyOperator();
        party_seat::Forget(kSeat);
        memberWeak_[0] = -1;
        memberWeak_[1] = 0;
        return;
    }
    auto* players = LocalPlayers(gameInstance_);
    if (players == nullptr || players->Num() < 1)
    {
        return;
    }
    if (const wchar_t* why = party_member::WhyNotInSlot(kSeat, member, party_seat::MemberType(kSeat),
                                                       Field((*players)[0], facts::kLocalPlayerController)))
    {
        ReleaseMember(why);
        return;
    }
    if (handedToAi_)
    {
        if (holdHand_)
        {
            // The hold's end decides: the member free again (the level's next setting, the scene's end), the game's
            // leader with its player, and nothing else holding it.
            if (party_member::HoldWait(member) == party_member::Wait::None)
            {
                ReturnFromHold(member);
            }
            return;
        }
        // The game's own state decides the return (RE-script-release.md §4: the event bit alone undid a keep-group
        // hand): its leader back with a player controller. The funnel is heard at its entry; this is the backstop,
        // and the retry while a script still held the member at the game's return.
        if (party_member::LeaderWithPlayer())
        {
            OnLeaderReturned(this);
        }
        return;
    }
    const party_member::Wait hold = party_member::HoldWait(member);
    if (hold == party_member::Wait::LeaderHanded)
    {
        // The hand funnel's backstop: the game's leader is with its AI and player 2's member is not (a seat that
        // landed inside the hand, after the funnel fired: seen live 2026-10-21, Tifa taken mid-animation).
        platform::Log(L"the game's leader is with its AI (lock {:#x}) and player 2's member is not: the hand was not heard for it; "
                      L"handed now",
                      party_member::ControlLockMask());
        OnLeaderHandedToAi(this);
        return;
    }
    if (party_member::ScriptHold(hold))
    {
        HandForHold(member, hold);
    }
}
catch (...)
{
    platform::Log(L"follow: an engine lookup raised an exception");
}

void Session::OnLeaderChanging(void* self, void* character) noexcept
{
    auto* session = static_cast<Session*>(self);
    if (character == nullptr || character != party_seat::Member(kSeat))
    {
        return;
    }
    session->ReleaseMember(L"about to become player 1's leader (the game's leader change)");
}

void Session::OnStreamFinishing(void* self) noexcept
{
    auto* session = static_cast<Session*>(self);
    if (party_seat::Member(kSeat) != nullptr && !party_member::InFormation(party_seat::MemberType(kSeat)))
    {
        session->ReleaseMember(L"leaving the deployed party (its status changed)");
    }
}

void Session::OnStreamFinished(void* self) noexcept
{
    auto* session = static_cast<Session*>(self);
    void* member  = party_seat::Member(kSeat);
    if (member == nullptr)
    {
        return;
    }
    // The rebuild typed every member PARTY; the game re-types only its own leader PLAYER.
    platform::LogDebug(L"player 2's member keeps the PLAYER actor type after the formation rebuild: {}",
                       party_member::ReassertPlayerType(member) ? L"set" : L"NOT set (the setter faulted)");
    session->FollowSlot();
}

void Session::OnLeaderHandedToAi(void* self) noexcept
{
    auto* session = static_cast<Session*>(self);
    void* member  = party_seat::Member(kSeat);
    if (member == nullptr || session->handedToAi_)
    {
        return;
    }
    // A lock set in battle (a boss arena, the colosseum) hands the game's leader there too [UNKNOWN whether the
    // battle AI then runs it]: the member stays player 2's until a log shows what the game does with its leader.
    if (party_member::ActorMode(member) == facts::kActorModeBattle)
    {
        platform::LogDebug(L"the game handed its leader to its AI in battle (lock {:#x}); player 2's member stays (unread case)",
                           party_member::ControlLockMask());
        return;
    }
    session->handedToAi_      = party_member::HandToAi(member);
    session->returnWaitLogged_ = false;
    platform::LogDebug(L"player 2's member handed to its AI with the game's leader (lock {:#x}): {}; the member: {}",
                       party_member::ControlLockMask(), session->handedToAi_ ? L"yes" : L"NO", party_member::AiDetail(member));
}

void Session::OnLeaderReturned(void* self) noexcept
try
{
    auto* session = static_cast<Session*>(self);
    void* member  = party_seat::Member(kSeat);
    if (member == nullptr || !session->handedToAi_ || session->holdHand_ || session->playerTwo_ == nullptr)
    {
        return;  // a hand for the member's own hold ends with the hold (FollowSlot), not with the game's leader
    }
    // The game's leader is back; the member follows once nothing holds it (a scene's own layers can outlast the
    // lock, a guide lead written at the scene's end runs to its mark). The periodic check retries while the game's
    // leader stays with its player.
    if (const wchar_t* why = party_member::WhyHeldByScript(member))
    {
        if (!session->returnWaitLogged_)
        {
            session->returnWaitLogged_ = true;
            platform::LogDebug(L"the game took its leader back (lock {:#x}) while player 2's member is still {}; it follows when free: {}",
                               party_member::ControlLockMask(), why, party_member::AiDetail(member));
        }
        return;
    }
    session->handedToAi_ = false;
    platform::LogDebug(L"player 2's member back on its controller with the game's leader (lock {:#x}): {}", party_member::ControlLockMask(),
                       party_member::ReturnTo(member, Field(session->playerTwo_, facts::kLocalPlayerController)) ? L"yes" : L"NO");
}
catch (...)
{
    platform::Log(L"return: an engine lookup raised an exception");
}

void Session::Report() const noexcept
try
{
    auto* players = LocalPlayers(GameInstance());
    if (players == nullptr)
    {
        platform::Log(L"report: no game instance");
        return;
    }
    for (std::int32_t i = 0; i < players->Num(); ++i)
    {
        UObject* player     = (*players)[i];
        UObject* controller = Field(player, facts::kLocalPlayerController);
        UObject* manager    = Field(controller, facts::kControllerCameraManager);
        platform::Log(L"player {}: {}", i + 1, Describe(player));
        platform::Log(L"  controller {} (pawn {})", Describe(controller), Describe(Field(controller, facts::kControllerPawn)));
        platform::Log(L"  camera manager {} (owner {})", Describe(manager), Describe(Field(manager, facts::kCameraManagerPcOwner)));
        platform::Log(L"  main camera actor {}", Describe(Field(manager, facts::kCameraManagerMainCamera)));
    }
}
catch (...)
{
    platform::Log(L"report: an engine lookup raised an exception");
}

void Session::Status() const noexcept
{
    Report();
    if (playerTwo_ != nullptr && cameraInstalled_)
    {
        player_two_camera::LogCounts();
    }
}

UObject* Session::LocalPlayer(int index) const noexcept
try
{
    auto* players = LocalPlayers(gameInstance_);  // found by Tick within a second of the start
    return players != nullptr && index >= 0 && index < players->Num() ? (*players)[index] : nullptr;
}
catch (...)
{
    return nullptr;
}

bool Session::Seated() const noexcept
{
    return seatInstalled_ && party_seat::Seated(kSeat);
}

std::wstring Session::FormationReading() const noexcept
try
{
    auto* players = LocalPlayers(GameInstance());
    if (!seatInstalled_ || players == nullptr || players->Num() < 1)
    {
        return L"no party";
    }
    return party_member::FormationReading(Field((*players)[0], facts::kLocalPlayerController));
}
catch (...)
{
    return L"unread";
}

std::wstring Session::MemberReading() const noexcept
try
{
    if (playerTwo_ == nullptr && !awaiting_)
    {
        return L"out";
    }
    if (Seated())
    {
        const auto type = party_seat::MemberType(kSeat);
        return holdHand_    ? std::format(L"handed type {} ({}; player 2's view on it)", type, party_member::Describe(hold_))
               : handedToAi_ ? std::format(L"handed type {} (with the game's leader, lock {:#x})", type, party_member::ControlLockMask())
                             : std::format(L"playing type {}", type);
    }
    auto* players = LocalPlayers(gameInstance_);
    if (!seatInstalled_ || players == nullptr || players->Num() < 1)
    {
        return L"waiting (no seat)";
    }
    if (awaiting_)
    {
        const party_member::Choice choice = party_member::Choose(kSeat, Field((*players)[0], facts::kLocalPlayerController), nullptr, false);
        return std::format(L"awaiting: row {} type {}: {}", choice.ordinal, choice.type, party_member::Describe(choice.wait));
    }
    if (players->Num() < 2)
    {
        return L"waiting (no seat)";
    }
    const party_member::Choice choice = party_member::Choose(kSeat, Field((*players)[0], facts::kLocalPlayerController),
                                                             Field(playerTwo_, facts::kLocalPlayerController), false);
    return std::format(L"waiting: row {} type {}: {}", choice.ordinal, choice.type, party_member::Describe(choice.wait));
}
catch (...)
{
    return L"unread";
}

bool Session::Console(const wchar_t* command) noexcept
try
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:ExecuteConsoleCommand"));
    auto* library = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary"));
    auto* players = LocalPlayers(GameInstance());
    if (function == nullptr || library == nullptr || players == nullptr || players->Num() < 1)
    {
        return false;
    }
    auto* context = function->FindProperty(RC::Unreal::FName(STR("WorldContextObject"), RC::Unreal::FNAME_Find));
    auto* text    = function->FindProperty(RC::Unreal::FName(STR("Command"), RC::Unreal::FNAME_Find));
    constexpr std::int32_t kMaxFrame = 64;
    const std::int32_t     size      = function->GetParmsSize();
    if (context == nullptr || text == nullptr || size > kMaxFrame || context->GetSize() != sizeof(UObject*) ||
        text->GetSize() != sizeof(RC::Unreal::FString) || context->GetOffset_Internal() < 0 ||
        context->GetOffset_Internal() + context->GetSize() > size || text->GetOffset_Internal() < 0 ||
        text->GetOffset_Internal() + text->GetSize() > size ||
        (context->GetOffset_Internal() < text->GetOffset_Internal() + text->GetSize() &&
         text->GetOffset_Internal() < context->GetOffset_Internal() + context->GetSize()))
    {
        return false;
    }
    // SpecificPlayer stays null: the engine then uses player 1's controller, as the console does.
    alignas(16) std::uint8_t frame[kMaxFrame]{};
    *reinterpret_cast<UObject**>(frame + context->GetOffset_Internal()) = Field((*players)[0], facts::kLocalPlayerController);
    auto*      string = new (frame + text->GetOffset_Internal()) RC::Unreal::FString(command);
    const bool called = GuardedProcessEvent(static_cast<UObject*>(library->GetClassDefaultObject()), function, frame);
    string->~FString();
    return called;
}
catch (...)
{
    return false;
}

}  // namespace lss::players
