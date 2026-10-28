#include "render/ao_origin.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <atomic>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::render::ao_origin {

namespace {

using SetParametersFn = void (*)(void* shader, void* commandList, const void* context);
using SetVec4Fn       = void* (*)(void* commandList, void* shader, const void* parameter, const float* value);

// Set once at install, on the game thread, before any redirect is live; read on the render thread afterwards.
// The redirects' atomic 16-byte code swaps order the writes before any read (x64).
platform::CallRedirect g_stage0Call;
platform::CallRedirect g_tapsCallA;
platform::CallRedirect g_tapsCallB;
SetParametersFn        g_setParameters = nullptr;
SetVec4Fn              g_setVec4       = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_loggedShift{false};

// The view whose stage-0 parameters are being set, on the thread setting them (the render thread).
thread_local const std::uint8_t* t_stage0View = nullptr;

/// The stage-0-only call of the shared SetParameters: notes the view for the length of the call.
void Stage0SetParameters(void* shader, void* commandList, const void* context)
{
    const std::uint8_t* view = nullptr;
    platform::CopyGuarded(&view, static_cast<const std::uint8_t*>(context) + facts::kPassContextView, sizeof(view));
    t_stage0View = view;
    g_setParameters(shader, commandList, context);
    t_stage0View = nullptr;
}

/// The two tap-value calls inside SetParameters: during stage 0, each tap is moved to the view's own rows.
void* SetTaps(void* commandList, void* shader, const void* parameter, const float* value)
{
    const std::uint8_t* view = t_stage0View;
    std::int32_t        min[2]{};
    if (view == nullptr || !g_enabled.load(std::memory_order_relaxed) ||
        !platform::CopyGuarded(min, view + facts::kViewRectMinX, sizeof(min)) || (min[0] == 0 && min[1] == 0))
    {
        return g_setVec4(commandList, shader, parameter, value);
    }
    const float x = static_cast<float>(min[0]);
    const float y = static_cast<float>(min[1]);
    alignas(16) const float shifted[4]{value[0] + x, value[1] + y, value[2] + x, value[3] + y};
    if (!g_loggedShift.exchange(true))
    {
        platform::Log(L"AO origin: stage-0 taps moved by ({}, {}) for a view that does not start at the corner (thread {})",
                      min[0], min[1], GetCurrentThreadId());
    }
    return g_setVec4(commandList, shader, parameter, shifted);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto setParameters = platform::FindFunction(image, facts::kAoDownsampleSetParams);
    const auto setVec4       = platform::FindFunction(image, facts::kSetShaderValueVec4);
    const auto stage0        = image.Rebase(facts::kAoStage0SetParamsCall, 5);
    const auto tapsA         = image.Rebase(facts::kAoTapsCallA, 5);
    const auto tapsB         = image.Rebase(facts::kAoTapsCallB, 5);
    if (!setParameters || !setVec4 || !stage0 || !tapsA || !tapsB)
    {
        platform::Log(L"AO origin: an engine check failed; the further views keep the AO ghost");
        return false;
    }
    g_setParameters = reinterpret_cast<SetParametersFn>(*setParameters);
    g_setVec4       = reinterpret_cast<SetVec4Fn>(*setVec4);
    // The tap calls first: until the stage-0 call is redirected no view is noted, so they pass values through.
    const bool installed =
        g_tapsCallA.Install(*tapsA, *setVec4, reinterpret_cast<std::uintptr_t>(&SetTaps)) &&
        g_tapsCallB.Install(*tapsB, *setVec4, reinterpret_cast<std::uintptr_t>(&SetTaps)) &&
        g_stage0Call.Install(*stage0, *setParameters, reinterpret_cast<std::uintptr_t>(&Stage0SetParameters));
    if (!installed)
    {
        g_stage0Call.Restore();
        g_tapsCallB.Restore();
        g_tapsCallA.Restore();
        platform::Log(L"AO origin: a call site is not the call the research read; the further views keep the AO ghost");
        return false;
    }
    g_installed.store(true);
    control::Register({.name  = L"ao-origin",
                       .shape = control::Shape::OnOff,
                       .help  = L"the AO downsample reads each view's own rows (render/ao_origin.h; on)",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           platform::Log(L"AO origin fix {}", Enabled() ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{Enabled()}; }});
    platform::Log(L"AO origin: installed; each further view's AO downsample will read its own rows");
    return true;
}

bool Installed() noexcept
{
    return g_installed.load();
}

void SetEnabled(bool enabled) noexcept
{
    g_enabled.store(enabled, std::memory_order_relaxed);
    g_loggedShift.store(false);
}

bool Enabled() noexcept
{
    return g_enabled.load(std::memory_order_relaxed);
}

}  // namespace lss::render::ao_origin
