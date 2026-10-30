#include "world/story_boxes.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_root.h"
#include "players/lead_gate.h"
#include "players/party_member.h"
#include "players/scopes.h"
#include "players/seat_camera.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "world/field_targets.h"
#include "world/story_box_rules.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace lss::world::story_boxes {

namespace {

using platform::CallGuarded;
using platform::Read;
using platform::ReadPointer;
using RC::Unreal::UClass;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;
namespace rules          = story_box_rules;

using BeginFn      = std::int64_t (*)(void* points, std::uint64_t box, void* collider);
using EndFn        = void (*)(void* points, std::uint64_t box, void* collider);
using ActorByTagFn = void* (*)(void* points, const std::uint64_t* tag);
using NameMapFindFn = std::int32_t* (*)(const void* map, std::int32_t* out, std::uint64_t name);
using FindRowFn    = void* (*)(const void* tables, std::uint64_t name);
using SlotIndexFn  = std::int32_t (*)(void* stateManager, const std::uint64_t* object, std::int32_t attribute);
using BaseLoadedFn = std::uint8_t (*)(void* stateManager, std::int32_t index);

constexpr std::size_t kMaxWaiting = 32;  // more held-back enters than this at once: the next ones are the game's at once

platform::EntryRedirect g_redirect;
std::uintptr_t          g_at          = 0;
bool                    g_pending     = false;
bool                    g_installed   = false;
std::atomic<bool>       g_on{true};       // the console's switch (game thread); Remove may clear it from any thread
std::atomic<bool>       g_placed{false};
std::atomic<std::size_t> g_waitingCount{0};  // g_waiting's size, for Remove's line on any thread
EndFn                   g_end         = nullptr;
ActorByTagFn            g_actorByTag  = nullptr;
NameMapFindFn           g_nameMapFind = nullptr;
FindRowFn               g_findRow     = nullptr;
SlotIndexFn             g_slotIndex   = nullptr;
BaseLoadedFn            g_baseLoaded  = nullptr;
const void*             g_layoutMap   = nullptr;  // g_Map_LayoutNodeName_ToObject (the TMap itself)
const void*             g_layoutRows  = nullptr;  // the three ObjectLayout tables
const void*             g_groupRows   = nullptr;  // the three ActorGroup tables
UClass*                 g_boxClass    = nullptr;  // /Script/Engine.BoxComponent, found on the first hold-back
bool                    g_boxClassAsked = false;

/// Each seat's member while its player is in (game thread).
std::array<void*, players::kSeatsBuilt + 1> g_members{};

/// An enter held back: the box, the member and seat, the box's shape when it is a box component.
struct Waiting
{
    int                         seat = 0;
    players::weak_object::Weak  member;
    players::weak_object::Weak  boxActor;
    std::uint64_t               box    = 0;  // the box's tag (its layout node name)
    std::uint64_t               object = 0;  // its layout object (the state manager's name)
    std::uint64_t               memberTag = 0;
    rules::Box                  shape{};
    bool                        shaped = false;
};
std::vector<Waiting> g_waiting;

// Counts for the reading.
std::uint32_t g_heldBack = 0, g_replayed = 0, g_replayedOff = 0, g_lost = 0, g_overflow = 0;

std::wstring NameOf(std::uint64_t name)
{
    return world::field_targets::NameOf(name);
}

/// An actor's first tag (0 without one).
std::uint64_t FirstTag(const void* actor) noexcept
{
    const auto*  tags  = static_cast<const std::uint8_t*>(actor) + facts::kActorTags;
    std::int32_t count = 0;
    std::uint64_t tag  = 0;
    return Read(tags + sizeof(void*), count) && count > 0 && Read(ReadPointer(tags, 0), tag) ? tag : 0;
}

void* FieldPoints() noexcept
{
    return ReadPointer(players::field_root::Root(), facts::kRootFieldPoints);
}

/// The box's layout object and its actor group (0: none). False when the box has no layout row.
bool LayoutOf(std::uint64_t box, std::uint64_t& object, std::uint64_t& group) noexcept
{
    std::int32_t  index = -1;
    std::int32_t* out   = nullptr;
    object = group = 0;
    if (!CallGuarded(g_nameMapFind, out, g_layoutMap, &index, box) || index < 0)
    {
        return false;
    }
    const auto* pairs = static_cast<const std::uint8_t*>(ReadPointer(g_layoutMap, 0));
    if (pairs == nullptr ||
        !Read(pairs + static_cast<std::size_t>(index) * facts::kLayoutObjectPairBytes + facts::kLayoutObjectPairValue, object) ||
        object == 0)
    {
        return false;
    }
    void* row = nullptr;
    return CallGuarded(g_findRow, row, g_layoutRows, object) && row != nullptr &&
           Read(static_cast<const std::uint8_t*>(row) + facts::kLayoutRowActorGroup, group);
}

/// The group's ActorList holds `tag` (as the enter's own test: at most the first 32). False when the row is missing.
bool GroupLists(std::uint64_t group, std::uint64_t tag) noexcept
{
    void* row = nullptr;
    if (tag == 0 || !CallGuarded(g_findRow, row, g_groupRows, group) || row == nullptr)
    {
        return false;
    }
    const auto*   r     = static_cast<const std::uint8_t*>(row);
    const auto*   data  = static_cast<const std::uint64_t*>(ReadPointer(r, facts::kActorGroupRowActors));
    std::int32_t  count = 0;
    if (data == nullptr || !Read(r + facts::kActorGroupRowActorsNum, count))
    {
        return false;
    }
    for (std::int32_t i = 0; i < count && i < facts::kActorGroupActorsRead; ++i)
    {
        std::uint64_t name = 0;
        if (Read(data + i, name) && name == tag)
        {
            return true;
        }
    }
    return false;
}

/// The group names the party leader: the default leader's tag (player 1's home character), and the current
/// leader's while no seat's leader scope is open (the battle leader option may have moved it).
bool GroupNamesLeader(std::uint64_t group) noexcept
{
    if (GroupLists(group, FirstTag(players::field_root::PartyLeader(facts::kLeaderDefault))))
    {
        return true;
    }
    return players::scopes::OpenPlayer(players::scopes::Leader) == 0 &&
           GroupLists(group, FirstTag(players::field_root::PartyLeader(facts::kLeaderCurrent)));
}

bool IsBoxComponentRaw(void* component)
{
    return static_cast<UObject*>(component)->IsA(g_boxClass);
}

/// The box actor's collision component as a placed box (its world transform and half-size). False when it is not a
/// box component or cannot be read.
bool ShapeOf(void* boxActor, rules::Box& out) noexcept
{
    if (!g_boxClassAsked)
    {
        g_boxClassAsked = true;
        try
        {
            g_boxClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.BoxComponent"));
        }
        catch (...)
        {
            g_boxClass = nullptr;
        }
        if (g_boxClass == nullptr)
        {
            platform::LogWarning(L"story boxes: the box component class was not found; a held-back box fires when the level "
                                 L"moves the member or its player leaves, not when player 1 comes to it");
        }
    }
    void* component = ReadPointer(boxActor, facts::kTriggerCollisionComponent);
    bool  isBox     = false;
    if (g_boxClass == nullptr || component == nullptr || !CallGuarded(&IsBoxComponentRaw, isBox, component) || !isBox)
    {
        return false;
    }
    const auto* c = static_cast<const std::uint8_t*>(component);
    return platform::CopyGuarded(out.rotation, c + facts::kComponentWorldRotation, sizeof(out.rotation)) &&
           platform::CopyGuarded(out.at, c + facts::kComponentWorldLocation, sizeof(out.at)) &&
           platform::CopyGuarded(out.scale, c + facts::kComponentWorldScale, sizeof(out.scale)) &&
           platform::CopyGuarded(out.extent, c + facts::kBoxComponentExtent, sizeof(out.extent));
}

/// What the game's enter does for the box now (its own reads, before the replay calls it).
enum class Sends : std::uint8_t
{
    Trigger,    // its trigger is sent (the group lists the member)
    Off,        // the box is switched off: the member is recorded inside, nothing is sent
    NoTrigger,  // the object has no trigger state
    NotLoaded,  // the object's states are not loaded
};

Sends WouldSend(std::uint64_t object) noexcept
{
    void*        manager = players::field_root::StateManager();
    std::int32_t base = -1, slotIndex = -1;
    std::uint8_t loaded = 0;
    if (manager == nullptr || !CallGuarded(g_slotIndex, base, manager, &object, 0) ||
        !CallGuarded(g_baseLoaded, loaded, manager, base) || loaded == 0)
    {
        return Sends::NotLoaded;
    }
    if (!CallGuarded(g_slotIndex, slotIndex, manager, &object, 1) || slotIndex < 0 ||
        static_cast<std::uint32_t>(slotIndex) >= facts::kStateManagerSlotCount)
    {
        return Sends::NoTrigger;
    }
    const auto* slot = static_cast<const std::uint8_t*>(
        ReadPointer(static_cast<const std::uint8_t*>(manager) + facts::kStateManagerSlots, static_cast<std::uint32_t>(slotIndex) * 8u));
    if (slot == nullptr)
    {
        return Sends::NoTrigger;
    }
    const void*  linked = ReadPointer(ReadPointer(slot, facts::kStateSlotLinkedBase), 0);
    std::uint8_t off    = 0;
    Read((linked != nullptr ? static_cast<const std::uint8_t*>(linked) : slot) + facts::kStateSlotOffByte, off);
    return off == 1 ? Sends::Off : Sends::Trigger;
}

/// The replay line's account of what was sent, given whether the member is still inside.
const wchar_t* SendsText(Sends sends, bool inside) noexcept
{
    switch (sends)
    {
    case Sends::Trigger:
        return L"its trigger sent";
    case Sends::Off:
        return inside ? L"the box is switched off: nothing sent now; the member is recorded inside, so the box fires for it "
                        L"when it is switched on, as the game does for anyone inside"
                      : L"the box is switched off and the member is outside: nothing sent (POSSIBLY LOST: a line it would "
                        L"start when switched on will not, as for a companion that passed it while off)";
    case Sends::NoTrigger:
        return L"the box has no trigger state: nothing to send";
    case Sends::NotLoaded:
        return L"the box's object is not loaded: nothing sent (POSSIBLY LOST)";
    }
    return L"?";
}

/// The member's own component holds the box in its key set (it is inside now).
bool Inside(const void* member, std::uint64_t box) noexcept
{
    const auto*  component = static_cast<const std::uint8_t*>(member) + facts::kCharacterOverlapComponent;
    const void*  heap      = ReadPointer(component, facts::kOverlapKeysHeap);
    const auto*  keys      = static_cast<const std::uint64_t*>(heap != nullptr ? heap : component + facts::kOverlapKeysInline);
    std::int32_t count     = 0;
    if (!Read(component + facts::kOverlapKeysNum, count))
    {
        return false;
    }
    for (std::int32_t i = 0; i < count && i < 256; ++i)
    {
        std::uint64_t key = 0;
        if (Read(keys + i, key) && key == box)
        {
            return true;
        }
    }
    return false;
}

int SeatOf(const void* collider) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (g_members[static_cast<std::size_t>(seat)] == collider)
        {
            return seat;
        }
    }
    return 0;
}

