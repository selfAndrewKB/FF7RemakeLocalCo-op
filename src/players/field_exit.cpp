#include "players/field_exit.h"

#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <cstdint>

namespace lss::players::field_exit {

namespace {

using RequestFn = std::uintptr_t (*)(void* root);  // the callers ignore the result; it is passed through all the same

// Set once at install, on the game thread, before either redirect is live.
platform::CallRedirect g_stateCall;
platform::CallRedirect g_blueprintJump;
LeaveFn                g_leave   = nullptr;
void*                  g_context = nullptr;

/// One request: player 2 leaves first if the field still runs (the game's request does nothing otherwise), then
/// the game's request.
std::uintptr_t Request(void* root, const platform::CallRedirect& call, const wchar_t* caller) noexcept
{
    std::uint8_t running = 0;
    const bool   read    = root != nullptr &&
                           platform::CopyGuarded(&running, static_cast<const std::uint8_t*>(root) + facts::kFieldRootRunning, 1);
    platform::Log(L"field exit: requested by {} (field running: {})", caller, !read ? L"unreadable" : running != 0 ? L"yes" : L"no");
    if (read && running != 0 && g_leave != nullptr)
    {
        g_leave(g_context, caller);
    }
    return reinterpret_cast<RequestFn>(call.Original())(root);
}

std::uintptr_t FromStateEntry(void* root) noexcept
{
    return Request(root, g_stateCall, L"the game state's leaving entry");
}

std::uintptr_t FromBlueprint(void* root) noexcept
{
    return Request(root, g_blueprintJump, L"the Blueprint ExitLoation");
}

}  // namespace

bool Install(const platform::GameImage& image, LeaveFn leave, void* context) noexcept
{
    const auto request   = platform::FindFunction(image, facts::kRequestFieldExit);
    const auto stateCall = image.Rebase(facts::kFieldExitStateCall, 5);
    const auto bpJump    = image.Rebase(facts::kFieldExitBlueprintJump, 5);
    if (!request || !stateCall || !bpJump)
    {
        platform::Log(L"field exit: an engine check failed; only the field-step check catches a field exit");
        return false;
    }
    g_leave   = leave;
    g_context = context;
    const bool installed = g_stateCall.Install(*stateCall, *request, reinterpret_cast<std::uintptr_t>(&FromStateEntry)) &&
                           g_blueprintJump.InstallTailJump(*bpJump, *request, reinterpret_cast<std::uintptr_t>(&FromBlueprint));
    if (!installed)
    {
        Remove();
        platform::Log(L"field exit: a call site is not the call the research read; only the field-step check catches a field exit");
        return false;
    }
    platform::Log(L"field exit: player 2 leaves before a retry, load, jump or return to the title leaves the field");
    return true;
}

void Remove() noexcept
{
    g_leave = nullptr;  // a call left in place (changed after us) then only runs the game's request
    for (platform::CallRedirect* call : {&g_stateCall, &g_blueprintJump})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"field exit: a call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::players::field_exit
