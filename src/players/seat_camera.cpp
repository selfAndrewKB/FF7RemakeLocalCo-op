#include "players/seat_camera.h"

#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/camera_override.h"
#include "players/field_root.h"
#include "players/game_globals.h"
#include "players/game_state.h"
#include "players/nav_area.h"
#include "players/operator_battle_mode.h"
#include "players/pad_buttons.h"
#include "players/party_member.h"
#include "players/scopes.h"
#include "players/seat_objects.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "render/history_cut.h"
#include "views/scene_camera.h"
#include "battle/command_camera.h"
#include "battle/lock_camera.h"
#include "battle/sequence_camera.h"
#include "battle/targeting.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::players::seat_camera {

namespace {

using platform::Read;
using weak_object::MakeWeak;
using weak_object::Resolve;
using weak_object::Weak;

using SpawnActorFn     = void* (*)(void* world, void* type, const void* transform, const void* parameters);
using StaticClassFn    = void* (*)();
using SetOwnerCharaFn  = void (*)(void* cameraOperator, void* character);
using ActorVoidFn      = void (*)(void* self);
using ActorFloatFn     = void (*)(void* self, float deltaSeconds);
using InputFn          = void (*)(void* self, float deltaSeconds, bool gamePaused);

constexpr int kPlayers = scopes::kMaxPlayers;  // index 0 = player 1, index s = seat s

// Set once at install, then only read.
void**           g_partyManager   = nullptr;  // the global holding the party manager pointer
void**           g_gameMode       = nullptr;
std::uint64_t*   g_frameStamp     = nullptr;
std::uint8_t*    g_corrections    = nullptr;
std::uint32_t*   g_nameNone       = nullptr;
std::uint8_t**   g_squareInput    = nullptr;
SpawnActorFn     g_spawnActor     = nullptr;
StaticClassFn    g_staticClass    = nullptr;
SetOwnerCharaFn  g_setOwnerChara  = nullptr;

platform::VTableEntry g_operatorBeginPlaySlot;
platform::VTableEntry g_operatorTickSlot;
platform::VTableEntry g_inputSlot;
platform::VTableEntry g_moveSlot;
platform::VTableEntry g_cameraUpdateSlot;
ActorVoidFn           g_operatorBeginPlay = nullptr;
ActorFloatFn          g_operatorTick      = nullptr;
InputFn               g_processInput      = nullptr;
ActorFloatFn          g_fieldMove         = nullptr;
ActorFloatFn          g_cameraUpdate      = nullptr;

// The game's input step for the primary controller (research/RE-input-p2.md, RE-camera-jump-combo.md): the one call the
// game makes during play, redirected so each seat's step follows it at the same point of the frame.
platform::CallRedirect g_inputTickCall;
bool                   g_seatInputOff = false;  // a seat's step raised an exception: the seats' steps stay off
int                    g_tickingSeat  = 0;      // the seat whose own operator ticks, else 0

// Set by the first Start, from the engine tick; until then, and on any other thread, the wrappers pass their calls through.
platform::GameThreadMark g_gameThread;
Weak  g_playerOne;  // player 1's controller while a seat is in (resolved at each use)
bool  g_spawning = false;

/// One seat's camera as this module keeps it: its controller and operator are the registry's (players/seat_objects.h;
/// a controller the engine destroyed or replaced resolves to null there, so the scopes stop applying).
struct SeatCamera
{
    bool noPlayerInputLogged = false;
    bool moveHeldOff         = false;  // the field move is held off: the seat's member is with its AI
};
std::array<SeatCamera, kPlayers> g_seats;  // index = seat; [0] unused (player 1 is the game's)

// Test input and diagnostics, game thread only.
struct StickHold
{
    bool                 active = false;
    std::array<float, 4> axes{};
};
std::array<StickHold, kPlayers> g_holds;  // index = XInput user

struct CallCounts
{
    std::uint32_t input = 0;      // ProcessPlayerInput under the player's scopes
    std::uint32_t move  = 0;      // Tick_FieldMove under the player's scopes
    std::uint32_t operatorTick = 0;
    std::uint32_t cameraUpdate = 0;  // the camera manager's UpdateCamera under the player's scopes
};
std::array<CallCounts, kPlayers> g_counts;  // index 0 = player 1, s = seat s
std::array<std::uint32_t, kPlayers> g_operatorTicks{};  // each seat's operator ticks since the start (never reset)

bool OnGameThread() noexcept
{
    return g_gameThread.Here();
}

}  // namespace

