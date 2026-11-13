#include "views/split_scale.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/instruction_patch.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "views/split_scale_rules.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace lss::views::split_scale {

namespace {

using platform::Read;
using StatusFn = void (*)(void* engine, void* out);

constexpr auto kCheckEvery = std::chrono::milliseconds(500);
constexpr auto kRetryEvery = std::chrono::seconds(5);  // a hold that failed, at the same sizes

// The witness covers both products, each inside its own aligned 16-byte block, holding the bytes the patch replaces.
static_assert(sizeof(facts::kBloomAreaBySp) == sizeof(facts::kBloomAreaByViewScale));
static_assert(facts::kBloomAreaSizeStart + sizeof(facts::kBloomAreaSize) == facts::kBloomAreaHeightMul + sizeof(facts::kBloomAreaBySp));
static_assert(facts::kBloomAreaWidthMul > facts::kBloomAreaSizeStart && facts::kBloomAreaWidthMul < facts::kBloomAreaHeightMul);
static_assert((facts::kBloomAreaWidthMul & 15) + sizeof(facts::kBloomAreaBySp) <= 16);
static_assert((facts::kBloomAreaHeightMul & 15) + sizeof(facts::kBloomAreaBySp) <= 16);
constexpr bool WitnessHolds(std::uintptr_t site) noexcept
{
    for (std::size_t i = 0; i < sizeof(facts::kBloomAreaBySp); ++i)
    {
        if (facts::kBloomAreaSize[site - facts::kBloomAreaSizeStart + i] != facts::kBloomAreaBySp[i])
        {
            return false;
        }
    }
    return true;
}
static_assert(WitnessHolds(facts::kBloomAreaWidthMul) && WitnessHolds(facts::kBloomAreaHeightMul));

struct Status
{
    std::int32_t status   = -1;
    float        fraction = 0;
    float        bound    = 0;
};

/// One console setting the mod holds: the value read before every write
/// (one the mod did not write is the one to put back), the mod's value counted as written once its command ran, and
/// put back only while the mod's is still in place.
template <class T>
struct Held
{
    const wchar_t* name;
    std::uintptr_t ref     = 0;  // the global holding the pointer to its values (game thread's at +0)
    bool           held    = false;
    T              saved{};      // the value to put back
    T              written{};    // the mod's last write
};

struct Sizes
{
    int  resX = 0, resY = 0, viewX = 0, viewY = 0;
    bool read = false;
    bool operator==(const Sizes&) const noexcept = default;
};

// Set at install, then only read.
const std::int32_t* g_systemRes    = nullptr;
StatusFn            g_status       = nullptr;
Console             g_console      = nullptr;
bool                g_installed    = false;
std::uintptr_t      g_bloomWidthSite  = 0;  // 0: the code differs from the research, the bloom area is left alone
std::uintptr_t      g_bloomHeightSite = 0;

// The bloom area's two products, swapped from SP to ViewRect's SP x f while the views are held. Applied and restored on
// the game thread (and at an unload on UE4SS's); the render thread runs the code, and each atomic 16-byte swap is all it
// sees of a change.
platform::InstructionPatch g_bloomWidth;
platform::InstructionPatch g_bloomHeight;

// Game thread.
Held<float>        g_max{L"r.DynamicRes.MaxScreenPercentage"};
Held<std::int32_t> g_deny{L"r.DynamicRes.PausingDenied"};
bool               g_on        = true;
bool               g_split     = false;              // as last ticked
int                g_cap       = split_scale_rules::kFull;  // the rule's cap the views are held at (kFull: none)
Sizes              g_sizes;                          // the last sample
Sizes              g_acted;                          // the sample the hold was made from
std::int32_t       g_lastStatus = -1;                // the dynamic-resolution status last reported while held
bool               g_statusDue  = false;             // the status is read from the check after a hold
std::wstring       g_lastWarning;                    // a failure retried twice a second is logged once
std::wstring       g_lastBloomWarning;               // the same for the bloom area (it can fail in the same check)
bool               g_holdFailed = false;             // the last hold failed: retried at new sizes or after kRetryEvery
Sizes              g_failedAt;
std::chrono::steady_clock::time_point g_failedWhen{};
platform::GameThreadMark g_gameThread;  // set by Tick once installed; Remove asks it from UE4SS's thread
std::chrono::steady_clock::time_point g_lastCheck{};

void Warn(std::wstring text, std::wstring& latch = g_lastWarning) noexcept
{
    if (text != latch)
    {
        platform::LogWarning(L"split scale: {}", text);
        latch = std::move(text);
    }
}

template <class T>
bool ReadSetting(std::uintptr_t ref, T& out) noexcept
{
    const void* values = nullptr;
    return Read(reinterpret_cast<const void*>(ref), values) && values != nullptr && Read(values, out);
}

bool Run(const std::wstring& command) noexcept
{
    return g_console != nullptr && g_console(command.c_str());
}

/// Holds `s` at `value`. False (warned) if it could not be read or the command did not take.
template <class T>
bool HoldAt(Held<T>& s, T value) noexcept
{
    T current{};
    if (!ReadSetting(s.ref, current))
    {
        Warn(std::format(L"{} could not be read; the views are left as they are", s.name));
        return false;
    }
    const bool wasHeld = s.held;
    if (!wasHeld || current != s.written)
    {
        s.saved = current;  // the first hold, or a value someone set meanwhile: the one to put back
    }
    if (current == value)
    {
        s.held    = true;
        s.written = value;
        return true;
    }
    if (!Run(std::format(L"{} {}", s.name, value)))
    {
        s.held = wasHeld;
        Warn(std::format(L"the console could not take {}; tried again later", s.name));
        return false;
    }
    s.held    = true;  // the write ran: whatever the read-back says, the saved value is the one to put back
    s.written = value;
    T read{};
    if (!ReadSetting(s.ref, read))
    {
        Warn(std::format(L"{} could not be read back after setting {}; tried again later", s.name, value));
        return false;
    }
    if (read != value)
    {
        Warn(std::format(L"{} did not take {} (reads {}); tried again later", s.name, value, read));
        return false;
    }
    return true;
}

/// Puts `s` back while the mod's value is in place; a value someone set meanwhile is left (`note` says which).
/// False (warned) if it must be tried again.
template <class T>
bool ReleaseHeld(Held<T>& s, std::wstring& note) noexcept
{
    if (!s.held)
    {
        return true;
    }
    T current{};
    if (!ReadSetting(s.ref, current))
    {
        Warn(std::format(L"{} could not be read to put it back; tried again twice a second", s.name));
        return false;  // an unread value is never overwritten
    }
    if (current != s.written)
    {
        s.held = false;
        note += std::format(L"{}{} left at {} (set meanwhile)", note.empty() ? L"" : L", ", s.name, current);
        return true;
    }
    if (current != s.saved)
    {
        T read{};
        if (!Run(std::format(L"{} {}", s.name, s.saved)) || !ReadSetting(s.ref, read) || read != s.saved)
        {
            Warn(std::format(L"{} could not be put back to {}; tried again twice a second", s.name, s.saved));
            return false;  // still the mod's value: kept held
        }
    }
    s.held = false;
    note += std::format(L"{}{} back to {}", note.empty() ? L"" : L", ", s.name, s.saved);
    return true;
}

Sizes ReadSizes() noexcept
{
    Sizes          sizes;
    std::int32_t   res[2]{};
    const auto     client   = reinterpret_cast<std::uintptr_t>(players::game_globals::GameViewportClient());
    std::uintptr_t viewport = 0;
    std::int32_t   view[2]{};
    sizes.read = platform::CopyGuarded(res, g_systemRes, sizeof(res)) && client != 0 &&
                 Read(reinterpret_cast<const void*>(client + facts::kViewportClientViewport), viewport) && viewport != 0 &&
                 platform::CopyGuarded(view, reinterpret_cast<const void*>(viewport + facts::kViewportSizeX), sizeof(view)) &&
                 res[0] > 0 && res[1] > 0 && view[0] > 0 && view[1] > 0;  // a minimized window reads 0x0: unread
    if (sizes.read)
    {
        sizes = {res[0], res[1], view[0], view[1], true};
    }
    return sizes;
}

Status ReadStatus() noexcept
{
    Status         status;
    std::uintptr_t engine = 0;
    engine = reinterpret_cast<std::uintptr_t>(players::game_globals::Object(players::game_globals::Global::Engine));
    if (engine == 0 ||
        !platform::CallGuarded(g_status, reinterpret_cast<void*>(engine), static_cast<void*>(&status)))
    {
        status.status = -1;
    }
    return status;
}

const wchar_t* StatusName(std::int32_t status) noexcept
{
    switch (status)
    {
        case 0: return L"not supported";
        case 1: return L"disabled";
        case 2: return L"paused";
        case 3: return L"enabled";
        default: return L"unread";
    }
}

/// The bloom area sized as the view (`on`) or as the game sizes it. Both products or neither: a failed swap puts the
/// other back. Logged at each change; a failure once until it changes.
void SizeBloomArea(bool on) noexcept
{
    if (g_bloomWidthSite == 0)
    {
        return;
    }
    const bool width  = g_bloomWidth.IsApplied();
    const bool height = g_bloomHeight.IsApplied();
    if (on)
    {
        if (width && height)
        {
            return;
        }
        if ((width || g_bloomWidth.Apply(g_bloomWidthSite, facts::kBloomAreaBySp, facts::kBloomAreaByViewScale)) &&
            (height || g_bloomHeight.Apply(g_bloomHeightSite, facts::kBloomAreaBySp, facts::kBloomAreaByViewScale)))
        {
            g_lastBloomWarning.clear();
            platform::Log(L"split scale: the bloom and glare areas follow the views' scale (no light streaks past the resolution's width)");
            return;
        }
        g_bloomWidth.Restore();
        g_bloomHeight.Restore();
        Warn(L"the bloom area could not be sized as the views; bright lights streak past the resolution's width", g_lastBloomWarning);
        return;
    }
    if (!width && !height)
    {
        return;
    }
    const bool widthBack  = !width || g_bloomWidth.Restore();
    const bool heightBack = !height || g_bloomHeight.Restore();
    if (widthBack && heightBack)
    {
        g_lastBloomWarning.clear();
        platform::Log(L"split scale: the bloom and glare areas sized as the game sizes them again");
    }
    else
    {
        Warn(L"the bloom area's code was changed by someone else; left as it is, tried again twice a second", g_lastBloomWarning);
    }
}

/// The views held at `cap` (the rule's): the cap the player set kept if it is lower, the pause denied. True if held.
bool Hold(int cap) noexcept
{
    const auto failed = [] {
        g_holdFailed = true;
        g_failedAt   = g_sizes;
        g_failedWhen = std::chrono::steady_clock::now();
        return false;
    };
    float current = 0;
    if (!ReadSetting(g_max.ref, current))
    {
        Warn(L"r.DynamicRes.MaxScreenPercentage could not be read; the views are left as they are");
        return failed();
    }
    const float own    = g_max.held && current == g_max.written ? g_max.saved : current;  // a value set meanwhile is the player's
    const float target = std::min(static_cast<float>(cap), own);
    if (!HoldAt(g_deny, std::int32_t{1}) || !HoldAt(g_max, target))
    {
        return failed();
    }
    g_holdFailed = false;
    g_lastWarning.clear();
    SizeBloomArea(true);
    g_cap       = cap;
    g_acted     = g_sizes;
    g_statusDue = true;  // read from the next check: the engine updates its status at the next frame's start
    platform::Log(L"split scale: the views render at {} % of their size and are scaled into place, as single player scales its "
                  L"view: the game's resolution {}x{} is below the window's {}x{} (set the resolution to {}x{} for full sharpness)",
                  target, g_sizes.resX, g_sizes.resY, g_sizes.viewX, g_sizes.viewY, g_sizes.viewX, g_sizes.viewY);
    return true;
}

void Release(const wchar_t* why) noexcept
{
    SizeBloomArea(false);  // first: with the views at full size the bloom area never passes the buffer, whatever the cap
    g_holdFailed = false;  // a later hold is tried at once
    if (!g_max.held && !g_deny.held)
    {
        return;
    }
    std::wstring note;
    const bool   max  = ReleaseHeld(g_max, note);
    const bool   deny = ReleaseHeld(g_deny, note);
    if (!max || !deny)
    {
        return;  // the one put back stays released; the other is tried again
    }
    g_lastWarning.clear();
    g_cap        = split_scale_rules::kFull;
    g_lastStatus = -1;
    g_statusDue  = false;
    platform::Log(L"split scale: the views at full size again ({}): {}", why, note);
}

/// While held: the game's dynamic-resolution status at each change (the cap acts only while it is enabled).
void CheckStatus() noexcept
{
    if (!g_statusDue)
    {
        return;
    }
    const Status status = ReadStatus();
    if (status.status == g_lastStatus)
    {
        return;
    }
    if (status.status == facts::kDynamicResolutionEnabled)
    {
        platform::Log(L"split scale: the game's dynamic resolution is enabled (fraction {:.2f}): the cap acts", status.fraction);
    }
    else
    {
        platform::LogWarning(L"split scale: the game's dynamic resolution is {}, so the cap cannot act; the views keep their undrawn "
                             L"edges until the resolution is set to the window's",
                             StatusName(status.status));
    }
    g_lastStatus = status.status;
}

std::wstring Reading()
{
    const Status status = ReadStatus();
    const bool   held   = g_max.held || g_deny.held;
    return std::format(L"{}; {}; the resolution {}x{}, the window {}x{}; dynamic resolution {} (fraction {:.2f}); the bloom area {}",
                       g_on ? L"on" : L"off",
                       held ? std::format(L"the views held at {} % (the rule's {} %)", g_max.written, g_cap) : std::wstring(L"the views at full size"),
                       g_sizes.resX, g_sizes.resY, g_sizes.viewX, g_sizes.viewY, StatusName(status.status), status.fraction,
                       g_bloomWidthSite == 0           ? L"left alone (the code differs from the research)"
                       : g_bloomWidth.IsApplied() && g_bloomHeight.IsApplied()   ? L"sized as the views"
                       : g_bloomWidth.IsApplied() || g_bloomHeight.IsApplied()   ? L"half sized as the views (a swap failed)"
                                                                                 : L"as the game sizes it");
}

}  // namespace