bool HeldByPlayer(const void* member) noexcept
{
    void* controller = nullptr;
    return Read(static_cast<const std::uint8_t*>(member) + facts::kCharacterPlayerSlot, controller) && controller != nullptr;
}

/// The level has the member on a move of its own (a run or a lead it wrote), kept by its player or not (story_boxes.h).
bool LevelMoves(const void* member) noexcept
{
    return players::lead_gate::OnLevelMove(member);
}

bool Remembered(const void* member, std::uint64_t box) noexcept
{
    for (const Waiting& w : g_waiting)
    {
        if (w.box == box && players::weak_object::Resolve(w.member) == member)
        {
            return true;
        }
    }
    return false;
}

/// The redirected enter (game thread, the world tick's dispatcher).
std::int64_t Begin(void* points, std::uint64_t box, void* collider)
{
    const auto original = reinterpret_cast<BeginFn>(g_redirect.Original());
    if (!g_on.load() || collider == nullptr || !players::seat_camera::IsGameThread())
    {
        return original(points, box, collider);
    }
    const int seat = SeatOf(collider);
    if (seat == 0)
    {
        return original(points, box, collider);
    }
    try
    {
        std::uint64_t object = 0, group = 0;
        const bool    laid   = LayoutOf(box, object, group);
        const std::uint64_t tag = FirstTag(collider);
        const rules::Entry entry{.heldByPlayer     = HeldByPlayer(collider),
                                 .levelSentIt      = LevelMoves(collider),
                                 .groupSet         = laid && group != 0,
                                 .groupNamesLeader = laid && group != 0 && GroupNamesLeader(group)};
        if (!rules::HoldBack(entry) || !GroupLists(group, tag))
        {
            return original(points, box, collider);  // the game's: a party box, the member on its AI or sent, not its box
        }
        if (Remembered(collider, box))
        {
            platform::LogDebug(L"story boxes: {}'s {} walked into {} again; its first enter still waits", players::SeatName(seat),
                               NameOf(tag), NameOf(box));
            return 0;  // walked out and in again while waiting: one enter is remembered
        }
        if (g_waiting.size() >= kMaxWaiting)
        {
            ++g_overflow;
            platform::Log(L"story boxes: {} enters already wait; {}'s {} enters {} now, as the game would", g_waiting.size(),
                          players::SeatName(seat), NameOf(tag), NameOf(box));
            return original(points, box, collider);
        }
        Waiting w;
        w.seat      = seat;
        w.member    = players::weak_object::MakeWeak(collider);
        w.box       = box;
        w.object    = object;
        w.memberTag = tag;
        void* boxActor = nullptr;
        if (points != nullptr && CallGuarded(g_actorByTag, boxActor, points, &box) && boxActor != nullptr)
        {
            w.boxActor = players::weak_object::MakeWeak(boxActor);
            w.shaped   = ShapeOf(boxActor, w.shape);
        }
        if (!w.member.Set() || !w.boxActor.Set())
        {
            return original(points, box, collider);  // cannot be followed between ticks: the game's now
        }
        g_waiting.push_back(w);
        g_waitingCount.store(g_waiting.size());
        ++g_heldBack;
        platform::Log(L"story boxes: {}'s {} walked into {} (group {}, object {}) ahead of the story; it waits until the level "
                      L"moves {} or hands it to its AI, its player leaves, or player 1 comes to the box{}",
                      players::SeatName(seat), NameOf(tag), NameOf(box), NameOf(group), NameOf(object), NameOf(tag),
                      w.shaped ? L"" : L" (no box shape read: not the last)");
        return 0;
    }
    catch (...)
    {
        return original(points, box, collider);
    }
}

