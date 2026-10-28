#include "views/letterbox_patch.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace lss::views::letterbox_patch {

namespace {

constexpr std::size_t  kCompareBytes  = 7;     // 83 3D rel32 imm8
constexpr std::uint8_t kGameImmediate = 1;     // EWindowMode::WindowedFullscreen
constexpr std::uint8_t kNeverImmediate = 7;    // no window mode (0 Fullscreen, 1 WindowedFullscreen, 2 Windowed)

bool                        g_installed = false;
bool                        g_on        = true;   // the switch: the patch follows the want (a further player in, or the span held) while on
bool                        g_applied   = false;
std::vector<std::uintptr_t> g_sites;              // live addresses of the immediates: the 321 compares', then the
                                                  // view-rect override's (CalcSceneView)
std::uintptr_t              g_rangeStart = 0;     // the first compare .. the last compare's end, for the log
std::size_t                 g_rangeBytes = 0;
unsigned                    g_applies = 0, g_restores = 0;

/// Writes `value` into every immediate holding `expected`. Returns how many were written; -1 if a page could not
/// be made writable.
int WriteImmediates(std::uint8_t expected, std::uint8_t value) noexcept
{
    int written = 0;
    for (const auto site : g_sites)
    {
        DWORD was = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(site), 1, PAGE_EXECUTE_READWRITE, &was))
        {
            return -1;
        }
        auto* immediate = reinterpret_cast<volatile std::uint8_t*>(site);
        if (*immediate == expected)
        {
            *immediate = value;  // one byte: an atomic store, the instruction's length unchanged
            ++written;
        }
        DWORD restored = 0;
        VirtualProtect(reinterpret_cast<void*>(site), 1, was, &restored);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 1);
    }
    return written;
}

void Apply() noexcept
{
    const int written = WriteImmediates(kGameImmediate, kNeverImmediate);
    g_applied         = written > 0;
    ++g_applies;
    if (written == static_cast<int>(g_sites.size()))
    {
        platform::Log(L"letterbox patch: on; the {} compares no longer fit a view into the target, and the view rects are "
                      L"no longer overridden to the whole resolution, while two views exist",
                      written);
    }
    else
    {
        platform::LogWarning(L"letterbox patch: {} of {} compares written (the rest did not hold the game's byte)", written,
                             g_sites.size());
    }
}

void Lift(const wchar_t* why) noexcept
{
    const int written = WriteImmediates(kNeverImmediate, kGameImmediate);
    g_applied         = false;
    ++g_restores;
    if (written == static_cast<int>(g_sites.size()))
    {
        platform::Log(L"letterbox patch: off ({}); the game's compares are back", why);
    }
    else
    {
        platform::LogWarning(L"letterbox patch: off ({}); {} of {} compares put back", why, written, g_sites.size());
    }
}

