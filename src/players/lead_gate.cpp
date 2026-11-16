#include "players/lead_gate.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/game_vectors.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/name_text.h"
#include "platform/settings.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "players/field_hold.h"
#include "players/field_root.h"
#include "players/lead_keep_rules.h"
#include "players/leash_rules.h"
#include "players/party_member.h"
#include "players/state_path.h"
#include "world/actor_place.h"
#include "world/data_rows.h"
#include "world/state_slots.h"
#include "world/story_boxes.h"
#include "world/traversal_walkup.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace lss::players::lead_gate {

namespace {

using platform::GameVector;
using platform::Read;
using platform::ReadPointer;
using lead_keep_rules::Verdict;
namespace data_rows   = world::data_rows;
namespace state_slots = world::state_slots;
using state_slots::Slot;

using StoryFlagIsSetFn = char (*)(const std::int64_t* flag);
using FieldPointFindFn = const std::uint8_t* (*)(void* points, const std::int64_t* name);

// A staging lead's or run's waiters are read again this often (the marker each call).
constexpr std::uint32_t kCensusEveryTicks = 60;
constexpr float         kAiOnlySpotCensus = 3000.0f;  // cm: how far from a kept move's start an AI-only spot is named in the log
constexpr std::uint32_t kMaxIconRecords   = 64;
constexpr std::size_t   kMaxNameChars     = 96;
// How many of the member's own states ahead are walked for a further move.
constexpr std::size_t   kPathLinks        = 16;

StoryFlagIsSetFn g_storyFlagIsSet    = nullptr;
FieldPointFindFn g_fieldPointFind    = nullptr;
bool             g_keeps             = false; // `lead-holder keeps` (experimental, players/lead_keep_rules.h); guide: every needed move to its AI
bool             g_marking           = false; // hud/lead_mark installed: a kept move's point can be shown

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
    Verdict       verdict    = Verdict::Staging;
    // Another waiter, a direct set on another object, or a further move in the member's own chain.
    bool          waiter     = false;
    bool          marker     = false;  // the story's marker rides on the member
    bool          leash      = false;  // a live AutoWalk act leashes player 1 to the member
    bool          signals    = false;  // the move names an arrival trigger
    std::int64_t  ownBox     = 0;      // the step moves on at this armed story box of its own (OwnBoxAhead): a guide
    bool          leadIn     = false;  // a scene lead-in by name (lead_keep_rules::SceneLeadIn); SceneLeadIn reads the object's slot
    bool          pointKnown = false;  // its destination is a field point of the loaded level (`point`)
    bool          reached    = false;  // measured within the hand radius while the move was needed, kept and signalling (this setting)
    std::wstring  crossing;            // reached at an AI-only spot on the way instead (its kind and tag; empty: at the point)
    std::int64_t  destination = 0;
    GameVector    point;
    std::uint32_t censusTick = 0;
    std::uint32_t calls      = 0;
    std::uint32_t touched    = 0;      // the clock at the last call (the least recently used slot is reused)
    std::wstring  reading;             // the judgment in words, as logged (the hand's log line quotes it)
};
/// One judgment per level object: a run-ahead is written on every member at once (one per further player), and
/// the seats ask in turn. Four covers three further players with one to spare.
std::array<Judgment, 4> g_judged;
std::uint32_t           g_clock   = 0;
std::wstring            g_reading = L"none yet";

/// The judgment slot of `object`, else an empty one, else the least recently used one.
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
        if (slot == nullptr || (j.object == 0 && slot->object != 0) ||
            ((j.object == 0) == (slot->object == 0) && j.touched < slot->touched))
        {
            slot = &j;
        }
    }
    slot->touched = ++g_clock;
    return *slot;
}

/// The level object each member was last judged on, for Forget (a seat's member each; the entry of a member not seen
/// before takes an empty slot, else the first).
struct MemberObject
{
    const void*  member = nullptr;
    std::int64_t object = 0;
};
std::array<MemberObject, 4> g_memberObjects;

void Remember(const void* member, std::int64_t object) noexcept
{
    MemberObject* slot = nullptr;
    for (MemberObject& m : g_memberObjects)
    {
        if (m.member == member)
        {
            slot = &m;
            break;
        }
        if (slot == nullptr || (slot->member != nullptr && m.member == nullptr))
        {
            slot = &m;
        }
    }
    *slot = MemberObject{.member = member, .object = object};
}

/// The level object `member` was last judged on, 0 when none.
std::int64_t RememberedObject(const void* member) noexcept
{
    for (const MemberObject& m : g_memberObjects)
    {
        if (m.member == member)
        {
            return m.object;
        }
    }
    return 0;
}

/// A name's text for the log: "None" for no name, its bits when the engine cannot say.
std::wstring Name(std::int64_t bits)
{
    return platform::NameOrBits(bits);
}

/// A short FString's text (a status, an act's argument), capped for the log.
std::wstring Text(const std::uint8_t* fstring) noexcept
{
    return state_slots::Text(fstring, kMaxNameChars);
}

/// A shown objective marker's target and where the game draws it (the record's shown position, world centimetres).
struct MarkerPoint
{
    std::int64_t target = 0;
    GameVector   at;
};

/// A named point's world location: the level's field point of that name, the lookup the objective marker makes
/// first (research/RE-lead-arrival-wait.md); false when the name is no field point of the loaded level.
bool PointLocation(std::int64_t name, GameVector& out) noexcept
{
    void*               points    = field_root::FieldPoints();
    const std::uint8_t* transform = nullptr;
    if (g_fieldPointFind == nullptr || points == nullptr ||
        !platform::CallGuarded(g_fieldPointFind, transform, points, &name) || transform == nullptr)
    {
        return false;
    }
    return Read(transform + facts::kFieldPointLocation, out);  // copied at once: the table moves as levels stream
}

