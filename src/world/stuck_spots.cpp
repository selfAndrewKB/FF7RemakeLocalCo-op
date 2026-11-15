#include "world/stuck_spots.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/game_array.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "platform/name_text.h"
#include "platform/steady_ms.h"
#include "players/field_root.h"
#include "players/game_state.h"
#include "world/data_rows.h"
#include "world/state_signal.h"
#include "world/state_slots.h"
#include "world/stuck_spot_rules.h"
#include "world/traversal_action.h"
#include "world/traversal_walkup.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lss::world::stuck_spots {

namespace {

using platform::Read;
using RC::Unreal::UClass;
using RC::Unreal::UObject;
namespace rules          = stuck_spot_rules;
using rules::Name;

// A field turned steady sends its own queued triggers and fades the party in first.
constexpr std::int64_t kSettleMs    = 3000;
// A held pass is sent only when a second scan this long after the first finds it the same, nobody busy at either: a
// pass under way (player 1's, a script's) has ended by then and no longer holds anything.
constexpr std::int64_t kConfirmMs   = 10000;
// How often the spot map is counted for spots streamed in since the last scan.
constexpr std::int64_t kSpotCheckMs = 2000;
// A sent pass's moves are read back this long after the send (the state manager fires it on its next tick).
constexpr std::int64_t kReadBackMs  = 500;

UClass* g_spotClass     = nullptr;  // the field-action spot class, looked up when first needed
bool    g_spotClassTold = false;
bool    g_installed     = false;
bool    g_auto          = true;

/// An active state waiting on one of a spot's ends: its slot, object, state and row, and where that end leads it
/// (nullopt: the row's next list could not be read there).
struct Waiter
{
    const std::uint8_t*         slot   = nullptr;
    std::int64_t                object = 0, state = 0;
    const std::uint8_t*         row    = nullptr;
    std::optional<std::wstring> next;
};

/// A move a pass's end makes on a state it holds: the slot, its object, and its state now and after.
struct Move
{
    const std::uint8_t* slot   = nullptr;
    std::int64_t        object = 0;
    std::wstring        from, to;

    bool operator==(const Move&) const = default;
};

/// One held pass from a scan.
struct Verdict
{
    std::wstring      spot;
    bool              left = false;
    rules::Plan       plan;
    std::vector<Move> moves;

    std::wstring Key() const { return spot + (left ? L" left" : L" right"); }
    bool         Same(const Verdict& o) const { return plan.send == o.plan.send && moves == o.moves; }
};

/// An accepted pass seen by a scan, waiting for the scan that confirms it.
struct Candidate
{
    Verdict      verdict;
    std::int64_t since = 0;
};

/// A sent pass awaiting its read-back.
struct Pending
{
    std::wstring      key;
    std::vector<Move> moves;
    std::int64_t      at = 0;
};

/// What this field has seen, forgotten when the field is left (its states are then gone from memory and saves). An end
/// is sent at most once per field: a pass that holds again after its send waits for player 1's pass or the console, and
/// a send that did not take is never repeated in a loop.
struct Field
{
    std::unordered_set<std::wstring> said;  // verdicts logged
    std::unordered_set<Name>         sent;  // end triggers sent
};

Field                                      g_field;
std::unordered_map<std::wstring, Candidate> g_candidates;
std::optional<Pending>                     g_pending;
bool                                       g_wasSteady   = false;
std::int64_t                               g_steadySince = 0;
std::int64_t                               g_scanAt      = 0;  // the next scan's time, 0 for none owed
std::int64_t                               g_nextSpotCheck = 0;
int                                        g_spotCount   = -1;
bool                                       g_repairAsked = false;  // stuck.repair: scan and send even with stuck.auto off
std::uint32_t                              g_scans = 0, g_held = 0, g_refused = 0, g_sentPasses = 0;

bool IsSpotRaw(void* actor)
{
    return static_cast<UObject*>(actor)->IsA(g_spotClass);
}

/// The field-action spot class (the class whose actors carry a pass's signals), looked up until found.
bool SpotClassFound()
{
    if (g_spotClass == nullptr)
    {
        try
        {
            g_spotClass = RC::Unreal::UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, facts::kFieldSpotClass);
        }
        catch (...)
        {
            g_spotClass = nullptr;
        }
        if (g_spotClass == nullptr && !std::exchange(g_spotClassTold, true))
        {
            platform::LogWarning(L"stuck spots: the field-action spot class was not found; no spot is judged until it is");
        }
    }
    return g_spotClass != nullptr;
}