/// The member enters the box now: the game's own enter, then its end when the member is no longer inside.
void Replay(const Waiting& w, void* member, const wchar_t* why)
{
    void*        points   = FieldPoints();
    const auto   original = reinterpret_cast<BeginFn>(g_redirect.Original());
    const Sends  sends    = WouldSend(w.object);
    std::int64_t result   = 0;
    const bool   began    = points != nullptr && CallGuarded(original, result, points, w.box, member);
    const bool   inside   = Inside(member, w.box);
    const bool   ended    = began && !inside && CallGuarded(g_end, points, w.box, member);
    ++g_replayed;
    if (sends == Sends::Off)
    {
        ++g_replayedOff;
    }
    platform::Log(L"story boxes: {}'s {} enters {} now ({}): {}; {}", players::SeatName(w.seat), NameOf(w.memberTag), NameOf(w.box),
                  why, began ? SendsText(sends, inside) : L"the game's enter could not be called (LOST: its line may not play)",
                  !began ? L"nothing recorded" : inside ? L"still inside" : ended ? L"outside again: the game's end followed"
                                                                                  : L"outside again: the end could not be called");
}

const wchar_t* WhyReplay(const rules::Waiting& w) noexcept
{
    return !w.seated ? L"its player left" : w.handedToAi ? L"handed to its AI" : w.levelMoves ? L"the level sent it on a move"
                                                                                           : L"player 1 came to the box";
}

