#include "players/lead_gate.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_root.h"

#include <Unreal/NameTypes.hpp>

#include <algorithm>
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

constexpr std::uint32_t kCensusEveryTicks = 60;  // a staging lead's waiters read again this often (the marker each call)
constexpr std::uint32_t kMaxIconRecords   = 64;
constexpr std::size_t   kMaxNameChars     = 96;

std::uintptr_t   g_triggerTables  = 0;
StoryFlagIsSetFn g_storyFlagIsSet = nullptr;

/// The lead last judged, and the verdict that holds for it.
struct Judgment
{
    std::int64_t  object = 0, state = 0;
    bool          guide      = false;
    bool          waiter     = false;  // an other waiter, or a direct set on another object
    bool          marker     = false;  // the story's marker rides on the member
    std::uint32_t censusTick = 0;
    std::uint32_t calls      = 0;
};
Judgment     g_last;
std::wstring g_reading = L"none yet";
bool         g_unreadLogged = false;

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

/// The waiters on the lead's arrival trigger: the slots whose current state lists it (in the trigger's scope), and
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
        census.text = std::format(L"the member's Lead slot not among {} active slots", slots.size());
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
        platform::Log(L"lead gate: an engine check failed; a LEAD on a further player's member stays the player's");
        return false;
    }
    g_triggerTables  = *tables;
    g_storyFlagIsSet = reinterpret_cast<StoryFlagIsSetFn>(*flag);
    control::Register({.name = L"lead.gate", .shape = control::Shape::Text,
                       .help = L"the last LEAD on player 2's member judged: its arrival trigger, the level's waiters, the story's "
                               L"marker, and the verdict (guide: to its AI; staging: the player's)",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"lead gate: installed; a LEAD on player 2's member goes to its AI when a level step waits on its arrival or the "
                  L"story's marker rides on it");
    return true;
}

bool Guide(const void* member, const void* fieldAi) noexcept
try
{
    if (g_storyFlagIsSet == nullptr)  // not installed
    {
        return false;
    }
    const auto*  handler = static_cast<const std::uint8_t*>(ReadPointer(fieldAi, facts::kFieldAiLeadHandler));
    std::int64_t object = 0, state = 0;
    if (handler == nullptr || !Read(handler + facts::kLeadHandlerObject, object) || !Read(handler + facts::kLeadHandlerState, state) ||
        object == 0)
    {
        if (!g_unreadLogged)
        {
            g_unreadLogged = true;
            platform::LogDebug(L"lead gate: the LEAD handler of the member's field AI could not be read (handler {:#x}); staging assumed",
                               reinterpret_cast<std::uintptr_t>(handler));
        }
        return false;
    }
    const bool same = g_last.object == object && g_last.state == state;
    if (same && g_last.guide)
    {
        return true;  // judged a guide: holds until the lead ends
    }
    std::uint8_t* root = field_root::Root();
    std::uint8_t* sm   = field_root::StateManager();
    if (root == nullptr || sm == nullptr)
    {
        return false;
    }
    if (!same)
    {
        g_last = Judgment{.object = object, .state = state};
    }
    ++g_last.calls;
    const bool   census = !same || g_last.calls - g_last.censusTick >= kCensusEveryTicks / 10;  // calls come every 10 ticks
    const Marker marker = Markers(root, object);
    Census       waiters;
    if (census)
    {
        waiters            = Waiters(sm, object, state);
        g_last.censusTick  = g_last.calls;
        g_last.waiter      = waiters.others > 0;
    }
    const bool wasMarker = g_last.marker;
    g_last.marker        = marker.onMember;
    g_last.guide         = g_last.waiter || g_last.marker;
    if (!same || census || g_last.marker != wasMarker)
    {
        g_reading = std::format(L"object {} state {}: {}; marker {}: {}; verdict {}", Name(object), Name(state),
                                census ? waiters.text : L"waiters as before", marker.onMember ? L"on the member" : L"elsewhere",
                                marker.text, g_last.guide ? L"GUIDE (to its AI, the player's view on it)" : L"staging (the player's)");
        if (!same || g_last.guide)
        {
            platform::Log(L"lead gate: {}", g_reading);
        }
    }
    return g_last.guide;
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