bool IsSpot(void* actor) noexcept
{
    bool is = false;
    return platform::CallGuarded(&IsSpotRaw, is, actor) && is;
}

/// A state row's steps for the rules (an unread next state as no state).
std::vector<rules::Step> RuleSteps(const std::uint8_t* row)
{
    std::vector<rules::Step> steps;
    if (row != nullptr)
    {
        for (const state_slots::Step& step : state_slots::Steps(row))
        {
            steps.push_back({step.trigger, step.next.value_or(L"")});
        }
    }
    return steps;
}

/// An act row's array field's count; false when unread.
bool Count(const std::uint8_t* row, std::uint32_t field, std::int32_t& count) noexcept
{
    platform::GameArray array;
    if (!Read(row + field, array))
    {
        return false;
    }
    count = array.num;
    return true;
}

/// Why finishing the acts the held state started, or starting the acts of the state the move returns to, would touch
/// the story; empty when neither does. An act whose row cannot be read counts as touching it.
std::wstring StoryActs(const std::uint8_t* heldRow, const std::uint8_t* backRow)
{
    for (const std::int64_t act : state_slots::EnteredActs(heldRow))
    {
        const std::uint8_t* row   = data_rows::Row(data_rows::Table::Action, act);
        std::int32_t        ends  = 0;
        std::int32_t        flags = 0;
        if (row == nullptr || !Count(row, facts::kActionRowEndTriggers, ends) || !Count(row, facts::kActionRowEndFlags, flags))
        {
            return std::format(L"the held state's act {} could not be read", platform::NameOrBits(act));
        }
        if (ends != 0 || flags != 0)
        {
            return std::format(L"finishing the held state's act {} would send {} trigger(s) and set {} story flag(s)",
                               platform::NameOrBits(act), ends, flags);
        }
    }
    for (const std::int64_t act : state_slots::EnteredActs(backRow))
    {
        const std::uint8_t* row  = data_rows::Row(data_rows::Table::Action, act);
        std::int32_t        ends = 0, starts = 0, flags = 0, work = 0;
        if (row == nullptr || !Count(row, facts::kActionRowEndTriggers, ends) || !Count(row, facts::kActionRowStartFlags, starts) ||
            !Count(row, facts::kActionRowEndFlags, flags) || !Count(row, facts::kActionRowLocationWork, work))
        {
            return std::format(L"the state it returns to starts act {}, whose row could not be read", platform::NameOrBits(act));
        }
        if (ends != 0 || starts != 0 || flags != 0 || work != 0)
        {
            return std::format(L"the state it returns to starts act {}, which touches the story ({} trigger(s), {} + {} story "
                               L"flag(s), {} location value(s))",
                               platform::NameOrBits(act), ends, starts, flags, work);
        }
    }
    return {};
}

/// A loaded spot and its passes.
struct Spot
{
    std::wstring             tag;
    bool                     onceOnly = false;
    std::vector<rules::Pass> passes;
};

