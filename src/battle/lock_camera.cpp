#include "battle/lock_camera.h"

#include "battle/command_menu_tick.h"
#include "battle/lock_camera_rules.h"
#include "battle/targeting.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/field_root.h"
#include "players/seat_camera.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::battle::lock_camera {

namespace {

using lock_camera_rules::WrapDegrees;
using platform::Read;
using players::kSeatsBuilt;
using players::SeatBuilt;
using players::SeatName;

using FindRotationFn    = char (*)(void* requests, float* outRotation, void* owner, void* view, void* params, char idFiltered);
using TransformFn       = void (*)(void* a1, void* a2, void* a3, void* a4);
using LockNearestFn     = void (*)(void* a1, char a2, char a3, char a4, char a5);
using RestoreLockFn     = char (*)(void* state, void* a2, void* a3, void* a4);

constexpr unsigned kSeatContexts = players::scopes::Primary | players::scopes::Operator | players::scopes::Leader;

// Set at install, then only read.
std::uintptr_t         g_requestSite   = 0;
std::uintptr_t         g_requestTarget = 0;
std::uintptr_t         g_transform     = 0;
std::uintptr_t         g_lockNearest   = 0;
std::uintptr_t         g_restoreA      = 0;
std::uintptr_t         g_restoreB      = 0;
platform::CallRedirect g_requestCall;
platform::CallRedirect g_transformCall;
platform::CallRedirect g_lockNearestCall;
std::array<platform::VTableEntry, facts::kCmdMenuRestoreLockSlots.size()> g_restoreSlots;
bool g_placed = false;

// Set at install, then only read.
std::uint64_t* g_frameStamp = nullptr;  // the operators' write-back frame stamp (one global)
void**         g_gameMode   = nullptr;

/// One player's camera between its operator's ticks, for the jump lines (a tick that turned or moved the camera far
/// faster than a stick, a follow or a dodge does: lock_camera_rules::IsJump). Game thread. Index = seat; [0] = player 1's.
struct JumpWatch
{
    bool                 have        = false;
    const void*          watched     = nullptr;  // the operator the pose is from (another one starts the comparison over)
    std::uint64_t        seenFrame   = 0;        // the frame of its last tick (a gap starts the comparison over)
    float                pitch       = 0.0f;
    float                yaw         = 0.0f;
    std::array<float, 3> location{};
    std::uint8_t         lock        = 0;
    void*                target      = nullptr;  // the displayed (lock-camera) target, compared only
    std::uint64_t        lockFrame   = 0;        // the frame its lock last changed (0: not seen)
    std::uint64_t        targetFrame = 0;        // the frame its target last changed (0: not seen)
    std::uint64_t        jumpFrame   = 0;        // the frame of its last jump (0: none)
    bool                 requestTick = false;    // a battle camera request re-aimed its lock camera in this tick
    std::uint32_t        bursts      = 0;        // bursts of jumps (lock_camera_rules::BurstClosed)
    bool                 inBurst     = false;
    std::uint64_t        burstFrame  = 0;        // the frame the current burst opened
    std::uint32_t        burstJumps  = 0;        // jumping ticks in the current burst
    float                burstTurn   = 0.0f;     // the current burst's largest turn (deg) and move (cm)
    float                burstMove   = 0.0f;
    float                turnMax     = 0.0f;     // the largest jump so far
    float                moveMax     = 0.0f;
};
std::array<JumpWatch, players::scopes::kMaxPlayers> g_jumps;
bool g_oneTicking = false;  // player 1's operator is ticking (a request found then is player 1's, not another operator's)

/// Within this many frames a change reads as "just before" a jump.
constexpr std::uint64_t kNearFrames = 120;

/// A seat's operator tick, read around it (RE-lock-camera-ledger.md L1, L2, L5; nothing is changed).
struct FrameReadings
{
    std::uint32_t ticks            = 0;
    std::uint32_t controlRewritten = 0;     // at the tick's start the controller's yaw was not the yaw the last tick wrote back
    float         controlRewriteMax = 0.0f;
    std::uint32_t writeBackSkipped = 0;     // the frame stamp suppressed this tick's write-back (stamp + 1 >= frame)
    std::uint32_t ownerNotSeat     = 0;     // the character was under another controller than the seat's
    std::uint32_t moves            = 0;     // ticks that moved the final yaw
    float         moveMax          = 0.0f;
    std::uint32_t reversals        = 0;     // the yaw's direction reversed between two consecutive moving ticks
    std::uint32_t stickWhileLocked = 0;     // input steps that added rotation input while the seat was locked
    bool          haveLast         = false;
    float         lastFinalYaw     = 0.0f;
    float         lastStep         = 0.0f;
    std::uint32_t lastStepTick     = 0;
    std::array<std::uint32_t, 4>          reversalTicks{};  // the latest reversals' ticks (a ring)
    std::size_t                           ring = 0;
    std::chrono::steady_clock::time_point lastBurstLog{};
};

// Game thread. Index = seat; [0] = player 1's or any other operator where a count has that side (the request hits,
// read as "player 1's or other"), else unused.
struct Seat
{
    std::uint32_t requestHits  = 0;  // battle camera requests that re-aimed this operator's lock camera
    bool          requestLogged = false;
    std::uint32_t transforms   = 0;  // the game's enemy-transform re-target repeated for the seat
    std::uint32_t lockNearests = 0;  // the game's scripted lock-nearest repeated for the seat
    FrameReadings frames;
};
std::array<Seat, players::scopes::kMaxPlayers> g_seats;
std::array<std::uint32_t, 4> g_restores{};  // the enemy detail window's close: player 1's tick, a seat's tick, a seat's pad's key event, elsewhere
std::array<bool, 4>          g_restoreLogged{};

/// BattleCamRequests_FindActiveRotation at the operator tick's Lock-state call: counted per operator (a seat's own,
/// else player 1's or any other).
char FindRotation(void* requests, float* outRotation, void* owner, void* view, void* params, char idFiltered)
{
    const char found = reinterpret_cast<FindRotationFn>(g_requestCall.Original())(requests, outRotation, owner, view, params, idFiltered);
    if (found != 0)
    {
        const int seat = players::seat_camera::TickingSeat();
        Seat&     s    = g_seats[seat];
        ++s.requestHits;
        if (seat != 0 || g_oneTicking)
        {
            g_jumps[seat].requestTick = true;
        }
        if (seat != 0 && !s.requestLogged)
        {
            s.requestLogged = true;
            platform::LogDebug(L"lock camera: a battle camera request re-aimed {}'s lock camera (yaw {:.1f})", SeatName(seat), outRotation[1]);
        }
    }
    return found;
}

/// The game's re-target call run again for every seat whose player holds a member, inside the seat's scopes with
/// its target copy in. A seat whose scope cannot open, or that has no copy yet (nothing of its own to re-target), is
/// skipped.
template <class Fn>
void ForEachSeatWithTargets(Fn&& fn) noexcept
{
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (players::scopes::Member(seat) == nullptr)
        {
            continue;
        }
        const players::scopes::Scope scope(seat, kSeatContexts);
        if (!scope.Held())
        {
            continue;
        }
        const targeting::FieldsScope targets(seat);
        if (targets.Held())
        {
            fn(seat);
        }
    }
}

