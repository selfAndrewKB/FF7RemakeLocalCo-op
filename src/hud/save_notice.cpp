#include "hud/save_notice.h"

#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "engine_facts.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/steady_ms.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "players/game_state.h"
#include "players/unreal_call.h"
#include "players/weak_object.h"

#include <cstdint>
#include <string>
#include <utility>

namespace lss::hud::save_notice {

namespace {

using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

using ShowFn    = void (*)(void* world, std::uint8_t isSave);
using SetTextFn = void (*)(void* block, const wchar_t* text);

constexpr std::uint8_t kCheckLook = 0;  // the show's kind: the "Checking" look, no timer, no text swap

ShowFn    g_show    = nullptr;
SetTextFn g_setText = nullptr;

// The game's hide, looked up on the game thread at the first line.
bool       g_hideLookedUp = false;
UFunction* g_hide         = nullptr;  // /Script/EndGame.EndMenuAPI:BPHideDataAccessIcon
UObject*   g_api          = nullptr;  // its class default object

// Game thread. A line waiting for its first draw, and the mod's text being on the game's notice, are kept apart: a new
// line while the old one is up keeps the second, so the old one is never taken for the game's own notice.
std::wstring g_text;
int          g_seconds   = 0;      // the line's time, from its first draw
bool         g_pending   = false;  // a line waits for its first draw
std::int64_t g_waitUntil = 0;      // the wait's cap: the game's notice holding the box past it drops the line
std::int64_t g_until     = 0;      // the drawn line is due until then (0: none)
bool         g_up        = false;  // the mod's text is on the game's notice now

using platform::NowMs;

/// The notice's manager (EndUISystem's save icon slot), or null.
const std::uint8_t* Manager() noexcept
{
    return static_cast<const std::uint8_t*>(players::game_globals::UiSlot(facts::kSaveIconSlot));
}

bool Shown(const std::uint8_t* manager) noexcept
{
    std::uint8_t shown = 0;
    return manager != nullptr && platform::Read(manager + facts::kSaveIconShown, shown) && shown != 0;
}

/// The game's save system runs or has queued a job (null: none made yet, idle). Its job at the title's press shows the
/// notice and hides it at its end.
bool SaveBusy() noexcept
{
    const auto*  system  = static_cast<const std::uint8_t*>(players::game_globals::Object(players::game_globals::Global::DataSingleton));
    std::uint8_t running = 0;
    std::int32_t queued  = 0;
    return system != nullptr && ((platform::Read(system + facts::kSaveSystemRunningJob, running) && running != 0) ||
                                 (platform::Read(system + facts::kSaveSystemQueuedJobs, queued) && queued > 0));
}

bool LookUpHide() noexcept
try
{
    if (g_hideLookedUp)
    {
        return g_hide != nullptr;
    }
    g_hideLookedUp = true;
    g_hide         = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, facts::kHideDataAccessIconPath);
    g_api          = players::ClassDefaultOf(facts::kEndMenuApiClassPath);
    if (g_hide == nullptr || g_api == nullptr || g_hide->GetParmsSize() != 0)
    {
        g_hide = nullptr;
        platform::Log(L"save notice: EndMenuAPI.BPHideDataAccessIcon is not the function the mod expects; no line is shown");
        return false;
    }
    return true;
}
catch (...)
{
    g_hide = nullptr;
    return false;
}

/// The game's hide; harmless when the notice is down already.
void Hide() noexcept
{
    if (g_hide != nullptr)
    {
        (void)players::GuardedProcessEvent(g_api, g_hide, nullptr);
    }
    g_up = false;
}

/// The game's show with the "Checking" look (skipped while the mod's own line is up: only the text changes), then the
/// mod's text on the widget's block. False (said) when a step failed; the notice is hidden then, so neither the game's
/// box after its show nor the mod's old line stays up.
bool Draw() noexcept
{
    void* const world = players::game_state::World();
    if (world == nullptr)
    {
        platform::Log(L"save notice: no world through the game mode; the line is dropped");
        Hide();
        return false;
    }
    const bool over = g_up && Shown(Manager());  // the new line over the mod's own
    if (!over && !platform::CallGuarded(g_show, world, kCheckLook))
    {
        platform::Log(L"save notice: the game's show faulted; the line is dropped");
        Hide();
        return false;
    }
    const std::uint8_t* manager = Manager();
    players::weak_object::Weak weak;
    void* const widget = manager != nullptr && platform::Read(manager + facts::kSaveIconWidget, weak)
                             ? players::weak_object::Resolve(weak)
                             : nullptr;
    void* const block  = platform::ReadPointer(widget, facts::kSaveIconText);
    if (block == nullptr || !Shown(manager))
    {
        platform::Log(L"save notice: the game's notice did not come up (shown {}, widget {}, text block {}); the line is dropped",
                      Shown(manager) ? L"yes" : L"no", widget != nullptr ? L"yes" : L"none", block != nullptr ? L"yes" : L"none");
        Hide();
        return false;
    }
    if (!platform::CallGuarded(g_setText, block, g_text.c_str()))
    {
        platform::Log(L"save notice: the text setter faulted; the line is dropped");
        Hide();
        return false;
    }
    g_up = true;
    if (g_pending)
    {
        g_pending = false;
        g_until   = NowMs() + static_cast<std::int64_t>(g_seconds) * 1000;
        std::uint8_t errorOnce = 0;
        platform::Read(manager + facts::kSaveIconErrorOnce, errorOnce);
        platform::Log(L"save notice: \"{}\" on the game's save-data notice{}", g_text,
                      errorOnce != 0 ? L" (the game's network notice was shown this session: its hide shows that text again)" : L"");
    }
    else
    {
        platform::LogDebug(L"save notice: \"{}\" shown again (the game's own check hid the box early)", g_text);
    }
    return true;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto show    = platform::FindFunction(image, facts::kShowSaveIcon);
    const auto setText = platform::FindFunction(image, facts::kTextBlockSetWide);
    g_show    = show ? reinterpret_cast<ShowFn>(*show) : nullptr;
    g_setText = setText ? reinterpret_cast<SetTextFn>(*setText) : nullptr;
    const bool ready = g_show != nullptr && g_setText != nullptr && players::weak_object::Installed() &&
                       players::game_globals::UiSlotAddress(facts::kSaveIconSlot) != 0;
    platform::Log(L"save notice: a line of the mod's on the game's save-data notice {}",
                  ready ? L"ready" : L"UNAVAILABLE (a check failed; nothing is shown)");
    if (!ready)
    {
        g_show = nullptr;
    }
    return ready;
}

void Remove() noexcept
{
    if (g_up && g_hide != nullptr)
    {
        if (platform::OnGameThread())
        {
            Hide();
        }
        else
        {
            platform::Log(L"save notice: the line is up at the mod's removal off the game thread; the box stays until the game's "
                          L"next save job ends");
        }
    }
    g_up        = false;
    g_pending   = false;
    g_waitUntil = 0;
    g_until     = 0;
    g_show      = nullptr;
}

void Show(std::wstring text, int seconds) noexcept
{
    if (g_show == nullptr || !platform::OnGameThread() || !LookUpHide())
    {
        return;
    }
    try
    {
        g_text = std::move(text);
    }
    catch (...)
    {
        return;
    }
    g_seconds   = seconds;
    g_pending   = true;
    g_waitUntil = NowMs() + static_cast<std::int64_t>(seconds) * 1000;
    g_until     = 0;  // the new line's own time runs from its draw; the old line, if up, is written over
    Tick();
}

void Tick() noexcept
{
    if (g_show == nullptr || (!g_pending && g_until == 0) || !platform::OnGameThread())
    {
        return;
    }
    const std::int64_t now   = NowMs();
    const bool         shown = Shown(Manager());
    if (g_until != 0 && now >= g_until)
    {
        g_until = 0;
        if (g_up)
        {
            Hide();
            platform::LogDebug(L"save notice: the line's time is up; the game's notice hidden");
        }
        return;
    }
    if (g_pending && !g_up && (shown || SaveBusy()))
    {
        // The game's own notice is up or its save job runs (the title press's check): the line waits, up to its time.
        // A redraw after the game's check hid the line early does not wait (the line is up): a job still running may hide
        // it once more, and the next tick draws it again.
        if (now >= g_waitUntil)
        {
            g_pending = false;
            platform::LogDebug(L"save notice: the game's own notice held the box past the line's time; the line is dropped");
        }
        return;
    }
    if (!g_pending && g_up && shown)
    {
        return;  // the line is up
    }
    // The first draw, a new line over the old, or the line again after the game's check hid it early.
    if (!Draw())
    {
        g_pending = false;  // a step failed (said): no retry every frame
        g_until   = 0;
    }
}

void End() noexcept
{
    if (g_show == nullptr || !platform::OnGameThread() || (!g_pending && g_until == 0))
    {
        return;
    }
    g_pending = false;
    g_until   = 0;
    if (g_up)
    {
        Hide();
        platform::LogDebug(L"save notice: the line ended early (the title screen left); the game's notice hidden");
    }
}

}  // namespace lss::hud::save_notice
