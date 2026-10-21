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
    UObject* value = nullptr;
    return object != nullptr && platform::CopyGuarded(&value, static_cast<const std::uint8_t*>(object) + offset,
                                                      sizeof(value))
               ? value
               : nullptr;
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

// UE4SS's object lookups can throw (a malformed path, or running out of memory), so every function here
// that makes one catches: an exception escaping a noexcept function would end the game.

UObject* GameInstance() noexcept
try
{
    return UObjectGlobals::FindFirstOf(STR("GameInstance"));
}
catch (...)
{
    return nullptr;
}

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

/// An object's class pointer (UObjectBase::ClassPrivate, +0x10 in this build's UE4SS layout), read under a
/// guard: a cheap check that an object read earlier is still the same kind of object.
const void* ClassOf(const void* object) noexcept
{
    return Field(object, 0x10);
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

/// Calls the stock UCameraComponent.SetConstraintAspectRatio(false) on a camera manager's main camera actor.
bool FillHalf(UObject* cameraManager) noexcept
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
        TryRejoin();
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
        TrySeat();
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
{
    if (!rejoin_.pending)
    {
        return;
    }
    const game_state::Snapshot game = game_state::Current();
    auto*                      players = LocalPlayers(gameInstance_);
    // Player 1 on a character also keeps the title screen out (a field in the play state too, with no character).
    const bool ready = game.valid && game.state == kStatePlay && game.hasRoot && game.running && game.step == kStepSteady &&
                       players != nullptr && players->Num() == 1 &&
                       Field(Field((*players)[0], facts::kLocalPlayerController), facts::kControllerPawn) != nullptr;
    const auto now = std::chrono::steady_clock::now();
    if (!ready)
    {
        rejoin_.steady = false;
        return;
    }
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
    Join();
    if (!HasPlayerTwo())
    {
        rejoin_.steady = false;  // another wait, then another try
        if (++rejoin_.failures >= kRejoinFailures)
        {
            rejoin_ = Rejoin{};
            platform::Log(L"rejoin: player 2 did not join in {} tries (see above); press Ctrl+F7 to join", kRejoinFailures);
        }
        return;
    }
    rejoin_ = Rejoin{};
    platform::Log(L"rejoin: player 2 is back");
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
}

void Session::Join() noexcept
try
{
    if (playerTwo_ != nullptr)
    {
        platform::Log(L"player 2 is already in");
        return;
    }
    if (!own_controller::CanRename())
    {
        platform::Log(L"join refused: the camera actor's name text cannot be changed (page protection refused)");
        return;
    }
    UObject* gameInstance = GameInstance();
    auto*    players      = LocalPlayers(gameInstance);
    if (players == nullptr || players->Num() != 1)
    {
        platform::Log(L"join refused: expected exactly one local player, found {}", players == nullptr ? -1 : players->Num());
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
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics:CreatePlayer"));
    auto* statics  = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics"));
    if (function == nullptr || statics == nullptr || function->GetParmsSize() != sizeof(CreatePlayerParams))
    {
        platform::Log(L"join refused: UGameplayStatics.CreatePlayer is not the function the mod expects");
        return;
    }

    CreatePlayerParams params{};
    params.WorldContextObject     = playerOneController;
    params.ControllerId           = kPlayerTwoControllerId;
    params.bSpawnPlayerController = true;
    own_controller::Report report;
    bool                   called = false;
    {
        own_controller::JoinScope scope(playerOneController);
        called = GuardedProcessEvent(static_cast<UObject*>(statics->GetClassDefaultObject()), function, &params);
        report = scope.Result();
    }
    platform::Log(L"join: CreatePlayer {}; player 2's camera manager {}: owner {}, camera actor {}",
                  called ? L"returned" : L"RAISED AN EXCEPTION",
                  report.cameraManager != nullptr ? L"seen" : L"NOT SEEN",
                  report.ownerFilled ? L"filled from Owner" : report.ownerRefused ? L"REFUSED" : L"already set",
                  report.renamed ? L"named EndCameraActr2" : L"NOT RENAMED");

    // The engine's list decides what happened: a player added before an exception must still be tracked,
    // so it can be removed before anything logs it in again.
    if (players->Num() != 2)
    {
        platform::Log(L"join failed: the engine did not add a local player (count now {})", players->Num());
        return;
    }
    playerTwo_    = (*players)[1];
    gameInstance_ = gameInstance;

    UObject* playerTwoController = Field(playerTwo_, facts::kLocalPlayerController);
    UObject* managerOne          = Field(playerOneController, facts::kControllerCameraManager);
    UObject* managerTwo          = Field(playerTwoController, facts::kControllerCameraManager);
    const bool filledOne         = FillHalf(managerOne);
    const bool filledTwo         = FillHalf(managerTwo);
    platform::Log(L"join: player 2 is in; halves filled: player 1 {}, player 2 {}", filledOne ? L"yes" : L"NO",
                  filledTwo ? L"yes" : L"NO");
    split_render::OnJoin();
    pad_buttons::SetPlayers(2);  // from here pad 2's buttons are player 2's
    scopes::BindController(kSeat, playerTwoController);
    waitLogged_  = 0;
    seatRetryAt_ = 0;
    if (seatInstalled_)
    {
        TrySeat();  // its member at once when free; else each tick (Tick)
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
    UObject* controller  = Field(playerTwo_, facts::kLocalPlayerController);
    UObject* manager     = Field(controller, facts::kControllerCameraManager);
    UObject* cameraActor = Field(manager, facts::kCameraManagerMainCamera);
    UObject* battleTalk  = Field(controller, facts::kControllerBattleTalkActor);
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

    const void* cameraClass = ClassOf(cameraActor);
    const void* talkClass   = ClassOf(battleTalk);
    platform::Log(L"leave: player 2's camera actor {}, battle-talk actor {}", Describe(cameraActor), Describe(battleTalk));

    // The engine's list is the evidence, whatever the call reported: it can remove the player and then fault.
    const bool raised  = !GuardedRemove(reinterpret_cast<RemoveLocalPlayerFn>(removeLocalPlayer_), gameInstance_, playerTwo_);
    const bool removed = !Contains(*players, playerTwo_);
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
    scopes::Unbind(kSeat);
    // Research says nothing destroys these two with their owners; a changed class pointer would mean the
    // memory was reused, and then they are left alone.
    const bool cameraGone = cameraClass != nullptr && ClassOf(cameraActor) == cameraClass &&
                            CallNoParams(cameraActor, STR("/Script/Engine.Actor:K2_DestroyActor"));
    const bool talkGone   = talkClass != nullptr && ClassOf(battleTalk) == talkClass &&
                            CallNoParams(battleTalk, STR("/Script/Engine.Actor:K2_DestroyActor"));
    platform::Log(L"leave: player 2 removed{}; leftovers destroyed: camera actor {}, battle-talk actor {}",
                  raised ? L" (the call raised an exception after removing it)" : L"", cameraGone ? L"yes" : L"no",
                  talkGone ? L"yes" : L"no");
    return true;
}
catch (...)
{
    platform::Log(L"leave: an engine lookup raised an exception");
    return playerTwo_ == nullptr;
}

void Session::TrySeat() noexcept
try
{
    auto* players = LocalPlayers(gameInstance_);
    if (playerTwo_ == nullptr || !seatInstalled_ || players == nullptr || !Contains(*players, playerTwo_) || ticks_ < seatRetryAt_)
    {
        return;
    }
    UObject* playerOne = Field((*players)[0], facts::kLocalPlayerController);
    UObject* playerTwo = Field(playerTwo_, facts::kLocalPlayerController);
    const party_member::Choice choice = party_member::Choose(kSeat, playerOne, playerTwo, false);
    if (choice.member == nullptr)
    {
        const std::uint32_t key = 1u | static_cast<std::uint32_t>(choice.type) << 8 | static_cast<std::uint32_t>(choice.wait) << 16 |
                                  static_cast<std::uint32_t>(choice.ordinal + 1) << 24;
        if (key != waitLogged_)
        {
            waitLogged_ = key;
            platform::Log(L"player 2 waits for its member (row {} of the party order, type {}): {}; the game's party order chooses it, "
                          L"so a party change ends the wait",
                          choice.ordinal, choice.type, party_member::Describe(choice.wait));
            if (choice.wait == party_member::Wait::AiHeld || choice.wait == party_member::Wait::NoFieldAi ||
                choice.wait == party_member::Wait::Cinema)
            {
                // The member's AI as the take test saw it (the actor of the row, read again).
                platform::Log(L"  the row's member: {}", party_member::AiDetail(party_member::RowActor(kSeat, playerOne)));
            }
        }
        return;
    }
    waitLogged_ = 0;
    if (!party_seat::Seat(kSeat, playerOne, playerTwo, choice.member, choice.type))
    {
        seatRetryAt_ = ticks_ + kSeatRetryTicks;  // the refusal is logged by the seat; tried again later
        return;
    }
    if (!player_two_camera::WeakPairOf(choice.member, memberWeak_))
    {
        memberWeak_[0] = -1;
        memberWeak_[1] = 0;
    }
    // After a script's hold: how soon the member came back measures the seat churn between scripts.
    platform::Log(L"player 2 plays member type {} (row {} of the party order){}", choice.type, choice.ordinal,
                  scriptReleaseAt_ == 0 ? L"" : std::format(L", {} ticks after a script's hold gave it back", ticks_ - scriptReleaseAt_));
    scriptReleaseAt_ = 0;
    if (cameraInstalled_ && !player_two_camera::Start(playerOne, playerTwo, choice.member))
    {
        platform::Log(L"player 2 keeps a still camera; each player's sticks are still their own pad's");
    }
    Report();
}
catch (...)
{
    platform::Log(L"seat: an engine lookup raised an exception");
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
    platform::Log(L"player 2's member (type {}) is {}: given back, to choose again from the party order",
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
}
catch (...)
{
    platform::Log(L"release: an engine lookup raised an exception");
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
        // The game's own state decides the return (RE-script-release.md §4: the event bit alone undid a keep-group
        // hand): its leader back with a player controller. The funnel is heard at its entry; this is the backstop,
        // and the retry while a script still held the member at the game's return.
        if (party_member::LeaderWithPlayer())
        {
            OnLeaderReturned(this);
        }
        return;
    }
    if (const wchar_t* why = party_member::WhyHeldByScript(member))
    {
        platform::LogDebug(L"  the member: {}", party_member::AiDetail(member));
        scriptReleaseAt_ = ticks_;
        ReleaseMember(why);
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
    if (member == nullptr || !session->handedToAi_ || session->playerTwo_ == nullptr)
    {
        return;
    }
    // The game's leader is back; the member follows once nothing holds it (a scene's own layers can outlast the
    // lock). The periodic check retries while the game's leader stays with its player.
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
    auto* players = LocalPlayers(gameInstance_);  // set at the first join; null before
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
    if (playerTwo_ == nullptr)
    {
        return L"out";
    }
    if (Seated())
    {
        return std::format(L"playing type {}", party_seat::MemberType(kSeat));
    }
    auto* players = LocalPlayers(gameInstance_);
    if (!seatInstalled_ || players == nullptr || players->Num() < 2)
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