/// An enemy's transform: the game's re-target of the displayed target, then once more on each seat's copy.
void Transform(void* a1, void* a2, void* a3, void* a4)
{
    reinterpret_cast<TransformFn>(g_transform)(a1, a2, a3, a4);
    ForEachSeatWithTargets([&](int seat) {
        Seat& s = g_seats[seat];
        ++s.transforms;
        reinterpret_cast<TransformFn>(g_transform)(a1, a2, a3, a4);
        platform::LogDebug(L"lock camera: an enemy transform re-targeted {}'s displayed target too ({} so far)", SeatName(seat), s.transforms);
    });
}

/// A script's lock on the nearest enemy: for player 1, then for each seat (its leader, its lock, its camera).
void LockNearest(void* a1, char a2, char a3, char a4, char a5)
{
    reinterpret_cast<LockNearestFn>(g_lockNearest)(a1, a2, a3, a4, a5);
    ForEachSeatWithTargets([&](int seat) {
        Seat& s = g_seats[seat];
        ++s.lockNearests;
        reinterpret_cast<LockNearestFn>(g_lockNearest)(a1, a2, a3, a4, a5);
        platform::LogDebug(L"lock camera: a scripted lock on the nearest enemy ran for {} too ({} so far)", SeatName(seat), s.lockNearests);
    });
}

