#include "battle/lock_camera_two.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/player_two_camera.h"

#include <cstdint>
#include <format>

namespace lss::battle::lock_camera_two {

namespace {

using FindRotationFn = char (*)(void* requests, float* outRotation, void* owner, void* view, void* params, char idFiltered);

std::uintptr_t         g_site   = 0;
std::uintptr_t         g_target = 0;
platform::CallRedirect g_call;
bool                   g_placed = false;

// Game thread.
std::uint32_t g_hits[2]{};  // true returns: [0] any other operator's tick, [1] player 2's operator's tick
bool          g_loggedTwo = false;

/// BattleCamRequests_FindActiveRotation at the operator tick's Lock-state call: counted per operator.
char FindRotation(void* requests, float* outRotation, void* owner, void* view, void* params, char idFiltered)
{
    const char found = reinterpret_cast<FindRotationFn>(g_call.Original())(requests, outRotation, owner, view, params, idFiltered);
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

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto target = platform::FindFunction(image, facts::kBattleCamFindActiveRotation);
    const auto site   = image.Rebase(facts::kBattleCamFindActiveRotationLockCall, 5);
    if (!target || !site)
    {
        platform::Log(L"lock camera 2: an engine check failed; player 2's lock camera requests are not counted");
        return false;
    }
    g_target = *target;
    g_site   = *site;
    control::Register({.name = L"lock-camera-two.requests", .shape = control::Shape::Text,
                       .help = L"battle camera requests that re-aimed a lock camera: in player 2's operator tick, in any other",
                       .get  = [] { return control::Value{std::format(L"player 2's operator {}, other {}", g_hits[1], g_hits[0])}; }});
    return true;
}

void Remove() noexcept
{
    g_call.Restore();
}

void Tick() noexcept
{
    if (g_placed || g_site == 0)
    {
        return;
    }
    g_placed = true;
    if (g_call.InstallFromOwnThread(g_site, g_target, reinterpret_cast<std::uintptr_t>(&FindRotation)))
    {
        platform::Log(L"lock camera 2: installed; battle camera requests reaching each operator's lock camera are counted");
    }
    else
    {
        platform::Log(L"lock camera 2: the request call could not be redirected; player 2's lock camera requests are not counted");
    }
}

}  // namespace lss::battle::lock_camera_two