/// Where a move goes, in words: its destination's name and location and the distance to the nearest shown marker.
std::wstring Where(std::int64_t destination, const std::vector<MarkerPoint>& markers) noexcept
try
{
    if (destination == 0)
    {
        return L"no destination named";
    }
    GameVector at;
    if (!PointLocation(destination, at))
    {
        return std::format(L"{} (not a field point: an actor or route name, or another level's point)", Name(destination));
    }
    std::wstring text =
        std::format(L"{} at ({:.1f}, {:.1f}, {:.1f}) m", Name(destination), at.x / 100, at.y / 100, at.z / 100);
    const MarkerPoint* nearest = nullptr;
    float              best    = 0;
    for (const MarkerPoint& m : markers)
    {
        const float d = std::hypot(at.x - m.at.x, at.y - m.at.y, at.z - m.at.z);
        if (nearest == nullptr || d < best)
        {
            nearest = &m;
            best    = d;
        }
    }
    if (nearest != nullptr)
    {
        text += std::format(L", {:.1f} m from the marker on {}", best / 100, Name(nearest->target));
    }
    return text;
}
catch (...)
{
    return L"?";
}

/// A state row as the walk needs it, through the game's own lookup (the state-change tables); nullopt when unknown.
std::optional<state_path::Row> PathRow(const std::wstring& name) noexcept
try
{
    const std::uint8_t* row = state_slots::StateRow(name);
    if (row == nullptr)
    {
        return std::nullopt;
    }
    state_path::Row out{.status = Text(row + facts::kStateRowStatus), .next = state_slots::NextStates(row)};
    const std::vector<std::int64_t> args = data_rows::Names(row + facts::kStateRowStringArgs, facts::kStateRowDestinationArg + 1);
    if (args.size() > facts::kStateRowDestinationArg)
    {
        out.destination = args[facts::kStateRowDestinationArg];
    }
    return out;
}
catch (...)
{
    return std::nullopt;
}

/// A trigger's special type by the game's name (EEndStateTriggerSpecialType).
constexpr const wchar_t* SpecialTypeName(std::uint8_t type) noexcept
{
    switch (type)
    {
    case 0:
        return L"None";
    case 1:
        return L"EnvironmentAnimationLastFrame";
    case 2:
        return L"IgnoreDelay";
    case 3:
        return L"IgnoreSe";
    }
    return L"?";
}

/// What ends a step that is not a move (a stay, an animation, a nameless step), in words: each trigger it waits on,
/// with its row's scope, special types and receivers and the step it leads to; the actions the step starts on entry;
/// its non-zero arguments (logging: which of these is the player's approach is read from the log, not judged here).
std::wstring Ends(const std::wstring& state) noexcept
try
{
    const std::uint8_t* row = state_slots::StateRow(state);
    if (row == nullptr)
    {
        return L"row unread";
    }
    std::wstring                         text;
    const std::vector<state_slots::Step> steps = state_slots::Steps(row);
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        const std::wstring  next    = steps[i].next.value_or(L"?");
        const std::uint8_t* trigger = data_rows::Row(data_rows::Table::Trigger, steps[i].trigger);
        std::wstring        detail  = L"row NOT found";
        if (trigger != nullptr)
        {
            std::int32_t scope = -1;
            Read(trigger + facts::kTriggerRowScope, scope);
            detail = std::format(L"scope {}", scope);
            const std::vector<std::uint8_t> types = data_rows::Array<std::uint8_t>(trigger + facts::kTriggerRowSpecialTypes, 8);
            if (!types.empty())
            {
                detail += L", special types";
                for (const std::uint8_t type : types)
                {
                    detail += std::format(L" {}", SpecialTypeName(type));
                }
            }
            const std::vector<std::int64_t> sends = data_rows::Names(trigger + facts::kTriggerRowSendObjects, 4);
            for (std::size_t s = 0; s < sends.size(); ++s)
            {
                detail += std::format(L"{}{}", s == 0 ? L", sends to " : L" ", Name(sends[s]));
            }
        }
        text += std::format(L"{}{} ({}) -> {}", i == 0 ? L"" : L", ", Name(steps[i].trigger), detail, next.empty() ? L"None" : next);
    }
    if (text.empty())
    {
        text = L"no trigger";
    }
    const std::vector<std::int64_t> actions = data_rows::Names(row + facts::kStateRowEnterActions, 4);
    for (std::size_t a = 0; a < actions.size(); ++a)
    {
        text += std::format(L"{}{}", a == 0 ? L"; starts " : L" ", Name(actions[a]));
    }
    const std::vector<std::int64_t> strings = data_rows::Names(row + facts::kStateRowStringArgs, 12);
    const std::vector<std::int32_t> ints    = data_rows::Array<std::int32_t>(row + facts::kStateRowIntArgs, 16);
    std::wstring                    args;
    for (std::size_t a = 0; a < strings.size(); ++a)
    {
        if (strings[a] != 0)
        {
            args += std::format(L" s{}={}", a, Name(strings[a]));
        }
    }
    for (std::size_t a = 0; a < ints.size(); ++a)
    {
        if (ints[a] != 0)
        {
            args += std::format(L" i{}={}", a, ints[a]);
        }
    }
    if (!args.empty())
    {
        text += L"; args" + args;
    }
    return text;
}
catch (...)
{
    return L"?";
}

struct Path
{
    bool         move = false;  // a further move among the states ahead, before the member stands
    std::wstring text;          // the states walked, to the move if one was found, and where each run goes
};