bool IsGameThread() noexcept
{
    return OnGameThread();
}

StickScope::StickScope(std::uint32_t user, bool clearKeys) noexcept
{
    std::uint8_t* owner = nullptr;
    if (!Read(g_squareInput, owner) || owner == nullptr || !Read(owner + facts::kStickAnalog, savedAnalog_) ||
        !Read(owner + facts::kStickDigital, savedDigital_))
    {
        return;
    }
    std::array<float, 4> pad{};  // still unless the pad's record exists
    const std::uint8_t*  bank    = owner + facts::kStickBanks + 16 * facts::kXInputBank;
    std::uint8_t*        records = nullptr;
    std::int32_t         count   = 0;
    if (user < g_holds.size() && g_holds[user].active)
    {
        pad = g_holds[user].axes;
    }
    else if (Read(bank, records) && Read(bank + 8, count) && records != nullptr && count > 0 &&
             user < static_cast<std::uint32_t>(count))
    {
        Read(records + static_cast<std::size_t>(user) * facts::kStickRecordBytes, pad);
    }
    const std::array<float, 4> still{};
    if (platform::WriteGuarded(owner + facts::kStickAnalog, pad.data(), sizeof(pad)) &&
        (!clearKeys || platform::WriteGuarded(owner + facts::kStickDigital, still.data(), sizeof(still))))
    {
        owner_ = owner;
    }
    else
    {
        platform::WriteGuarded(owner + facts::kStickAnalog, savedAnalog_, sizeof(savedAnalog_));
    }
}

StickScope::~StickScope()
{
    if (owner_ != nullptr)
    {
        platform::WriteGuarded(owner_ + facts::kStickAnalog, savedAnalog_, sizeof(savedAnalog_));
        platform::WriteGuarded(owner_ + facts::kStickDigital, savedDigital_, sizeof(savedDigital_));
    }
}

namespace {

std::int32_t* OperatorPair() noexcept
{
    std::uint8_t* root = field_root::Root();
    return root != nullptr ? reinterpret_cast<std::int32_t*>(root + facts::kRootOperatorWeak) : nullptr;
}

void* PlayerOneOperator() noexcept
{
    const std::int32_t* pair = OperatorPair();
    Weak                weak;
    return pair != nullptr && Read(pair, weak) ? Resolve(weak) : nullptr;
}

/// PartyManager+0 names the given controller for its lifetime; put back only if still what was written.
class PrimaryScope
{
public:
    explicit PrimaryScope(void* controller) noexcept
    {
        void* manager = nullptr;
        if (controller != nullptr && Read(g_partyManager, manager) && manager != nullptr &&
            Read(manager, saved_))
        {
            if (platform::WriteGuarded(manager, &controller, sizeof(controller)))
            {
                field_ = static_cast<void**>(manager);
                wrote_ = controller;
            }
        }
    }
    ~PrimaryScope()
    {
        void* now = nullptr;
        if (field_ != nullptr && Read(field_, now) && now == wrote_ && !platform::WriteGuarded(field_, &saved_, sizeof(saved_)))
        {
            platform::Log(L"a seat's camera: the primary controller could not be put back");
        }
    }
    PrimaryScope(const PrimaryScope&)            = delete;
    PrimaryScope& operator=(const PrimaryScope&) = delete;

private:
    void** field_ = nullptr;
    void*  saved_ = nullptr;
    void*  wrote_ = nullptr;
};

/// The root's operator weak pair names the given operator for its lifetime; put back only if unchanged.
class OperatorScope
{
public:
    explicit OperatorScope(const Weak& weak) noexcept
    {
        std::int32_t* pair = OperatorPair();
        if (weak.serial != 0 && pair != nullptr && Read(pair, saved_) && platform::WriteGuarded(pair, &weak, sizeof(weak)))
        {
            pair_  = pair;
            wrote_ = weak;
        }
    }
    ~OperatorScope()
    {
        Weak now;
        if (pair_ != nullptr && Read(pair_, now) && now.index == wrote_.index && now.serial == wrote_.serial &&
            !platform::WriteGuarded(pair_, &saved_, sizeof(saved_)))
        {
            platform::Log(L"a seat's camera: the operator pair could not be put back");
        }
    }
    OperatorScope(const OperatorScope&)            = delete;
    OperatorScope& operator=(const OperatorScope&) = delete;

private:
    std::int32_t* pair_ = nullptr;
    Weak          saved_;
    Weak          wrote_;
};

// --- The seats as the replacements see them ----------------------------------------------------------------

/// The seat's player plays a member (the registry's controller and member both resolve): the window this module's
/// replacements act in, from the seating to the release, as its own pair was set at Start and cleared at Take.
bool SeatActive(int seat) noexcept
{
    return seat_objects::Controller(seat) != nullptr && seat_objects::Member(seat) != nullptr;
}

/// Some seat's player plays a member (game thread only; off it nothing is a seat's).
bool AnySeatIn() noexcept
{
    if (!OnGameThread())
    {
        return false;
    }
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (SeatActive(seat))
        {
            return true;
        }
    }
    return false;
}