bool Install(const platform::GameImage& image, Console console) noexcept
{
    const auto engine    = players::game_globals::Address(players::game_globals::Global::Engine);
    const auto systemRes = players::game_globals::Address(players::game_globals::Global::SystemResolution);
    const auto maxRef    = image.Rebase(facts::kDynResMaxScreenPercentageRef, sizeof(void*));
    const auto denyRef   = image.Rebase(facts::kDynResPausingDeniedRef, sizeof(void*));
    const auto status    = platform::FindFunction(image, facts::kGetDynamicResolutionStatus);
    if (!engine || !systemRes || !maxRef || !denyRef || !status || console == nullptr)
    {
        platform::LogWarning(L"split scale: the engine's values are not where the research read them; split at a resolution below "
                             L"the window keeps undrawn edges");
        return false;
    }
    g_systemRes = reinterpret_cast<const std::int32_t*>(systemRes);
    g_max.ref   = *maxRef;
    g_deny.ref  = *denyRef;
    g_status    = reinterpret_cast<StatusFn>(*status);
    g_console   = console;
    g_installed = true;
    const auto width  = image.Rebase(facts::kBloomAreaWidthMul, sizeof(facts::kBloomAreaBySp));
    const auto height = image.Rebase(facts::kBloomAreaHeightMul, sizeof(facts::kBloomAreaBySp));
    if (width && height && image.BytesMatch(facts::kBloomAreaSizeStart, facts::kBloomAreaSize))
    {
        g_bloomWidthSite  = *width;
        g_bloomHeightSite = *height;
    }
    else
    {
        platform::LogWarning(L"split scale: the bloom area's code is not as the research read it; at a resolution below the window, "
                             L"bright lights streak past the resolution's width");
    }
    control::Register({.name  = L"split-scale",
                       .shape = control::Shape::OnOff,
                       .help  = L"split views render at the game's resolution's share of the window and are scaled into place when the "
                                L"resolution is below the window (views/split_scale.h; on)",
                       .set   = [](const control::Value& value) {
                           g_on = std::get<bool>(value);
                           platform::Log(L"split scale {}", g_on ? L"on" : L"off");
                           g_lastCheck = {};  // act on the next tick
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_on}; }});
    control::Register({.name  = L"split-scale.state",
                       .shape = control::Shape::Text,
                       .help  = L"the cap the split views are held at, the resolution, the window and the game's dynamic resolution",
                       .get   = [] { return control::Value{Reading()}; }});
    platform::Log(L"split scale: ready (split views keep inside the game's resolution through its dynamic-resolution cap)");
    return true;
}

