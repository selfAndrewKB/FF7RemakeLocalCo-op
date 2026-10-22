#include "players/player_two_camera.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/camera_override_two.h"
#include "players/nav_area_two.h"
#include "players/operator_battle_mode.h"
#include "players/pad_buttons.h"
#include "players/scopes.h"
#include "views/scene_camera_two.h"
#include "battle/lock_camera_two.h"
#include "battle/sequence_camera_two.h"
#include "battle/targeting_two.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::players::player_two_camera {

namespace {

using platform::Read;

/// A UE4 FWeakObjectPtr as the game stores it: the object's index in GUObjectArray and its serial number.
struct Weak
{
    std::int32_t index  = -1;
    std::int32_t serial = 0;
};

using SpawnActorFn     = void* (*)(void* world, void* type, const void* transform, const void* parameters);
using GetWorldFn       = void* (*)(void* gameMode);
using StaticClassFn    = void* (*)();
using SetOwnerCharaFn  = void (*)(void* cameraOperator, void* character);
using AllocateSerialFn = std::int32_t (*)(void* objectArray, std::int32_t index);
using ActorVoidFn      = void (*)(void* self);
using ActorFloatFn     = void (*)(void* self, float deltaSeconds);
using InputFn          = void (*)(void* self, float deltaSeconds, bool gamePaused);
using PhaseHookFn      = void (*)(void* world, void* context, std::int32_t phase, bool start);

// Set once at install, then only read.
void**           g_partyManager   = nullptr;  // the global holding the party manager pointer
std::uint8_t**   g_root           = nullptr;
void**           g_gameMode       = nullptr;
std::uint64_t*   g_frameStamp     = nullptr;
std::uint8_t*    g_corrections    = nullptr;
std::uint32_t*   g_nameNone       = nullptr;
std::uint8_t**   g_objects        = nullptr;
std::int32_t*    g_objectCount    = nullptr;
void*            g_objectArray    = nullptr;
std::uint8_t**   g_squareInput    = nullptr;
SpawnActorFn     g_spawnActor     = nullptr;
StaticClassFn    g_staticClass    = nullptr;
SetOwnerCharaFn  g_setOwnerChara  = nullptr;
AllocateSerialFn g_allocateSerial = nullptr;

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

// The game's input tick for the primary controller (RE-input-p2.md §1): its phase hook is reached through a
// global function pointer the game sets at runtime, so the pointer is checked every engine tick.
std::uintptr_t* g_phaseHookPointer = nullptr;
PhaseHookFn     g_phaseHook        = nullptr;
InputFn         g_tickPlayerInput  = nullptr;
int             g_operatorTicking  = 0;  // 2 while player 2's operator ticks
bool            g_phaseHookLogged  = false;

// Game thread only (set by Start from the engine tick; the wrappers act only on that thread).
DWORD g_gameThread  = 0;
Weak  g_playerOne;              // player 1's controller while player 2 is in (resolved at each use)
Weak  g_playerTwo;              // player 2's controller while player 2 is in
Weak  g_operatorTwo;            // player 2's operator
bool  g_spawning    = false;

// Test input and diagnostics, game thread only.
struct StickHold
{
    bool                 active = false;
    std::array<float, 4> axes{};
};
std::array<StickHold, 2> g_holds;

struct CallCounts
{
    std::uint32_t input = 0;      // ProcessPlayerInput under the player's scopes
    std::uint32_t move  = 0;      // Tick_FieldMove under the player's scopes
    std::uint32_t operatorTick = 0;
    std::uint32_t cameraUpdate = 0;  // the camera manager's UpdateCamera under the player's scopes
};
std::array<CallCounts, 2> g_counts;

/// Player 1's input step this frame: the game's arguments, for player 2's step right after it.
struct PrimaryInput
{
    bool  ran    = false;
    float dt     = 0.0f;
    bool  paused = false;
};
PrimaryInput g_primaryInput;
bool         g_noPlayerInputLogged = false;

constexpr std::uint32_t kItemBytes       = 0x18;  // FUObjectItem: Object +0, Flags +8, SerialNumber +0x10
constexpr std::uint32_t kItemFlags       = 0x08;
constexpr std::uint32_t kItemSerial      = 0x10;
constexpr std::int32_t  kUnreachableBits = 0x30000000;  // PendingKill | Unreachable: the pointer is not to be used

bool OnGameThread() noexcept
{
    return g_gameThread != 0 && GetCurrentThreadId() == g_gameThread;
}

/// The object a weak pair names, if it is still that object and alive (the engine's own validity rule).
void* Resolve(const Weak& weak) noexcept
{
    std::uint8_t* objects = nullptr;
    std::int32_t  count   = 0;
    if (weak.index < 0 || weak.serial == 0 || !Read(g_objects, objects) || !Read(g_objectCount, count) ||
        weak.index >= count || objects == nullptr)
    {
        return nullptr;
    }
    const std::uint8_t* item   = objects + static_cast<std::size_t>(weak.index) * kItemBytes;
    void*               object = nullptr;
    std::int32_t        flags = 0, serial = 0;
    if (!Read(item, object) || !Read(item + kItemFlags, flags) || !Read(item + kItemSerial, serial) ||
        serial != weak.serial || (flags & kUnreachableBits) != 0)
    {
        return nullptr;
    }
    return object;
}

/// A weak pair for a live object, the way the game builds one (InternalIndex plus the allocated serial).
Weak MakeWeak(void* object) noexcept
{
    Weak weak;
    std::int32_t index = -1;
    std::int32_t serial = 0;
    if (object != nullptr && Read(static_cast<std::uint8_t*>(object) + facts::kObjectIndex, index) && index >= 0 &&
        platform::CallGuarded(g_allocateSerial, serial, g_objectArray, index) && serial != 0)
    {
        weak = {index, serial};
    }
    return weak;
}

}  // namespace

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