/// The enemy detail window's close, counted by the context it runs in (a reading, no change).
char CountRestore(std::uintptr_t original, void* state, void* a2, void* a3, void* a4)
{
    const int  seat    = command_menu_tick::TickingSeat();
    const int  context = command_menu_tick::Ticking() ? (seat == 0 ? 0 : 1) : command_menu_tick::EventSeat() != 0 ? 2 : 3;
    ++g_restores[context];
    if (!g_restoreLogged[context])
    {
        g_restoreLogged[context] = true;
        const wchar_t* player = SeatName(context == 2 ? command_menu_tick::EventSeat() : seat);
        platform::LogDebug(L"lock camera: the enemy detail window's close restored a lock in {}",
                           context == 0   ? std::wstring(L"player 1's controller tick")
                           : context == 1 ? std::format(L"{}'s controller tick", player)
                           : context == 2 ? std::format(L"{}'s pad's key event", player)
                                          : std::wstring(L"another context"));
    }
    return reinterpret_cast<RestoreLockFn>(original)(state, a2, a3, a4);
}

char RestoreA(void* state, void* a2, void* a3, void* a4)
{
    return CountRestore(g_restoreA, state, a2, a3, a4);
}

char RestoreB(void* state, void* a2, void* a3, void* a4)
{
    return CountRestore(g_restoreB, state, a2, a3, a4);
}

std::wstring FramesReading()
{
    std::wstring text;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const FrameReadings& f = g_seats[seat].frames;
        text += std::format(L"{}{}: ticks {}, moves {} (max {:.2f}), reversals {}, control rewritten {} (max {:.2f}), write-back skipped {}, "
                            L"character under another controller {}, stick while locked {}",
                            seat == 1 ? L"" : L"; ", SeatName(seat), f.ticks, f.moves, f.moveMax, f.reversals, f.controlRewritten,
                            f.controlRewriteMax, f.writeBackSkipped, f.ownerNotSeat, f.stickWhileLocked);
    }
    return text;
}

std::wstring RequestsReading()
{
    std::wstring text;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        text += std::format(L"{}'s operator {}, ", SeatName(seat), g_seats[seat].requestHits);
    }
    return text + std::format(L"player 1's or other {}", g_seats[0].requestHits);
}

std::wstring RestoreReading()
{
    std::wstring text = std::format(L"player 1's tick {}, a seat's tick {}, a seat's pad event {}, elsewhere {}", g_restores[0], g_restores[1],
                                    g_restores[2], g_restores[3]);
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        text += std::format(L"; {}: transforms {}, scripted locks {}", SeatName(seat), g_seats[seat].transforms, g_seats[seat].lockNearests);
    }
    return text;
}

std::uint64_t Frame() noexcept
{
    void*         gameMode = nullptr;
    std::uint64_t frame    = 0;
    return g_gameMode != nullptr && Read(g_gameMode, gameMode) && gameMode != nullptr &&
                   Read(static_cast<std::uint8_t*>(gameMode) + facts::kGameModeFrameCounter, frame)
               ? frame
               : 0;
}

using players::field_root::BattleSystem;

/// "this frame", "N frames ago", or "not in the last 120 frames" for a remembered frame (0: never).
std::wstring Ago(std::uint64_t then, std::uint64_t now)
{
    if (then == 0 || now == 0 || then > now || now - then > kNearFrames)
    {
        return std::format(L"not in the last {} frames", kNearFrames);
    }
    return now == then ? std::wstring(L"this frame") : std::format(L"{} frame(s) ago", now - then);
}