/// The seat whose controller is `self`, while its player plays a member, else 0 (player 1's controller is never a seat's).
int SeatOfController(const void* self) noexcept
{
    const int seat = seat_objects::SeatOf(self);
    return seat != 0 && seat_objects::Controller(seat) == self && SeatActive(seat) ? seat : 0;
}

/// The seat whose own operator is `self`, while its player plays a member, else 0.
int SeatOfOperator(const void* self) noexcept
{
    const int seat = seat_objects::SeatOf(self);
    return seat != 0 && seat_objects::Operator(seat) == self && SeatActive(seat) ? seat : 0;
}

/// The seat whose controller's camera manager is `self`, while its player plays a member, else 0.
int SeatOfManager(const void* self) noexcept
{
    const int seat = seat_objects::SeatOfCameraManager(self);
    return seat != 0 && SeatActive(seat) ? seat : 0;
}

void* PlayerOne() noexcept
{
    return OnGameThread() ? Resolve(g_playerOne) : nullptr;
}

// --- The slot replacements -----------------------------------------------------------------------------

// The engine's own functions are called unguarded from these replacements, as the engine calls them. A fault
// inside one is the game's own crash: catching it here and carrying on would hide it and continue in a broken
// state. If something further up handles it, unwinding runs the scopes' destructors, which put every swapped
// value back.

/// AEndFieldCameraOperator::BeginPlay. A seat's operator begins play inside the spawn Start makes, with
/// PartyManager+0 already naming the seat's controller; here the root's pair names it too, so the leader delegate
/// and the owner the game picks are bound through the seat's controller.
void OperatorBeginPlay(void* self)
{
    if (!OnGameThread() || !g_spawning || self == PlayerOneOperator())
    {
        g_operatorBeginPlay(self);
        return;
    }
    const Weak weak = MakeWeak(self);
    if (weak.serial == 0)
    {
        platform::Log(L"a seat's camera: no weak pair for the new operator; it begins play as player 1's would");
    }
    const OperatorScope scope(weak);
    g_operatorBeginPlay(self);
}

/// AEndFieldCameraOperator::Tick. A seat's operator ticks as the primary one, for that seat.
void OperatorTick(void* self, float deltaSeconds)
{
    const bool seatsIn = AnySeatIn();
    const int  seat    = seatsIn ? SeatOfOperator(self) : 0;
    if (seat == 0)
    {
        const bool one = seatsIn && self == PlayerOneOperator();
        if (one)
        {
            battle::lock_camera::BeforePlayerOneOperatorTick();
        }
        g_operatorTick(self, deltaSeconds);
        if (one)
        {
            battle::sequence_camera::AfterOperatorTick(0);  // player 1's operator
            battle::lock_camera::AfterPlayerOneOperatorTick(self, deltaSeconds);
        }
        return;
    }
    ++g_counts[seat].operatorTick;
    ++g_operatorTicks[static_cast<std::size_t>(seat)];
    void* const                      controller = seat_objects::Controller(seat);
    const PrimaryScope               primary(controller);
    const OperatorScope              scope(seat_objects::OperatorWeak(seat));
    const StickScope                 stick(static_cast<std::uint32_t>(seat), true);  // a sequence shot's input offset reads the stick (RE-seq-camera-two.md)
    // Its leader is the seat's member: the battle camera requests, the move and camera-input checks and the battle
    // sequence overrides the tick reads are keyed on the party leader (RE-lock-camera-two.md), as the lock
    // camera follows the seat's target, not player 1's.
    const scopes::Scope              leader(seat, scopes::Leader);
    // In battle the seat's UI context too: the Command camera flavor frames the target the seat's own list cursor is
    // on, read from the UI slot and the focus path inside this tick (battle/command_camera.h).
    const battle::command_camera::TickScope ui(seat);
    const battle::targeting::FieldsScope targets(seat);
    g_tickingSeat = seat;
    battle::lock_camera::BeforeOperatorTick(seat, self, controller);
    g_operatorTick(self, deltaSeconds);
    battle::lock_camera::AfterOperatorTick(seat, self, deltaSeconds, targets.Held());
    g_tickingSeat = 0;
    battle::sequence_camera::AfterOperatorTick(seat);
    // The owner's camera-override route follows the owner (the tick may have changed it: the LeaderChange flavor).
    void* owner = nullptr;
    camera_override::Route(seat, Read(static_cast<std::uint8_t*>(self) + facts::kOperatorOwnerChara, owner) ? owner : nullptr);
}

