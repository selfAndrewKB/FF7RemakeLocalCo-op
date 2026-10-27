#include "players/lead_gate.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_hold.h"
#include "players/field_root.h"
#include "players/state_path.h"

#include <Unreal/NameTypes.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace lss::players::lead_gate {

namespace {

using platform::Read;
using platform::ReadPointer;

using StoryFlagIsSetFn = char (*)(const std::int64_t* flag);
using FindRowFn        = void* (*)(void* table, std::int64_t name);
using FieldPointFindFn = const std::uint8_t* (*)(void* points, const std::int64_t* name);

// A staging lead's or run's waiters are read again this often (the marker each call).
constexpr std::uint32_t kCensusEveryTicks = 60;
constexpr std::uint32_t kMaxIconRecords   = 64;
constexpr std::size_t   kMaxNameChars     = 96;
constexpr std::size_t   kMaxStateChars    = 1024;  // a next-state name (the FName limit): read whole, or its lookup misses
// How many of the member's own states ahead are walked for a further move.
constexpr std::size_t   kPathLinks        = 16;
constexpr std::size_t   kPathNexts        = 8;     // next states read per row (the game reads 8 triggers per row)

std::uintptr_t   g_triggerTables     = 0;
std::uintptr_t   g_stateChangeTables = 0;
StoryFlagIsSetFn g_storyFlagIsSet    = nullptr;
FieldPointFindFn g_fieldPointFind    = nullptr;

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
    // Another waiter, a direct set on another object, or a further move in the member's own chain.
    bool          waiter     = false;
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
std::wstring Text(const std::uint8_t* fstring, std::size_t cap = kMaxNameChars) noexcept
{
    const wchar_t* data = nullptr;
    std::int32_t   num  = 0;
    if (!Read(fstring, data) || !Read(fstring + 8, num) || data == nullptr || num <= 1)
    {
        return L"";
    }
    const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(num) - 1, cap);
    std::wstring      text(count, L'\0');
    if (!platform::CopyGuarded(text.data(), data, count * sizeof(wchar_t)))
    {
        return L"";
    }
    return text;
}

/// An FName array field (`ptr` at `base`, `num` at `base + 8`): its elements, capped. May throw (allocation): every
/// caller catches.
std::vector<std::int64_t> Names(const std::uint8_t* base, std::uint32_t cap)
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

/// The row named `name` in one of three data tables at `tables`, through the tables' own lookup (the virtual
/// StateMgr_FireStateTrigger and StateSlot_SetState call).
const std::uint8_t* TableRow(std::uintptr_t tables, std::int64_t name) noexcept
{
    for (std::uint32_t k = 0; k < 3; ++k)
    {
        void* table = nullptr;
        if (!Read(reinterpret_cast<const void*>(tables + 8 * k), table) || table == nullptr)
        {
            continue;
        }
        void* vtable = ReadPointer(table, 0);
        auto  find   = reinterpret_cast<FindRowFn>(ReadPointer(vtable, facts::kDataTableFindRowSlot));
        void* row    = nullptr;
        if (find != nullptr && platform::CallGuarded(find, row, table, name) && row != nullptr)
        {
            return static_cast<const std::uint8_t*>(row);
        }
    }
    return nullptr;
}

/// The FName of an existing name's text (0 if the pool has no such name; never adds one); a free function for the
/// guard.
void FindName(const wchar_t* text, std::int64_t* out) noexcept
try
{
    const RC::Unreal::FName name(text, RC::Unreal::FNAME_Find);
    *out = (static_cast<std::int64_t>(name.GetNumber()) << 32) | static_cast<std::int64_t>(name.GetComparisonIndex().ToUnstableInt());
}
catch (...)
{
    *out = 0;
}

