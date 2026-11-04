#include "world/area_levels.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "players/game_state.h"
#include "players/seats.h"
#include "world/area_levels_rules.h"
#include "world/env_sets.h"

#include <Unreal/NameTypes.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <span>
#include <string>

namespace lss::world::area_levels {

namespace {

namespace rules = area_levels_rules;
using platform::Read;
using players::kSeatsBuilt;
using players::SeatName;

using ListRequestFn = std::int32_t (*)(std::uint8_t slot, std::int32_t priority, void* names, void* hidden);

/// A TArray as the engine lays it out; the request reads data and num, and sorts the names in place.
template <typename T>
struct EngineArray
{
    T*           data = nullptr;
    std::int32_t num  = 0;
    std::int32_t max  = 0;
};
static_assert(sizeof(EngineArray<std::uint64_t>) == 0x10 && sizeof(EngineArray<std::uint8_t>) == 0x10);

constexpr int kSeats = players::scopes::kMaxPlayers;

platform::CallRedirect g_listCall;  // crosses a block: placed by Tick
std::uintptr_t         g_listSite   = 0;
std::uintptr_t         g_listTarget = 0;
bool                   g_listPlaced = false;
ListRequestFn          g_request    = nullptr;  // the game's function: the requests the mod asks for itself
std::uint8_t**         g_manager    = nullptr;  // &the EnvironmentSet manager (its hold mode)

constexpr std::int32_t kSteadyFieldStep = 13;  // the field step machine's steady step (facts::kFieldRootStep)

std::atomic<bool> g_on{true};  // the switch; Remove turns it off from any thread
bool              g_dirty     = false;  // a request is owed (the switch flipped, or a change came while the game held)
bool              g_wasSteady = false;  // the field was at its steady step last engine tick

// Game thread only: the manager's tick (the game's request and env_sets' read) and the console.

/// Player 1's list as the game last asked for it, with where and how it asked.
struct One
{
    std::array<std::uint64_t, rules::kMaxLevels> names{};
    std::array<std::uint8_t, rules::kMaxLevels>  hidden{};
    int           count    = 0;
    bool          seen     = false;  // the game has asked since the mod loaded
    bool          fits     = true;   // its list is within the request's room (else passed through, never merged)
    void*         world    = nullptr;
    std::uint8_t  slot     = 0;
    std::int32_t  priority = 0;
    rules::Area   Area() const noexcept { return {{names.data(), static_cast<std::size_t>(count)}, {hidden.data(), static_cast<std::size_t>(count)}}; }
};
One g_one;

// The further players' areas of the last build (env_sets' copies; [0] unused).
std::array<std::array<std::uint64_t, rules::kMaxLevels>, kSeats> g_seatNames{};
std::array<std::array<std::uint8_t, rules::kMaxLevels>, kSeats>  g_seatHidden{};
std::array<int, kSeats>                                          g_seatCount{};

rules::Request            g_last;               // the list the slot was last asked for (as handed over)
bool                      g_lastKnown = false;
rules::Request            g_sent;               // the mod's arrays the game was handed (it sorts them in place)
EngineArray<std::uint64_t> g_sentNames;
EngineArray<std::uint8_t>  g_sentHidden;
std::uint32_t g_gameRequests = 0, g_merged = 0, g_own = 0, g_held = 0, g_cuts = 0;

std::wstring NameText(std::uint64_t bits)
{
    try
    {
        return RC::Unreal::FName(static_cast<std::int64_t>(bits)).ToString();
    }
    catch (...)
    {
        return L"?";
    }
}

std::wstring Marked(std::uint64_t name, std::uint8_t hidden)
{
    return NameText(name) + (hidden == 0 ? L"" : hidden == 2 ? L" (hidden, collision kept)" : std::format(L" (hidden {})", hidden));
}

bool Holding() noexcept
{
    std::uint8_t* manager = nullptr;
    std::uint8_t  hold    = 0;
    return Read(g_manager, manager) && manager != nullptr && Read(manager + facts::kEnvSetMgrHoldMode, hold) && hold != 0;
}

/// Player 1's area alone, or with each further player's area env_sets' paced set lists.
rules::Request Build(bool withSeats) noexcept
{
    std::array<rules::Area, kSeats> areas{};
    int                             count = 0;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const auto at  = static_cast<std::size_t>(seat);
        g_seatCount[at] = withSeats ? env_sets::SeatArea(seat, g_seatNames[at], g_seatHidden[at]) : 0;
        if (g_seatCount[at] > 0)
        {
            const auto n = static_cast<std::size_t>(g_seatCount[at]);
            areas[static_cast<std::size_t>(count++)] = {{g_seatNames[at].data(), n}, {g_seatHidden[at].data(), n}};
        }
    }
    return rules::Merge(g_one.Area(), {areas.data(), static_cast<std::size_t>(count)});
}

/// The further players' areas by their sets, for the log.
std::wstring SeatsText()
{
    std::wstring text;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        text += std::format(L"{}{}'s area {} ({} levels)", seat == 1 ? L"" : L", ", SeatName(seat), env_sets::SeatSetText(seat),
                            g_seatCount[static_cast<std::size_t>(seat)]);
    }
    return text;
}