bool WeakPairOf(void* object, std::int32_t (&pair)[2]) noexcept
{
    const Weak weak = MakeWeak(object);
    pair[0] = weak.index;
    pair[1] = weak.serial;
    return weak.index >= 0 && weak.serial != 0;
}

void* ObjectOf(const std::int32_t (&pair)[2]) noexcept
{
    return Resolve(Weak{pair[0], pair[1]});
}

namespace {

std::int32_t* OperatorPair() noexcept
{
    std::uint8_t* root = nullptr;
    return Read(g_root, root) && root != nullptr ? reinterpret_cast<std::int32_t*>(root + facts::kRootOperatorWeak)
                                                 : nullptr;
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
            platform::Log(L"player 2's camera: the primary controller could not be put back");
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
            platform::Log(L"player 2's camera: the operator pair could not be put back");
        }
    }
    OperatorScope(const OperatorScope&)            = delete;
    OperatorScope& operator=(const OperatorScope&) = delete;

private:
    std::int32_t* pair_ = nullptr;
    Weak          saved_;
    Weak          wrote_;
};

// --- The slot replacements -----------------------------------------------------------------------------

// The engine's own functions are called unguarded from these replacements, as the engine calls them. A fault
// inside one is the game's own crash: catching it here and carrying on would hide it and continue in a broken
// state. If something further up handles it, unwinding runs the scopes' destructors, which put every swapped
// value back.

/// Player 1's and player 2's controllers while player 2 is in, resolved through their weak pairs (a
/// controller the engine destroyed or replaced resolves to null, so the scopes stop applying).
struct Players
{
    void* one = nullptr;
    void* two = nullptr;
};

Players CurrentPlayers() noexcept
{
    return OnGameThread() ? Players{Resolve(g_playerOne), Resolve(g_playerTwo)} : Players{};
}

/// AEndFieldCameraOperator::BeginPlay. Player 2's operator begins play inside the spawn Start makes, with
/// PartyManager+0 already naming player 2; here the root's pair names it too, so the leader delegate and the
/// owner the game picks are bound through player 2's controller.
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
        platform::Log(L"player 2's camera: no weak pair for the new operator; it begins play as player 1's would");
    }
    const OperatorScope scope(weak);
    g_operatorBeginPlay(self);
}