/// AEndPlayerCameraManager::UpdateCamera: a seat's camera manager updates as the primary player's, with the seat's
/// member as the leader (the occlusion fade toward the leader, the slot blends that write the primary
/// controller's rotation; RE-lock-camera-ledger.md).
void UpdateCamera(void* self, float deltaSeconds)
{
    if (const int mirror = views::scene_camera::MirrorSlotOf(self); mirror >= 0)
    {
        // A mirror player's manager (views/mirror_player): every slot of player 1's set on it, under the mirror's
        // primary scope so the game's primary-scoped setters (the blend records, the cut) reach it.
        const PrimaryScope primary(views::scene_camera::MirrorController(mirror));
        views::scene_camera::BeforeMirrorUpdate(mirror);
        g_cameraUpdate(self, deltaSeconds);
        render::history_cut::AfterCameraUpdate(self, false);
        return;
    }
    const bool  seatsIn = AnySeatIn();
    void* const one     = PlayerOne();
    const int   seat    = seatsIn ? SeatOfManager(self) : 0;
    if (seat == 0)
    {
        void* const managerOne = actor_fields::CameraManager(one);
        if ((seatsIn && self == managerOne) ||
            (self != nullptr && self == views::scene_camera::MirrorSource()))
        {
            views::scene_camera::BeforeUpdateCamera(self, 0, nullptr);  // player 1's: its camera cut noted for this frame
        }
        g_cameraUpdate(self, deltaSeconds);
        if (self != nullptr && self == managerOne)
        {
            render::history_cut::AfterCameraUpdate(self, true);
        }
        return;
    }
    ++g_counts[seat].cameraUpdate;
    const PrimaryScope  primary(seat_objects::Controller(seat));
    const scopes::Scope leader(seat, scopes::Leader);
    // A scene's camera (slots player 1's manager was given this frame) onto the seat's, with the game's own setters,
    // inside the scope so the primary-scoped ones reach the seat's manager.
    views::scene_camera::BeforeUpdateCamera(actor_fields::CameraManager(one), seat, self);
    g_cameraUpdate(self, deltaSeconds);
    render::history_cut::AfterCameraUpdate(self, false);
}

/// AEndPlayerController::ProcessPlayerInput: each player's right stick reaches that player's operator.
void ProcessPlayerInput(void* self, float deltaSeconds, bool gamePaused)
{
    if (!AnySeatIn())
    {
        g_processInput(self, deltaSeconds, gamePaused);
        return;
    }
    if (self == PlayerOne())
    {
        ++g_counts[0].input;
        const StickScope stick(0, false);
        g_processInput(self, deltaSeconds, gamePaused);
        return;
    }
    const int seat = SeatOfController(self);
    if (seat == 0)
    {
        g_processInput(self, deltaSeconds, gamePaused);
        return;
    }
    ++g_counts[seat].input;
    if (seat_objects::Operator(seat) == nullptr)
    {
        // Without its own operator, the seat's stick would reach player 1's: a still stick instead.
        const StickScope stick(UINT32_MAX, true);
        g_processInput(self, deltaSeconds, gamePaused);
        return;
    }
    const PrimaryScope             primary(self);
    const OperatorScope            scope(seat_objects::OperatorWeak(seat));
    const StickScope               stick(static_cast<std::uint32_t>(seat), true);
    const pad_buttons::ButtonScope buttons(seat);
    // The right stick is the lock's while the leader is locked on a target in range (the stick switches targets
    // then; the camera does not turn): the seat's lock and leader, not player 1's (RE-lock-camera-ledger.md).
    const scopes::Scope                      leader(seat, scopes::Leader);
    const battle::targeting::FieldsScope targets(seat);
    g_processInput(self, deltaSeconds, gamePaused);
    battle::lock_camera::AfterInput(seat, self);
}

