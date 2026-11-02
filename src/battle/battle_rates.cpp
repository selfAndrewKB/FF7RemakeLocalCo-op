#include "battle/battle_rates.h"

#include "battle/battle_rates_rules.h"
#include "battle/enemy_coin.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>

namespace lss::battle::battle_rates {

namespace {

namespace rules    = battle_rates_rules;
namespace settings = platform::settings;

using platform::Read;
using platform::ReadPointer;

using ComputeFn     = std::uint64_t (*)(void* estimate, void* param, std::int32_t* hp, std::int32_t* bp, void* pa, void* ma,
                                    void* pd, void* md);
using OverrideFn    = std::uint64_t (*)(void* component, std::uint64_t row);
using ClampFn       = void (*)(std::int32_t* damage, void* attacker, void* victim, const std::uint8_t* source, std::uint8_t heal);
using AwardFn       = std::uint64_t (*)(void* result);
using ClearedFn     = char (*)();
using DifficultyFn  = std::int32_t (*)();
using IsPartySideFn = bool (*)(void* component);
using FindRowFn     = std::uint8_t* (*)(const void* tables, std::uint64_t name);

/// The game's functions and places, found at install.
struct Game
{
    ComputeFn      compute     = nullptr;
    ClampFn        clamp       = nullptr;
    AwardFn        award       = nullptr;
    ClearedFn      cleared     = nullptr;
    DifficultyFn   difficulty  = nullptr;
    IsPartySideFn  isPartySide = nullptr;
    FindRowFn      findRow     = nullptr;
    void* const*   tables      = nullptr;  // the ResidentParameter tables' three slots
    std::uint8_t** save        = nullptr;  // &the save data singleton
    std::uintptr_t expName = 0, apName = 0, easyName = 0, hardName = 0;  // the rows' FName globals (live addresses)
};
Game g;

platform::EntryRedirect g_override;
platform::CallRedirect  g_startCompute, g_overrideCompute, g_award, g_awardCleared, g_clamp, g_clampB;
bool                    g_statsPlaced = false, g_damagePlaced = false, g_awardPlaced = false;

/// The rates in tenths (10: the game's own). Set on the game thread, read on any.
struct Rates
{
    std::atomic<int> damage{rules::kOne}, hp{rules::kOne}, boss{rules::kOne}, stagger{rules::kOne}, exp{rules::kOne},
        ap{rules::kOne};
};
Rates g_rates;

thread_local void* t_overriding = nullptr;  // the component whose phase change runs on this thread
thread_local bool  t_awardOpen  = false;    // the award call runs with the mod's rows: its bonus gate answers open

/// One of the game's Easy/Hard damage rate rows: where it is (the resident table's row) and its own value.
struct RateRow
{
    const wchar_t* what;
    std::uintptr_t name  = 0;
    float*         value = nullptr;
    float          stock = 0.0f;
    float          written = 0.0f;
};
std::array<RateRow, 2> g_takeDamage{{{L"Easy"}, {L"Hard"}}};
bool                   g_rowsWanted = true;  // sync the rows on the next tick
unsigned               g_rowTicks   = 0;
bool                   g_rowOverrideSaid = false;

// --- HP and the stagger gauge --------------------------------------------------------------------------------------

/// The game's computed HP and gauge for `component`, scaled before the game sets them.
void ScaleStats(void* component, std::int32_t* hp, std::int32_t* bp, const wchar_t* when) noexcept
{
    bool party = true;
    if (component == nullptr || !platform::CallGuarded(g.isPartySide, party, component) || party)
    {
        return;  // a summon or an ally: as the game made it
    }
    std::uint8_t      kind = 0;
    const auto* const spec = static_cast<const std::uint8_t*>(ReadPointer(component, facts::kBattleCompBaseSpec));
    if (spec != nullptr)
    {
        Read(spec + facts::kCharaSpecKind, kind);
    }
    const bool boss    = kind == facts::kCharaSpecKindLeader;
    const int  hpRate  = (boss ? g_rates.boss : g_rates.hp).load(std::memory_order_relaxed);
    const int  bpRate  = g_rates.stagger.load(std::memory_order_relaxed);
    const std::int32_t hpWas = *hp, bpWas = *bp;
    *hp = rules::Scaled(hpWas, hpRate);
    *bp = rules::Scaled(bpWas, bpRate);
    if (hpRate != rules::kOne || bpRate != rules::kOne)
    {
        platform::LogDebug(L"battle rates: {} {:p} at {}: max HP {} to {} (x{}), stagger gauge {} to {} (x{})",
                           boss ? L"the boss" : L"an enemy", component, when, hpWas, *hp, rules::WordOf(hpRate), bpWas, *bp,
                           rules::WordOf(bpRate));
    }
}

/// The two enemy calls of the game's stat computation (battle start, phase change).
std::uint64_t Compute(void* estimate, void* param, std::int32_t* hp, std::int32_t* bp, void* pa, void* ma, void* pd, void* md)
{
    const std::uint64_t result = g.compute(estimate, param, hp, bp, pa, ma, pd, md);
    if (hp != nullptr && bp != nullptr)
    {
        if (t_overriding != nullptr)
        {
            ScaleStats(t_overriding, hp, bp, L"a phase change");
        }
        else if (void* const starting = enemy_coin::StartingComponent())
        {
            ScaleStats(starting, hp, bp, L"its battle start");
        }
    }
    return result;
}

/// The game's phase change: the component named for the computation inside it.
std::uint64_t Override(void* component, std::uint64_t row)
{
    void* const previous = t_overriding;
    t_overriding         = component;
    const std::uint64_t result = reinterpret_cast<OverrideFn>(g_override.Original())(component, row);
    t_overriding = previous;
    return result;
}

// --- Enemy damage --------------------------------------------------------------------------------------------------

bool NormalDifficulty() noexcept
{
    std::int32_t difficulty = -1;
    return platform::CallGuarded(g.difficulty, difficulty) && difficulty == facts::kDifficultyNormal;
}

/// The clamp after the game's per-hit step: on Normal, the step the game has on Easy and Hard, with the mod's rate.
void Clamp(std::int32_t* damage, void* attacker, void* victim, const std::uint8_t* source, std::uint8_t heal)
{
    const int rate = g_rates.damage.load(std::memory_order_relaxed);
    if (rate != rules::kOne && damage != nullptr && *damage > 0 && heal == 0 && attacker != nullptr && victim != nullptr &&
        source != nullptr)
    {
        void* const   from      = ReadPointer(attacker, facts::kCharacterBattleComponent);
        void* const   to        = ReadPointer(victim, facts::kCharacterBattleComponent);
        bool          fromParty = true, toParty = false;
        std::uint8_t  type = 0, power = 0;
        if (from != nullptr && to != nullptr && platform::CallGuarded(g.isPartySide, fromParty, from) && !fromParty &&
            platform::CallGuarded(g.isPartySide, toParty, to) && toParty && Read(source + facts::kDamageSourceType, type) &&
            Read(source + facts::kDamageSourcePowerType, power) && rules::HitKindScales(type, power) && NormalDifficulty())
        {
            const std::int32_t was = *damage;
            *damage                = rules::Scaled(was, rate);
            platform::LogDebug(L"battle rates: an enemy's hit on the party {} to {} (x{}, Normal)", was, *damage,
                               rules::WordOf(rate));
        }
    }
    g.clamp(damage, attacker, victim, source, heal);
}

/// The resident table's row named by the FName global at `nameAt`, as the game's lookup finds it there; null if the
/// table is not loaded. Also answers the row the whole set finds first (`first`), to tell an area's own copy.
std::uint8_t* ResidentRow(std::uintptr_t nameAt, std::uint8_t** first) noexcept
{
    void*         tables[facts::kTableSlots]{};
    std::uint64_t name  = 0;
    std::uint8_t* found = nullptr;
    if (g.tables == nullptr || !Read(g.tables + facts::kTableSlotResident, tables[facts::kTableSlotResident]) ||
        tables[facts::kTableSlotResident] == nullptr || !Read(reinterpret_cast<const void*>(nameAt), name) || name == 0 ||
        !platform::CallGuarded(g.findRow, found, static_cast<const void*>(tables), name))
    {
        return nullptr;
    }
    if (first != nullptr && !platform::CallGuarded(g.findRow, *first, static_cast<const void*>(g.tables), name))
    {
        *first = nullptr;
    }
    return found;
}

/// The game's Easy and Hard rate rows (the resident table's), written as the game's value times the rate. A row found
/// at another address, or holding a value the mod did not write (its table loaded again, perhaps at the same address),
/// is taken fresh: its value is the game's.
void SyncRows() noexcept
{
    const int rate = g_rates.damage.load(std::memory_order_relaxed);
    for (RateRow& row : g_takeDamage)
    {
        std::uint8_t*       first = nullptr;
        std::uint8_t* const found = ResidentRow(row.name, &first);
        float               current = 0.0f;
        if (found == nullptr || !Read(found + facts::kParamRowFloat, current))
        {
            continue;
        }
        auto* const value = reinterpret_cast<float*>(found + facts::kParamRowFloat);
        if (value != row.value || current != row.written)
        {
            row.value   = value;
            row.stock   = current;
            row.written = current;
            platform::LogDebug(L"battle rates: the game's {} damage rate row found ({:.2f})", row.what, current);
        }
        const float wanted = rules::ScaledRate(row.stock, rate);
        if (wanted != row.written && platform::WriteGuarded(row.value, &wanted, sizeof(wanted)))
        {
            row.written = wanted;
            platform::LogDebug(L"battle rates: the game's {} damage rate {:.2f} (its own {:.2f} x{})", row.what, wanted, row.stock,
                               rules::WordOf(rate));
        }
        if (!g_rowOverrideSaid && first != nullptr && first != found)
        {
            g_rowOverrideSaid = true;
            platform::LogWarning(L"battle rates: this area's own table carries the game's {} damage rate; the enemy damage "
                                 L"rate does not reach it here",
                                 row.what);
        }
    }
}

/// The rows put back, each only where the game's lookup still finds it and it still holds what the mod wrote (a table
/// freed or loaded again since is left alone).
void RestoreRows() noexcept
{
    for (RateRow& row : g_takeDamage)
    {
        float current = 0.0f;
        if (row.value != nullptr && row.written != row.stock &&
            ResidentRow(row.name, nullptr) == reinterpret_cast<std::uint8_t*>(row.value) - facts::kParamRowFloat &&
            Read(row.value, current) && current == row.written)
        {
            platform::WriteGuarded(row.value, &row.stock, sizeof(row.stock));
        }
        row.value = nullptr;
    }
}

// --- EXP and AP ----------------------------------------------------------------------------------------------------

bool BoosterOn() noexcept
{
    std::uint8_t* save  = nullptr;
    std::int32_t  state = 0;
    std::uint8_t  on    = 0;
    return Read(g.save, save) && save != nullptr && Read(save + facts::kSaveDataState, state) &&
           state == facts::kSaveDataStateActive && Read(save + facts::kSaveDataActive + facts::kSaveDataBoosterExp, on) && on != 0;
}

/// The bonus gate's first test inside the award: open while the mod's rows are in.
char AwardCleared()
{
    return t_awardOpen ? char{1} : g.cleared();
}

/// The battle's award, with the bonus rows times the rates for this one call. A fault inside the game's award ends the
/// game as it would without the mod; nothing may catch it here without first putting the rows back.
std::uint64_t Award(void* result)
{
    const int exp = g_rates.exp.load(std::memory_order_relaxed);
    const int ap  = g_rates.ap.load(std::memory_order_relaxed);
    if (exp == rules::kOne && ap == rules::kOne)
    {
        return g.award(result);
    }
    std::uint64_t expName = 0, apName = 0;
    std::uint8_t *expRow = nullptr, *apRow = nullptr;
    char          cleared = 0;
    std::int32_t  expStock = 0, apStock = 0;
    if (!Read(reinterpret_cast<const void*>(g.expName), expName) || !Read(reinterpret_cast<const void*>(g.apName), apName) ||
        !platform::CallGuarded(g.findRow, expRow, static_cast<const void*>(g.tables), expName) || expRow == nullptr ||
        !platform::CallGuarded(g.findRow, apRow, static_cast<const void*>(g.tables), apName) || apRow == nullptr ||
        !Read(expRow + facts::kParamRowInt, expStock) || !Read(apRow + facts::kParamRowInt, apStock) ||
        !platform::CallGuarded(g.cleared, cleared))
    {
        platform::LogWarning(L"battle rates: the game's EXP and AP rate rows could not be read; this battle's award is the game's");
        return g.award(result);
    }
    const bool         open       = cleared != 0 || BoosterOn();
    const std::int32_t expPercent = rules::AwardPercent(expStock, open, exp);
    const std::int32_t apPercent  = rules::AwardPercent(apStock, open, ap);
    if (!platform::WriteGuarded(expRow + facts::kParamRowInt, &expPercent, sizeof(expPercent)) ||
        !platform::WriteGuarded(apRow + facts::kParamRowInt, &apPercent, sizeof(apPercent)))
    {
        platform::WriteGuarded(expRow + facts::kParamRowInt, &expStock, sizeof(expStock));
        platform::LogWarning(L"battle rates: the game's EXP and AP rate rows could not be written; this battle's award is the game's");
        return g.award(result);
    }
    platform::LogDebug(L"battle rates: this battle's award at EXP {} % and AP {} % (the game's {}; EXP x{}, AP x{})", expPercent,
                       apPercent, open ? L"bonus on: its rows' 200 % and 300 %" : L"bonus off: 100 %", rules::WordOf(exp),
                       rules::WordOf(ap));
    t_awardOpen                 = true;
    const std::uint64_t awarded = g.award(result);
    t_awardOpen                 = false;
    platform::WriteGuarded(expRow + facts::kParamRowInt, &expStock, sizeof(expStock));
    platform::WriteGuarded(apRow + facts::kParamRowInt, &apStock, sizeof(apStock));
    return awarded;
}

// --- The options ---------------------------------------------------------------------------------------------------

struct OptionRate
{
    const settings::Option* option;
    std::atomic<int>*       rate;
    const wchar_t*          what;
};
const std::array<OptionRate, 6> kOptionRates{{
    {&settings::kEnemyDamage, &g_rates.damage, L"enemy damage"},
    {&settings::kEnemyHp, &g_rates.hp, L"enemy HP"},
    {&settings::kBossHp, &g_rates.boss, L"boss HP"},
    {&settings::kEnemyStagger, &g_rates.stagger, L"enemy stagger gauge"},
    {&settings::kExpGain, &g_rates.exp, L"EXP"},
    {&settings::kApGain, &g_rates.ap, L"AP"},
}};

/// A rate set from the console, the page or the keys (game thread).
void SetRate(const OptionRate& entry, int tenths, const wchar_t* from) noexcept
{
    const int was = entry.rate->exchange(tenths, std::memory_order_relaxed);
    if (was == tenths)
    {
        return;
    }
    platform::Log(L"battle rates: {} x{} (was x{}; {})", entry.what, rules::WordOf(tenths), rules::WordOf(was), from);
    if (entry.rate == &g_rates.damage)
    {
        g_rowsWanted = true;
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    namespace f = facts;
    const auto compute     = platform::FindFunction(image, f::kEnemyComputeStats);
    const auto override    = platform::FindFunction(image, f::kSpecOverride);
    const auto clamp       = platform::FindFunction(image, f::kDamageClamp);
    const auto difficulty  = platform::FindFunction(image, f::kDifficulty);
    const auto award       = platform::FindFunction(image, f::kAward);
    const auto cleared     = platform::FindFunction(image, f::kStoryCleared);
    const auto isPartySide = platform::FindFunction(image, f::kBattleCompIsPartySide);
    const auto findRow     = platform::FindFunction(image, f::kFindRowInTables);
    const auto startSite   = image.Rebase(f::kEnemyComputeStatsStartCall, 5);
    const auto overSite    = image.Rebase(f::kEnemyComputeStatsOverrideCall, 5);
    const auto clampSite   = image.Rebase(f::kDamageClampCall, 5);
    const auto clampSiteB  = image.Rebase(f::kDamageClampCallB, 5);
    const auto awardSite   = image.Rebase(f::kAwardCall, 5);
    const auto clearedSite = image.Rebase(f::kAwardStoryClearedCall, 5);
    const auto tables      = image.Rebase(f::kResidentParameterTables, f::kTableSlots * sizeof(void*));
    const auto save        = image.Rebase(f::kSaveDataGlobal, sizeof(void*));
    const auto expName     = image.Rebase(f::kNameExpRateGameClear, 8);
    const auto apName      = image.Rebase(f::kNameApRateGameClear, 8);
    const auto easyName    = image.Rebase(f::kNameEasyTakeDamage, 8);
    const auto hardName    = image.Rebase(f::kNameHardTakeDamage, 8);
    if (!isPartySide || !findRow || !tables)
    {
        platform::LogWarning(L"battle rates: the game's side test or table lookup is not what the research read; every rate "
                             L"stays the game's");
        return false;
    }
    g.isPartySide = reinterpret_cast<IsPartySideFn>(*isPartySide);
    g.findRow     = reinterpret_cast<FindRowFn>(*findRow);
    g.tables      = reinterpret_cast<void* const*>(*tables);

    // HP and the stagger gauge: the phase change named, then both enemy calls of the computation.
    // The battle start's enemy comes from the coin's wrapper of that start (one slot, one replacement).
    if (compute && override && startSite && overSite && enemy_coin::StartWatched())
    {
        g.compute     = reinterpret_cast<ComputeFn>(*compute);
        g_statsPlaced = g_override.Install(*override, std::span(f::kSpecOverrideEntry.data(), f::kSpecOverrideFirstBytes),
                                           reinterpret_cast<std::uintptr_t>(&Override)) &&
                        g_startCompute.Install(*startSite, *compute, reinterpret_cast<std::uintptr_t>(&Compute)) &&
                        g_overrideCompute.Install(*overSite, *compute, reinterpret_cast<std::uintptr_t>(&Compute));
        if (!g_statsPlaced)
        {
            g_overrideCompute.Restore();
            g_startCompute.Restore();
            g_override.Restore();
        }
    }
    // Enemy damage: the clamp's two calls (the first crosses a block: placed now, before any battle) and the game's
    // Easy and Hard rows.
    if (clamp && difficulty && clampSite && clampSiteB && easyName && hardName)
    {
        g.clamp                 = reinterpret_cast<ClampFn>(*clamp);
        g.difficulty            = reinterpret_cast<DifficultyFn>(*difficulty);
        g_takeDamage[0].name    = *easyName;
        g_takeDamage[1].name    = *hardName;
        g_damagePlaced = g_clampB.Install(*clampSiteB, *clamp, reinterpret_cast<std::uintptr_t>(&Clamp)) &&
                         g_clamp.InstallFromOwnThread(*clampSite, *clamp, reinterpret_cast<std::uintptr_t>(&Clamp));
        if (!g_damagePlaced)
        {
            g_clamp.Restore();
            g_clampB.Restore();
        }
    }
    // EXP and AP: the award's one call, and its bonus gate's first test.
    if (award && cleared && awardSite && clearedSite && save && expName && apName)
    {
        g.award        = reinterpret_cast<AwardFn>(*award);
        g.cleared      = reinterpret_cast<ClearedFn>(*cleared);
        g.save         = reinterpret_cast<std::uint8_t**>(*save);
        g.expName      = *expName;
        g.apName       = *apName;
        g_awardPlaced  = g_awardCleared.Install(*clearedSite, *cleared, reinterpret_cast<std::uintptr_t>(&AwardCleared)) &&
                         g_award.Install(*awardSite, *award, reinterpret_cast<std::uintptr_t>(&Award));
        if (!g_awardPlaced)
        {
            g_award.Restore();
            g_awardCleared.Restore();
        }
    }

    // The options are registered even when a lever is not placed: the page and the file keep the player's choice, and
    // the line below says which rates stay the game's.
    std::wstring readings;
    for (const OptionRate& entry : kOptionRates)
    {
        const int tenths = rules::TenthsOf(settings::Of(*entry.option).word).value_or(rules::kOne);
        entry.rate->store(tenths, std::memory_order_relaxed);
        readings += std::wstring(readings.empty() ? L"" : L", ") + entry.what + L" x" + rules::WordOf(tenths) + L" (" +
                    settings::Why(*entry.option) + L")";
        control::RegisterOption(
            *entry.option,
            [&entry](const std::wstring& word) {
                if (const auto tenths = rules::TenthsOf(word))
                {
                    SetRate(entry, *tenths, L"the console");
                }
                return control::Outcome{};
            },
            [&entry] { return std::wstring(rules::WordOf(entry.rate->load(std::memory_order_relaxed))); });
    }
    platform::Log(L"battle rates: {}; HP and stagger {}, enemy damage {}, EXP and AP {}", readings,
                  g_statsPlaced ? L"placed" : L"NOT placed (the game's)", g_damagePlaced ? L"placed" : L"NOT placed (the game's)",
                  g_awardPlaced ? L"placed" : L"NOT placed (the game's)");
    return g_statsPlaced || g_damagePlaced || g_awardPlaced;
}
catch (...)
{
    return false;
}

void Remove() noexcept
{
    g_award.Restore();
    g_awardCleared.Restore();
    g_clampB.Restore();
    g_clamp.Restore();
    g_overrideCompute.Restore();
    g_startCompute.Restore();
    g_override.Restore();
    RestoreRows();
}

void Tick() noexcept
{
    if (g_damagePlaced && (g_rowsWanted || ++g_rowTicks >= 60))
    {
        g_rowsWanted = false;
        g_rowTicks   = 0;
        SyncRows();
    }
}

}  // namespace lss::battle::battle_rates