/// AEndFieldCameraOperator::Tick. Player 2's operator ticks as the primary one, for player 2.
void OperatorTick(void* self, float deltaSeconds)
{
    const Players players = CurrentPlayers();
    if (players.two == nullptr || self != Resolve(g_operatorTwo))
    {
        g_operatorTick(self, deltaSeconds);
        if (players.two != nullptr && self == PlayerOneOperator())
        {
            battle::sequence_camera_two::AfterOperatorTick(1);
        }
        return;
    }
    ++g_counts[1].operatorTick;
    const PrimaryScope               primary(players.two);
    const OperatorScope              scope(g_operatorTwo);
    const StickScope                 stick(1, true);  // a sequence shot's input offset reads the stick (RE-seq-camera-two.md L-H)
    // Its leader is player 2's member: the battle camera requests, the move and camera-input checks and the battle
    // sequence overrides the tick reads are keyed on the party leader (RE-lock-camera-two.md §1.4), as the lock
    // camera follows player 2's target, not player 1's.
    const scopes::Scope              leader(1, scopes::Leader);
    const battle::targeting_two::FieldsScope targets;
    g_operatorTicking = 2;
    battle::lock_camera_two::BeforeOperatorTick(self, players.two);
    g_operatorTick(self, deltaSeconds);
    battle::lock_camera_two::AfterOperatorTick(self);
    g_operatorTicking = 0;
    battle::sequence_camera_two::AfterOperatorTick(2);
    // The owner's camera-override route follows the owner (the tick may have changed it: the LeaderChange flavor).
    void* owner = nullptr;
    camera_override_two::Route(Read(static_cast<std::uint8_t*>(self) + facts::kOperatorOwnerChara, owner) ? owner : nullptr);
}

/// AEndPlayerCameraManager::UpdateCamera: player 2's camera manager updates as the primary player's, with player
/// 2's member as the leader (the occlusion fade toward the leader, the slot blends that write the primary
/// controller's rotation; RE-lock-camera-ledger.md row X7).
void UpdateCamera(void* self, float deltaSeconds)
{
    const Players players    = CurrentPlayers();
    void*         managerTwo = nullptr;
    if (void* mirror = views::scene_camera_two::MirrorManager(); mirror != nullptr && self == mirror)
    {
        // The mirror player's manager (views/mirror_player): every slot of player 1's set on it, under the mirror's
        // primary scope so the game's primary-scoped setters (the blend records, the cut) reach it.
        const PrimaryScope primary(views::scene_camera_two::MirrorController());
        views::scene_camera_two::BeforeMirrorUpdate();
        g_cameraUpdate(self, deltaSeconds);
        return;
    }
    if (players.two == nullptr ||
        !Read(static_cast<std::uint8_t*>(players.two) + facts::kControllerCameraManager, managerTwo) || self != managerTwo)
    {
        if ((players.two != nullptr && self == platform::ReadPointer(players.one, facts::kControllerCameraManager)) ||
            (self != nullptr && self == views::scene_camera_two::MirrorSource()))
        {
            views::scene_camera_two::BeforeUpdateCamera(self, nullptr);  // player 1's: its camera cut noted for this frame
        }
        g_cameraUpdate(self, deltaSeconds);
        return;
    }
    ++g_counts[1].cameraUpdate;
    const PrimaryScope  primary(players.two);
    const scopes::Scope leader(1, scopes::Leader);
    // A scene's camera (slots player 1's manager was given this frame) onto player 2's, with the game's own setters,
    // inside the scope so the primary-scoped ones reach player 2's manager.
    views::scene_camera_two::BeforeUpdateCamera(platform::ReadPointer(players.one, facts::kControllerCameraManager), self);
    g_cameraUpdate(self, deltaSeconds);
}

/// AEndPlayerController::ProcessPlayerInput: each player's right stick reaches that player's operator.
void ProcessPlayerInput(void* self, float deltaSeconds, bool gamePaused)
{
    const Players players = CurrentPlayers();
    if (players.two == nullptr || (self != players.one && self != players.two))
    {
        g_processInput(self, deltaSeconds, gamePaused);
        return;
    }
    if (self == players.one)
    {
        ++g_counts[0].input;
        g_primaryInput = {true, deltaSeconds, gamePaused};
        const StickScope stick(0, false);
        g_processInput(self, deltaSeconds, gamePaused);
        return;
    }
    ++g_counts[1].input;
    if (Resolve(g_operatorTwo) == nullptr)
    {
        // Without its own operator, player 2's stick would reach player 1's: a still stick instead.
        const StickScope stick(UINT32_MAX, true);
        g_processInput(self, deltaSeconds, gamePaused);
        return;
    }
    const PrimaryScope             primary(players.two);
    const OperatorScope            scope(g_operatorTwo);
    const StickScope               stick(1, true);
    const pad_buttons::ButtonScope buttons(1);
    // The right stick is the lock's while the leader is locked on a target in range (the stick switches targets
    // then; the camera does not turn): player 2's lock and leader, not player 1's (RE-lock-camera-ledger.md X1).
    const scopes::Scope                      leader(1, scopes::Leader);
    const battle::targeting_two::FieldsScope targets;
    g_processInput(self, deltaSeconds, gamePaused);
    battle::lock_camera_two::AfterInput(self);
}