/// The loaded spots with a pass that can hold a state; empty when the map could not be walked whole.
std::vector<Spot> Spots()
{
    std::vector<Spot> spots;
    if (!SpotClassFound())
    {
        return spots;
    }
    const bool walked = traversal_walkup::ForEachSpot([&](void* actor, std::uint64_t tag) {
        if (!IsSpot(actor))
        {
            return;
        }
        const auto*                                    at = static_cast<const std::uint8_t*>(actor);
        std::array<Name, facts::kFieldSpotSignalCount> s{};
        std::uint8_t                                   once = 0;
        if (!platform::CopyGuarded(s.data(), at + facts::kFieldSpotSignals, sizeof(s)) || !Read(at + facts::kFieldSpotOnceOnly, once))
        {
            return;
        }
        std::vector<rules::Pass> passes = rules::Passes({s[0], s[1], s[2], s[3], s[4], s[5]});
        if (!passes.empty())
        {
            spots.push_back({platform::NameOrBits(tag), once != 0, std::move(passes)});
        }
    });
    if (!walked)
    {
        spots.clear();
    }
    return spots;
}

/// Every active state waiting on one of `ends`, by trigger (a state's first entry for a trigger, as the game takes it).
/// Nullopt when the census is not whole (a slot unread, the list cut short): a part census could call a pass clean of
/// other moves.
std::optional<std::unordered_map<Name, std::vector<Waiter>>> WaitersOn(const std::unordered_set<Name>& ends)
{
    std::unordered_map<Name, std::vector<Waiter>> waiting;
    const std::uint8_t*                           sm = players::field_root::StateManager();
    if (sm == nullptr)
    {
        return std::nullopt;
    }
    if (ends.empty())
    {
        return waiting;
    }
    bool failed = false;
    const state_slots::Walk walk = state_slots::ForEach(sm, [&](const std::uint8_t* slot) noexcept {
        try
        {
            Waiter w{.slot = slot};
            if (!Read(slot + facts::kStateSlotObject, w.object) || !Read(slot + facts::kStateSlotState, w.state) ||
                !Read(slot + facts::kStateSlotRow, w.row))
            {
                failed = true;
                return true;
            }
            if (w.row == nullptr)
            {
                return false;  // a slot with no state row waits on nothing
            }
            std::unordered_set<Name> seen;
            for (const state_slots::Step& step : state_slots::Steps(w.row))
            {
                if (ends.contains(step.trigger) && seen.insert(step.trigger).second)
                {
                    w.next = step.next;
                    waiting[step.trigger].push_back(w);
                }
            }
            return false;
        }
        catch (...)
        {
            failed = true;
            return true;
        }
    });
    if (failed || walk != state_slots::Walk::Ended)
    {
        return std::nullopt;
    }
    return waiting;
}

/// The held passes of the loaded spots, each judged; nullopt when the census was not whole.
std::optional<std::vector<Verdict>> Scan()
{
    ++g_scans;
    std::vector<Verdict>     out;
    const std::vector<Spot>  spots = Spots();
    std::unordered_set<Name> ends;
    for (const Spot& spot : spots)
    {
        for (const rules::Pass& pass : spot.passes)
        {
            for (const Name end : pass.ends)
            {
                if (end != 0)
                {
                    ends.insert(end);
                }
            }
        }
    }
    const std::optional<std::unordered_map<Name, std::vector<Waiter>>> waiting = WaitersOn(ends);
    if (!waiting)
    {
        return std::nullopt;
    }
    for (const Spot& spot : spots)
    {
        for (const rules::Pass& pass : spot.passes)
        {
            std::vector<rules::EndCheck> checks;
            std::vector<Move>            moves;
            for (std::size_t e = 0; e < pass.ends.size(); ++e)
            {
                const Name end = pass.ends[e];
                if (end == 0 || (e == 1 && pass.ends[0] == end))
                {
                    continue;
                }
                rules::EndCheck check{.trigger = end};
                if (const auto found = waiting->find(end); found != waiting->end())
                {
                    for (const Waiter& w : found->second)
                    {
                        if (w.next && w.next->empty())
                        {
                            continue;  // the row names no next state for it: the trigger leaves it where it is
                        }
                        ++check.moves;  // an unread next state counts as a move nothing holds
                        const std::wstring current = platform::NameText(static_cast<std::uint64_t>(w.state));
                        if (!w.next || current.empty())
                        {
                            continue;
                        }
                        const std::uint8_t*            back  = state_slots::StateRow(*w.next);
                        const std::vector<rules::Step> steps = RuleSteps(back);
                        if (rules::HeldBy(pass, current, steps))
                        {
                            ++check.held;
                            moves.push_back({w.slot, w.object, current, *w.next});
                            check.chained = check.chained || rules::WaitsOnEnds(pass, steps);
                            if (check.storyActs.empty())
                            {
                                check.storyActs = StoryActs(w.row, back);
                            }
                        }
                    }
                }
                checks.push_back(std::move(check));
            }
            if (std::none_of(checks.begin(), checks.end(), [](const rules::EndCheck& c) { return c.held > 0; }))
            {
                continue;
            }
            for (rules::EndCheck& check : checks)
            {
                const state_signal::RowReading row = state_signal::ReadRow(static_cast<std::uint64_t>(check.trigger));
                check.rowRefusal                   = state_signal::Refusal(row);
                check.scope                        = row.scope;
            }
            out.push_back({spot.tag, pass.left, rules::Judge(spot.onceOnly, checks), std::move(moves)});
        }
    }
    return out;
}

