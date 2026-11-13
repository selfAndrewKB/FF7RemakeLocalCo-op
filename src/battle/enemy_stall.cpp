#include "battle/enemy_stall.h"

#include "battle/enemy_coin.h"
#include "battle/enemy_watch.h"
#include "battle/stall_rules.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/steady_ms.h"
#include "platform/verified_code.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>

namespace lss::battle::enemy_stall {

namespace {

using platform::Read;
using platform::ReadPointer;
using stall_rules::Skip;

using ReadyFn       = char (*)(void* component);  // 0, 1 light, 2 heavy
using ComponentTest = bool (*)(void* component);  // BattleComp_IsHateEnemy

constexpr std::size_t  kWaits     = 32;    // enemies timed at once (a battle has a handful)
constexpr std::int64_t kReclaimMs = 5000;  // an entry whose enemy was not ready for this long may be taken over

// One enemy's ready span, keyed by its battle component. Written by the ready test (any thread) and the gate (the
// same enemy's AI thread); an entry is claimed by a compare-and-swap on its component. Two enemies racing for one stale
// entry can mix their times for one wait: at worst one re-pick early or late, never a read of a destroyed enemy (the
// gate hands its live controller and component; nothing here is dereferenced).
struct Wait
{
    std::atomic<std::uintptr_t> component{0};
    std::atomic<std::int64_t>   seenMs{0};        // the last ready answer recorded
    std::atomic<int>            ready{0};         // that answer
    std::atomic<std::int64_t>   readySinceMs{0};  // when it turned ready (0: not ready)
    std::atomic<std::int64_t>   thresholdMs{0};   // this wait's threshold, read at its first check (0: not yet)
    std::atomic<std::int64_t>   decidedMs{0};     // the last re-pick or skip in this wait (0: none)
    std::atomic<std::int64_t>   repickedMs{0};    // the last re-pick, until an action or a stall again reports it
};
std::array<Wait, kWaits> g_waits;

struct Counts
{
    std::atomic<unsigned>                                       repicks{};
    // A re-pick after an earlier one with no action between (the same wait, or a later one after a wait ended without
    // acting: a stagger, a KO, the battle's end).
    std::atomic<unsigned>                                       again{};
    std::atomic<unsigned>                                       acted{};   // a re-pick followed by an action
    std::array<std::atomic<unsigned>, stall_rules::kSkipCount> skips{};    // by reason (index 0 unused)
};
Counts g_counts;  // since the last summary
Counts g_totals;  // since install

platform::EntryRedirect g_ready;
ComponentTest           g_isHate = nullptr;  // set at install (published by g_installed), then only read
std::atomic<bool>       g_installed{false};
std::atomic<bool>       g_on{true};
std::chrono::steady_clock::time_point g_lastSummary{};  // game thread

using platform::NowMs;

void Count(std::atomic<unsigned>& since, std::atomic<unsigned>& total) noexcept
{
    since.fetch_add(1, std::memory_order_relaxed);
    total.fetch_add(1, std::memory_order_relaxed);
}

const wchar_t* SkipText(Skip skip) noexcept
{
    switch (skip)
    {
    case Skip::Unread:
        return L"a field could not be read";
    case Skip::NotHate:
        return L"not a hate enemy (a boss, summon or mine: its script picks)";
    case Skip::Locked:
        return L"its script holds its target";
    case Skip::ForcedList:
        return L"its hate list is forced";
    case Skip::Blocked:
        return L"its script blocks its requests";
    case Skip::GivesUpItself:
        return L"it gives up on a far target itself (LookChange)";
    case Skip::NotAHuman:
        return L"its target is no human's member";
    case Skip::TargetDown:
        return L"its target is down (the game picks again itself)";
    case Skip::Alone:
        return L"no other human stands";
    case Skip::None:
        break;
    }
    return L"none";
}

Wait* Find(const void* component) noexcept
{
    const auto key = reinterpret_cast<std::uintptr_t>(component);
    for (auto& w : g_waits)
    {
        if (key != 0 && w.component.load(std::memory_order_relaxed) == key)
        {
            return &w;
        }
    }
    return nullptr;
}

/// The entry for `component`, claimed if it has none (an empty entry, or one not written for kReclaimMs); null when
/// every entry is another enemy's.
Wait* Claim(const void* component, std::int64_t now) noexcept
{
    if (Wait* w = Find(component); w != nullptr)
    {
        return w;
    }
    const auto key = reinterpret_cast<std::uintptr_t>(component);
    for (auto& w : g_waits)
    {
        std::uintptr_t seen = w.component.load(std::memory_order_relaxed);
        if (seen != 0 && now - w.seenMs.load(std::memory_order_relaxed) <= kReclaimMs)
        {
            continue;
        }
        if (w.component.compare_exchange_strong(seen, key, std::memory_order_relaxed))
        {
            w.ready.store(0, std::memory_order_relaxed);
            w.readySinceMs.store(0, std::memory_order_relaxed);
            w.thresholdMs.store(0, std::memory_order_relaxed);
            w.decidedMs.store(0, std::memory_order_relaxed);
            w.repickedMs.store(0, std::memory_order_relaxed);
            w.seenMs.store(now, std::memory_order_relaxed);
            return &w;
        }
        if (seen == key)
        {
            return &w;  // the same enemy claimed it on another thread meanwhile
        }
    }
    return nullptr;
}

/// The ready span: begins at the first ready answer, ends (and with it the wait's decisions) at the first 0.
void Record(const void* component, char ready) noexcept
{
    const std::int64_t now = NowMs();
    Wait* const        w   = ready != 0 ? Claim(component, now) : Find(component);
    if (w == nullptr)
    {
        return;
    }
    if (ready == 0)
    {
        w->readySinceMs.store(0, std::memory_order_relaxed);
        w->thresholdMs.store(0, std::memory_order_relaxed);
        w->decidedMs.store(0, std::memory_order_relaxed);
    }
    else if (w->ready.load(std::memory_order_relaxed) == 0 || w->readySinceMs.load(std::memory_order_relaxed) == 0)
    {
        w->readySinceMs.store(now, std::memory_order_relaxed);
    }
    w->ready.store(ready, std::memory_order_relaxed);
    w->seenMs.store(now, std::memory_order_relaxed);
}

/// The enemy component's attack-ready test: the game's answer unchanged, timed here and handed to the watch.
char AttackReady(void* component)
{
    const char ready = reinterpret_cast<ReadyFn>(g_ready.Original())(component);
    if (g_on.load(std::memory_order_relaxed) && enemy_coin::Active())
    {
        Record(component, ready);
    }
    enemy_watch::Ready(component, ready);
    return ready;
}

/// What the rule reads about a stalled enemy: its controller's and component's fields and its target's seat.
stall_rules::Facts ReadFacts(void* controller, void* component, int& seat) noexcept
{
    stall_rules::Facts f;
    const auto*        aic    = static_cast<const std::uint8_t*>(controller);
    const auto*        comp   = static_cast<const std::uint8_t*>(component);
    std::uint32_t      lock   = 0;
    std::int32_t       forced = 0;
    std::uint8_t       block  = 0;
    std::uint32_t      mode   = 0;
    const auto*        flags  = static_cast<const std::uint8_t*>(ReadPointer(aic, facts::kBattleAIRequestFlags));
    const auto*        values = static_cast<const std::uint8_t*>(ReadPointer(aic, facts::kBattleAILookChangeValues));
    f.read = platform::CallGuarded(g_isHate, f.hate, component) && Read(comp + facts::kBattleCompTargetLock, lock) &&
             Read(aic + facts::kBattleAIForcedHateCount, forced) && flags != nullptr && Read(flags + facts::kBattleAIRequestsBlocked, block) &&
             (values == nullptr || Read(values + facts::kBattleAILookChangeMode, mode));  // the give-up tick reads it unchecked
    f.locked        = lock != 0;
    f.forcedList    = forced > 0;
    f.blocked       = block != 0;
    f.givesUpItself = mode != 0;
    seat            = enemy_coin::HumanSeat(enemy_watch::TargetCharacter(component));
    f.targetSeat    = seat;
    f.targetStanding = seat >= 0 && enemy_coin::HumanStands(seat);
    f.otherStanding  = seat >= 0 && enemy_coin::OtherHumanStands(seat);
    return f;
}

void LogRepick(void* controller, void* component, int ready, double waitedS, double thresholdS, int seat, bool again) noexcept
try
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    platform::LogDebug(L"enemy stall: enemy {:#x} ready ({}) {:.1f} s without acting (its wait {:.1f} s){}; the game's hate update "
                       L"picks again, player {} set aside: {}",
                       reinterpret_cast<std::uintptr_t>(controller), ready == 2 ? L"heavy" : L"light", waitedS, thresholdS,
                       again ? L", again after its last re-pick" : L"", seat + 1, enemy_watch::TargetAndDistances(component));
}
catch (...)
{
    // A failed log line (an allocation) never unwinds into the game's frames.
}

void LogActed(void* controller, void* component, double afterS) noexcept
try
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    platform::LogDebug(L"enemy stall: enemy {:#x} acted {:.1f} s after its re-pick, on {}", reinterpret_cast<std::uintptr_t>(controller),
                       afterS, enemy_watch::TargetAndDistances(component));
}
catch (...)
{
}

