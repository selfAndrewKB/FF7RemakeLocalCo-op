#include "world/talk_gather.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/game_vectors.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "players/field_root.h"
#include "players/game_globals.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "world/actor_place.h"
#include "world/catch_up_move.h"
#include "world/field_targets.h"
#include "world/talk_gather_rules.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

#include <Windows.h>

namespace lss::world::talk_gather {

namespace {

using platform::Read;
using players::SeatName;

using TickFn      = void* (*)(void* channel, float dt, std::uint8_t* out);
using CamResetFn  = void (*)(void* params);
using FindRowFn   = void* (*)(const void* tables, std::uint64_t name);
using FindActorFn = void* (*)(std::uint64_t level, std::uint64_t name);

using talk_gather_rules::kBesideCm;
using talk_gather_rules::kFarCm;

using platform::GameVector;

// Set at install, then only read.
std::uintptr_t         g_tick = 0, g_actions = 0, g_namedLevel = 0;
FindRowFn              g_findRow  = nullptr;
FindActorFn            g_findActor = nullptr;
CamResetFn             g_camReset = nullptr;
std::array<platform::CallRedirect, facts::kActTickCalls.size()> g_tickCalls;

// Game thread.
void*         g_channel = nullptr;  // the act channel ticking now
std::uint32_t g_moved = 0, g_near = 0, g_noPoint = 0, g_inScope = 0, g_shops = 0, g_busy = 0, g_again = 0, g_left = 0;
std::uint32_t g_ownTalks = 0;   // player 1's own talks that left the party where it stood
std::uint64_t g_keptObject = 0, g_keptAt = 0;  // the last such talk's object and the time (ms) its lock was last seen
bool          g_gather = true;  // the option `talk-gather`: player 1's character brought to another player's talk
bool          g_follow = true;  // the option `talk-follow`: the party goes to its AI at player 1's own talk

void* TickSteps(void* channel, float dt, std::uint8_t* out)
{
    void* const outer = g_channel;
    g_channel         = channel;
    void* const result = reinterpret_cast<TickFn>(g_tick)(channel, dt, out);
    g_channel          = outer;
    return result;
}

/// The type of the channel's act (its action row's), or -1 when unread.
std::int32_t ActType(const void* channel) noexcept
{
    std::uint64_t act  = 0;
    void*         row  = nullptr;
    std::int32_t  type = -1;
    return Read(static_cast<const std::uint8_t*>(channel) + facts::kActChannelAct, act) && act != 0 &&
                   platform::CallGuarded(g_findRow, row, reinterpret_cast<const void*>(g_actions), act) && row != nullptr &&
                   Read(static_cast<const std::uint8_t*>(row) + facts::kActionRowType, type)
               ? type
               : -1;
}

/// The channel's act is the show-menu act (a shop: the opener's alone, world/shop_act.h); false when unread.
bool ShowMenuAct(const void* channel) noexcept
{
    return ActType(channel) == facts::kActionTypeShowMenu;
}

/// The talked-to NPC, found by name as the game's own field-target dispatch finds it; null when not found (or the
/// lookup is off) or a dying object.
void* FindNpc(std::uint64_t name) noexcept
{
    std::uint64_t level = 0;
    void*         actor = nullptr;
    return g_findActor != nullptr && Read(reinterpret_cast<const void*>(g_namedLevel), level) &&
                   platform::CallGuarded(g_findActor, actor, level, name) && actor != nullptr &&
                   players::weak_object::Live(actor)
               ? actor
               : nullptr;
}

std::wstring Report()
{
    return std::format(L"{}; talk-gather {}, talk-follow {}; player 1's character brought to another player's talk {}, already "
                       L"near {}, no navmesh point {}, in a field action {}, a shop (not moved) {}, already decided for that press "
                       L"{}, skipped inside a player's scope {}, left where it stood (talk-gather off) {}; player 1's own talks "
                       L"that left the party where it stood (talk-follow off) {}",
                       g_tickCalls[0].IsInstalled() ? L"installed" : L"not installed", g_gather ? L"on" : L"off",
                       g_follow ? L"on" : L"off", g_moved, g_near, g_noPoint, g_busy, g_shops, g_again, g_inScope, g_left,
                       g_ownTalks);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tick     = platform::FindFunction(image, facts::kActTick);
    const auto camReset = platform::FindFunction(image, facts::kFieldCameraResetBehindLeader);
    const auto actions  = players::game_globals::Address(players::game_globals::Global::TableAction);
    const auto findRow  = platform::FindFunction(image, facts::kFindRowInTables);
    const auto findNpc  = platform::FindFunction(image, facts::kFindActorByName);  // optional: without it, toward
    const auto named    = image.Rebase(facts::kNamedActorLevel, 8);                 // where player 1's character stood
    if (!tick || !players::field_root::Installed() || !catch_up_move::Installed() || !camReset || !actions || !findRow)
    {
        platform::Log(L"talk gather: an engine check failed; player 1's character stays where it is when another player "
                      L"starts a talk");
        return false;
    }
    g_tick     = *tick;
    g_camReset = reinterpret_cast<CamResetFn>(*camReset);
    g_actions  = actions;
    g_findRow  = reinterpret_cast<FindRowFn>(*findRow);
    if (findNpc && named)
    {
        g_findActor  = reinterpret_cast<FindActorFn>(*findNpc);
        g_namedLevel = *named;
    }
    else
    {
        platform::Log(L"talk gather: the NPC lookup by name failed its check; player 1's character is brought 1.2 m from the "
                      L"presser toward where it stood, facing the presser, instead of beside the presser facing the NPC");
    }
    for (std::size_t i = 0; i < g_tickCalls.size(); ++i)
    {
        const auto site = image.Rebase(facts::kActTickCalls[i], 5);
        if (!site || !g_tickCalls[i].Install(*site, g_tick, reinterpret_cast<std::uintptr_t>(&TickSteps)))
        {
            Remove();
            platform::Log(L"talk gather: an act tick call could not be redirected; player 1's character stays where it is "
                          L"when another player starts a talk");
            return false;
        }
    }
    control::Register({.name  = L"world.talk-gather",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) talks and who is moved for them: the two options, player 1's character brought to "
                                L"other players' talks (moved, already near, no navmesh point, left where it stood), and "
                                L"player 1's own talks that left the party where it stood",
                       .get   = [] { return control::Value{Report()}; }});
    // The two experimental options (platform/settings.h kTalkGather, kTalkFollow): the file, the console and the Co-op
    // page set each through its one setter.
    namespace settings = platform::settings;
    g_gather = settings::Of(settings::kTalkGather).word == L"on";
    g_follow = settings::Of(settings::kTalkFollow).word == L"on";
    control::RegisterOption(
        settings::kTalkGather,
        [](const std::wstring& word) {
            g_gather = word == L"on";
            platform::Log(L"talk gather: talk-gather {}: player 1's character {} a talk another player starts far from it", word,
                          g_gather ? L"is brought to" : L"stays where it stands at");
            return control::Outcome{};
        },
        [] { return std::wstring(g_gather ? L"on" : L"off"); });
    control::RegisterOption(
        settings::kTalkFollow,
        [](const std::wstring& word) {
            g_follow = word == L"on";
            platform::Log(L"talk gather: talk-follow {}: at a talk player 1 starts the other players' characters {}", word,
                          g_follow ? L"go to their AI and walk over" : L"stay with their players, standing where they are");
            return control::Outcome{};
        },
        [] { return std::wstring(g_follow ? L"on" : L"off"); });
    platform::Log(L"talk gather: installed; a talk another player starts more than 8 m from player 1's character {} "
                  L"(talk-gather {}, from {}); at a talk player 1 starts the other players' characters {} (talk-follow {}, from {})",
                  g_gather ? L"brings it beside them" : L"leaves it where it stands", g_gather ? L"on" : L"off",
                  settings::Why(settings::kTalkGather),
                  g_follow ? L"go to their AI and walk over" : L"stay with their players, standing where they are",
                  g_follow ? L"on" : L"off", settings::Why(settings::kTalkFollow));
    return true;
}

void Remove() noexcept
{
    for (auto& call : g_tickCalls)
    {
        call.Restore();
    }
    g_channel = nullptr;
}

bool PlayerOneTalkKeepsParty() noexcept
{
    std::uint64_t object = 0, pressed = 0, pressedAt = 0;
    if (g_follow || g_channel == nullptr || !Read(g_channel, object) || object == 0 ||
        !field_targets::PlayerOnePress(pressed, pressedAt))
    {
        return false;
    }
    if (field_targets::PeekTalkPress(object) != 0 || field_targets::TalkJudged(object))
    {
        return false;  // another player pressed it since: the talk is theirs (OnLeaderHanded judges it)
    }
    // A talk's acts: the planner events, and the shop a shopkeeper's talk opens (the show-menu act on the same channel).
    static_assert(talk_gather_rules::kActPlanEvent == facts::kActionTypePlanEvent &&
                      talk_gather_rules::kActPlanEventAlt == facts::kActionTypePlanEventAlt &&
                      talk_gather_rules::kActShowMenu == facts::kActionTypeShowMenu,
                  "the rule's act types are the game's action row types");
    const std::int32_t  type    = ActType(g_channel);
    const bool          talkAct = talk_gather_rules::TalkAct(type);
    const std::uint64_t now     = GetTickCount64();
    if (!talk_gather_rules::PlayerOneTalk(pressed, pressedAt, object, now, talkAct))
    {
        if (pressed == object)
        {
            platform::LogDebug(L"talk gather: player 1's press of {} took the field lock with an act of type {}, not a talk's, or "
                               L"too long after the press: the party goes with player 1's character as in any scene",
                               field_targets::NameOf(object), type);
        }
        return false;
    }
    // One talk takes the lock several times (a greeting, the talk, a shop, the goodbye): said once at Info, each further
    // time at Debug.
    const bool again = object == g_keptObject && now - g_keptAt <= talk_gather_rules::kPressFreshMs;
    g_keptObject     = object;
    g_keptAt         = now;
    if (again)
    {
        platform::LogDebug(L"talk gather: player 1's talk with {} takes the field lock again (an act of type {}); the other "
                           L"players' characters stay with their players",
                           field_targets::NameOf(object), type);
        return true;
    }
    ++g_ownTalks;
    platform::Log(L"talk gather: player 1 started a talk with {}; talk-follow is off: the other players' characters stay with "
                  L"their players, standing where they are, unless the talk itself takes one",
                  field_targets::NameOf(object));
    return true;
}

void OnKeptTalkEnded() noexcept
{
    field_targets::RefreshPlayerOnePress();
    g_keptAt = GetTickCount64();  // the talk's next act counts from its last lock's end
}

void OnLeaderHanded() noexcept
{
    std::uint64_t object = 0;
    if (g_channel == nullptr || !Read(g_channel, object) || object == 0)
    {
        return;  // not an act's lock
    }
    const int seat = field_targets::PeekTalkPress(object);
    if (seat == 0)
    {
        return;  // player 1's talk, or not a talk
    }
    if (field_targets::TalkJudged(object))
    {
        ++g_again;  // a later act of the same talk (or its follow-up): decided once per press
        return;
    }
    if (!g_gather)
    {
        ++g_left;
        field_targets::MarkTalkJudged(object);
        platform::LogDebug(L"talk gather: {} started a talk with {}; talk-gather is off: player 1's character is not moved",
                           SeatName(seat), field_targets::NameOf(object));
        return;
    }
    if (ShowMenuAct(g_channel))
    {
        ++g_shops;  // the opener's shop: player 1's character plays no part in it
        platform::LogDebug(L"talk gather: {}'s press of {} opens a shop; player 1's character is not moved", SeatName(seat),
                           field_targets::NameOf(object));
        return;
    }
    using namespace players::scopes;
    if (OpenPlayer(Leader | Primary | Operator) != 0)
    {
        ++g_inScope;  // "the leader" would be a seat's character
        platform::LogDebug(L"talk gather: {}'s talk took the lock inside a player's scope; player 1's character is not moved",
                           SeatName(seat));
        return;
    }
    void* const member = Member(seat);
    void* const cloud  = member != nullptr ? players::field_root::PartyLeader(facts::kLeaderCurrent) : nullptr;
    GameVector  at{}, presser{};
    if (member == nullptr || cloud == nullptr || cloud == member ||
        !actor_place::ActorLocation(cloud, at) || !actor_place::ActorLocation(member, presser))
    {
        return;
    }
    const float distance = actor_place::FlatDistance(at, presser);
    if (distance <= kFarCm)
    {
        ++g_near;
        field_targets::MarkTalkJudged(object);
        platform::LogDebug(L"talk gather: {} started a talk with {}; player 1's character is {:.1f} m away: not moved",
                           SeatName(seat), field_targets::NameOf(object), distance / 100.0f);
        return;
    }
    if (catch_up_move::InFieldAction(cloud))
    {
        ++g_busy;
        field_targets::MarkTalkJudged(object);
        platform::LogDebug(L"talk gather: {} started a talk with {}; player 1's character is in a field action: not moved",
                           SeatName(seat), field_targets::NameOf(object));
        return;
    }
    // A navmesh point just beside the presser (the rule: world/talk_gather_rules.h). The NPC's place: where the pressed
    // object stood by its target entry at the press (a talk target carries its state object's name, which the character
    // lookup by name does not know: none found live), else that lookup (the lookup is not tried after a noted
    // place: the entry is the surer source).
    GameVector     npcAt{};
    float          spot[3] = {};
    const wchar_t* npcNote = L"; where the NPC stands is unknown, facing that character";
    bool           known   = field_targets::TalkPressSpot(object, spot);
    if (known)
    {
        npcAt   = {spot[0], spot[1], spot[2]};
        npcNote = L", facing the NPC (its place from the game's marker of the pressed object)";
    }
    else if (void* const npc = FindNpc(object); npc != nullptr && npc != cloud && npc != member)
    {
        known   = actor_place::ActorLocation(npc, npcAt);
        npcNote = L", facing the NPC (its place from the actor of its name)";
    }
    const talk_gather_rules::Beside beside  = talk_gather_rules::BesideSpot(presser, at, known ? &npcAt : nullptr);
    const bool                      withNpc = beside.withNpc;
    if (known && !withNpc)
    {
        npcNote = L"; where the NPC stands is not within 8 m of that character, facing that character";
    }
    GameVector  point{};
    std::size_t used = beside.tries.size();
    for (std::size_t i = 0; i < beside.tries.size(); ++i)
    {
        if (catch_up_move::NavPoint(presser, beside.tries[i], kBesideCm, point))
        {
            used = i;
            break;
        }
    }
    if (used == beside.tries.size())
    {
        ++g_noPoint;
        field_targets::MarkTalkJudged(object);
        platform::LogDebug(L"talk gather: {} started a talk with {}; player 1's character is {:.1f} m away, but no navmesh "
                           L"point was found beside {}: not moved",
                           SeatName(seat), field_targets::NameOf(object), distance / 100.0f, SeatName(seat));
        return;
    }
    field_targets::MarkTalkJudged(object);  // decided: a later act of the talk never moves it again
    // Facing the NPC (else the presser).
    if (!catch_up_move::Move(cloud, point, talk_gather_rules::FacingYaw(point, beside.face)))
    {
        platform::LogWarning(L"talk gather: the move of player 1's character raised; it may stand where it was");
        return;
    }
    alignas(16) std::array<std::uint8_t, facts::kCameraResetParamsSize> params{};  // all zero, as the game's own init
    if (!platform::CallGuarded(g_camReset, static_cast<void*>(params.data())))
    {
        platform::LogWarning(L"talk gather: player 1's camera reset raised; its camera may stay where it was");
    }
    ++g_moved;
    platform::Log(L"talk gather: {} started a talk with {}; player 1's character was {:.1f} m away and is brought beside {}'s "
                  L"character ({}{})",
                  SeatName(seat), field_targets::NameOf(object), distance / 100.0f, SeatName(seat),
                  talk_gather_rules::SpotName(withNpc, used),
                  npcNote);
    if (platform::Enabled(platform::Level::Debug))
    {
        // The move ran inside the game's lock set (RE-talk-far.md): the lock and the channel's step after it.
        const std::uint32_t lock = players::field_root::ControlLockMask();
        std::uint8_t        step = 0;
        Read(static_cast<const std::uint8_t*>(g_channel) + facts::kActChannelStep, step);
        platform::LogDebug(L"talk gather: after the move the field lock is {:#x}, the act's step {}", lock, step);
    }
}

}  // namespace lss::world::talk_gather
