#include "players/lead_gate.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_hold.h"
#include "players/field_root.h"

#include <Unreal/NameTypes.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace lss::players::lead_gate {

namespace {

using platform::Read;
using platform::ReadPointer;

using StoryFlagIsSetFn = char (*)(const std::int64_t* flag);
using FindRowFn        = void* (*)(void* table, std::int64_t name);

// A staging lead's or run's waiters are read again this often (the marker each call).
constexpr std::uint32_t kCensusEveryTicks = 60;
constexpr std::uint32_t kMaxIconRecords   = 64;
constexpr std::size_t   kMaxNameChars     = 96;

std::uintptr_t   g_triggerTables  = 0;
StoryFlagIsSetFn g_storyFlagIsSet = nullptr;

/// The field AI whose handler last failed to read, and the kind: the failure is logged once (Debug) and then at
/// Trace while it lasts.
const void*  g_failingAi   = nullptr;
std::uint8_t g_failingKind = 0;

/// The rule's list of arrival kinds (field_hold::SignalsArrival) and the handler fields table (engine_facts.h) name
/// the same kinds: a kind added to one without the other would fall through to staging unseen.
constexpr bool ArrivalKindsHaveNames() noexcept
{
    for (std::uint8_t kind = 0; kind <= field_hold::kUnique; ++kind)
    {
        bool named = false;
        for (const facts::ArrivalHandlerNames& n : facts::kArrivalHandlerNames)
        {
            named = named || n.kind == kind;
        }
        if (named != field_hold::SignalsArrival(kind))
        {
            return false;
        }
    }
    return true;
}
static_assert(ArrivalKindsHaveNames(), "every arrival kind needs its handler's object/state offsets, and only those");

/// A lead or run judged, and the verdict that holds for it.
struct Judgment
{
    std::int64_t  object = 0, state = 0;
    std::uint8_t  kind       = 0;
    bool          guide      = false;
    bool          waiter     = false;  // an other waiter, or a direct set on another object
    bool          marker     = false;  // the story's marker rides on the member
    std::uint32_t censusTick = 0;
    std::uint32_t calls      = 0;
    std::uint32_t touched    = 0;      // the clock at the last call (the least recently used slot is reused)
};
/// One judgment per level object: a run-ahead is written on every member at once (one per further player), and
/// the seats ask in turn. Four covers three further players with one to spare.
std::array<Judgment, 4> g_judged;
std::uint32_t           g_clock   = 0;
std::wstring            g_reading = L"none yet";

/// The judgment slot of `object`, else the least recently used one.
Judgment& JudgmentOf(std::int64_t object) noexcept
{
    Judgment* slot = nullptr;
    for (Judgment& j : g_judged)
    {
        if (j.object == object)
        {
            slot = &j;
            break;
        }
        if (slot == nullptr || j.touched < slot->touched)
        {
            slot = &j;
        }
    }
    slot->touched = ++g_clock;
    return *slot;
}

/// The name's text through UE4SS's FName (the game's own pool); a free function so a fault is caught by the guard.
void NameText(std::int64_t bits, std::wstring* out) noexcept
try
{
    *out = RC::Unreal::FName(bits).ToString();
}
catch (...)
{
    out->clear();
}

std::wstring Name(std::int64_t bits) noexcept
{
    if (bits == 0)
    {
        return L"None";
    }
    std::wstring text;
    if (!platform::CallGuarded(&NameText, bits, &text) || text.empty())
    {
        return std::format(L"name#{:x}", static_cast<std::uint64_t>(bits));
    }
    return text;
}

/// An FString's text (`data` pointer, `num` count with the terminator), guarded and capped.
std::wstring Text(const std::uint8_t* fstring) noexcept
{
    const wchar_t* data = nullptr;
    std::int32_t   num  = 0;
    if (!Read(fstring, data) || !Read(fstring + 8, num) || data == nullptr || num <= 1)
    {
        return L"";
    }
    const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(num) - 1, kMaxNameChars);
    std::wstring      text(count, L'\0');
    if (!platform::CopyGuarded(text.data(), data, count * sizeof(wchar_t)))
    {
        return L"";
    }
    return text;
}