/// The member's own states ahead of `first` (players/state_path.h: breadth first, each once, a Stable ending a
/// branch, kPathLinks at most): a further move among them makes the chain the level's path for the member. Every
/// run on the path is named with its destination and the distance to the nearest shown marker (logging; the
/// verdict does not read it).
Path PathAhead(const std::wstring& first, const std::vector<MarkerPoint>& markers) noexcept
try
{
    Path path;
    if (first.empty())
    {
        path.text = L"?";
        return path;
    }
    const state_path::Walk walk = state_path::WalkFrom(first, &PathRow, kPathLinks);
    const auto status = [&walk](std::size_t i) -> std::wstring {
        const state_path::Link& link = walk.links[i];
        return !link.read ? L"status unread" : link.status.empty() ? L"no status" : link.status;
    };
    if (walk.firstMove)
    {
        const std::vector<std::size_t> trail = state_path::Trail(walk, *walk.firstMove);
        for (std::size_t k = 0; k < trail.size(); ++k)
        {
            path.text += std::format(L"{}{} ({})", k == 0 ? L"" : L" -> ", walk.links[trail[k]].name, status(trail[k]));
        }
        path.text += L": a further move ahead, the level walks the member on";
        path.move = true;
    }
    else
    {
        // No further move: every state walked, so the log shows the whole chain.
        for (std::size_t i = 0; i < walk.links.size(); ++i)
        {
            path.text += std::format(L"{}{} ({})", i == 0 ? L"" : L", ", walk.links[i].name, status(i));
        }
        path.text += walk.truncated ? L": the walk's limit, no further move among them"
                                    : L": the path's end, no further move";
    }
    // Where the path goes: every move reached, in walking order.
    for (const state_path::Link& link : walk.links)
    {
        if (link.read && field_hold::MoveSetting(link.status))
        {
            path.text += std::format(L"; {} moves to {}", link.name, Where(link.destination, markers));
        }
    }
    // What ends every other step reached (a stay, an animation, a nameless step): the stand ends the path, so skipped.
    for (const state_path::Link& link : walk.links)
    {
        if (link.read && !field_hold::MoveSetting(link.status) && !field_hold::PathEnd(link.status))
        {
            path.text += std::format(L"; {} ({}) ends on {}", link.name, link.status.empty() ? L"no status" : link.status,
                                     Ends(link.name));
        }
    }
    return path;
}
catch (...)
{
    return Path{.text = L"the walk raised an exception"};
}

struct Census
{
    bool         read        = false;
    std::int64_t destination = 0;  // the move's point (string argument 1), 0 when unnamed
    std::int64_t trigger     = 0;
    bool         self    = false;  // the member's own step waits (always, with a trigger)
    bool         chain   = false;  // a further move ahead in the member's own chain of states: the level walks it on
    int          others  = 0;      // other waiters (scope) plus direct sets on other objects
    std::int64_t ownBox  = 0;      // the step moves on at a trigger its own armed member-only story box sends (OwnBoxAhead)
    std::wstring text;
};

/// The member's move step moves on at a trigger (other than its arrival) that a member-only story box of the member's own
/// sends, and the box is armed: the level expects the run to carry the member into that box, on the clock of whatever
/// else runs meanwhile (the Ch. 5 dash crosses Tifa's fence box within 3 s, before the talk's timed "head for the
/// stairs"; held by its player, she would miss it and stall). Any active object that waits for the trigger or names it
/// as a state's Status is asked (a box's switch waits for the trigger it sends; its trigger state names it but need not
/// list it); world/story_boxes reads the box's own trigger state through the game's slot lookup and checks that it names
/// the trigger, the box's group and that it is switched on (RE-held-member-story-boxes.md). 0 when none.
std::int64_t OwnBoxAhead(const std::vector<Slot>& slots, const Slot& self, std::int64_t arrival, const void* member)
{
    std::vector<std::int64_t> exits = state_slots::Triggers(self.row);
    std::erase_if(exits, [arrival](std::int64_t t) { return t == 0 || t == arrival; });
    if (exits.empty())
    {
        return 0;
    }
    std::vector<std::wstring> exitNames;
    exitNames.reserve(exits.size());
    for (const std::int64_t exit : exits)
    {
        exitNames.push_back(Name(exit));
    }
    for (const Slot& s : slots)
    {
        if (s.row == nullptr || s.object == self.object)
        {
            continue;
        }
        const std::vector<std::int64_t> listens = state_slots::Triggers(s.row);
        std::optional<std::wstring>     status;  // read only for a slot that waits for none of the exits
        for (std::size_t i = 0; i < exits.size(); ++i)
        {
            if (std::find(listens.begin(), listens.end(), exits[i]) == listens.end())
            {
                if (!status)
                {
                    status = Text(s.row + facts::kStateRowStatus);
                }
                if (_wcsicmp(status->c_str(), exitNames[i].c_str()) != 0)
                {
                    continue;  // this object neither waits for the trigger nor sends it: not the box
                }
            }
            if (world::story_boxes::ArmedOwnBox(static_cast<std::uint64_t>(s.object), member, static_cast<std::uint64_t>(exits[i])))
            {
                return s.object;
            }
        }
    }
    return 0;
}

/// The waiters on the setting's arrival trigger: the slots whose current state lists it (in the trigger's scope), and
/// the trigger row's own direct sets on other objects (research/RE-lead-arrival-wait.md).
Census Waiters(const std::vector<Slot>& slots, std::int64_t object, std::int64_t state,
               const std::vector<MarkerPoint>& markers, const void* member) noexcept