/// Each player's levels the request shows that their own area hides (they show in that player's view: the per-view
/// hides, not built yet).
std::wstring ShownText(const rules::Request& request)
{
    std::wstring text = std::format(L"player 1 {}", rules::ShownAgainst(request, g_one.Area()));
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const auto at = static_cast<std::size_t>(seat);
        const auto n  = static_cast<std::size_t>(g_seatCount[at]);
        text += std::format(L", {} {}", SeatName(seat), rules::ShownAgainst(request, {{g_seatNames[at].data(), n}, {g_seatHidden[at].data(), n}}));
    }
    return text;
}

/// The levels of `request` that `other` does not list, as text; their number in `count`.
std::wstring NotIn(const rules::Request& request, const rules::Request& other, int& count)
{
    std::wstring text;
    count = 0;
    for (int i = 0; i < request.count; ++i)
    {
        const auto at = static_cast<std::size_t>(i);
        if (rules::Find(other, request.names[at]) < 0)
        {
            text += (count++ == 0 ? L"" : L", ") + Marked(request.names[at], request.hidden[at]);
        }
    }
    return text.empty() ? std::wstring(L"none") : text;
}

/// Copies `request` into the mod's arrays the game is handed (it sorts the names there, never in `request`).
void Stage(const rules::Request& request) noexcept
{
    g_sent             = request;
    g_sentNames.data   = g_sent.names.data();
    g_sentNames.num    = g_sent.count;
    g_sentNames.max    = rules::kMaxLevels;
    g_sentHidden.data  = g_sent.hidden.data();
    g_sentHidden.num   = g_sent.count;
    g_sentHidden.max   = rules::kMaxLevels;
}

/// Player 1's list as the game hands it over (read before the call sorts it), and the Info line level_watch kept.
void TakeOne(std::uint8_t slot, std::int32_t priority, void* names, void* hidden)
{
    const std::uint64_t* data      = static_cast<const std::uint64_t*>(platform::ReadPointer(names, 0));
    const std::uint8_t*  bytes     = hidden != nullptr ? static_cast<const std::uint8_t*>(platform::ReadPointer(hidden, 0)) : nullptr;
    std::int32_t         count     = 0;
    std::int32_t         byteCount = 0;
    if (hidden != nullptr)
    {
        Read(static_cast<const std::uint8_t*>(hidden) + 8, byteCount);
    }
    g_one.count = 0;
    g_one.fits  = Read(static_cast<const std::uint8_t*>(names) + 8, count) && count >= 0 && (count == 0 || data != nullptr) &&
                 count <= rules::kMaxLevels;
    std::wstring text;
    int          hiddenCount = 0;
    for (std::int32_t i = 0; g_one.fits && i < count; ++i)
    {
        std::uint64_t name = 0;
        std::uint8_t  mark = 0;
        if (!Read(data + i, name) || (bytes != nullptr && i < byteCount && !Read(bytes + i, mark)))
        {
            g_one.fits = false;
            break;
        }
        g_one.names[static_cast<std::size_t>(i)]  = name;
        g_one.hidden[static_cast<std::size_t>(i)] = mark;
        g_one.count                               = i + 1;
        hiddenCount += mark != 0 ? 1 : 0;
        text += (i == 0 ? L"" : L", ") + Marked(name, mark);
    }
    g_one.seen     = true;
    g_one.world    = players::game_state::World();
    g_one.slot     = slot;
    g_one.priority = priority;
    ++g_gameRequests;
    platform::Log(L"area levels: player 1's area asks visibility slot {} (priority {}) for {} level(s), {} of them hidden: {}", slot,
                  priority, count, hiddenCount, text.empty() ? std::wstring(L"none") : text);
    if (!g_one.fits)
    {
        platform::Log(L"area levels: player 1's list ({} levels) is past the request's room or unreadable: passed through as the "
                      L"game made it, no further player's area added",
                      count);
    }
}