void LogSkip(void* controller, void* component, double waitedS, Skip skip) noexcept
try
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    platform::LogDebug(L"enemy stall: enemy {:#x} ready {:.1f} s without acting, left to the game: {}; {}",
                       reinterpret_cast<std::uintptr_t>(controller), waitedS, SkipText(skip), enemy_watch::TargetAndDistances(component));
}
catch (...)
{
}

std::wstring Reading(const Counts& c) noexcept
try
{
    std::wstring text = std::format(L"{} re-pick(s), {} followed by an action, {} after an earlier re-pick with no action between",
                                    c.repicks.load(std::memory_order_relaxed), c.acted.load(std::memory_order_relaxed),
                                    c.again.load(std::memory_order_relaxed));
    for (int i = 1; i < stall_rules::kSkipCount; ++i)
    {
        if (const unsigned n = c.skips[static_cast<std::size_t>(i)].load(std::memory_order_relaxed); n != 0)
        {
            text += std::format(L"; left to the game {} time(s): {}", n, SkipText(static_cast<Skip>(i)));
        }
    }
    return text;
}
catch (...)
{
    return L"unreadable";
}

bool Any(const Counts& c) noexcept
{
    if (c.repicks.load(std::memory_order_relaxed) != 0 || c.acted.load(std::memory_order_relaxed) != 0)
    {
        return true;
    }
    for (const auto& n : c.skips)
    {
        if (n.load(std::memory_order_relaxed) != 0)
        {
            return true;
        }
    }
    return false;
}