/// AEndPlayerController::Tick_FieldMove: each player's left stick moves only that player's character, and a seat's
/// member with its AI is left to the AI. A seat's movement also takes its direction from the seat's camera (the game
/// reads the primary controller's camera manager for the basis) and marks the seat's operator (the game writes flags
/// on "the" operator).
void FieldMove(void* self, float deltaSeconds)
{
    if (!AnySeatIn())
    {
        g_fieldMove(self, deltaSeconds);
        return;
    }
    if (self == PlayerOne())
    {
        ++g_counts[0].move;
        const StickScope stick(0, false);
        g_fieldMove(self, deltaSeconds);
        return;
    }
    const int seat = SeatOfController(self);
    if (seat == 0)
    {
        g_fieldMove(self, deltaSeconds);
        return;
    }
    // A member with its AI (a scripted hold: its PLAYER slot empty, the controller still naming it as its character) is
    // the AI's to move. The field move drives whatever character the controller names, possessed or not, adding the
    // seat's stick and writing the gait the AI's path following writes too; the game's own give back leaves the
    // controller naming no one, so nothing of the player's moves it. The game calls it every frame for every controller
    // (its actor tick), so it is held off here for the same state.
    const bool withAi = party_member::HandedToAi(actor_fields::LeaderCharacter(self));
    if (withAi != g_seats[seat].moveHeldOff)
    {
        g_seats[seat].moveHeldOff = withAi;
        platform::LogDebug(L"{}'s field move {}", SeatName(seat),
                           withAi ? L"held off: its member is with its AI, which alone moves it"
                                  : L"runs again: its member is the player's");
    }
    if (withAi)
    {
        return;
    }
    ++g_counts[seat].move;
    const PrimaryScope             primary(self);
    const OperatorScope            scope(seat_objects::OperatorWeak(seat));  // no-op while the seat has no operator
    const StickScope               stick(static_cast<std::uint32_t>(seat), true);
    const pad_buttons::ButtonScope buttons(seat);
    // Locked-on movement faces the target (strafes) on the seat's lock, not player 1's (RE-lock-camera-ledger.md).
    const battle::targeting::FieldsScope targets(seat);
    g_fieldMove(self, deltaSeconds);
}

/// The game's input step for the primary controller (the only controller whose input the game ticks during play).
/// Right after it, at the same point of the frame and with the same arguments, each seat's controller gets the same
/// step in seat order; the ProcessPlayerInput replacement then scopes it to the seat's operator and pad. The point
/// matters: the field step that follows changes the game-speed slots (the hit stop of player 1's hits, the command
/// menu's slow-down), and the right stick's turn divides by the game speed of the moment while DeltaTime carries the
/// last frame's, so a step run after the field step turned a held stick up to a hundred times too far in one frame
/// (RE-camera-jump-combo.md).
void TickInputs(void* controller, float deltaSeconds, bool gamePaused)
{
    const auto tickPlayerInput = reinterpret_cast<InputFn>(g_inputTickCall.Original());
    tickPlayerInput(controller, deltaSeconds, gamePaused);
    if (g_seatInputOff || !AnySeatIn())
    {
        return;
    }
    for (int seat = 1; seat <= kSeatsBuilt && !g_seatInputOff; ++seat)
    {
        void* seatController = SeatActive(seat) ? seat_objects::Controller(seat) : nullptr;
        void* playerInput    = nullptr;
        if (seatController == nullptr)
        {
            continue;  // no member yet, or gone: the seat's input step waits, as it did before the member was seated
        }
        if (!Read(static_cast<std::uint8_t*>(seatController) + facts::kControllerPlayerInput, playerInput) || playerInput == nullptr)
        {
            if (!g_seats[seat].noPlayerInputLogged)
            {
                platform::Log(L"{}'s input: the controller has no PlayerInput object; its input step is skipped", SeatName(seat));
                g_seats[seat].noPlayerInputLogged = true;
            }
            continue;
        }
        // Unlike the replacements above, the engine never makes this call itself (it ticks only the primary
        // controller's input), so it is guarded like the mod's other calls: a fault turns the seats' input step off.
        if (!platform::CallGuarded(tickPlayerInput, seatController, deltaSeconds, gamePaused))
        {
            g_seatInputOff = true;
            platform::Log(L"{}'s input: the input step RAISED AN EXCEPTION; every seat's input step is off until the game restarts (that player's scopes may stay open for the rest of this frame's field step)",
                          SeatName(seat));
        }
    }
}