void Tick(bool split) noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_gameThread.Set();
    const auto now  = std::chrono::steady_clock::now();
    const bool flip = split != g_split;
    g_split         = split;
    if (!flip && now - g_lastCheck < kCheckEvery)
    {
        return;
    }
    g_lastCheck        = now;
    const Sizes sample = ReadSizes();
    const bool  steady = sample.read && sample == g_sizes;  // two equal samples: a resolution change and its window resize both landed
    g_sizes            = sample.read ? sample : g_sizes;
    if (!split || !g_on)
    {
        Release(!split ? L"the split ended" : L"switched off");
        return;
    }
    if (!steady)
    {
        return;  // sizes unread (a minimized window) or still moving: the state is kept
    }
    const int want = split_scale_rules::CapPercent(sample.resX, sample.resY, sample.viewX, sample.viewY);
    if (want < split_scale_rules::kFull)
    {
        const bool due   = want != g_cap || !g_max.held || !g_deny.held || !(sample == g_acted);
        const bool retry = !g_holdFailed || !(sample == g_failedAt) || now - g_failedWhen >= kRetryEvery;
        if (due && retry && Hold(want))
        {
            return;  // the status is read from the next check (the engine updates it at the next frame's start)
        }
        if (!due)
        {
            SizeBloomArea(true);  // held: the bloom area too (a release that failed after putting the code back)
        }
        CheckStatus();
    }
    else
    {
        Release(L"the resolution covers the window");
    }
}

void Remove() noexcept
{
    SizeBloomArea(false);  // a code swap, safe from any thread
    if (!g_gameThread.Here())
    {
        if (g_max.held || g_deny.held)
        {
            std::wstring values;
            if (g_max.held)
            {
                values = std::format(L"r.DynamicRes.MaxScreenPercentage {}, the player's own {}", g_max.written, g_max.saved);
            }
            if (g_deny.held)
            {
                values += std::format(L"{}r.DynamicRes.PausingDenied {}, the player's own {}", values.empty() ? L"" : L"; ",
                                      g_deny.written, g_deny.saved);
            }
            platform::Log(L"split scale: unloading off the game thread: the dynamic-resolution values stay as the mod set them ({})",
                          values);
        }
        return;
    }
    Release(L"the mod's end");
}

}  // namespace lss::views::split_scale