std::wstring Describe(const Verdict& v)
{
    std::wstring held;
    for (const Move& m : v.moves)
    {
        held += std::format(L"{}{} in {} (back to {})", held.empty() ? L"" : L", ", platform::NameOrBits(m.object), m.from, m.to);
    }
    return std::format(L"{}'s {} pass holds {}: {}", v.spot, v.left ? L"left" : L"right", held,
                       v.plan.send.empty() ? L"refused: " + v.plan.refusal : L"sends " + platform::NameList(v.plan.send));
}

/// Why a level state may not be judged at rest now; empty when it may.
std::wstring Busy()
{
    if (players::game_state::MenuOwnsInput())
    {
        return L"a menu is up";
    }
    if (players::field_root::InBattle())
    {
        return L"a battle is on";
    }
    if (players::field_root::ControlLockMask() != 0)
    {
        return L"the field control lock is held (a talk, an event, a scene, a crawl)";
    }
    if (players::field_root::OnOff(facts::kOnOffDisableFieldAction))
    {
        return L"field actions are switched off (an action runs, or the story holds them)";
    }
    if (traversal_action::AnyoneActing())
    {
        return L"a player's character is in a field action";
    }
    return {};
}

/// The pass's ends not yet sent in this field.
bool Unsent(const Verdict& v)
{
    return std::any_of(v.plan.send.begin(), v.plan.send.end(), [](Name n) { return !g_field.sent.contains(n); });
}

/// Sends one confirmed pass's ends not yet sent in this field, through the game's sender, and sets its read-back.
void SendPass(const Verdict& v)
{
    std::vector<std::int64_t> sent;
    for (const Name end : v.plan.send)
    {
        if (!g_field.sent.contains(end) && state_signal::Send(static_cast<std::uint64_t>(end)))
        {
            g_field.sent.insert(end);
            sent.push_back(end);
        }
    }
    if (sent.empty())
    {
        return;
    }
    ++g_sentPasses;
    platform::Log(L"stuck spot: sent {} for {}'s {} pass, as player 1's pass through it ends it", platform::NameList(sent), v.spot,
                  v.left ? L"left" : L"right");
    g_pending = Pending{v.Key(), v.moves, platform::NowMs() + kReadBackMs};
}

/// Logs the new verdicts, keeps the accepted passes as candidates (a changed one starts its wait again, a gone one is
/// dropped), and sends the first candidate a scan has now confirmed. The number of candidates left waiting.
std::size_t Take(std::vector<Verdict> verdicts, std::int64_t now)
{
    std::unordered_map<std::wstring, Candidate> candidates;
    std::optional<std::wstring>                 confirmed;
    for (Verdict& v : verdicts)
    {
        const std::wstring key = v.Key();
        if (g_field.said.insert(key).second)
        {
            ++g_held;
            g_refused += v.plan.send.empty() ? 1 : 0;
            platform::Log(L"stuck spot: {}", Describe(v));
        }
        if (v.plan.send.empty() || !Unsent(v))
        {
            continue;
        }
        const auto         before = g_candidates.find(key);
        const bool         same   = before != g_candidates.end() && before->second.verdict.Same(v);
        const std::int64_t since  = same ? before->second.since : now;
        candidates[key]           = Candidate{std::move(v), since};
        if (!confirmed && now - since >= kConfirmMs)
        {
            confirmed = key;
        }
    }
    g_candidates = std::move(candidates);
    if (confirmed)
    {
        SendPass(g_candidates.at(*confirmed).verdict);
        g_candidates.erase(*confirmed);
    }
    return g_candidates.size();
}