template <class Fn>
bool FindInto(const platform::GameImage& image, const platform::FunctionFact& fact, Fn& out) noexcept
{
    const auto address = platform::FindFunction(image, fact);
    out                = address ? reinterpret_cast<Fn>(*address) : nullptr;
    return out != nullptr;
}

template <class T>
bool Global(const platform::GameImage& image, std::uintptr_t address, T*& out) noexcept
{
    const auto live = image.Rebase(address, sizeof(T));
    out             = live ? reinterpret_cast<T*>(*live) : nullptr;
    return out != nullptr;
}

/// A global another module reads too (players/game_globals), as `out`; false when it is outside the image.
template <class T>
bool Shared(game_globals::Global global, T*& out) noexcept
{
    out = reinterpret_cast<T*>(game_globals::Address(global));
    return out != nullptr;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (g_operatorBeginPlaySlot.Active())
    {
        return false;  // already installed: a second install must not undo the first
    }
    if (!weak_object::Installed() || !Shared(game_globals::Global::PartyManager, g_partyManager) || !field_root::Installed() ||
        !Shared(game_globals::Global::GameMode, g_gameMode) || !Shared(game_globals::Global::OperatorFrameStamp, g_frameStamp) ||
        !Global(image, facts::kOperatorCorrections, g_corrections) || !Shared(game_globals::Global::NameNoneIndex, g_nameNone) ||
        !Shared(game_globals::Global::SquareInputOwner, g_squareInput) ||
        !FindInto(image, facts::kSpawnActor, g_spawnActor) || !FindInto(image, facts::kOperatorStaticClass, g_staticClass) ||
        !FindInto(image, facts::kSetOwnerChara, g_setOwnerChara))
    {
        platform::Log(L"the seats' cameras: an engine check failed; a seat keeps a still camera");
        return false;
    }

    const auto beginPlay = platform::FindFunction(image, facts::kOperatorBeginPlay.target);
    const auto tick      = platform::FindFunction(image, facts::kOperatorTick.target);
    const auto input     = platform::FindFunction(image, facts::kProcessPlayerInput.target);
    const auto move      = platform::FindFunction(image, facts::kFieldMove.target);
    const auto update    = platform::FindFunction(image, facts::kCameraManagerUpdateCamera.target);
    if (!beginPlay || !tick || !input || !move || !update)
    {
        return false;
    }
    g_operatorBeginPlay = reinterpret_cast<ActorVoidFn>(*beginPlay);
    g_operatorTick      = reinterpret_cast<ActorFloatFn>(*tick);
    g_processInput      = reinterpret_cast<InputFn>(*input);
    g_fieldMove         = reinterpret_cast<ActorFloatFn>(*move);
    g_cameraUpdate      = reinterpret_cast<ActorFloatFn>(*update);

    // All five or none: a partial set would split one player's state between two cameras.
    const bool installed =
        platform::ReplaceSlot(image, facts::kOperatorBeginPlay, reinterpret_cast<std::uintptr_t>(&OperatorBeginPlay),
                              g_operatorBeginPlaySlot) &&
        platform::ReplaceSlot(image, facts::kOperatorTick, reinterpret_cast<std::uintptr_t>(&OperatorTick), g_operatorTickSlot) &&
        platform::ReplaceSlot(image, facts::kProcessPlayerInput, reinterpret_cast<std::uintptr_t>(&ProcessPlayerInput),
                              g_inputSlot) &&
        platform::ReplaceSlot(image, facts::kFieldMove, reinterpret_cast<std::uintptr_t>(&FieldMove), g_moveSlot) &&
        platform::ReplaceSlot(image, facts::kCameraManagerUpdateCamera, reinterpret_cast<std::uintptr_t>(&UpdateCamera),
                              g_cameraUpdateSlot);
    if (!installed)
    {
        g_cameraUpdateSlot.Restore();
        g_moveSlot.Restore();
        g_inputSlot.Restore();
        g_operatorTickSlot.Restore();
        g_operatorBeginPlaySlot.Restore();
        return false;
    }
    camera_override::Install(image);  // optional: without it a seat's attack cameras stay on player 1's camera
    operator_battle_mode::Install(image);  // optional: without it a seat's camera plays battles on the field setting
    nav_area::Install(image);          // optional: without it a seat's InDoor settings reach player 1's operator
    // Optional: without it a seat's right stick does nothing, and everything else still works.
    const auto tickPlayerInput = platform::FindFunction(image, facts::kTickPlayerInput);
    const auto inputCall       = image.Rebase(facts::kPrimaryInputTickCall, 5);
    if (tickPlayerInput && inputCall &&
        g_inputTickCall.Install(*inputCall, *tickPlayerInput, reinterpret_cast<std::uintptr_t>(&TickInputs)))
    {
        platform::Log(L"the seats' input: each seat's input step follows player 1's at the game's own input call");
    }
    else
    {
        platform::Log(L"the seats' input: the game's input call could not be verified; a seat's right stick stays unused");
    }
    return true;
}