/// An FName array field (`ptr` at `base`, `num` at `base + 8`): its elements, capped.
std::vector<std::int64_t> Names(const std::uint8_t* base, std::uint32_t cap) noexcept
{
    const std::uint8_t* data = nullptr;
    std::int32_t        num  = 0;
    std::vector<std::int64_t> out;
    if (!Read(base, data) || !Read(base + 8, num) || data == nullptr || num <= 0)
    {
        return out;
    }
    const std::uint32_t count = std::min<std::uint32_t>(static_cast<std::uint32_t>(num), cap);
    out.resize(count);
    if (!platform::CopyGuarded(out.data(), data, count * sizeof(std::int64_t)))
    {
        out.clear();
    }
    return out;
}

struct Slot
{
    std::int64_t        object = 0, state = 0;
    const std::uint8_t* row    = nullptr;
};

/// The state manager's active slots (object, state, row), capped at the slot array's size.
std::vector<Slot> ActiveSlots(const std::uint8_t* sm) noexcept
{
    std::vector<Slot>   slots;
    const std::uint8_t* node = nullptr;
    const std::uint8_t* end  = sm + facts::kStateManagerActiveList;
    if (!Read(sm + facts::kStateManagerActiveHead, node))
    {
        return slots;
    }
    slots.reserve(8192);
    for (std::uint32_t n = 0; node != nullptr && node != end && n < facts::kStateManagerSlotCount; ++n)
    {
        std::int32_t index = -1;
        if (Read(node + 0x18, index) && index >= 0 && static_cast<std::uint32_t>(index) < facts::kStateManagerSlotCount)
        {
            const std::uint8_t* slot = nullptr;
            if (Read(sm + facts::kStateManagerSlots + 8u * static_cast<std::uint32_t>(index), slot) && slot != nullptr)
            {
                Slot s;
                if (Read(slot + facts::kStateSlotObject, s.object) && Read(slot + facts::kStateSlotState, s.state) &&
                    Read(slot + facts::kStateSlotRow, s.row))
                {
                    slots.push_back(s);
                }
            }
        }
        if (!Read(node + 8, node))
        {
            break;
        }
    }
    return slots;
}

/// The trigger row named `trigger`, through the tables' own lookup (the virtual StateMgr_FireStateTrigger calls).
const std::uint8_t* TriggerRow(std::int64_t trigger) noexcept
{
    for (std::uint32_t k = 0; k < 3; ++k)
    {
        void* table = nullptr;
        if (!Read(reinterpret_cast<const void*>(g_triggerTables + 8 * k), table) || table == nullptr)
        {
            continue;
        }
        void* vtable = ReadPointer(table, 0);
        auto  find   = reinterpret_cast<FindRowFn>(ReadPointer(vtable, facts::kDataTableFindRowSlot));
        void* row    = nullptr;
        if (find != nullptr && platform::CallGuarded(find, row, table, trigger) && row != nullptr)
        {
            return static_cast<const std::uint8_t*>(row);
        }
    }
    return nullptr;
}

struct Census
{
    bool         read    = false;
    std::int64_t trigger = 0;
    bool         self    = false;  // the member's own step waits (always, with a trigger)
    int          others  = 0;      // other waiters (scope) plus direct sets on other objects
    std::wstring text;
};