/// The operator's active flavors by type (the camera behaviours on: follow, reset, command, ...), e.g. "2 8 9 14".
std::wstring ActiveFlavors(void* cameraOperator)
{
    std::wstring text;
    for (std::uint32_t type = 0; type < facts::kOperatorFlavorTypes; ++type)
    {
        void*        flavor = nullptr;
        std::uint8_t active = 0;
        if (Read(static_cast<std::uint8_t*>(cameraOperator) + facts::kOperatorFlavors + 8 * type, flavor) && flavor != nullptr &&
            Read(static_cast<std::uint8_t*>(flavor) + facts::kFlavorActive, active) && active != 0)
        {
            text += std::format(L"{}{}", text.empty() ? L"" : L" ", type);
        }
    }
    return text.empty() ? std::wstring(L"none") : text;
}

/// The line at a burst's first jump: the jump and what surrounded it (the player's lock and target, a battle camera
/// request, the camera behaviours on, every other watched player's lock, target and last jump).
void LogBurstStart(int index, void* cameraOperator, float turn, float move, float deltaSeconds, std::uint64_t frame)
{
    const JumpWatch& w = g_jumps[index];
    std::wstring     others;
    for (int other = 0; other < players::scopes::kMaxPlayers; ++other)
    {
        const JumpWatch& o = g_jumps[other];
        if (other != index && o.have)
        {
            others += std::format(L"; {}: lock {} (changed {}), target changed {}, camera jumped {}", SeatName(other), o.lock != 0 ? L"on" : L"off",
                                  Ago(o.lockFrame, frame), Ago(o.targetFrame, frame), Ago(o.jumpFrame, frame));
        }
    }
    platform::LogDebug(L"camera jump: {}'s camera turned {:.1f} deg and moved {:.2f} m in one tick ({:.1f} ms; in battle {}; its lock {} (changed "
                       L"{}), its target changed {}; a battle camera request re-aimed it this tick: {}; camera behaviours on: {}{})",
                       SeatName(index), turn, move / 100.0f, deltaSeconds * 1000.0f, BattleSystem() != nullptr ? L"yes" : L"no",
                       w.lock != 0 ? L"on" : L"off", Ago(w.lockFrame, frame), Ago(w.targetFrame, frame), w.requestTick ? L"yes" : L"no",
                       ActiveFlavors(cameraOperator), others);
}

/// The burst ends: a line when it held more than one jump, and its counts start over.
void EndBurst(int index)
{
    JumpWatch& w = g_jumps[index];
    if (w.inBurst && w.burstJumps > 1)
    {
        platform::LogDebug(L"camera jump: {}'s camera jumped on {} ticks within {} frames (largest turn {:.1f} deg, largest move {:.2f} m)",
                           SeatName(index), w.burstJumps, w.jumpFrame >= w.burstFrame ? w.jumpFrame - w.burstFrame + 1 : 0, w.burstTurn,
                           w.burstMove / 100.0f);
    }
    w.inBurst    = false;
    w.burstJumps = 0;
    w.burstTurn  = 0.0f;
    w.burstMove  = 0.0f;
}

/// After an operator's tick: its camera's step since the last tick; a burst of jumps is a Debug line at its first
/// jump and one at its end (lock_camera_rules::BurstClosed). `haveTargets`: `lock`/`target` are the player's own, read
/// in its tick (else the last ones are kept).
void WatchJump(int index, void* cameraOperator, float deltaSeconds, bool haveTargets, std::uint8_t lock, void* target)
{
    JumpWatch&           w     = g_jumps[index];
    const std::uint64_t  frame = Frame();
    std::array<float, 2> rotation{};
    std::array<float, 3> location{};
    if (!Read(static_cast<std::uint8_t*>(cameraOperator) + facts::kOperatorFinalRotation, rotation) ||
        !Read(static_cast<std::uint8_t*>(cameraOperator) + facts::kOperatorCameraLocation, location))
    {
        w.requestTick = false;
        return;
    }
    // Another operator, or a gap of ticks (a pause, player 1's camera while no seat was in): the old pose is no step's start.
    if (w.have && (cameraOperator != w.watched || frame == 0 || w.seenFrame == 0 || frame < w.seenFrame || frame - w.seenFrame > 2))
    {
        EndBurst(index);
        w.have = false;
    }
    if (haveTargets)
    {
        if (w.have && lock != w.lock)
        {
            w.lockFrame = frame;
        }
        if (w.have && target != w.target)
        {
            w.targetFrame = frame;
        }
        w.lock   = lock;
        w.target = target;
    }
    if (w.have)
    {
        const float turn = std::max(std::fabs(WrapDegrees(rotation[1] - w.yaw)), std::fabs(WrapDegrees(rotation[0] - w.pitch)));
        const float dx = location[0] - w.location[0], dy = location[1] - w.location[1], dz = location[2] - w.location[2];
        const float move = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (lock_camera_rules::IsJump(turn, move, deltaSeconds))
        {
            if (!w.inBurst)
            {
                ++w.bursts;
                w.inBurst    = true;
                w.burstFrame = frame;
                LogBurstStart(index, cameraOperator, turn, move, deltaSeconds, frame);
            }
            ++w.burstJumps;
            w.burstTurn = std::max(w.burstTurn, turn);
            w.burstMove = std::max(w.burstMove, move);
            w.turnMax   = std::max(w.turnMax, turn);
            w.moveMax   = std::max(w.moveMax, move);
            w.jumpFrame = frame;
        }
        else if (w.inBurst && lock_camera_rules::BurstClosed(w.jumpFrame, frame))
        {
            EndBurst(index);
        }
    }
    w.have        = true;
    w.watched     = cameraOperator;
    w.seenFrame   = frame;
    w.pitch       = rotation[0];
    w.yaw         = rotation[1];
    w.location    = location;
    w.requestTick = false;
}