void RemoveInputTick() noexcept
{
    if (g_inputTickCall.IsInstalled() && !g_inputTickCall.Restore())
    {
        platform::Log(L"the seats' input: the game's input call was changed after the mod's; left alone");
    }
}

bool Start(int seat, void* playerOneController, void* seatController, void* member) noexcept
{
    if (!SeatBuilt(seat))
    {
        platform::Log(L"a seat's camera: seat {} is not built; still camera", seat);
        return false;
    }
    const wchar_t* name = SeatName(seat);
    g_gameThread.Set();
    g_playerOne         = MakeWeak(playerOneController);
    g_seats[seat]       = {};
    seat_objects::BindOperator(seat, nullptr);  // no operator of an earlier start survives here
    if (seat_objects::Controller(seat) != seatController)
    {
        platform::Log(L"{}'s camera: its controller is not the one that joined (gone or replaced); still camera", name);
        return false;
    }

    void* operatorOne = PlayerOneOperator();
    void* manager     = nullptr;
    void* gameMode    = nullptr;
    if (operatorOne == nullptr || !Read(g_partyManager, manager) || manager == nullptr || !Read(g_gameMode, gameMode) ||
        gameMode == nullptr)
    {
        platform::Log(L"{}'s camera: no field camera operator or party manager (not in the field?); still camera", name);
        return false;
    }
    if (member == nullptr)
    {
        platform::Log(L"{}'s camera: no member to follow; still camera", name);
        return false;
    }
    void* const world = game_state::WorldOf(gameMode);
    void*       type  = nullptr;
    if (world == nullptr || !platform::CallGuarded(g_staticClass, type) || type == nullptr)
    {
        platform::Log(L"{}'s camera: the world or the operator class could not be found; still camera", name);
        return false;
    }

    // The game's own spawn parameters for the operator, with no name: the engine then makes a unique one
    // (the game's literal name would make the spawn rebuild player 1's operator in place).
    alignas(8) std::array<std::uint8_t, 0x30> parameters{};
    std::uint32_t                             none = 0;
    if (!Read(g_nameNone, none))
    {
        platform::Log(L"{}'s camera: NAME_None could not be read; still camera", name);
        return false;
    }
    std::memcpy(parameters.data(), &none, sizeof(none));
    const std::uint32_t flags = facts::kOperatorObjectFlags;
    std::memcpy(parameters.data() + facts::kSpawnParamsObjectFlags, &flags, sizeof(flags));

    // The operator's constructor resets two globals player 1's operator uses; they are put back after,
    // unconditionally: undoing the constructor's write is the point.
    std::uint64_t stamp       = 0;
    std::uint8_t  corrections = 0;
    if (!Read(g_frameStamp, stamp) || !Read(g_corrections, corrections))
    {
        platform::Log(L"{}'s camera: the operator globals could not be read; still camera", name);
        return false;
    }
    void*               spawned     = nullptr;
    bool                called      = false;
    {
        const PrimaryScope primary(seatController);  // BeginPlay runs inside the spawn
        g_spawning = true;
        called     = platform::CallGuarded(g_spawnActor, spawned, world, type, static_cast<const void*>(nullptr),
                                           static_cast<const void*>(parameters.data()));
        g_spawning = false;
    }
    platform::WriteGuarded(g_frameStamp, &stamp, sizeof(stamp));
    platform::WriteGuarded(g_corrections, &corrections, sizeof(corrections));
    if (!called || spawned == nullptr)
    {
        platform::Log(L"{}'s camera: the operator spawn {}; still camera", name, called ? L"returned nothing" : L"RAISED AN EXCEPTION");
        return false;
    }
    seat_objects::BindOperator(seat, spawned);
    {
        const Weak         weak = seat_objects::OperatorWeak(seat);
        const std::int32_t pair[2]{weak.index, weak.serial};
        scopes::BindOperator(seat, pair);  // the operator the game's camera and menu code finds in the seat's scopes
    }

    bool owned = false;
    {
        const PrimaryScope  primary(seatController);
        const OperatorScope scope(seat_objects::OperatorWeak(seat));
        owned = platform::CallGuarded(g_setOwnerChara, spawned, member);
    }
    if (owned)
    {
        camera_override::Route(seat, member);  // the game's SetOwnerChara just wrote the member's delegate
        operator_battle_mode::OnOperatorStarted(seat, spawned, operatorOne);  // a join mid-battle
        nav_area::OnOperatorStarted(seat, seatController);                // the volumes entered before the spawn
    }
    // Moving the new operator off the leader cleared the leader's camera delegate that player 1's operator
    // had bound; the game's own re-apply puts it back.
    void* ownerOne     = nullptr;
    const bool reowned = Read(static_cast<std::uint8_t*>(operatorOne) + facts::kOperatorOwnerChara, ownerOne) &&
                         ownerOne != nullptr && platform::CallGuarded(g_setOwnerChara, operatorOne, ownerOne);
    platform::Log(L"{}'s camera: own operator spawned (index {}), following {}'s member {}; player 1's operator {}", name,
                  seat_objects::OperatorWeak(seat).index, name, owned ? L"yes" : L"NO (the owner call raised)",
                  reowned ? L"re-applied" : L"NOT re-applied");
    return owned;
}