try
{
    Census      census;
    const Slot* self = nullptr;
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
    const std::vector<std::int64_t> args = data_rows::Names(self->row + facts::kStateRowStringArgs, 3);
    census.read                          = true;
    if (args.size() > facts::kStateRowDestinationArg)
    {
        census.destination = args[facts::kStateRowDestinationArg];
    }
    census.ownBox = OwnBoxAhead(slots, *self, args.size() > 2 ? args[2] : 0, member);
    const std::wstring ownBox =
        census.ownBox != 0 ? std::format(L"; moves on at its own story box {} (the run carries it in)", Name(census.ownBox)) : L"";
    if (args.size() < 3 || args[2] == 0)
    {
        census.text = L"no arrival trigger (nothing can wait)" + ownBox;
        return census;
    }
    census.trigger = args[2];
    // Where this move goes (logging).
    census.text = std::format(L"; this move goes to {}", Where(args[facts::kStateRowDestinationArg], markers));
    // The trigger row: its scope and direct sets.
    const std::uint8_t* trigger = data_rows::Row(data_rows::Table::Trigger, census.trigger);
    std::uint8_t        scope   = 0xFF;
    std::vector<std::int64_t> sendObjects;
    if (trigger != nullptr)
    {
        Read(trigger + facts::kTriggerRowScope, scope);
        sendObjects = data_rows::Names(trigger + facts::kTriggerRowSendObjects, 64);
        const std::vector<std::int64_t> objects = data_rows::Names(trigger + facts::kTriggerRowObjects, facts::kTriggerRowDirectSetsMax);
        const std::vector<std::int64_t> attrs   = data_rows::Names(trigger + facts::kTriggerRowAttributes, facts::kTriggerRowDirectSetsMax);
        const std::vector<std::int64_t> states  = data_rows::Names(trigger + facts::kTriggerRowStates, facts::kTriggerRowDirectSetsMax);
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
        for (const state_slots::Step& step : state_slots::Steps(s.row))
        {
            if (step.trigger != census.trigger)
            {
                continue;
            }
            const std::optional<std::wstring>& known = step.next;
            const std::wstring                 next  = known.value_or(L"?");
            if (&s == self)
            {
                census.self     = true;
                const Path path = PathAhead(known.value_or(L""), markers);
                census.chain    = path.move;
                census.text += std::format(L"; self -> {}", path.text);
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
                              slots.size(), census.text + ownBox);
    return census;
}
catch (...)
{
    return Census{.text = L"the walk raised an exception"};
}

struct Marker
{
    bool                     read     = false;
    bool                     onMember = false;
    std::wstring             text;
    std::vector<MarkerPoint> points;  // where the shown markers are drawn
};

/// The objective markers shown now and what they ride on, by the game's own slot choice (RE-navi-icon-target.md).
Marker Markers(const std::uint8_t* root, std::int64_t object) noexcept
try
{
    Marker              marker;
    const std::uint8_t* registry = static_cast<const std::uint8_t*>(ReadPointer(root, facts::kRootObjectiveIcons));
    const std::uint8_t* data     = nullptr;
    std::int32_t        num      = 0;
    if (registry == nullptr || !Read(registry + facts::kObjectiveIconsData, data) || !Read(registry + facts::kObjectiveIconsNum, num) || data == nullptr)
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
        const std::vector<std::int64_t> targets = data_rows::Names(row + facts::kStoryRowIconTargets, facts::kStoryRowIconSlots);
        const std::vector<std::int64_t> hides   = data_rows::Names(row + facts::kStoryRowIconHideFlags, facts::kStoryRowIconSlots);
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
        MarkerPoint point{.target = target};
        const bool  placed = Read(record + facts::kObjectiveIconRecordAt, point.at);
        if (placed)
        {
            marker.points.push_back(point);
        }
        const wchar_t* comma = shown == 1 ? L"" : L", ";
        marker.text += placed ? std::format(L"{}{} -> {} at ({:.1f}, {:.1f}, {:.1f}) m", comma, Name(key), Name(target),
                                            point.at.x / 100, point.at.y / 100, point.at.z / 100)
                              : std::format(L"{}{} -> {}", comma, Name(key), Name(target));
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

/// The member's tag (AActor::Tags[0], the name CharaRegistry_FindActorByName keys on and an AutoWalk act's Str[2]
/// names), or empty.
std::wstring MemberTag(const void* member) noexcept
try
{
    const std::vector<std::int64_t> tags = data_rows::Names(static_cast<const std::uint8_t*>(member) + facts::kActorTags, 1);
    return tags.empty() || tags[0] == 0 ? std::wstring() : Name(tags[0]);
}
catch (...)
{
    return {};
}

struct Leash
{
    bool         on = false;
    std::wstring text;
};

/// One AutoWalk act of the level's act tables, decoded once (a table's rows are fixed while it is loaded).
struct AutoWalk
{
    std::int64_t act = 0;  // its row name
    std::wstring object, state, target, lookAt;
    std::int32_t turnBack = 0, lookAtOn = 0, lookAtBand = 0;
    float        turnBackDistance = 0.0f;

    leash_rules::AutoWalkAct Rule() const noexcept { return {object, state, target, lookAt, turnBack, lookAtOn}; }
};

/// The level's AutoWalk acts, found by one walk of the act tables' own rows and kept until a table changes: another
/// object in a slot, another element array or row count (RE-data-table-rows.md: rows are built at the asset's load,
/// the slots emptied at its unload). A table that cannot be walked leaves the index incomplete for that key: it is
/// walked again only when the tables change, so a lasting fault never brings back a walk at every census.
struct AutoWalkIndex
{
    std::array<std::uintptr_t, 9> key{};  // per table: the object, its element array, its row count
    std::vector<AutoWalk>         acts;
    std::int32_t                  rows     = 0;  // act rows walked
    bool                          walked   = false;  // an attempt was made for `key`
    bool                          complete = false;  // every table walked
};
AutoWalkIndex g_autoWalks;

std::array<std::uintptr_t, 9> ActTablesKey() noexcept
{
    std::array<std::uintptr_t, 9> key{};
    for (std::uint32_t k = 0; k < 3; ++k)
    {
        const std::uint8_t* table = nullptr;
        if (!Read(reinterpret_cast<const void*>(data_rows::Tables(data_rows::Table::Action) + 8 * k), table) || table == nullptr)
        {
            continue;
        }
        std::uintptr_t elements = 0;
        std::int32_t   count    = 0;
        Read(table + facts::kDataObjectRows, elements);
        Read(table + facts::kDataObjectRowCount, count);
        key[3 * k]     = reinterpret_cast<std::uintptr_t>(table);
        key[3 * k + 1] = elements;
        key[3 * k + 2] = static_cast<std::uint32_t>(count);
    }
    return key;
}

AutoWalk Decode(std::int64_t name, const std::uint8_t* act)
{
    const std::uint8_t* strings = nullptr;
    std::int32_t        count   = 0;
    Read(act + facts::kActionRowStrings, strings);
    Read(act + facts::kActionRowStrings + 8, count);
    auto str = [&](std::uint32_t i) {
        return strings != nullptr && static_cast<std::int32_t>(i) < count ? Text(strings + 16 * i) : std::wstring();
    };
    const std::vector<std::int32_t> ints   = data_rows::Array<std::int32_t>(act + facts::kActionRowIntegers, facts::kAutoWalkIntLookAtBand + 1);
    const std::vector<float>        floats = data_rows::Array<float>(act + facts::kActionRowFloats, facts::kAutoWalkFltTurnBack + 1);
    auto num = [&](std::uint32_t i) { return i < ints.size() ? ints[i] : 0; };
    return AutoWalk{.act              = name,
                    .object           = str(facts::kAutoWalkStrObject),
                    .state            = str(facts::kAutoWalkStrState),
                    .target           = str(facts::kAutoWalkStrTarget),
                    .lookAt           = str(facts::kAutoWalkStrLookAt),
                    .turnBack         = num(facts::kAutoWalkIntTurnBack),
                    .lookAtOn         = num(facts::kAutoWalkIntLookAt),
                    .lookAtBand       = num(facts::kAutoWalkIntLookAtBand),
                    .turnBackDistance = floats.empty() ? 0.0f : floats[0]};
}

/// Rebuilds the index when the act tables changed; true if it did (logged with the acts found).
bool RefreshAutoWalks()
{
    const std::array<std::uintptr_t, 9> key = ActTablesKey();
    if (g_autoWalks.walked && key == g_autoWalks.key)
    {
        return false;  // walked for these tables already (completely or not: see AutoWalkIndex)
    }
    const auto    start = std::chrono::steady_clock::now();
    AutoWalkIndex index{.key = key, .walked = true, .complete = true};
    for (std::uint32_t k = 0; k < 3; ++k)
    {
        if (key[3 * k] == 0)
        {
            continue;
        }
        index.complete &= platform::ForEachSparseElement(key[3 * k] + facts::kDataObjectRows, facts::kActionElementBytes, facts::kDataObjectRowsMax,
                                       [&](std::uintptr_t element) {
                                           ++index.rows;
                                           const auto*  row  = reinterpret_cast<const std::uint8_t*>(element + facts::kDataObjectElementRow);
                                           std::int32_t type = 0;
                                           std::int64_t name = 0;
                                           // Kept only if the game's own lookup finds this very row: an earlier
                                           // table's row of the same name hides it, as FindRow takes the first.
                                           if (Read(row + facts::kActionRowType, type) && type == facts::kActionTypeAutoWalk &&
                                               Read(reinterpret_cast<const void*>(element), name) && name != 0 &&
                                               data_rows::Row(data_rows::Table::Action, name) == row)
                                           {
                                               index.acts.push_back(Decode(name, row));
                                           }
                                           return true;
                                       });
    }
    g_autoWalks   = std::move(index);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    std::wstring names;
    for (const AutoWalk& walk : g_autoWalks.acts)
    {
        names += std::format(L"{}{} ({} {})", names.empty() ? L"" : L", ", Name(walk.act), walk.target.empty() ? L"-" : walk.target,
                             walk.turnBack != 0 || walk.lookAtOn != 0 ? L"leashes" : L"speed only");
    }
    platform::Log(L"lead gate: the level's AutoWalk acts: {} of {} act rows [read in {} us]{}{}{}", g_autoWalks.acts.size(),
                  g_autoWalks.rows, us,
                  g_autoWalks.complete ? L"" : L" (a table's rows were unreadable: the acts found so far stand until the tables change)",
                  names.empty() ? L"" : L": ", names);
    return true;
}

/// Whether the state row `row` enters `act` (state_slots::EnteredActs).
bool Enters(const std::uint8_t* row, std::int64_t act)
{
    const std::vector<std::int64_t> acts = state_slots::EnteredActs(row);
    return std::find(acts.begin(), acts.end(), act) != acts.end();
}

/// The act and act-group tables were found: a leash can be read.
bool ActTablesFound() noexcept
{
    return data_rows::Installed(data_rows::Table::Action) && data_rows::Installed(data_rows::Table::ActionGroup);
}

/// A live AutoWalk act that leashes player 1 to the member (players/leash_rules.h; RE-lead-unlock.md). The search
/// runs from the level's few AutoWalk acts (the index) rather than from every active slot: an act aimed at the member
/// and armed is live when an active slot of its object holds its state and that state enters it, as the act runner
/// tests each tick. The same verdict as a walk over every slot's enter actions, at a few lookups a census
/// (such a walk makes some 15,000 on Ch. 4). Reads only; timed in its text.
Leash LeashOn(const std::vector<Slot>& slots, const std::wstring& tag, const std::wstring& memberObject) noexcept
try
{
    Leash leash;
    if (!ActTablesFound())
    {
        leash.text = L"leash not read (the act tables were not found at install)";
        return leash;
    }
    const auto start = std::chrono::steady_clock::now();
    RefreshAutoWalks();
    int aimed = 0;
    for (const AutoWalk& walk : g_autoWalks.acts)
    {
        const leash_rules::AutoWalkAct rule = walk.Rule();
        if (!leash_rules::AimedAndArmed(rule, tag, memberObject))
        {
            continue;
        }
        ++aimed;
        const auto object = static_cast<std::int64_t>(platform::FindName(walk.object.c_str()));
        const auto state  = static_cast<std::int64_t>(platform::FindName(walk.state.c_str()));
        if (object == 0 || state == 0)
        {
            continue;  // a name the pool does not hold: no slot holds it
        }
        const auto live = std::find_if(slots.begin(), slots.end(), [&](const Slot& s) {
            return s.object == object && s.state == state && s.row != nullptr && Enters(s.row, walk.act) &&
                   leash_rules::LeashesMember(rule, Name(s.object), Name(s.state), tag, memberObject);
        });
        if (live == slots.end())
        {
            continue;
        }
        leash.on   = true;
        leash.text = std::format(L"leash on the member: act {} of {} {} (measured to {}, looks at {}{}{})", Name(walk.act), walk.object,
                                 walk.state, walk.target.empty() ? L"nothing" : walk.target, walk.lookAt.empty() ? L"nothing" : walk.lookAt,
                                 walk.lookAtBand > 0 ? std::format(L" past {:.0f} m", walk.lookAtBand / 100.0) : std::wstring(),
                                 walk.turnBack != 0 ? std::format(L", turns player 1 back past {:.0f} m", walk.turnBackDistance / 100.0)
                                                    : std::wstring());
        break;
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    if (!leash.on)
    {
        leash.text = g_autoWalks.acts.empty() ? std::wstring(L"no leash on the member (no AutoWalk act in the level's act tables)")
                     : aimed == 0
                         ? std::format(L"no leash on the member ({} AutoWalk act(s) in the level, none aimed at it)", g_autoWalks.acts.size())
                         : std::format(L"no leash on the member ({} AutoWalk act(s) aimed at it, none running among {} active slots)",
                                       aimed, slots.size());
    }
    leash.text += std::format(L" [read in {} us]", us);
    return leash;
}
catch (...)
{
    return Leash{.text = L"the leash read raised an exception"};
}

/// The level object and state a move's arrival signal passes: the kind's handler holds them (engine_facts.h). False
/// when unknown or unreadable (logged once per field AI and kind at Debug, then at Trace).
bool MoveOf(const void* fieldAi, std::uint8_t kind, std::int64_t& object, std::int64_t& state) noexcept
{
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
    const auto* handler = static_cast<const std::uint8_t*>(ReadPointer(fieldAi, facts::kFieldAiHandlers + 8u * kind));
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
    return true;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto flag = platform::FindFunction(image, facts::kStoryFlagIsSet);
    if (!field_root::Installed() || !data_rows::Installed(data_rows::Table::Trigger) || !flag)
    {
        platform::Log(L"lead gate: an engine check failed; a LEAD or a run to a mark on a further player's member stays the player's");
        return false;
    }
    g_storyFlagIsSet = reinterpret_cast<StoryFlagIsSetFn>(*flag);
    if (!ActTablesFound())
    {
        platform::Log(L"lead gate: the act tables are out of the image; a leash on player 1 is not read (such a lead is judged "
                      L"by its waiters and the marker alone)");
    }
    if (const auto find = platform::FindFunction(image, facts::kFieldPointFindByName))
    {
        g_fieldPointFind = reinterpret_cast<FieldPointFindFn>(*find);
    }
    else
    {
        platform::Log(L"lead gate: the field point lookup failed its check; a move's destination is logged by name "
                      L"only");
    }
    control::Register({.name = L"lead.gate", .shape = control::Shape::Text,
                       .help = L"the last LEAD or run to a mark on a further player's member judged: its arrival trigger, the level's waiters, the story's "
                               L"marker, the member's own states ahead, and the verdict (guide: to its AI; staging: the player's)",
                       .get  = [] { return control::Value{Reading()}; }});
    control::Register({.name = L"lead.path", .shape = control::Shape::Text,
                       .help = L"walks a level state's chain as the lead gate does, from the state named: each "
                               L"state's setting, where every move goes, and the objective markers shown now with "
                               L"their positions (read only)",
                       .set  = [](const control::Value& value) {
                           const std::wstring* state = std::get_if<std::wstring>(&value);
                           std::uint8_t*       root  = field_root::Root();
                           if (state == nullptr || state->empty() || root == nullptr)
                           {
                               return control::Outcome{.ok = false, .detail = L"name a level state, in the field"};
                           }
                           const Marker marker = Markers(root, 0);
                           const Path   path   = PathAhead(*state, marker.points);
                           const std::wstring text = std::format(L"{}; markers: {}", path.text, marker.text);
                           platform::Log(L"lead gate: lead.path {}: {}", *state, text);
                           return control::Outcome{.detail = text};
                       }});
    // The experimental option (platform/settings.h kLeadHolder, `keeps` by default): the console, the file and the Co-op page
    // set it through the one setter.
    g_keeps = platform::settings::Of(platform::settings::kLeadHolder).word == L"keeps";
    control::RegisterOption(
        platform::settings::kLeadHolder,
        [](const std::wstring& word) {
            g_keeps = word == L"keeps";
            platform::Log(L"lead gate: lead-holder {} (the console)", word);
            return control::Outcome{};
        },
        [] { return std::wstring(g_keeps ? L"keeps" : L"guide"); });
    platform::Log(L"lead gate: installed; a LEAD or a run to a mark on a further player's member is needed finished when a level step "
                  L"waits on its arrival, the story's marker rides on it, a further move is ahead in its own chain of states, or the "
                  L"level leashes player 1 to it; such a move {} (lead-holder {}, experimental, from {})",
                  g_keeps ? L"stays the player's, its point marked, its AI finishing the arrival at the point"
                          : L"goes to the member's AI with the player's view on it",
                  g_keeps ? L"keeps" : L"guide", platform::settings::Why(platform::settings::kLeadHolder));
    return true;
}

Verdict Judge(const void* member, const void* fieldAi, std::uint8_t kind) noexcept
try
{
    // Not installed, or a kind whose arrival the level cannot wait on (field_hold.h names them): staging.
    if (g_storyFlagIsSet == nullptr || !field_hold::SignalsArrival(kind))
    {
        return Verdict::Staging;
    }
    std::int64_t object = 0, state = 0;
    if (!MoveOf(fieldAi, kind, object, state))
    {
        return Verdict::Staging;
    }
    if (object == 0)
    {
        platform::LogTrace(L"lead gate: the {} on the level layer names no level object (a state-manager command's run): no arrival "
                           L"signal, staging",
                           field_hold::KindName(kind));
        return Verdict::Staging;
    }
    Judgment&  last = JudgmentOf(object);
    Remember(member, object);
    const bool same = last.object == object && last.state == state && last.kind == kind;
    if (same && lead_keep_rules::ToAi(last.verdict))
    {
        return last.verdict;  // a guide, or brought to its mark: holds until the setting ends (its state changes)
    }
    std::uint8_t* root = field_root::Root();
    std::uint8_t* sm   = field_root::StateManager();
    if (root == nullptr || sm == nullptr)
    {
        return Verdict::Staging;
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
    Leash        leash;
    if (census)
    {
        const auto              start = std::chrono::steady_clock::now();
        const std::vector<Slot> slots = state_slots::Active(sm);
        waiters                       = Waiters(slots, object, state, marker.points, member);
        waiters.text += std::format(L" [slots and waiters read in {} us]",
                                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        leash           = LeashOn(slots, MemberTag(member), Name(object));
        last.censusTick = last.calls;
        last.waiter     = waiters.others > 0 || waiters.chain;
        last.leash      = leash.on;
        last.signals    = waiters.trigger != 0;
        last.ownBox     = waiters.ownBox;
        if (waiters.read)
        {
            last.destination = waiters.destination;
            last.pointKnown  = last.destination != 0 && PointLocation(last.destination, last.point);
        }
    }
    const bool wasMarker = last.marker;
    last.marker          = marker.onMember;
    const bool needed    = last.waiter || last.marker || last.leash;
    // How far the member stands from the point (2D, as the game measures its arrival), while it may be handed there.
    float      away  = -1.0f;
    const bool keeps     = g_keeps && g_marking;
    const bool measuring = needed && keeps && last.pointKnown && last.signals && !last.reached;
    const bool starting  = !same && keeps;
    GameVector at;
    const bool placed = (measuring || starting) && world::actor_place::ActorLocation(member, at);
    if (measuring && placed)
    {
        const float dx = at.x - last.point.x, dy = at.y - last.point.y;
        away           = std::hypot(dx, dy);
        last.reached   = lead_keep_rules::Reached(dx, dy);
        // A spot on the way only the companion's AI may take: no player can, and the level opens it to everyone only
        // after the companions took it, so the move would wait for ever on a player's member.
        world::traversal_walkup::AiOnlySpot spot;
        if (!last.reached && world::traversal_walkup::AiOnlySpotNear(at, lead_keep_rules::kCrossingRadius, spot))
        {
            last.reached  = true;
            last.crossing = std::format(L"{} {}", spot.kind, spot.tag);
            away          = spot.distance;
        }
    }
    if (starting && placed)
    {
        // Which AI-only spot the move starts near, if any: a crossing too far from the spot's actor for the radius shows
        // here first.
        world::traversal_walkup::AiOnlySpot spot;
        platform::LogDebug(L"lead gate: {}'s kept move starts {}", Name(object),
                           world::traversal_walkup::AiOnlySpotNear(at, kAiOnlySpotCensus, spot)
                               ? std::format(L"{:.1f} m from the {} {}, which only a companion's AI may take now", spot.distance / 100,
                                             spot.kind, spot.tag)
                               : std::format(L"with no AI-only spot within {:.0f} m", kAiOnlySpotCensus / 100));
    }
    const Verdict was = last.verdict;
    last.verdict      = lead_keep_rules::Decide(needed, keeps, last.pointKnown, last.signals, last.reached, last.ownBox != 0);
    if (!same || census || last.marker != wasMarker || last.verdict != was)
    {
        g_reading = std::format(L"{} on object {} state {}: {}; {}; marker {}: {}; point {}; verdict {}", field_hold::KindName(kind),
                                Name(object), Name(state), census ? waiters.text : L"waiters as before",
                                census ? leash.text : L"leash as before", marker.onMember ? L"on the member" : L"elsewhere", marker.text,
                                last.pointKnown ? std::format(L"{} at ({:.1f}, {:.1f}, {:.1f}) m", Name(last.destination), last.point.x / 100,
                                                              last.point.y / 100, last.point.z / 100)
                                                : last.destination != 0 ? std::format(L"{} (not a field point)", Name(last.destination))
                                                                        : std::wstring(L"none named"),
                                lead_keep_rules::Name(last.verdict));
        last.reading = g_reading;
        if (!same || last.verdict != was)
        {
            platform::Log(L"lead gate: {}", g_reading);
        }
    }
    if (last.verdict == Verdict::AtMark && was != Verdict::AtMark && !last.crossing.empty())
    {
        platform::Log(L"lead gate: {} brought within {:.1f} m of the {} (the crossing radius {:.1f} m), which only its AI may take "
                      L"now (the level's NPC-only state): to its AI, which takes it and finishes the move to {}; back to its "
                      L"player at the level's next setting",
                      Name(object), away / 100, last.crossing, lead_keep_rules::kCrossingRadius / 100, Name(last.destination));
    }
    else if (last.verdict == Verdict::AtMark && was != Verdict::AtMark)
    {
        platform::Log(L"lead gate: {} brought within {:.1f} m of its point {} (the hand radius {:.0f} m): to its AI, which walks the "
                      L"last steps and signals the arrival; back to its player at the level's next setting",
                      Name(object), away / 100, Name(last.destination), lead_keep_rules::kHandRadius / 100);
    }
    return last.verdict;
}
catch (...)
{
    platform::Log(L"lead gate: the judgment raised an exception; staging assumed");
    return Verdict::Staging;
}

namespace {

/// `member`'s current level move (a LEAD or a run to a mark on the level layer; `object` 0 for a state-manager command's
/// run), as the hold judges its state: an interest scene on top leaves the move beneath it.
bool CurrentMove(const void* member, std::uint8_t& kind, std::int64_t& object, std::int64_t& state) noexcept
{
    const auto*        ai = party_member::FieldAiController(member);
    field_hold::Judged judged;
    if (ai == nullptr || !party_member::JudgedState(member, judged) ||
        field_hold::Of(judged.layer, judged.kind) != field_hold::Hold::Arrival)
    {
        return false;
    }
    kind = judged.kind;
    return MoveOf(ai, kind, object, state);
}

/// The judgment of `object` set to the scene lead-in `state` the member's field AI runs as `kind`, logged at Info when the
/// state is new to it.
void NoteSceneLeadIn(std::int64_t object, std::int64_t state, std::uint8_t kind) noexcept
try
{
    Judgment& j = JudgmentOf(object);
    if (j.object == object && j.state == state && j.leadIn)
    {
        return;
    }
    j = Judgment{.object = object, .state = state, .kind = kind, .verdict = Verdict::Guide, .leadIn = true, .touched = j.touched};
    j.reading = std::format(L"{} on object {} is a scene lead-in, excluded from the lead rule by name; to its AI, which plays it "
                            L"through to the scene with the player's view on it",
                            Name(state), Name(object));
    g_reading = j.reading;
    platform::Log(L"lead gate: {}", j.reading);
}
catch (...)
{
    platform::LogDebug(L"lead gate: the scene lead-in's note raised; the hold stands");
}

/// The judgment of `object` is a scene lead-in's.
bool LeadInNoted(std::int64_t object) noexcept
{
    for (const Judgment& j : g_judged)
    {
        if (j.object == object && j.leadIn)
        {
            return true;
        }
    }
    return false;
}

/// The current state of level object `object` among the state manager's active slots, stopping at its slot. False when
/// the object has no active slot or a read fails.
bool StateOfObject(const std::uint8_t* sm, std::int64_t object, std::int64_t& state) noexcept
{
    bool read = false;
    state_slots::ForEach(sm, [&](const std::uint8_t* slot) noexcept {
        std::int64_t holder = 0;
        if (!Read(slot + facts::kStateSlotObject, holder) || holder != object)
        {
            return false;
        }
        read = Read(slot + facts::kStateSlotState, state);
        return true;
    });
    return read;
}

}  // namespace

bool SceneLeadIn(const void* member) noexcept
try
{
    if (g_storyFlagIsSet == nullptr)
    {
        return false;  // not installed: no state read
    }
    // The run: its state is on the member's own arrival handler.
    std::uint8_t kind   = 0;
    std::int64_t object = 0, state = 0;
    if (CurrentMove(member, kind, object, state) && object != 0)
    {
        if (!lead_keep_rules::SceneLeadIn(Name(state)))
        {
            if (object == RememberedObject(member) && LeadInNoted(object))
            {
                Forget(member);  // a move off the list after a lead-in: the lead-in is over
            }
            return false;
        }
        Remember(member, object);
        NoteSceneLeadIn(object, state, kind);
        return true;
    }
    // The stand after it: the object's own slot, read only while the member's judgment is a lead-in's (no slot walk
    // otherwise; a stand met with no run before it is the player's). A state off the list ends the lead-in: its judgment
    // goes, so the walk stops with it whichever way the hold ends. A slot not read this check (a read that failed, a
    // slot between states) leaves the hold standing until a state is read.
    object = RememberedObject(member);
    if (object == 0 || !LeadInNoted(object))
    {
        return false;
    }
    const std::uint8_t* sm = field_root::StateManager();
    if (sm == nullptr || !StateOfObject(sm, object, state))
    {
        return true;
    }
    if (!lead_keep_rules::SceneLeadIn(Name(state)))
    {
        Forget(member);
        return false;
    }
    field_hold::Judged judged;
    NoteSceneLeadIn(object, state, party_member::JudgedState(member, judged) ? judged.kind : field_hold::kEmpty);
    return true;
}
catch (...)
{
    platform::LogDebug(L"lead gate: the scene lead-in's read raised; the member is judged by the rule this check");
    return false;
}

bool OnLevelMove(const void* member) noexcept
try
{
    std::uint8_t kind   = 0;
    std::int64_t object = 0, state = 0;
    return CurrentMove(member, kind, object, state);
}
catch (...)
{
    return false;
}

bool KeptMark(const void* member, Mark& out) noexcept
try
{
    std::uint8_t kind   = 0;
    std::int64_t object = 0, state = 0;
    if (!CurrentMove(member, kind, object, state) || object == 0)
    {
        return false;
    }
    for (const Judgment& j : g_judged)
    {
        if (j.object == object && j.state == state && j.kind == kind && j.verdict == Verdict::Keep && j.pointKnown)
        {
            out = Mark{.at = {j.point.x, j.point.y, j.point.z}, .point = j.destination};
            return true;
        }
    }
    return false;
}
catch (...)
{
    return false;
}

bool OwnBoxRun(const void* member, Mark& point, std::int64_t& box) noexcept
try
{
    std::uint8_t kind   = 0;
    std::int64_t object = 0, state = 0;
    if (!CurrentMove(member, kind, object, state) || object == 0)
    {
        return false;
    }
    for (const Judgment& j : g_judged)
    {
        if (j.object == object && j.state == state && j.kind == kind && j.ownBox != 0 && j.pointKnown)
        {
            point = Mark{.at = {j.point.x, j.point.y, j.point.z}, .point = j.destination};
            box   = j.ownBox;
            return true;
        }
    }
    return false;
}
catch (...)
{
    return false;
}

std::wstring PointName(std::int64_t point) noexcept
{
    return Name(point);
}

void SetMarking(bool on) noexcept
{
    g_marking = on;
}

void Forget(const void* member) noexcept
{
    for (MemberObject& m : g_memberObjects)
    {
        if (member == nullptr || m.member != member)
        {
            continue;
        }
        for (Judgment& j : g_judged)
        {
            if (m.object != 0 && j.object == m.object)
            {
                j = Judgment{.touched = j.touched};
            }
        }
        m = MemberObject{};
    }
}

void ForgetAll() noexcept
{
    for (Judgment& j : g_judged)
    {
        j = Judgment{.touched = j.touched};
    }
    g_memberObjects = {};
}

std::wstring Reading() noexcept
{
    return g_reading;
}

std::wstring ReadingOf(const void* member) noexcept
try
{
    std::uint8_t kind   = 0;
    std::int64_t object = 0, state = 0;
    if (CurrentMove(member, kind, object, state))
    {
        for (const Judgment& j : g_judged)
        {
            if (j.object == object && j.state == state && j.kind == kind && !j.reading.empty())
            {
                return j.reading;
            }
        }
    }
    // A scene lead-in's stand: no move on the member, the judgment is its object's.
    object = RememberedObject(member);
    if (object != 0)
    {
        for (const Judgment& j : g_judged)
        {
            if (j.object == object && j.leadIn && !j.reading.empty())
            {
                return j.reading;
            }
        }
    }
    return std::format(L"no judgment of this move; the last judged: {}", g_reading);
}
catch (...)
{
    return L"the judgment could not be read";
}

}  // namespace lss::players::lead_gate