std::wstring JumpsReading()
{
    std::wstring text;
    for (int index = 0; index < players::scopes::kMaxPlayers; ++index)
    {
        const JumpWatch& w = g_jumps[index];
        if (w.have)
        {
            text += std::format(L"{}{}: {} burst(s) of jumps, largest turn {:.1f} deg, largest move {:.2f} m", text.empty() ? L"" : L"; ",
                                SeatName(index), w.bursts, w.turnMax, w.moveMax / 100.0f);
        }
    }
    return text.empty() ? std::wstring(L"no camera watched yet") : text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto requestTarget = platform::FindFunction(image, facts::kBattleCamFindActiveRotation);
    const auto requestSite   = image.Rebase(facts::kBattleCamFindActiveRotationLockCall, 5);
    const auto transform     = platform::FindFunction(image, facts::kTransformRetarget);
    const auto transformSite = image.Rebase(facts::kTransformRetargetCall, 5);
    const auto lockNearest   = platform::FindFunction(image, facts::kScriptLockNearest);
    const auto lockSite      = image.Rebase(facts::kScriptLockNearestCall, 5);
    const auto restoreA      = platform::FindFunction(image, facts::kCmdMenuRestoreLockA);
    const auto restoreB      = platform::FindFunction(image, facts::kCmdMenuRestoreLockB);
    if (!requestTarget || !requestSite || !transform || !transformSite || !lockNearest || !lockSite || !restoreA || !restoreB)
    {
        platform::Log(L"lock camera: an engine check failed; the seats' lock edges are not covered");
        return false;
    }
    g_requestTarget = *requestTarget;
    g_requestSite   = *requestSite;
    g_transform     = *transform;
    g_lockNearest   = *lockNearest;
    g_restoreA      = *restoreA;
    g_restoreB      = *restoreB;

    const bool retargets = g_transformCall.Install(*transformSite, g_transform, reinterpret_cast<std::uintptr_t>(&Transform)) &&
                           g_lockNearestCall.Install(*lockSite, g_lockNearest, reinterpret_cast<std::uintptr_t>(&LockNearest));
    if (!retargets)
    {
        g_lockNearestCall.Restore();
        g_transformCall.Restore();
        platform::Log(L"lock camera: a re-target call could not be redirected; the seats' locks miss enemy transforms and scripted locks");
    }
    bool readings = true;
    for (std::size_t i = 0; i < g_restoreSlots.size(); ++i)
    {
        const auto& slot = facts::kCmdMenuRestoreLockSlots[i];
        const auto  fn   = slot.target.address == facts::kCmdMenuRestoreLockA.address ? &RestoreA : &RestoreB;
        readings = platform::ReplaceSlot(image, slot, reinterpret_cast<std::uintptr_t>(fn), g_restoreSlots[i]) && readings;
    }
    if (!readings)
    {
        platform::Log(L"lock camera: an enemy detail window close slot could not be replaced; its context reading is partial");
    }
    if (const auto stamp = image.Rebase(facts::kOperatorFrameStamp, sizeof(std::uint64_t)))
    {
        g_frameStamp = reinterpret_cast<std::uint64_t*>(*stamp);
    }
    if (const auto mode = image.Rebase(facts::kGameModeGlobal, sizeof(void*)))
    {
        g_gameMode = reinterpret_cast<void**>(*mode);
    }
    control::Register({.name = L"lock-camera-two.jumps", .shape = control::Shape::Text,
                       .help = L"each player's camera (player 1's while a further player is in): the bursts of jumps (a tick turning over 10 "
                               L"deg at over 720 deg/s, or moving over 1 m at over 25 m/s; a Debug line at a burst's start with the locks, "
                               L"targets and camera behaviours around it, one at its end) and the largest jump",
                       .get  = [] { return control::Value{JumpsReading()}; }});
    control::Register({.name = L"lock-camera-two.frames", .shape = control::Shape::Text,
                       .help = L"each seat's operator tick read around it: ticks, yaw moves (max deg), reversals, control yaw rewritten between "
                               L"ticks (max deg), write-back skipped, the character under another controller, stick input while locked",
                       .get  = [] { return control::Value{FramesReading()}; }});
    control::Register({.name = L"lock-camera-two.requests", .shape = control::Shape::Text,
                       .help = L"battle camera requests that re-aimed a lock camera: in each seat's operator tick, in any other",
                       .get  = [] { return control::Value{RequestsReading()}; }});
    control::Register({.name = L"lock-camera-two.restore", .shape = control::Shape::Text,
                       .help = L"the enemy detail window's close (lock restore, row X5) by context, and the re-targets repeated for each seat",
                       .get  = [] { return control::Value{RestoreReading()}; }});
    return true;
}