void* Take(int seat) noexcept
{
    if (!SeatBuilt(seat))
    {
        return nullptr;
    }
    camera_override::Route(seat, nullptr);  // the member's delegate holds the game's handler again
    void* cameraOperator = seat_objects::Operator(seat);
    g_seats[seat]        = {};
    seat_objects::BindOperator(seat, nullptr);
    {
        const std::int32_t none[2]{-1, 0};
        scopes::BindOperator(seat, none);
    }
    bool another = false;  // player 1's controller is kept only while some other seat still plays a member
    for (int other = 1; other <= kSeatsBuilt; ++other)
    {
        another = another || (other != seat && SeatActive(other));
    }
    if (!another)
    {
        g_playerOne = {};
    }
    return cameraOperator;
}

void HoldStick(unsigned user, const float (&axes)[4]) noexcept
{
    if (user < g_holds.size())
    {
        g_holds[user].active = true;
        std::memcpy(g_holds[user].axes.data(), axes, sizeof(axes));
    }
}

void ReleaseStick(unsigned user) noexcept
{
    if (user < g_holds.size())
    {
        g_holds[user] = {};
    }
}

int TickingSeat() noexcept
{
    return g_tickingSeat;
}

void* OperatorOne() noexcept
{
    return PlayerOneOperator();
}

void* Operator(int seat) noexcept
{
    return seat_objects::Operator(seat);
}

std::uint32_t OperatorTicks(int seat) noexcept
{
    return seat >= 0 && seat < kPlayers ? g_operatorTicks[static_cast<std::size_t>(seat)] : 0;
}

void LogCounts() noexcept
try
{
    // Diagnostic fields of each controller: its control rotation (stock AController) and the camera
    // sensitivity the game's ProcessPlayerInput scales the right stick by (RE-camera.md).
    constexpr std::uint32_t kCameraSensitivity = 0x9E8;
    for (int i = 0; i <= kSeatsBuilt; ++i)
    {
        void* const          controller = i == 0 ? PlayerOne() : (OnGameThread() ? seat_objects::Controller(i) : nullptr);
        std::array<float, 3> rotation{};
        float                sensitivity = 0.0f;
        const auto*          base        = static_cast<const std::uint8_t*>(controller);
        const bool           read = base != nullptr && Read(base + facts::kControllerControlRotation, rotation) && Read(base + kCameraSensitivity, sensitivity);
        platform::Log(L"player {}: input calls {}, move calls {}, own operator ticks {}, camera updates in scope {}; control rotation {}; camera sensitivity {}",
                      i + 1, g_counts[i].input, g_counts[i].move, g_counts[i].operatorTick, g_counts[i].cameraUpdate,
                      read ? std::format(L"P{:.1f} Y{:.1f} R{:.1f}", rotation[0], rotation[1], rotation[2]) : std::wstring(L"unread"),
                      read ? std::format(L"{:.3f}", sensitivity) : std::wstring(L"unread"));
        g_counts[i] = {};
    }
}
catch (...)
{
    // Formatting fails only by running out of memory; a lost diagnostic line must not take the game down.
}

}  // namespace lss::players::seat_camera