/// The state row named `state` (the game's state-change tables); null if unknown.
const std::uint8_t* StateRow(const std::wstring& state) noexcept
{
    std::int64_t name = 0;
    if (state.empty() || !platform::CallGuarded(&FindName, state.c_str(), &name) || name == 0)
    {
        return nullptr;
    }
    return TableRow(g_stateChangeTables, name);
}

/// A row's next states (NextStateChange, FStrings of 16 bytes), capped; the empty ones skipped.
std::vector<std::wstring> NextStates(const std::uint8_t* row) noexcept
{
    std::vector<std::wstring> out;
    const std::uint8_t*       data = nullptr;
    std::int32_t              num  = 0;
    if (!Read(row + facts::kStateRowNextStates, data) || !Read(row + facts::kStateRowNextStates + 8, num) || data == nullptr)
    {
        return out;
    }
    for (std::int32_t i = 0; i < num && static_cast<std::size_t>(i) < kPathNexts; ++i)
    {
        std::wstring next = Text(data + 16 * i, kMaxStateChars);
        if (!next.empty())
        {
            out.push_back(std::move(next));
        }
    }
    return out;
}

/// A world location (UE centimetres), as the game's FVector lays it out.
struct Vec3
{
    float x = 0, y = 0, z = 0;
};

/// A shown objective marker's target and where the game draws it (the record's shown position, world centimetres).
struct MarkerPoint
{
    std::int64_t target = 0;
    Vec3         at;
};