/// AEndPlayerController::Tick_FieldMove: each player's left stick moves only that player's character. Player
/// 2's movement also takes its direction from player 2's camera (the game reads the primary controller's camera
/// manager for the basis) and marks player 2's operator (the game writes flags on "the" operator).
void FieldMove(void* self, float deltaSeconds)
{
    const Players players = CurrentPlayers();
    if (players.two == nullptr || (self != players.one && self != players.two))
    {
        g_fieldMove(self, deltaSeconds);
        return;
    }
    if (self == players.one)
    {
        ++g_counts[0].move;
        const StickScope stick(0, false);
        g_fieldMove(self, deltaSeconds);
        return;
    }
    ++g_counts[1].move;
    const PrimaryScope             primary(players.two);
    const OperatorScope            scope(g_operatorTwo);  // no-op while player 2 has no operator
    const StickScope               stick(1, true);
    const pad_buttons::ButtonScope buttons(1);
    // Locked-on movement faces the target (strafes) on player 2's lock, not player 1's (RE-lock-camera-ledger.md X2).
    const battle::targeting_two::FieldsScope targets;
    g_fieldMove(self, deltaSeconds);
}

/// The game's world-tick phase hook. At phase 1's start it ticks the primary controller's input (the only
/// controller whose input the game ticks during play). Right after it, player 2's controller gets the same input
/// step with the same arguments; the ProcessPlayerInput replacement then scopes it to player 2's operator and pad.
void PhaseHook(void* world, void* context, std::int32_t phase, bool start)
{
    g_phaseHook(world, context, phase, start);
    if (phase != facts::kInputTickPhase || !start || !g_primaryInput.ran || g_tickPlayerInput == nullptr)
    {
        return;
    }
    const PrimaryInput primary = g_primaryInput;
    g_primaryInput             = {};
    const Players players      = CurrentPlayers();
    void*         playerInput  = nullptr;
    if (players.two == nullptr)
    {
        return;
    }
    if (!Read(static_cast<std::uint8_t*>(players.two) + facts::kControllerPlayerInput, playerInput) || playerInput == nullptr)
    {
        if (!g_noPlayerInputLogged)
        {
            platform::Log(L"player 2's input: the controller has no PlayerInput object; its input step is skipped");
            g_noPlayerInputLogged = true;
        }
        return;
    }
    // Unlike the replacements above, the engine never makes this call itself (it ticks only the primary
    // controller's input), so it is guarded like the mod's other calls: a fault turns player 2's input step off.
    if (!platform::CallGuarded(g_tickPlayerInput, players.two, primary.dt, primary.paused))
    {
        g_tickPlayerInput = nullptr;
        platform::Log(L"player 2's input: the input step RAISED AN EXCEPTION; player 2's input step is off until the game restarts");
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

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (g_operatorBeginPlaySlot.Active())
    {
        return false;  // already installed: a second install must not undo the first
    }
    const auto array = image.Rebase(facts::kObjectArray);
    if (!Global(image, facts::kPartyManagerGlobal, g_partyManager) || !Global(image, facts::kEndGameRootGlobal, g_root) ||
        !Global(image, facts::kGameModeGlobal, g_gameMode) || !Global(image, facts::kOperatorFrameStamp, g_frameStamp) ||
        !Global(image, facts::kOperatorCorrections, g_corrections) || !Global(image, facts::kNameNoneIndex, g_nameNone) ||
        !Global(image, facts::kObjectArrayObjects, g_objects) || !Global(image, facts::kObjectArrayCount, g_objectCount) ||
        !Global(image, facts::kSquareInputOwnerGlobal, g_squareInput) || !array ||
        !FindInto(image, facts::kSpawnActor, g_spawnActor) || !FindInto(image, facts::kOperatorStaticClass, g_staticClass) ||
        !FindInto(image, facts::kSetOwnerChara, g_setOwnerChara) ||
        !FindInto(image, facts::kAllocateSerialNumber, g_allocateSerial))
    {
        platform::Log(L"player 2's camera: an engine check failed; player 2 keeps a still camera");
        return false;
    }
    g_objectArray = reinterpret_cast<void*>(*array);

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
    camera_override_two::Install(image);  // optional: without it player 2's attack cameras stay on player 1's camera
    operator_battle_mode::Install(image);  // optional: without it player 2's camera plays battles on the field setting
    nav_area_two::Install(image);          // optional: without it player 2's InDoor settings reach player 1's operator
    // Optional: without it player 2's right stick does nothing, and everything else still works.
    if (!Global(image, facts::kWorldTickPhaseHookGlobal, g_phaseHookPointer) ||
        !FindInto(image, facts::kWorldTickPhaseHook, g_phaseHook) || !FindInto(image, facts::kTickPlayerInput, g_tickPlayerInput))
    {
        g_phaseHookPointer = nullptr;
        platform::Log(L"player 2's input: the game's input tick could not be verified; player 2's right stick stays unused");
    }
    return true;
}

void MaintainInputTick() noexcept
{
    if (g_phaseHookPointer == nullptr)
    {
        return;
    }
    std::uintptr_t current = 0;
    const auto     ours    = reinterpret_cast<std::uintptr_t>(&PhaseHook);
    if (!Read(g_phaseHookPointer, current) || current == ours)
    {
        return;
    }
    if (current == reinterpret_cast<std::uintptr_t>(g_phaseHook))
    {
        const bool swapped = platform::WriteGuarded(g_phaseHookPointer, &ours, sizeof(ours));
        if (swapped || !g_phaseHookLogged)
        {
            platform::Log(L"player 2's input: the game's world-tick phase hook {}", swapped ? L"is wrapped (player 2's input step follows player 1's)" : L"could NOT be wrapped");
        }
        g_phaseHookLogged = !swapped;
    }
    else if (current != 0 && !g_phaseHookLogged)
    {
        platform::Log(L"player 2's input: the world-tick phase hook points somewhere unexpected; left alone");
        g_phaseHookLogged = true;
    }
}

void RemoveInputTick() noexcept
{
    std::uintptr_t current = 0;
    if (g_phaseHookPointer != nullptr && Read(g_phaseHookPointer, current) &&
        current == reinterpret_cast<std::uintptr_t>(&PhaseHook))
    {
        const auto original = reinterpret_cast<std::uintptr_t>(g_phaseHook);
        platform::WriteGuarded(g_phaseHookPointer, &original, sizeof(original));
    }
}

bool Start(void* playerOneController, void* playerTwoController, void* member) noexcept
{
    g_gameThread          = GetCurrentThreadId();
    g_noPlayerInputLogged = false;
    g_playerOne           = MakeWeak(playerOneController);
    g_playerTwo  = MakeWeak(playerTwoController);  // from here each player's sticks are their own pad's

    void* operatorOne = PlayerOneOperator();
    void* manager     = nullptr;
    void* gameMode    = nullptr;
    if (operatorOne == nullptr || !Read(g_partyManager, manager) || manager == nullptr || !Read(g_gameMode, gameMode) ||
        gameMode == nullptr)
    {
        platform::Log(L"player 2's camera: no field camera operator or party manager (not in the field?); still camera");
        return false;
    }
    if (member == nullptr)
    {
        platform::Log(L"player 2's camera: no member to follow; still camera");
        return false;
    }
    void*      world    = nullptr;
    void*      type     = nullptr;
    void**     vtable   = nullptr;
    GetWorldFn getWorld = nullptr;
    if (!Read(gameMode, vtable) || !Read(reinterpret_cast<std::uint8_t*>(vtable) + facts::kGameModeGetWorldSlot, getWorld) ||
        !platform::CallGuarded(getWorld, world, gameMode) || world == nullptr ||
        !platform::CallGuarded(g_staticClass, type) || type == nullptr)
    {
        platform::Log(L"player 2's camera: the world or the operator class could not be found; still camera");
        return false;
    }

    // The game's own spawn parameters for the operator, with no name: the engine then makes a unique one
    // (the game's literal name would make the spawn rebuild player 1's operator in place).
    alignas(8) std::array<std::uint8_t, 0x30> parameters{};
    std::uint32_t                             none = 0;
    if (!Read(g_nameNone, none))
    {
        platform::Log(L"player 2's camera: NAME_None could not be read; still camera");
        return false;
    }
    std::memcpy(parameters.data(), &none, sizeof(none));
    const std::uint32_t flags = facts::kOperatorObjectFlags;
    std::memcpy(parameters.data() + 0x2C, &flags, sizeof(flags));

    // The operator's constructor resets two globals player 1's operator uses; they are put back after,
    // unconditionally: undoing the constructor's write is the point.
    std::uint64_t stamp       = 0;
    std::uint8_t  corrections = 0;
    if (!Read(g_frameStamp, stamp) || !Read(g_corrections, corrections))
    {
        platform::Log(L"player 2's camera: the operator globals could not be read; still camera");
        return false;
    }
    void*               spawned     = nullptr;
    bool                called      = false;
    {
        const PrimaryScope primary(playerTwoController);  // BeginPlay runs inside the spawn
        g_spawning = true;
        called     = platform::CallGuarded(g_spawnActor, spawned, world, type, static_cast<const void*>(nullptr),
                                           static_cast<const void*>(parameters.data()));
        g_spawning = false;
    }
    platform::WriteGuarded(g_frameStamp, &stamp, sizeof(stamp));
    platform::WriteGuarded(g_corrections, &corrections, sizeof(corrections));
    if (!called || spawned == nullptr)
    {
        platform::Log(L"player 2's camera: the operator spawn {}; still camera", called ? L"returned nothing" : L"RAISED AN EXCEPTION");
        return false;
    }
    g_operatorTwo = MakeWeak(spawned);
    {
        const std::int32_t pair[2]{g_operatorTwo.index, g_operatorTwo.serial};
        scopes::BindOperator(1, pair);  // the operator the game's camera and menu code finds in player 2's scopes
    }

    bool owned = false;
    {
        const PrimaryScope  primary(playerTwoController);
        const OperatorScope scope(g_operatorTwo);
        owned = platform::CallGuarded(g_setOwnerChara, spawned, member);
    }
    if (owned)
    {
        camera_override_two::Route(member);  // the game's SetOwnerChara just wrote the member's delegate
        operator_battle_mode::OnOperatorStarted(spawned, operatorOne);  // a join mid-battle
        nav_area_two::OnOperatorStarted(playerTwoController);           // the volumes entered before the spawn
    }
    // Moving the new operator off the leader cleared the leader's camera delegate that player 1's operator
    // had bound; the game's own re-apply puts it back.
    void* ownerOne     = nullptr;
    const bool reowned = Read(static_cast<std::uint8_t*>(operatorOne) + facts::kOperatorOwnerChara, ownerOne) &&
                         ownerOne != nullptr && platform::CallGuarded(g_setOwnerChara, operatorOne, ownerOne);
    platform::Log(L"player 2's camera: own operator spawned (index {}), following player 2's member {}; player 1's operator {}",
                  g_operatorTwo.index, owned ? L"yes" : L"NO (the owner call raised)", reowned ? L"re-applied" : L"NOT re-applied");
    return owned;
}

void* Take() noexcept
{
    camera_override_two::Route(nullptr);  // the member's delegate holds the game's handler again
    void* operatorTwo = Resolve(g_operatorTwo);
    g_operatorTwo     = {};
    {
        const std::int32_t none[2]{-1, 0};
        scopes::BindOperator(1, none);
    }
    g_playerOne       = {};
    g_playerTwo       = {};
    return operatorTwo;
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

int OperatorTicking() noexcept
{
    return g_operatorTicking;
}

void* OperatorOne() noexcept
{
    return PlayerOneOperator();
}

void* OperatorTwo() noexcept
{
    return Resolve(g_operatorTwo);
}

void LogCounts() noexcept
try
{
    // Diagnostic fields of each controller: its control rotation (stock AController) and the camera
    // sensitivity the game's ProcessPlayerInput scales the right stick by (RE-camera.md §9.3).
    constexpr std::uint32_t kCameraSensitivity = 0x9E8;
    const Players players = CurrentPlayers();
    void* const   controllers[2] = {players.one, players.two};
    for (std::size_t i = 0; i < g_counts.size(); ++i)
    {
        std::array<float, 3> rotation{};
        float                sensitivity = 0.0f;
        const auto*          base        = static_cast<const std::uint8_t*>(controllers[i]);
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

}  // namespace lss::players::player_two_camera