/// The game's request for player 1's area: handed the merged list when a further player's area adds to it.
std::int32_t RequestList(std::uint8_t slot, std::int32_t priority, void* names, void* hidden)
{
    const auto original   = reinterpret_cast<ListRequestFn>(g_listCall.Original());
    void*      passNames  = names;
    void*      passHidden = hidden;
    try
    {
        TakeOne(slot, priority, names, hidden);
        const bool held = Holding();
        g_held += held ? 1 : 0;
        const rules::Request next = Build(g_on.load(std::memory_order_relaxed) && g_one.fits && !held);
        if (g_one.fits && !held && (next.added > 0 || next.changed > 0))
        {
            Stage(next);
            passNames  = &g_sentNames;
            passHidden = &g_sentHidden;
            ++g_merged;
            g_cuts += next.cut > 0 ? 1 : 0;
            int          added = 0;
            const std::wstring addedText = NotIn(next, rules::Merge(g_one.Area(), {}), added);
            platform::Log(L"area levels: player 1's request goes out with the further players' areas ({}): {} levels, {} added ({}), "
                          L"{} of player 1's shown or kept solid for another player{}; levels shown that a player's own area hides: {}",
                          SeatsText(), next.count, next.added, addedText, next.changed,
                          next.cut > 0 ? std::format(L"; {} did not fit the request's {} and were left out", next.cut, rules::kMaxLevels)
                                       : std::wstring{},
                          ShownText(next));
        }
        else if (held)
        {
            platform::LogDebug(L"area levels: the game holds one set for everyone; player 1's request passed through as made");
        }
        g_last = next.added > 0 || next.changed > 0 ? next : rules::Merge(g_one.Area(), {});
        // A list made during a hold (a scene's, the field exit's empty one) is never built on: the game's own request
        // after the hold comes first.
        g_lastKnown = g_one.fits && !held;
        g_dirty     = false;
    }
    catch (...)
    {
        passNames  = names;
        passHidden = hidden;
    }
    return original(slot, priority, passNames, passHidden);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto target  = platform::FindFunction(image, facts::kLevelListRequest);
    const auto site    = image.Rebase(facts::kEnvSetLevelListCall, 5);
    const auto manager = image.Rebase(facts::kEnvSetManagerGlobal, sizeof(void*));
    if (!target || !site || !manager)
    {
        platform::LogWarning(L"area levels: the level request is not what the research read; the world follows player 1's area alone");
        return false;
    }
    g_request    = reinterpret_cast<ListRequestFn>(*target);
    g_listTarget = *target;
    g_listSite   = *site;
    g_manager    = reinterpret_cast<std::uint8_t**>(*manager);
    namespace settings = platform::settings;
    g_on.store(settings::Of(settings::kAreaLevels).word == L"on", std::memory_order_relaxed);
    control::RegisterOption(
        settings::kAreaLevels,
        [](const std::wstring& word) {
            const bool on = word == L"on";
            g_on.store(on, std::memory_order_relaxed);
            g_dirty = true;  // the next read asks for the list with or without the further players' areas
            platform::Log(L"area levels {}", on ? L"on: the further players' areas join the next request"
                                                : L"off: the next request is player 1's area alone");
            return control::Outcome{};
        },
        [] { return std::wstring(g_on.load(std::memory_order_relaxed) ? L"on" : L"off"); });
    control::Register({.name  = L"world.area-levels",
                       .shape = control::Shape::Text,
                       .help  = L"the last level request (player 1's area with the further players'), and the requests made",
                       .get   = [] { return control::Value{Reading()}; }});
    platform::Log(L"area levels: ready, {} ({}) (every player's area joins the game's level request; the redirect is placed on the "
                  L"first tick)",
                  g_on.load(std::memory_order_relaxed) ? L"on" : L"off", settings::Why(settings::kAreaLevels));
    return true;
}