void Remove() noexcept
{
    for (auto& slot : g_restoreSlots)
    {
        slot.Restore();
    }
    g_lockNearestCall.Restore();
    g_transformCall.Restore();
    g_requestCall.Restore();
}

void BeforeOperatorTick(int seat, void* seatOperator, void* seatController) noexcept
{
    if (!SeatBuilt(seat))
    {
        return;
    }
    FrameReadings& f = g_seats[seat].frames;
    ++f.ticks;
    std::uint64_t       stamp = 0;
    const std::uint64_t frame = Frame();
    if (g_frameStamp != nullptr && frame != 0 && Read(g_frameStamp, stamp) && stamp + 1 >= frame)
    {
        ++f.writeBackSkipped;
    }
    void* owner           = nullptr;
    void* ownerController = nullptr;
    if (Read(static_cast<std::uint8_t*>(seatOperator) + facts::kOperatorOwnerChara, owner) && owner != nullptr &&
        Read(static_cast<std::uint8_t*>(owner) + facts::kPawnController, ownerController) && ownerController != seatController)
    {
        ++f.ownerNotSeat;
    }
    std::array<float, 3> control{};
    if (f.haveLast && seatController != nullptr &&
        Read(static_cast<std::uint8_t*>(seatController) + facts::kControllerControlRotation, control))
    {
        const float delta = std::fabs(WrapDegrees(control[1] - f.lastFinalYaw));
        if (delta > 0.01f)
        {
            ++f.controlRewritten;
            f.controlRewriteMax = std::max(f.controlRewriteMax, delta);
        }
    }
}