/// Reads a sent pass's moved states back and logs the outcome (a slot now holding another object counts as gone).
void ReadBack(const Pending& p)
{
    std::wstring missed;
    int          moved = 0;
    for (const Move& m : p.moves)
    {
        std::int64_t object = 0, state = 0;
        if (!Read(m.slot + facts::kStateSlotObject, object) || object != m.object || !Read(m.slot + facts::kStateSlotState, state))
        {
            missed += std::format(L"; {}'s slot no longer holds it", platform::NameOrBits(m.object));
            continue;
        }
        const std::wstring now = platform::NameText(static_cast<std::uint64_t>(state));
        if (rules::SameText(now, m.to))
        {
            ++moved;
        }
        else
        {
            missed += std::format(L"; {} is in {}, not {}", platform::NameOrBits(m.object), now, m.to);
        }
    }
    if (missed.empty())
    {
        platform::Log(L"stuck spot: {}: {} of {} held state(s) back where the pass leaves them", p.key, moved, p.moves.size());
    }
    else
    {
        platform::LogWarning(L"stuck spot: {}: {} of {} held state(s) back where the pass leaves them{}", p.key, moved, p.moves.size(),
                             missed);
    }
}

void LeaveField()
{
    if (!g_field.said.empty() || !g_candidates.empty() || g_pending)
    {
        platform::LogDebug(L"stuck spots: the field is left; its verdicts are forgotten");
    }
    g_field = Field{};
    g_candidates.clear();
    g_pending.reset();
    g_spotCount   = -1;
    g_scanAt      = 0;
    g_repairAsked = false;
}

int SpotCount() noexcept
{
    int count = 0;
    traversal_walkup::ForEachSpot([&](void*, std::uint64_t) { ++count; });
    return count;
}

/// One scan on the steady field with nobody busy: its verdicts taken, the next scan set when a candidate waits.
void ScanNow(std::int64_t now)
{
    const std::int64_t                  began    = platform::NowMs();
    std::optional<std::vector<Verdict>> verdicts = Scan();
    if (!verdicts)
    {
        platform::LogDebug(L"stuck spots: scan {}: the level's states could not be read whole; tried again in {} ms", g_scans, kSpotCheckMs);
        g_scanAt = now + kSpotCheckMs;
        return;
    }
    const std::size_t held    = verdicts->size();
    const std::size_t waiting = Take(std::move(*verdicts), now);
    g_scanAt                  = waiting > 0 ? now + kConfirmMs : 0;
    g_repairAsked             = g_repairAsked && waiting > 0;
    platform::LogDebug(L"stuck spots: scan {} ({} entries in the spot map): {} held pass(es), {} waiting to be confirmed, {} ms", g_scans,
                       g_spotCount, held, waiting, platform::NowMs() - began);
}

/// Why the console may not scan now; empty on the steady field.
std::wstring NotSteady()
{
    return players::game_state::SteadyNow() ? std::wstring()
                                            : std::format(L"the field is not steady (field step {})", players::game_state::Current().step);
}

