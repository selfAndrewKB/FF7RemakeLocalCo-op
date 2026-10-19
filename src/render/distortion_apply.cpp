#include "render/distortion_apply.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/image.h"
#include "platform/instruction_patch.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <atomic>
#include <cstdint>

namespace lss::render::distortion_apply {

namespace {

// Applied and restored on the game thread only (install, then the console command); the render thread runs the
// patched code, and the patch's atomic 16-byte swap is all it ever sees of a change.
platform::InstructionPatch g_targetSize;
std::uintptr_t             g_site = 0;

std::atomic<bool> g_installed{false};

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto apply = platform::FindFunction(image, facts::kDistortionApply);
    const auto site  = image.Rebase(facts::kDistortionApplyTargetLoad, sizeof(facts::kDistortionApplyTargetView));
    if (!apply || !site || !image.BytesMatch(facts::kDistortionApplyArgsStart, facts::kDistortionApplyArgs))
    {
        platform::Log(L"distortion apply: an engine check failed; a distortion crossing the split line streaks over "
                      L"the other half");
        return false;
    }
    g_site = *site;
    if (!g_targetSize.Apply(g_site, facts::kDistortionApplyTargetView, facts::kDistortionApplyTargetBuffer))
    {
        platform::Log(L"distortion apply: the target-size load could not be patched; a distortion crossing the split "
                      L"line streaks over the other half");
        return false;
    }
    g_installed.store(true);
    control::Register({.name  = L"distortion-apply",
                       .shape = control::Shape::OnOff,
                       .help  = L"the distortion apply draws each view in its own half (render/distortion_apply.h; on)",
                       .set   = [](const control::Value& value) {
                           if (!SetEnabled(std::get<bool>(value)))
                           {
                               return control::Outcome{.ok = false, .detail = L"the instruction changed under the mod"};
                           }
                           platform::Log(L"distortion apply fix {}", Enabled() ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{Enabled()}; }});
    platform::Log(L"distortion apply: installed; each view's distortion is drawn with the buffer's size as the target "
                  L"(0x{:X})",
                  g_site);
    return true;
}

bool Installed() noexcept
{
    return g_installed.load();
}

bool SetEnabled(bool enabled) noexcept
{
    if (!g_installed.load() || enabled == g_targetSize.IsApplied())
    {
        return true;
    }
    return enabled ? g_targetSize.Apply(g_site, facts::kDistortionApplyTargetView, facts::kDistortionApplyTargetBuffer)
                   : g_targetSize.Restore();
}

bool Enabled() noexcept
{
    return g_targetSize.IsApplied();
}

}  // namespace lss::render::distortion_apply