std::wstring Reading()
{
    std::wstring text = std::format(L"redirect {}{}; held back {}, replayed {} ({} with the box off), lost {}, overflow {}; waiting {}",
                                    g_placed.load() ? L"placed" : L"not placed", g_on.load() ? L"" : L" (switched off)", g_heldBack, g_replayed,
                                    g_replayedOff, g_lost, g_overflow, g_waiting.size());
    for (const Waiting& w : g_waiting)
    {
        text += std::format(L"; {}'s {} in {}{}", players::SeatName(w.seat), NameOf(w.memberTag), NameOf(w.box),
                            w.shaped ? L"" : L" (no shape)");
    }
    return text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto at         = image.Rebase(facts::kStoryBoxBegin, 16);
    const auto end        = platform::FindFunction(image, facts::kStoryBoxEnd);
    const auto actorByTag = platform::FindFunction(image, facts::kFieldPointActorByTag);
    const auto mapFind    = platform::FindFunction(image, facts::kNameMapFindIndex);
    const auto findRow    = platform::FindFunction(image, facts::kFindRowInTables);
    const auto slotIndex  = platform::FindFunction(image, facts::kStateSlotIndex);
    const auto baseLoaded = platform::FindFunction(image, facts::kStateBaseSlotLoaded);
    const auto layoutMap  = image.Rebase(facts::kLayoutObjectByNodeMap, 3 * sizeof(void*));
    const auto layoutRows = image.Rebase(facts::kTableObjectLayout, 3 * sizeof(void*));
    const auto groupRows  = image.Rebase(facts::kTableActorGroup, 3 * sizeof(void*));
    if (!at || !image.BytesMatch(facts::kStoryBoxBegin, facts::kStoryBoxBeginFirst) || !end || !actorByTag || !mapFind ||
        !findRow || !slotIndex || !baseLoaded || !layoutMap || !layoutRows || !groupRows || !players::field_root::Installed() ||
        !players::weak_object::Installed())
    {
        platform::Log(L"story boxes: an engine check failed; a further player's member fires its own story boxes as it walks "
                      L"into them, as before");
        return false;
    }
    g_at          = *at;
    g_end         = reinterpret_cast<EndFn>(*end);
    g_actorByTag  = reinterpret_cast<ActorByTagFn>(*actorByTag);
    g_nameMapFind = reinterpret_cast<NameMapFindFn>(*mapFind);
    g_findRow     = reinterpret_cast<FindRowFn>(*findRow);
    g_slotIndex   = reinterpret_cast<SlotIndexFn>(*slotIndex);
    g_baseLoaded  = reinterpret_cast<BaseLoadedFn>(*baseLoaded);
    g_layoutMap   = reinterpret_cast<const void*>(*layoutMap);
    g_layoutRows  = reinterpret_cast<const void*>(*layoutRows);
    g_groupRows   = reinterpret_cast<const void*>(*groupRows);
    g_pending     = true;  // placed by the game thread's tick, outside the world tick
    g_installed   = true;
    control::Register({.name  = L"story-box-hold",
                       .shape = control::Shape::OnOff,
                       .help  = L"a further player's member walking into its own story box ahead of the story waits, and fires "
                                L"it when the level moves it, its player leaves or player 1 comes to the box; off: it fires "
                                L"at once, as before (on at start; switching off fires every waiting one now)",
                       .set   = [](const control::Value& value) {
                           g_on.store(std::get<bool>(value));
                           platform::Log(L"story boxes: {}", g_on.load() ? L"on" : L"off: waiting enters fire at the next tick");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_on.load()}; }});
    control::Register({.name  = L"world.story-boxes",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the story-box hold: what it held back, replayed and lost, and what waits",
                       .get   = [] {
                           try
                           {
                               return control::Value{Reading()};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
    return true;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_on.store(false);
    if (const std::size_t waiting = g_waitingCount.load(); waiting != 0)
    {
        // The list is the game thread's; Remove may run on any thread, so it is left to the next Tick, if any.
        platform::Log(L"story boxes: removed with {} enter(s) waiting; unless a tick still runs they are dropped (POSSIBLY "
                      L"LOST: their lines may not play)",
                      waiting);
    }
    g_placed.store(false);
    if (!g_redirect.Restore())
    {
        platform::Log(L"story boxes: the enter's entry was changed after us; left in place (it passes everything while off)");
    }
}

void TickSeat(int seat, void* member) noexcept
{
    if (seat >= 1 && seat <= players::kSeatsBuilt)
    {
        g_members[static_cast<std::size_t>(seat)] = member;
    }
}

void Tick(const void* pawnOne) noexcept
try
{
    if (g_pending)
    {
        g_pending = false;
        g_placed.store(g_redirect.Install(g_at, facts::kStoryBoxBeginFirst, reinterpret_cast<std::uintptr_t>(&Begin)));
        platform::Log(L"story boxes: {}", g_placed.load() ? L"a further player's member walking into its own story box ahead of the "
                                                     L"story waits for the story's moment (story-box-hold on)"
                                                   : L"the enter could not be redirected; members fire their own story boxes "
                                                     L"as they walk into them, as before");
    }
    if (g_waiting.empty())
    {
        return;
    }
    float      one[3]  = {};
    const bool haveOne = pawnOne != nullptr &&
                         platform::CopyGuarded(one,
                                               static_cast<const std::uint8_t*>(ReadPointer(pawnOne, facts::kActorRootComponent)) +
                                                   facts::kComponentWorldLocation,
                                               sizeof(one));
    void* points = FieldPoints();
    for (std::size_t i = 0; i < g_waiting.size();)
    {
        const Waiting  w        = g_waiting[i];  // a copy: the game calls below must never see a reference into the list
        void*          member   = players::weak_object::Resolve(w.member);
        void*          boxActor = players::weak_object::Resolve(w.boxActor);
        void*          current  = nullptr;
        const bool     boxThere = points != nullptr && boxActor != nullptr && CallGuarded(g_actorByTag, current, points, &w.box) &&
                              current == boxActor;
        rules::Waiting state{.boxGone = !boxThere || member == nullptr};
        if (!state.boxGone)
        {
            state.seated           = g_members[static_cast<std::size_t>(w.seat)] == member;
            state.handedToAi       = !HeldByPlayer(member);
            state.inCinema         = players::party_member::ActorMode(member) == facts::kActorModeCinema;
            state.levelMoves       = LevelMoves(member);
            state.playerOneReached = w.shaped && haveOne && rules::Reaches(w.shape, one);
        }
        if (!g_on.load() && !state.boxGone)
        {
            state.seated = false;  // switched off: every waiting enter fires now, as the game would have
        }
        const rules::Step step = rules::Next(state);
        if (step == rules::Step::Wait)
        {
            ++i;
            continue;
        }
        if (step == rules::Step::Replay)
        {
            Replay(w, member, g_on.load() ? WhyReplay(state) : L"story-box-hold switched off");
        }
        else
        {
            ++g_lost;
            platform::Log(L"story boxes: LOST {}'s {} enter of {}: {} before the story's moment (a line it starts may not play)",
                          players::SeatName(w.seat), NameOf(w.memberTag), NameOf(w.box),
                          member == nullptr ? L"the member went away" : L"the box went away (its level unloaded, a load)");
        }
        g_waiting.erase(g_waiting.begin() + static_cast<std::ptrdiff_t>(i));
        g_waitingCount.store(g_waiting.size());
    }
}
catch (...)
{
    platform::LogWarning(L"story boxes: the tick raised; the waiting enters are kept for the next");
}

}  // namespace lss::world::story_boxes