std::wstring ScanText()
{
    if (const std::wstring why = NotSteady(); !why.empty())
    {
        return why;
    }
    const std::optional<std::vector<Verdict>> verdicts = Scan();
    if (!verdicts)
    {
        return L"the level's states could not be read whole";
    }
    std::wstring text = std::format(L"{} held pass(es)", verdicts->size());
    for (const Verdict& v : *verdicts)
    {
        text += L"; " + Describe(v);
    }
    const std::wstring busy = Busy();
    return text + (busy.empty() ? L"; nothing in the way of a repair now" : L"; a repair waits: " + busy) +
           std::format(L"; {} candidate(s) waiting to be confirmed", g_candidates.size());
}

}  // namespace

bool Install() noexcept
{
    if (!state_signal::Installed() || !data_rows::Installed(data_rows::Table::Action) ||
        !data_rows::Installed(data_rows::Table::ActionGroup))
    {
        platform::Log(L"stuck spots: the trigger sender or the act tables are missing; a spot's state left on is repaired by "
                      L"player 1's pass or the console only");
        return false;
    }
    using control::Shape;
    using control::Value;
    control::Register({.name = L"stuck.scan", .shape = Shape::Text,
                       .help = L"the loaded spots' passes that hold a level state now, each with what a repair would send or why "
                               L"it is refused (read only)",
                       .get  = [] {
                           try
                           {
                               return Value{ScanText()};
                           }
                           catch (...)
                           {
                               return Value{std::wstring(L"the scan failed")};
                           }
                       }});
    control::Register({.name = L"stuck.repair", .shape = Shape::None,
                       .help = L"scans now and sends each accepted pass once a second scan confirms it (stuck.auto off too)",
                       .set  = [](const Value&) {
                           control::Outcome refused{false, NotSteady()};
                           if (!refused.detail.empty())
                           {
                               return refused;
                           }
                           g_repairAsked = true;
                           g_scanAt      = platform::NowMs();
                           return control::Outcome{true, std::format(L"scanning now; a held pass is sent when a scan {} s later "
                                                                     L"confirms it",
                                                                     kConfirmMs / 1000)};
                       }});
    control::Register({.name = L"stuck.auto", .shape = Shape::OnOff,
                       .help = L"the repair at a field's load and as spots stream in",
                       .set  = [](const Value& v) {
                           g_auto = std::get<bool>(v);
                           platform::Log(L"stuck spots: the repair at a field's load {}", g_auto ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return Value{g_auto}; }});
    g_installed = true;
    platform::Log(L"stuck spots: installed, on (a spot's level state left on by a pass without its end is ended at a field's "
                  L"load, as player 1's pass would end it; stuck.scan, stuck.repair, stuck.auto)");
    return true;
}

void Tick() noexcept
try
{
    if (!g_installed)
    {
        return;
    }
    const std::int64_t now = platform::NowMs();
    if (!players::game_state::SteadyNow())
    {
        if (g_wasSteady)
        {
            LeaveField();
        }
        g_wasSteady = false;
        return;
    }
    if (!g_wasSteady)
    {
        g_wasSteady     = true;
        g_steadySince   = now;
        g_nextSpotCheck = now + kSpotCheckMs;
        g_scanAt        = now + kSettleMs;
    }
    if (g_pending && now >= g_pending->at)
    {
        ReadBack(*g_pending);
        g_pending.reset();
    }
    if (now - g_steadySince < kSettleMs || g_pending)
    {
        return;
    }
    if (g_auto && now >= g_nextSpotCheck)
    {
        g_nextSpotCheck = now + kSpotCheckMs;
        if (const int count = SpotCount(); count != g_spotCount)
        {
            g_spotCount = count;
            g_scanAt    = g_scanAt == 0 ? now : std::min(g_scanAt, now);
        }
    }
    if ((!g_auto && !g_repairAsked) || g_scanAt == 0 || now < g_scanAt)
    {
        return;
    }
    if (!Busy().empty())
    {
        g_candidates.clear();  // nobody may be judged at rest now: a candidate's wait starts again after
        return;
    }
    ScanNow(now);
}
catch (...)
{
    try
    {
        platform::LogDebug(L"stuck spots: a tick raised; the next one tries again");
    }
    catch (...)
    {
    }
}

}  // namespace lss::world::stuck_spots