void ResetSince() noexcept
{
    g_counts.repicks.store(0, std::memory_order_relaxed);
    g_counts.again.store(0, std::memory_order_relaxed);
    g_counts.acted.store(0, std::memory_order_relaxed);
    for (auto& n : g_counts.skips)
    {
        n.store(0, std::memory_order_relaxed);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto ready  = platform::FindFunction(image, facts::kBattleEnemyAttackReady);
    const auto isHate = platform::FindFunction(image, facts::kBattleCompIsHateEnemy);
    if (!ready || !isHate)
    {
        platform::LogWarning(L"enemy stall: the attack-ready test or the hate-enemy test differs from the research; stalled enemies are "
                             L"left to the game (and ready enemies are not logged)");
        return false;
    }
    g_isHate = reinterpret_cast<ComponentTest>(*isHate);
    if (!g_ready.Install(*ready, facts::kBattleEnemyAttackReadyFirst, reinterpret_cast<std::uintptr_t>(&AttackReady)))
    {
        platform::LogWarning(L"enemy stall: the attack-ready test could not be redirected; stalled enemies are left to the game (and ready "
                             L"enemies are not logged)");
        return false;
    }
    control::Register({.name  = L"enemy-stall",
                       .shape = control::Shape::OnOff,
                       .help  = L"an enemy ready but not attacking its far human target picks again by the game's own hate update "
                                L"(battle/enemy_stall.h; on)",
                       .set   = [](const control::Value& value) {
                           g_on.store(std::get<bool>(value), std::memory_order_relaxed);
                           platform::Log(L"enemy stall {}", std::get<bool>(value) ? L"on: a stalled enemy picks again"
                                                                                   : L"off: a stalled enemy waits, as before");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_on.load(std::memory_order_relaxed)}; }});
    control::Register({.name  = L"enemy-stall.counts",
                       .shape = control::Shape::Text,
                       .help  = L"the re-picks since the start, their outcomes, and the stalls left to the game by reason",
                       .get   = [] { return control::Value{Reading(g_totals)}; }});
    g_installed.store(true, std::memory_order_release);
    platform::Log(L"enemy stall: installed; in co-op an enemy ready for two of its own attack intervals without attacking its "
                  L"human target picks again through the game's hate update");
    return true;
}

