#include "battle/lock_camera_two.h"

#include "battle/command_menu_tick.h"
#include "battle/targeting_two.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/player_two_camera.h"
#include "players/scopes.h"

#include <array>
#include <cstdint>
#include <format>

namespace lss::battle::lock_camera_two {

namespace {

using FindRotationFn    = char (*)(void* requests, float* outRotation, void* owner, void* view, void* params, char idFiltered);
using TransformFn       = void (*)(void* a1, void* a2, void* a3, void* a4);
using LockNearestFn     = void (*)(void* a1, char a2, char a3, char a4, char a5);
using RestoreLockFn     = char (*)(void* state, void* a2, void* a3, void* a4);

constexpr int      kPlayer      = 1;
constexpr unsigned kTwoContexts = players::scopes::Primary | players::scopes::Operator | players::scopes::Leader;

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

// Game thread.
std::uint32_t g_hits[2]{};       // request re-aims: [0] any other operator's tick, [1] player 2's operator's tick
bool          g_loggedTwo = false;
std::uint32_t g_transforms = 0;  // the game's call repeated for player 2
std::uint32_t g_lockNearests = 0;
std::uint32_t g_restores[4]{};   // the target-list end: ctrl1's tick, ctrl2's tick, a pad-2 key event, elsewhere
bool          g_restoreLogged[4]{};

/// BattleCamRequests_FindActiveRotation at the operator tick's Lock-state call: counted per operator.
char FindRotation(void* requests, float* outRotation, void* owner, void* view, void* params, char idFiltered)
{
    const char found = reinterpret_cast<FindRotationFn>(g_requestCall.Original())(requests, outRotation, owner, view, params, idFiltered);
    if (found != 0)
    {
        const bool two = players::player_two_camera::OperatorTicking() == 2;
        ++g_hits[two ? 1 : 0];
        if (two && !g_loggedTwo)
        {
            g_loggedTwo = true;
            platform::LogDebug(L"lock camera 2: a battle camera request re-aimed player 2's lock camera (yaw {:.1f})", outRotation[1]);
        }
    }
    return found;
}

/// True while player 2 holds a member: the game's call may run again for player 2.
bool PlayerTwoIn() noexcept
{
    return players::scopes::Member(kPlayer) != nullptr;
}

/// An enemy's transform: the game's re-target of the displayed target, then once more on player 2's copy.
void Transform(void* a1, void* a2, void* a3, void* a4)
{
    reinterpret_cast<TransformFn>(g_transform)(a1, a2, a3, a4);
    if (!PlayerTwoIn())
    {
        return;
    }
    const players::scopes::Scope     scope(kPlayer, kTwoContexts);
    const targeting_two::FieldsScope targets;
    if (scope.Held())
    {
        ++g_transforms;
        reinterpret_cast<TransformFn>(g_transform)(a1, a2, a3, a4);
        platform::LogDebug(L"lock camera 2: an enemy transform re-targeted player 2's displayed target too ({} so far)", g_transforms);
    }
}

/// A script's lock on the nearest enemy: for player 1, then for player 2 (its leader, its lock, its camera).
void LockNearest(void* a1, char a2, char a3, char a4, char a5)
{
    reinterpret_cast<LockNearestFn>(g_lockNearest)(a1, a2, a3, a4, a5);
    if (!PlayerTwoIn())
    {
        return;
    }
    const players::scopes::Scope     scope(kPlayer, kTwoContexts);
    const targeting_two::FieldsScope targets;
    if (scope.Held())
    {
        ++g_lockNearests;
        reinterpret_cast<LockNearestFn>(g_lockNearest)(a1, a2, a3, a4, a5);
        platform::LogDebug(L"lock camera 2: a scripted lock on the nearest enemy ran for player 2 too ({} so far)", g_lockNearests);
    }
}

/// The command menu's target-list end, counted by the context it runs in (a reading, no change).
char CountRestore(std::uintptr_t original, void* state, void* a2, void* a3, void* a4)
{
    const int  ticking = command_menu_tick::TickingPlayer();
    const int  context = ticking == 1 ? 0 : ticking == 2 ? 1 : command_menu_tick::EventContextActive() ? 2 : 3;
    ++g_restores[context];
    if (!g_restoreLogged[context])
    {
        g_restoreLogged[context] = true;
        static constexpr const wchar_t* kNames[] = {L"player 1's controller tick", L"player 2's controller tick", L"a pad-2 key event", L"another context"};
        platform::LogDebug(L"lock camera 2: the target-list end restored a lock in {}", kNames[context]);
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
        platform::Log(L"lock camera 2: an engine check failed; player 2's lock edges are not covered");
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
        platform::Log(L"lock camera 2: a re-target call could not be redirected; player 2's lock misses enemy transforms and scripted locks");
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
        platform::Log(L"lock camera 2: a target-list end slot could not be replaced; its context reading is partial");
    }
    control::Register({.name = L"lock-camera-two.requests", .shape = control::Shape::Text,
                       .help = L"battle camera requests that re-aimed a lock camera: in player 2's operator tick, in any other",
                       .get  = [] { return control::Value{std::format(L"player 2's operator {}, other {}", g_hits[1], g_hits[0])}; }});
    control::Register({.name = L"lock-camera-two.restore", .shape = control::Shape::Text,
                       .help = L"the command menu's target-list end (lock restore) by context, and the re-targets repeated for player 2",
                       .get  = [] {
                           return control::Value{std::format(L"ctrl1 tick {}, ctrl2 tick {}, pad-2 event {}, elsewhere {}; transforms {}, scripted locks {}",
                                                             g_restores[0], g_restores[1], g_restores[2], g_restores[3], g_transforms, g_lockNearests)};
                       }});
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

void Tick() noexcept
{
    if (g_placed || g_requestSite == 0)
    {
        return;
    }
    g_placed = true;
    if (g_requestCall.InstallFromOwnThread(g_requestSite, g_requestTarget, reinterpret_cast<std::uintptr_t>(&FindRotation)))
    {
        platform::Log(L"lock camera 2: installed; battle camera requests reaching each operator's lock camera are counted");
    }
    else
    {
        platform::Log(L"lock camera 2: the request call could not be redirected; player 2's lock camera requests are not counted");
    }
}

}  // namespace lss::battle::lock_camera_two
