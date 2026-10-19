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
#include "players/party_seat.h"
#include "players/player_two_camera.h"
#include "battle/command_menu_two.h"
#include "battle/targeting_two.h"
#include "players/pad_buttons.h"
#include "players/scopes.h"
#include "players/split_render.h"

#include <Unreal/Core/Containers/FString.hpp>

#include <cstdint>
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

/// The field is back after a world change (RE-lifecycle-world.md §8): play state, the steady step, running. The
/// entry's step 12 re-reconciles every non-leader controller, so a join before step 13 would be undone.
constexpr std::uint8_t kStatePlay  = 6;
constexpr std::int32_t kStepSteady = 13;

/// How long the field must stay back, with player 1 on a character, before player 2 rejoins; and how many joins
/// may fail before the rejoin is left to Ctrl+F7. Timed, not counted in ticks: loads tick uncapped.
constexpr std::chrono::seconds kRejoinSteady{2};
constexpr int                  kRejoinFailures = 3;

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
    seatInstalled_     = party_seat::Install(image);         // optional probe (Ctrl+F9)
    split_render::Install(image);                            // optional: without it the halves keep the split rule's reduced effects
    fieldExitInstalled_ = field_exit::Install(image, &Session::OnFieldExit, this);  // without it, Tick's step check alone
    return true;
}

void Session::Remove() noexcept
{
    if (fieldExitInstalled_)
    {
        field_exit::Remove();
        fieldExitInstalled_ = false;
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
    const bool seated = Seated();
    platform::Log(L"world change ({}): player 2 leaves first{}", why, seated ? L" and gives Aerith back" : L"");
    if (!TakeOut(true))
    {
        worldChangeStuck_ = true;
        platform::Log(L"world change: player 2 could NOT be taken out; the game tears the field down with player 2 in");
        return;
    }
    rejoin_ = Rejoin{.pending = true, .reseat = seated};
    platform::Log(L"world change: player 2 joins again once the field is back (Ctrl+F8 cancels)");
}

void Session::Tick() noexcept
try
{
    gameThread_                     = GetCurrentThreadId();
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
    const bool reseat = rejoin_.reseat;
    rejoin_           = Rejoin{};
    platform::Log(L"rejoin: player 2 is back{}", reseat ? L"; taking Aerith again" : L"");
    if (reseat && seatInstalled_ && !Seated())
    {
        ToggleSeat();
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
        party_seat::Forget();
    }
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
    scopes::BindController(1, playerTwoController);
    if (cameraInstalled_)
    {
        if (!player_two_camera::Start(playerOneController, playerTwoController))
        {
            platform::Log(L"join: player 2 keeps a still camera; each player's sticks are still their own pad's");
        }
    }
    Report();
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
    // Player 2's character goes back to its AI first, through the game's leader funnel.
    if (seatInstalled_ && !party_seat::Unseat(Field((*players)[0], facts::kLocalPlayerController), controller))
    {
        if (!worldChange)
        {
            platform::Log(L"leave refused: player 2's controller still holds Aerith; press Ctrl+F8 again to retry");
            return false;
        }
        // The game destroys and rebuilds the party next; removing the controller unpossesses her on the way.
        platform::Log(L"leave: player 2's controller still holds Aerith; the world change takes player 2 out regardless");
    }
    // Player 2's field camera operator next: its cleanup unbinds the delegates it bound on player 2's
    // controller, which must still exist then.
    UObject* operatorTwo = cameraInstalled_ ? static_cast<UObject*>(player_two_camera::Take()) : nullptr;
    const bool operatorGone = operatorTwo != nullptr && CallNoParams(operatorTwo, STR("/Script/Engine.Actor:K2_DestroyActor"));
    platform::Log(L"leave: player 2's camera operator {}", operatorTwo == nullptr ? L"was not running" : operatorGone ? L"destroyed" : L"NOT DESTROYED");

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
        party_seat::Forget();  // a later player 2 is a new controller, set up afresh
    }
    scopes::Unbind(1);
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

void Session::ToggleSeat() noexcept
try
{
    auto* players = LocalPlayers(gameInstance_);
    if (playerTwo_ == nullptr || !seatInstalled_ || players == nullptr || !Contains(*players, playerTwo_))
    {
        platform::Log(L"seat probe: player 2 is not in (or the probe did not install)");
        return;
    }
    UObject* playerOne = Field((*players)[0], facts::kLocalPlayerController);
    UObject* playerTwo = Field(playerTwo_, facts::kLocalPlayerController);
    if (party_seat::Seated())
    {
        party_seat::Unseat(playerOne, playerTwo);
    }
    else
    {
        party_seat::Seat(playerOne, playerTwo);
    }
    Report();
}
catch (...)
{
    platform::Log(L"seat probe: an engine lookup raised an exception");
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
    return seatInstalled_ && party_seat::Seated();
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