/// The waiters on the setting's arrival trigger: the slots whose current state lists it (in the trigger's scope), and
/// the trigger row's own direct sets on other objects. RE-lead-arrival-wait.md §3.
Census Waiters(const std::uint8_t* sm, std::int64_t object, std::int64_t state) noexcept
try
{
    Census                  census;
    const std::vector<Slot> slots = ActiveSlots(sm);
    const Slot*             self  = nullptr;
    for (const Slot& s : slots)
    {
        if (s.object == object && s.state == state)
        {
            self = &s;
            break;
        }
    }
    if (self == nullptr || self->row == nullptr)
    {
        census.text = std::format(L"the member's AI-setting slot not among {} active slots", slots.size());
        return census;
    }
    const std::vector<std::int64_t> args = Names(self->row + facts::kStateRowStringArgs, 3);
    census.read                          = true;
    if (args.size() < 3 || args[2] == 0)
    {
        census.text = L"no arrival trigger (nothing can wait)";
        return census;
    }
    census.trigger = args[2];
    // The trigger row: its scope and direct sets.
    const std::uint8_t* trigger = TriggerRow(census.trigger);
    std::uint8_t        scope   = 0xFF;
    std::vector<std::int64_t> sendObjects;
    if (trigger != nullptr)
    {
        Read(trigger + facts::kTriggerRowScope, scope);
        sendObjects = Names(trigger + facts::kTriggerRowSendObjects, 64);
        const std::vector<std::int64_t> objects = Names(trigger + facts::kTriggerRowObjects, facts::kTriggerRowDirectSetsMax);
        const std::vector<std::int64_t> attrs   = Names(trigger + facts::kTriggerRowAttributes, facts::kTriggerRowDirectSetsMax);
        const std::vector<std::int64_t> states  = Names(trigger + facts::kTriggerRowStates, facts::kTriggerRowDirectSetsMax);
        for (std::size_t i = 0; i < objects.size(); ++i)
        {
            if (objects[i] == object)
            {
                continue;
            }
            ++census.others;
            census.text += std::format(L"; sets {} {} -> {}", Name(objects[i]), i < attrs.size() ? Name(attrs[i]) : L"?",
                                       i < states.size() ? Name(states[i]) : L"?");
        }
    }
    // The scope's candidates: Self = the member's own slots; SpecificObject = the named objects; else every slot.
    int listed = 0;
    for (const Slot& s : slots)
    {
        if (s.row == nullptr)
        {
            continue;
        }
        if (scope == 0 && s.object != object)
        {
            continue;
        }
        if (scope == 3 && std::find(sendObjects.begin(), sendObjects.end(), s.object) == sendObjects.end() && s.object != object)
        {
            continue;
        }
        const std::vector<std::int64_t> triggers = Names(s.row + facts::kStateRowTriggers, facts::kStateRowTriggersRead);
        for (std::size_t i = 0; i < triggers.size() && triggers[i] != 0; ++i)
        {
            if (triggers[i] != census.trigger)
            {
                continue;
            }
            const std::uint8_t* nextStates = nullptr;
            std::int32_t        nextCount  = 0;
            Read(s.row + facts::kStateRowNextStates, nextStates);
            Read(s.row + facts::kStateRowNextStates + 8, nextCount);
            const std::wstring next = nextStates != nullptr && static_cast<std::int32_t>(i) < nextCount ? Text(nextStates + 16 * i) : L"?";
            if (&s == self)
            {
                census.self = true;
                census.text += std::format(L"; self -> {}", next);
            }
            else
            {
                ++census.others;
                census.text += std::format(L"; {} {} -> {}", Name(s.object), Name(s.state), next);
            }
            ++listed;
            break;
        }
    }
    census.text = std::format(L"trigger {} (row {}, scope {}), {} waiting of {} active{}", Name(census.trigger),
                              trigger != nullptr ? L"found" : L"NOT found", scope == 0xFF ? L"?" : std::to_wstring(scope), listed,
                              slots.size(), census.text);
    return census;
}
catch (...)
{
    return Census{.text = L"the walk raised an exception"};
}

struct Marker
{
    bool         read     = false;
    bool         onMember = false;
    std::wstring text;
};