void Tick() noexcept
{
    // The field's exit (the step machine leaves its steady step): player 1's last list belongs to the field being left
    // (fields are sub-levels of one world, so the world check alone does not see it). Nothing is built on it again until
    // the game's own next request (research §2, the field exit and GameMode reset rows).
    const players::game_state::Snapshot& state = players::game_state::Current();
    const bool steady = state.hasRoot && state.running && state.step == kSteadyFieldStep;
    if (g_wasSteady && !steady && g_lastKnown)
    {
        g_lastKnown = false;
        platform::LogDebug(L"area levels: the field is left (step {}); no request of the mod's until the game's own next one", state.step);
    }
    g_wasSteady = steady;
    if (g_listPlaced || g_listSite == 0)
    {
        return;
    }
    g_listPlaced = true;  // once, whatever the outcome; outside the world tick, as the crossing call needs
    if (g_listCall.InstallFromOwnThread(g_listSite, g_listTarget, reinterpret_cast<std::uintptr_t>(&RequestList)))
    {
        platform::Log(L"area levels: the level request is redirected; every player's area stays loaded");
    }
    else
    {
        platform::LogWarning(L"area levels: the level request could not be redirected; the world follows player 1's area alone");
    }
}

void SeatsRead(bool changed) noexcept
try
{
    if ((!changed && !g_dirty) || !g_listCall.IsInstalled() || !g_one.seen || !g_one.fits || !g_lastKnown)
    {
        return;
    }
    if (Holding() || players::game_state::World() != g_one.world)
    {
        g_dirty = true;  // the game's own request comes when its hold ends, or the new world's first; asked again after
        return;
    }
    g_dirty                   = false;
    const rules::Request next = Build(g_on.load(std::memory_order_relaxed));
    if (next == g_last)
    {
        return;
    }
    Stage(next);
    std::int32_t id = -1;
    // The game's own function, as its manager calls it, on the same thread. It holds the request queue's critical
    // sections inside: a fault there is caught, but leaves the queue suspect (a guarded call, not a clean failure).
    if (!platform::CallGuarded(g_request, id, g_one.slot, g_one.priority, static_cast<void*>(&g_sentNames),
                               static_cast<void*>(&g_sentHidden)))
    {
        platform::LogWarning(L"area levels: the game's level request failed; the world keeps the last list");
        return;
    }
    ++g_own;
    g_cuts += next.cut > 0 ? 1 : 0;
    int                added = 0, dropped = 0;
    const std::wstring addedText   = NotIn(next, g_last, added);
    const std::wstring droppedText = NotIn(g_last, next, dropped);
    platform::Log(L"area levels: a further player's area changed ({}): the request now holds {} levels (player 1's {}, {} added by the "
                  L"further players), {} new ({}), {} dropped ({}){}; levels shown that a player's own area hides: {}",
                  SeatsText(), next.count, g_one.count, next.added, added, addedText, dropped, droppedText,
                  next.cut > 0 ? std::format(L"; {} did not fit the request's {} and were left out", next.cut, rules::kMaxLevels)
                               : std::wstring{},
                  ShownText(next));
    g_last = next;
}
catch (...)
{
}

void Remove() noexcept
{
    g_on.store(false, std::memory_order_relaxed);  // the redirect, left in place, passes player 1's list through from here
}

std::wstring Reading() noexcept
try
{
    return std::format(L"{}; redirect {}; last request {} levels (player 1's area {}, {} added by the further players, {} of player 1's "
                       L"changed); {}; requests: the game's {} ({} with the further players' areas, {} while it held one set), "
                       L"the mod's own {}, cut {}",
                       g_on.load(std::memory_order_relaxed) ? L"on" : L"off", g_listCall.IsInstalled() ? L"placed" : L"not placed",
                       g_last.count, g_one.count, g_last.added, g_last.changed, SeatsText(), g_gameRequests, g_merged, g_held, g_own,
                       g_cuts);
}
catch (...)
{
    return L"?";
}

}  // namespace lss::world::area_levels