/// The compare at `bytes` (live address `at`) names the window-mode global and holds the game's immediate.
bool IsCompare(const std::uint8_t* bytes, std::uintptr_t at, std::uintptr_t modeGlobal) noexcept
{
    if (bytes[0] != 0x83 || bytes[1] != 0x3D || bytes[6] != kGameImmediate)
    {
        return false;
    }
    std::int32_t rel = 0;
    std::memcpy(&rel, bytes + 2, sizeof(rel));
    return at + kCompareBytes + static_cast<std::intptr_t>(rel) == modeGlobal;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto first = image.Rebase(facts::kLetterboxFirstCompare, kCompareBytes);
    const auto last  = image.Rebase(facts::kLetterboxLastCompare, kCompareBytes);
    const auto mode  = image.Rebase(facts::kSystemResolution + facts::kSystemResolutionWindowMode, sizeof(std::int32_t));
    if (!first || !last || !mode || *last < *first)
    {
        platform::LogWarning(L"letterbox patch: the compare range or the window-mode global is outside the image; unavailable");
        return false;
    }
    // The range (about 2.7 MB of the final pass and its setters) is read once and scanned in memory.
    const std::size_t         bytes = *last + kCompareBytes - *first;
    std::vector<std::uint8_t> code(bytes);
    if (!platform::CopyGuarded(code.data(), reinterpret_cast<const void*>(*first), bytes))
    {
        platform::LogWarning(L"letterbox patch: the compare range could not be read; unavailable");
        return false;
    }
    g_sites.clear();
    for (std::size_t i = 0; i + kCompareBytes <= bytes; ++i)
    {
        if (IsCompare(code.data() + i, *first + i, *mode))
        {
            g_sites.push_back(*first + i + kCompareBytes - 1);  // the imm8
            i += kCompareBytes - 1;
        }
    }
    if (g_sites.size() != facts::kLetterboxCompares)
    {
        platform::LogWarning(L"letterbox patch: {} compares of the window mode found between {:#x} and {:#x}, not {}; "
                             L"unavailable (split in Borderless keeps the game's letterbox)",
                             g_sites.size(), *first, *last, facts::kLetterboxCompares);
        g_sites.clear();
        return false;
    }
    // Upstream of the pass: CalcSceneView's override of the view rects to the whole resolution in mode 1.
    const auto override = image.Rebase(facts::kViewRectOverrideSite, facts::kViewRectOverrideWitness.size());
    if (!override || !image.BytesMatch(facts::kViewRectOverrideSite, facts::kViewRectOverrideWitness))
    {
        platform::LogWarning(L"letterbox patch: CalcSceneView's view-rect override is not the code the research read; "
                             L"unavailable (split in Borderless keeps the game's letterbox)");
        g_sites.clear();
        return false;
    }
    g_sites.push_back(*override + facts::kViewRectOverrideImmediate);
    g_rangeStart = *first;
    g_rangeBytes = *last + kCompareBytes - *first;
    g_installed  = true;
    control::Register({.name  = L"letterbox-patch",
                       .shape = control::Shape::OnOff,
                       .help  = L"the Borderless letterbox is switched off while a further player is in or the span is held (on by "
                                L"default); off keeps the game's compares: in split the last view covers the window, with "
                                L"one view the game pillarboxes it into the window's middle",
                       .set   = [](const control::Value& v) {
                           g_on = std::get<bool>(v);
                           if (!g_on && g_applied)
                           {
                               Lift(L"switched off");
                           }
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_on}; }});
    control::Register({.name = L"letterbox-patch.sites", .shape = control::Shape::Text,
                       .help = L"the window-mode compares found and how many are patched now",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"letterbox patch: ready; {} window-mode compares found between {:#x} and {:#x}, and CalcSceneView's "
                  L"view-rect override at {:#x}; patched while a further player is in or the span is held",
                  g_sites.size() - 1, g_rangeStart, g_rangeStart + g_rangeBytes, *override);
    return true;
}

void Tick(bool wanted) noexcept
{
    if (!g_installed)
    {
        return;
    }
    const bool want = g_on && wanted;
    if (want && !g_applied)
    {
        Apply();
    }
    else if (!want && g_applied)
    {
        Lift(L"every further player is out and the span is not held");
    }
}

void Restore() noexcept
{
    if (g_installed && g_applied)
    {
        Lift(L"the mod unloads");
    }
}

bool Applied() noexcept
{
    return g_applied;
}

std::wstring Reading() noexcept
{
    if (!g_installed)
    {
        return L"unavailable";
    }
    std::size_t patched = 0;
    for (const auto site : g_sites)
    {
        std::uint8_t byte = 0;
        if (platform::Read(reinterpret_cast<const void*>(site), byte) && byte == kNeverImmediate)
        {
            ++patched;
        }
    }
    return std::format(L"{} compares and the view-rect override, {} patched now; applied {} time(s), lifted {}",
                       g_sites.size() - 1, patched, g_applies, g_restores);
}

}  // namespace lss::views::letterbox_patch