/// The objective markers shown now and what they ride on: RE-navi-icon-target.md §2, the game's own slot choice.
Marker Markers(const std::uint8_t* root, std::int64_t object) noexcept
try
{
    Marker              marker;
    const std::uint8_t* registry = static_cast<const std::uint8_t*>(ReadPointer(root, facts::kRootObjectiveIcons));
    const std::uint8_t* data     = nullptr;
    std::int32_t        num      = 0;
    if (registry == nullptr || !Read(registry + 8, data) || !Read(registry + 0x10, num) || data == nullptr)
    {
        marker.text = L"registry unread";
        return marker;
    }
    marker.read = true;
    int shown   = 0;
    for (std::int32_t i = 0; i < num && i < static_cast<std::int32_t>(kMaxIconRecords); ++i)
    {
        const std::uint8_t* element = data + 24 * i;
        std::int64_t        key     = 0;
        const std::uint8_t* record  = nullptr;
        std::int64_t        recKey  = 0;
        std::uint8_t        gone    = 1;
        const std::uint8_t* row     = nullptr;
        if (!Read(element, key) || !Read(element + 8, record) || record == nullptr || !Read(record + facts::kObjectiveIconRecordKey, recKey) ||
            recKey != key || !Read(record + facts::kObjectiveIconRecordGone, gone) || gone != 0 ||
            !Read(record + facts::kObjectiveIconRecordRow, row) || row == nullptr)
        {
            continue;
        }
        const std::vector<std::int64_t> targets = Names(row + facts::kStoryRowIconTargets, facts::kStoryRowIconSlots);
        const std::vector<std::int64_t> hides   = Names(row + facts::kStoryRowIconHideFlags, facts::kStoryRowIconSlots);
        std::int64_t                    target  = 0;
        for (std::size_t s = 0; s < targets.size(); ++s)
        {
            if (targets[s] == 0)
            {
                continue;
            }
            char hidden = 0;
            if (s < hides.size() && hides[s] != 0 && platform::CallGuarded(g_storyFlagIsSet, hidden, &hides[s]) && hidden != 0)
            {
                continue;
            }
            target = targets[s];
            break;
        }
        if (target == 0)
        {
            std::int64_t shownId = 0;
            char         set     = 0;
            if (Read(row + facts::kStoryRowInvNaviShown, shownId) && shownId != 0 &&
                platform::CallGuarded(g_storyFlagIsSet, set, reinterpret_cast<const std::int64_t*>(row + facts::kStoryRowInvNaviFlag)) &&
                set != 0)
            {
                Read(row + facts::kStoryRowInvNaviTarget, target);
            }
        }
        ++shown;
        marker.text += std::format(L"{}{} -> {}", shown == 1 ? L"" : L", ", Name(key), Name(target));
        if (target != 0 && target == object)
        {
            marker.onMember = true;
        }
    }
    if (shown == 0)
    {
        marker.text = L"none shown";
    }
    return marker;
}
catch (...)
{
    return Marker{.text = L"the walk raised an exception"};
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tables = image.Rebase(facts::kStateTriggerTablesGlobal, 3 * sizeof(void*));
    const auto flag   = platform::FindFunction(image, facts::kStoryFlagIsSet);
    if (!field_root::Installed() || !tables || !flag)
    {
        platform::Log(L"lead gate: an engine check failed; a LEAD or a run to a mark on a further player's member stays the player's");
        return false;
    }
    g_triggerTables  = *tables;
    g_storyFlagIsSet = reinterpret_cast<StoryFlagIsSetFn>(*flag);
    control::Register({.name = L"lead.gate", .shape = control::Shape::Text,
                       .help = L"the last LEAD or run to a mark on a further player's member judged: its arrival trigger, the level's waiters, the story's "
                               L"marker, and the verdict (guide: to its AI; staging: the player's)",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"lead gate: installed; a LEAD or a run to a mark on a further player's member goes to its AI when a level step "
                  L"waits on its arrival or the story's marker rides on it");
    return true;
}

bool Guide(const void* fieldAi, std::uint8_t kind) noexcept
try
{
    // Not installed, or a kind whose arrival the level cannot wait on (field_hold.h names them): staging.
    if (g_storyFlagIsSet == nullptr || !field_hold::SignalsArrival(kind))
    {
        return false;
    }
    // The kind's handler holds the object and state names its arrival signal passes (engine_facts.h).
    const facts::ArrivalHandlerNames* names = nullptr;
    for (const facts::ArrivalHandlerNames& n : facts::kArrivalHandlerNames)
    {
        if (n.kind == kind)
        {
            names = &n;
            break;
        }
    }
    if (names == nullptr)
    {
        platform::LogDebug(L"lead gate: no handler names known for {} (kind {}); staging assumed", field_hold::KindName(kind), kind);
        return false;
    }
    const auto*  handler = static_cast<const std::uint8_t*>(ReadPointer(fieldAi, facts::kFieldAiHandlers + 8u * kind));
    std::int64_t object = 0, state = 0;
    if (handler == nullptr || !Read(handler + names->object, object) || !Read(handler + names->state, state))
    {
        if (fieldAi != g_failingAi || kind != g_failingKind)
        {
            g_failingAi   = fieldAi;
            g_failingKind = kind;
            platform::LogDebug(L"lead gate: the {} handler of a member's field AI {:#x} could not be read (handler {:#x}); staging assumed",
                               field_hold::KindName(kind), reinterpret_cast<std::uintptr_t>(fieldAi), reinterpret_cast<std::uintptr_t>(handler));
        }
        else
        {
            platform::LogTrace(L"lead gate: the {} handler of field AI {:#x} still unread; staging", field_hold::KindName(kind),
                               reinterpret_cast<std::uintptr_t>(fieldAi));
        }
        return false;
    }
    g_failingAi = nullptr;
    if (object == 0)
    {
        platform::LogTrace(L"lead gate: the {} on the level layer names no level object (a state-manager command's run): no arrival "
                           L"signal, staging",
                           field_hold::KindName(kind));
        return false;
    }
    Judgment&  last = JudgmentOf(object);
    const bool same = last.object == object && last.state == state && last.kind == kind;
    if (same && last.guide)
    {
        return true;  // judged a guide: holds until the setting ends (its state changes)
    }
    std::uint8_t* root = field_root::Root();
    std::uint8_t* sm   = field_root::StateManager();
    if (root == nullptr || sm == nullptr)
    {
        return false;
    }
    if (!same)
    {
        last = Judgment{.object = object, .state = state, .kind = kind, .touched = last.touched};
    }
    ++last.calls;
    // The hold check calls every 10 ticks.
    const bool   census = !same || last.calls - last.censusTick >= kCensusEveryTicks / 10;
    const Marker marker = Markers(root, object);
    Census       waiters;
    if (census)
    {
        waiters         = Waiters(sm, object, state);
        last.censusTick = last.calls;
        last.waiter     = waiters.others > 0;
    }
    const bool wasMarker = last.marker;
    last.marker          = marker.onMember;
    last.guide           = last.waiter || last.marker;
    if (!same || census || last.marker != wasMarker)
    {
        g_reading = std::format(L"{} on object {} state {}: {}; marker {}: {}; verdict {}", field_hold::KindName(kind), Name(object),
                                Name(state), census ? waiters.text : L"waiters as before", marker.onMember ? L"on the member" : L"elsewhere",
                                marker.text, last.guide ? L"GUIDE (to its AI, the player's view on it)" : L"staging (the player's)");
        if (!same || last.guide)
        {
            platform::Log(L"lead gate: {}", g_reading);
        }
    }
    return last.guide;
}
catch (...)
{
    platform::Log(L"lead gate: the judgment raised an exception; staging assumed");
    return false;
}

std::wstring Reading() noexcept
{
    return g_reading;
}

}  // namespace lss::players::lead_gate