void AfterOperatorTick(int seat, void* seatOperator, float deltaSeconds, bool ownTargets) noexcept
{
    if (!SeatBuilt(seat))
    {
        return;
    }
    // In the tick's scopes the battle system's target fields are the seat's own copy (when `ownTargets`), read live:
    // the tick may have changed them. Outside a battle the seat has no lock and no target; in one without its copy
    // in, the fields are player 1's and the seat's last are kept.
    void* const system = BattleSystem();
    if (system == nullptr)
    {
        WatchJump(seat, seatOperator, deltaSeconds, true, 0, nullptr);
    }
    else
    {
        std::uint8_t lock = 0;
        const bool   own  = ownTargets && Read(static_cast<std::uint8_t*>(system) + facts::kBattleSysLock, lock);
        WatchJump(seat, seatOperator, deltaSeconds, own, lock, own ? platform::ReadPointer(system, facts::kBattleSysDisplayedTarget) : nullptr);
    }
    FrameReadings&       f = g_seats[seat].frames;
    std::array<float, 2> final{};
    if (!Read(static_cast<std::uint8_t*>(seatOperator) + facts::kOperatorFinalRotation, final))
    {
        return;
    }
    if (f.haveLast)
    {
        const float step = WrapDegrees(final[1] - f.lastFinalYaw);
        if (std::fabs(step) > 0.05f)
        {
            ++f.moves;
            f.moveMax = std::max(f.moveMax, std::fabs(step));
            if (f.lastStepTick + 1 == f.ticks && (step > 0.0f) != (f.lastStep > 0.0f))
            {
                ++f.reversals;
                f.reversalTicks[f.ring % f.reversalTicks.size()] = f.ticks;
                ++f.ring;
                // A burst: the four latest reversals within forty ticks (a stutter, not a single turn-around).
                const std::uint32_t oldest = f.reversalTicks[f.ring % f.reversalTicks.size()];
                const auto          now    = std::chrono::steady_clock::now();
                if (f.ring >= f.reversalTicks.size() && f.ticks - oldest <= 40 && now - f.lastBurstLog > std::chrono::seconds(2))
                {
                    f.lastBurstLog = now;
                    platform::LogDebug(L"lock camera: {}'s lock camera reversed its yaw 4 times in {} ticks (steps {:.2f} then {:.2f} deg; so far "
                                       L"control rewritten {}, write-back skipped {}, character under another controller {}, stick while locked {})",
                                       SeatName(seat), f.ticks - oldest, f.lastStep, step, f.controlRewritten, f.writeBackSkipped, f.ownerNotSeat,
                                       f.stickWhileLocked);
                }
            }
            f.lastStep     = step;
            f.lastStepTick = f.ticks;
        }
    }
    f.haveLast     = true;
    f.lastFinalYaw = final[1];
}

void BeforePlayerOneOperatorTick() noexcept
{
    g_oneTicking = true;
}

void AfterPlayerOneOperatorTick(void* cameraOperator, float deltaSeconds) noexcept
{
    g_oneTicking = false;
    // Outside every seat's scope the battle system's target fields are player 1's.
    void*        system = BattleSystem();
    std::uint8_t lock   = 0;
    if (system != nullptr)
    {
        Read(static_cast<std::uint8_t*>(system) + facts::kBattleSysLock, lock);
    }
    WatchJump(0, cameraOperator, deltaSeconds, true, lock, system != nullptr ? platform::ReadPointer(system, facts::kBattleSysDisplayedTarget) : nullptr);
}

void OnLeave(int seat) noexcept
{
    if (SeatBuilt(seat))
    {
        g_seats[seat].frames = FrameReadings{};  // the next player's ticks start their own readings
        EndBurst(seat);
        g_jumps[seat] = JumpWatch{};
        EndBurst(0);  // player 1's camera may stop being watched now (no seat left): its burst ends with the leave

    }
}

void AfterInput(int seat, void* seatController) noexcept
{
    std::array<float, 2> input{};
    if (SeatBuilt(seat) && seatController != nullptr && targeting::Lock(seat) != 0 &&
        Read(static_cast<std::uint8_t*>(seatController) + facts::kControllerRotationInput, input) && (input[0] != 0.0f || input[1] != 0.0f))
    {
        ++g_seats[seat].frames.stickWhileLocked;
    }
}

void Tick() noexcept
{
    if (g_placed || g_requestSite == 0)
    {
        return;
    }
    g_placed = true;
    if (g_requestCall.InstallFromOwnThread(g_requestSite, g_requestTarget, reinterpret_cast<std::uintptr_t>(&FindRotation)))
    {
        platform::Log(L"lock camera: installed; battle camera requests reaching each operator's lock camera are counted");
    }
    else
    {
        platform::Log(L"lock camera: the request call could not be redirected; the seats' lock camera requests are not counted");
    }
}

}  // namespace lss::battle::lock_camera