void Remove() noexcept
{
    g_on.store(false, std::memory_order_relaxed);
    g_ready.Restore();
}

void BeforeGate(void* controller, void* component, std::uint64_t action) noexcept
{
    if (!g_installed.load(std::memory_order_acquire) || !g_on.load(std::memory_order_relaxed) || controller == nullptr)
    {
        return;
    }
    Wait* const w = Find(component);
    if (w == nullptr)
    {
        return;
    }
    const std::int64_t now = NowMs();
    if (action != 0)
    {
        if (const std::int64_t repicked = w->repickedMs.exchange(0, std::memory_order_relaxed); repicked != 0)
        {
            Count(g_counts.acted, g_totals.acted);
            LogActed(controller, component, static_cast<double>(now - repicked) / 1000.0);
        }
        return;
    }
    const std::int64_t since = w->readySinceMs.load(std::memory_order_relaxed);
    if (since == 0)
    {
        return;
    }
    std::int64_t threshold = w->thresholdMs.load(std::memory_order_relaxed);
    if (threshold == 0)
    {
        float interval = 0.0f;
        Read(static_cast<const std::uint8_t*>(controller) + facts::kBattleAILightInterval, interval);
        threshold = stall_rules::ThresholdMs(interval);  // an unread interval gives the floor
        w->thresholdMs.store(threshold, std::memory_order_relaxed);
    }
    if (!stall_rules::Due(now, since, w->decidedMs.load(std::memory_order_relaxed), threshold))
    {
        return;
    }
    w->decidedMs.store(now, std::memory_order_relaxed);
    int        seat = -1;
    const Skip skip = stall_rules::Judge(ReadFacts(controller, component, seat));
    const double waited = static_cast<double>(now - since) / 1000.0;
    if (skip != Skip::None)
    {
        Count(g_counts.skips[static_cast<std::size_t>(skip)], g_totals.skips[static_cast<std::size_t>(skip)]);
        LogSkip(controller, component, waited, skip);
        return;
    }
    const std::uint8_t pending = 1;  // Blueprint SetHateUpdateFlag's one write; the gate's update reads it next
    if (!platform::WriteGuarded(static_cast<std::uint8_t*>(controller) + facts::kBattleAIHateUpdatePending, &pending, sizeof(pending)))
    {
        Count(g_counts.skips[static_cast<std::size_t>(Skip::Unread)], g_totals.skips[static_cast<std::size_t>(Skip::Unread)]);
        return;
    }
    const bool again = w->repickedMs.exchange(now, std::memory_order_relaxed) != 0;
    Count(g_counts.repicks, g_totals.repicks);
    if (again)
    {
        Count(g_counts.again, g_totals.again);
    }
    LogRepick(controller, component, w->ready.load(std::memory_order_relaxed), waited, static_cast<double>(threshold) / 1000.0, seat, again);
}

void Tick() noexcept
{
    if (!g_installed.load(std::memory_order_acquire))
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastSummary >= std::chrono::seconds(60))
    {
        g_lastSummary = now;
        if (Any(g_counts))
        {
            platform::LogDebug(L"enemy stall (60 s): {}", Reading(g_counts));
        }
        ResetSince();
    }
}

}  // namespace lss::battle::enemy_stall
