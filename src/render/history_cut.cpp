#include "render/history_cut.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <atomic>
#include <cstdint>
#include <format>
#include <string>

namespace lss::render::history_cut {

namespace {

using SetCutFn = void (*)(void* manager);

// Everything runs on the game thread (the console's commands and the camera updates); g_installed is atomic only so
// Remove can be called from any thread without a race.
SetCutFn          g_setCut    = nullptr;
std::atomic<bool> g_installed{false};

// A frame has at most two further managers (two seats, or mirror players in their columns); more raises than this
// without a player 1 update means player 1's manager stopped updating, and the hold stops instead of running on.
constexpr int kMostRaisesPerFrame = 4;

std::int64_t  g_left     = 0;      // frames of the hold still to count (player 1's updates)
bool          g_cutFrame = false;  // this frame's further managers get the cut
int           g_sinceOne = 0;      // cuts raised since player 1's last update
std::uint64_t g_raised   = 0;      // cuts raised since the start

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto setCut = platform::FindFunction(image, facts::kSetGameCameraCut);
    if (!setCut)
    {
        platform::Log(L"history cut: the game's camera cut setter differs from the research; the history-cut test is "
                      L"not available");
        return false;
    }
    g_setCut = reinterpret_cast<SetCutFn>(*setCut);
    g_installed.store(true);
    control::Register({.name  = L"history-cut",
                       .shape = control::Shape::Integer,
                       .min   = 0,
                       .max   = 600,
                       .help  = L"test: hold the game's camera cut on players 2's and 3's cameras (and the mirrors') for "
                                L"this many frames, dropping their frame history; 0 stops (render/history_cut.h)",
                       .set   = [](const control::Value& value) {
                           g_left     = std::get<std::int64_t>(value);
                           g_cutFrame = false;
                           platform::Log(L"history cut: {}", g_left > 0
                                                                 ? std::format(L"the further cameras cut for {} frames (test)", g_left)
                                                                 : std::wstring(L"stopped"));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_left}; }});
    control::Register({.name  = L"history-cut.raised",
                       .shape = control::Shape::Integer,
                       .max   = INT32_MAX,
                       .help  = L"camera cuts the history-cut test raised on further cameras since the start",
                       .get   = [] { return control::Value{static_cast<std::int64_t>(g_raised)}; }});
    platform::Log(L"history cut: installed (command history-cut <frames> tests dropping the further views' history)");
    return true;
}

void Remove() noexcept
{
    g_installed.store(false);
    g_left     = 0;
    g_cutFrame = false;
}

void AfterCameraUpdate(void* manager, bool playerOne) noexcept
{
    if (!g_installed.load(std::memory_order_relaxed) || manager == nullptr)
    {
        return;
    }
    if (playerOne)
    {
        // One frame of the hold per player 1 update; the managers updated after it this frame take the cut.
        const bool was = g_cutFrame;
        g_cutFrame     = g_left > 0;
        g_sinceOne     = 0;
        if (g_cutFrame)
        {
            --g_left;
        }
        if (was && !g_cutFrame)
        {
            platform::Log(L"history cut: the hold ended ({} cuts raised so far)", g_raised);
        }
        return;
    }
    if (!g_cutFrame)
    {
        return;
    }
    if (++g_sinceOne > kMostRaisesPerFrame)
    {
        g_cutFrame = false;
        g_left     = 0;
        platform::Log(L"history cut: player 1's camera stopped updating; the hold stopped ({} cuts raised so far)", g_raised);
        return;
    }
    if (platform::CallGuarded(g_setCut, manager))
    {
        ++g_raised;
        platform::LogTrace(L"history cut: cut raised on manager {}", manager);
    }
}

}  // namespace lss::render::history_cut