/// A named point's world location: the level's field point of that name, the lookup the objective marker makes
/// first (RE-lead-arrival-wait.md §6); false when the name is no field point of the loaded level.
bool PointLocation(std::int64_t name, Vec3& out) noexcept
{
    std::uint8_t*       root      = field_root::Root();
    void*               points    = root != nullptr ? ReadPointer(root, facts::kRootFieldPoints) : nullptr;
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
    Vec3 at;
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
    const std::uint8_t* row = StateRow(name);
    if (row == nullptr)
    {
        return std::nullopt;
    }
    state_path::Row out{.status = Text(row + facts::kStateRowStatus), .next = NextStates(row)};
    const std::vector<std::int64_t> args = Names(row + facts::kStateRowStringArgs, facts::kStateRowDestinationArg + 1);
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

/// A number array field (`ptr` at `base`, `num` at `base + 8`) of `T`, capped. May throw (allocation): every caller
/// catches.
template <typename T>
std::vector<T> Numbers(const std::uint8_t* base, std::uint32_t cap)
{
    static_assert(std::is_trivially_copyable_v<T>, "copied as raw bytes");
    const std::uint8_t* data = nullptr;
    std::int32_t        num  = 0;
    std::vector<T>      out;
    if (!Read(base, data) || !Read(base + 8, num) || data == nullptr || num <= 0)
    {
        return out;
    }
    out.resize(std::min<std::uint32_t>(static_cast<std::uint32_t>(num), cap));
    if (!platform::CopyGuarded(out.data(), data, out.size() * sizeof(T)))
    {
        out.clear();
    }
    return out;
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
    const std::uint8_t* row = StateRow(state);
    if (row == nullptr)
    {
        return L"row unread";
    }
    std::wstring                    text;
    const std::vector<std::int64_t> triggers = Names(row + facts::kStateRowTriggers, facts::kStateRowTriggersRead);
    const std::uint8_t*             nexts    = nullptr;
    std::int32_t                    count    = 0;
    Read(row + facts::kStateRowNextStates, nexts);
    Read(row + facts::kStateRowNextStates + 8, count);
    for (std::size_t i = 0; i < triggers.size() && triggers[i] != 0; ++i)
    {
        const bool          known   = nexts != nullptr && static_cast<std::int32_t>(i) < count;
        const std::wstring  next    = known ? Text(nexts + 16 * i, kMaxStateChars) : L"?";
        const std::uint8_t* trigger = TableRow(g_triggerTables, triggers[i]);
        std::wstring        detail  = L"row NOT found";
        if (trigger != nullptr)
        {
            std::int32_t scope = -1;
            Read(trigger + facts::kTriggerRowScope, scope);
            detail = std::format(L"scope {}", scope);
            const std::vector<std::uint8_t> types = Numbers<std::uint8_t>(trigger + facts::kTriggerRowSpecialTypes, 8);
            if (!types.empty())
            {
                detail += L", special types";
                for (const std::uint8_t type : types)
                {
                    detail += std::format(L" {}", SpecialTypeName(type));
                }
            }
            const std::vector<std::int64_t> sends = Names(trigger + facts::kTriggerRowSendObjects, 4);
            for (std::size_t s = 0; s < sends.size(); ++s)
            {
                detail += std::format(L"{}{}", s == 0 ? L", sends to " : L" ", Name(sends[s]));
            }
        }
        text += std::format(L"{}{} ({}) -> {}", i == 0 ? L"" : L", ", Name(triggers[i]), detail, next.empty() ? L"None" : next);
    }
    if (text.empty())
    {
        text = L"no trigger";
    }
    const std::vector<std::int64_t> actions = Names(row + facts::kStateRowEnterActions, 4);
    for (std::size_t a = 0; a < actions.size(); ++a)
    {
        text += std::format(L"{}{}", a == 0 ? L"; starts " : L" ", Name(actions[a]));
    }
    const std::vector<std::int64_t> strings = Names(row + facts::kStateRowStringArgs, 12);
    const std::vector<std::int32_t> ints    = Numbers<std::int32_t>(row + facts::kStateRowIntArgs, 16);
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
    bool         read    = false;
    std::int64_t trigger = 0;
    bool         self    = false;  // the member's own step waits (always, with a trigger)
    bool         chain   = false;  // a further move ahead in the member's own chain of states: the level walks it on
    int          others  = 0;      // other waiters (scope) plus direct sets on other objects
    std::wstring text;
};

/// The waiters on the setting's arrival trigger: the slots whose current state lists it (in the trigger's scope), and
/// the trigger row's own direct sets on other objects. RE-lead-arrival-wait.md §3.
Census Waiters(const std::uint8_t* sm, std::int64_t object, std::int64_t state,
               const std::vector<MarkerPoint>& markers) noexcept
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
    // Where this move goes (logging).
    census.text = std::format(L"; this move goes to {}", Where(args[facts::kStateRowDestinationArg], markers));
    // The trigger row: its scope and direct sets.
    const std::uint8_t* trigger = TableRow(g_triggerTables, census.trigger);
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
            const bool         known = nextStates != nullptr && static_cast<std::int32_t>(i) < nextCount;
            const std::wstring next  = known ? Text(nextStates + 16 * i, kMaxStateChars) : L"?";
            if (&s == self)
            {
                census.self     = true;
                const Path path = PathAhead(known ? next : L"", markers);
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
                              slots.size(), census.text);
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

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tables = image.Rebase(facts::kStateTriggerTablesGlobal, 3 * sizeof(void*));
    const auto states = image.Rebase(facts::kStateChangeTablesGlobal, 3 * sizeof(void*));
    const auto flag   = platform::FindFunction(image, facts::kStoryFlagIsSet);
    if (!field_root::Installed() || !tables || !states || !flag)
    {
        platform::Log(L"lead gate: an engine check failed; a LEAD or a run to a mark on a further player's member stays the player's");
        return false;
    }
    g_triggerTables     = *tables;
    g_stateChangeTables = *states;
    g_storyFlagIsSet    = reinterpret_cast<StoryFlagIsSetFn>(*flag);
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
    platform::Log(L"lead gate: installed; a LEAD or a run to a mark on a further player's member goes to its AI when a level step "
                  L"waits on its arrival, the story's marker rides on it, or a further move is ahead in its own chain of states");
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
        waiters         = Waiters(sm, object, state, marker.points);
        last.censusTick = last.calls;
        last.waiter     = waiters.others > 0 || waiters.chain;
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
