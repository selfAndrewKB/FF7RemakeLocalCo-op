#include "world/talk_gather.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_root.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "world/actor_place.h"
#include "world/field_targets.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::world::talk_gather {

namespace {

using platform::Read;
using players::SeatName;

using TickFn      = void* (*)(void* channel, float dt, std::uint8_t* out);
using NavPointFn  = bool (*)(float* out, const float* origin, const float* dir, float distance, const void* agent,
                            std::uint64_t, std::uint8_t, std::uint32_t, std::uint64_t);
using WarpFn      = void (*)(void* character, const float* location, const float* rotation, const float* direct, bool fade);
using CamResetFn  = void (*)(void* params);
using FindRowFn   = void* (*)(const void* tables, std::uint64_t name);
using FindActorFn = void* (*)(std::uint64_t level, std::uint64_t name);

constexpr float kFarCm    = 800.0f;  // the game's own "far from the leader" (the battle end's warp threshold)
constexpr float kBesideCm = 120.0f;  // shoulder to shoulder with the presser (the user, 2026-10-26: "Just beside.")
constexpr float kPi     = 3.14159265358979f;

using actor_place::Vec3;

// Set at install, then only read.
std::uintptr_t         g_tick = 0, g_agent = 0, g_actions = 0, g_namedLevel = 0;
FindRowFn              g_findRow  = nullptr;
FindActorFn            g_findActor = nullptr;
NavPointFn             g_navPoint = nullptr;
WarpFn                 g_warp     = nullptr;
CamResetFn             g_camReset = nullptr;
std::array<platform::CallRedirect, facts::kActTickCalls.size()> g_tickCalls;

// Game thread.
void*         g_channel = nullptr;  // the act channel ticking now
std::uint32_t g_moved = 0, g_near = 0, g_noPoint = 0, g_inScope = 0, g_shops = 0, g_busy = 0, g_again = 0;

void* TickSteps(void* channel, float dt, std::uint8_t* out)
{
    void* const outer = g_channel;
    g_channel         = channel;
    void* const result = reinterpret_cast<TickFn>(g_tick)(channel, dt, out);
    g_channel          = outer;
    return result;
}

/// The channel's act is the show-menu act (a shop: the opener's alone, world/shop_act.h); false when unread.
bool ShowMenuAct(const void* channel) noexcept
{
    std::uint64_t act  = 0;
    void*         row  = nullptr;
    std::int32_t  type = -1;
    return Read(static_cast<const std::uint8_t*>(channel) + facts::kActChannelAct, act) && act != 0 &&
           platform::CallGuarded(g_findRow, row, reinterpret_cast<const void*>(g_actions), act) && row != nullptr &&
           Read(static_cast<const std::uint8_t*>(row) + facts::kActionRowType, type) && type == facts::kActionTypeShowMenu;
}

/// The character runs a field action (a ladder, a crawl, the grappling hook): not moved out of it.
bool InFieldAction(void* character) noexcept
{
    const void*  component = platform::ReadPointer(character, facts::kCharacterFieldAction);
    std::uint8_t running   = 0;
    return component != nullptr && Read(static_cast<const std::uint8_t*>(component) + facts::kFieldActionRunning, running) &&
           running != 0;
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

/// A navmesh point `kBesideCm` from `origin` along the flat `dir` (the game's own query, its catch-up teleport's shape).
bool NavPoint(const Vec3& origin, const Vec3& dir, Vec3& out) noexcept
{
    alignas(16) std::array<std::uint8_t, facts::kNavAgentSize> agent{};
    alignas(16) float point[8] = {};  // room for an FNavLocation (location, padding, node), not only an FVector
    const float       from[3]  = {origin.x, origin.y, origin.z};
    const float       toward[3] = {dir.x, dir.y, 0.0f};
    bool              found     = false;
    if (!platform::CopyGuarded(agent.data(), reinterpret_cast<const void*>(g_agent), agent.size()) ||
        !platform::CallGuarded(g_navPoint, found, point, from, toward, kBesideCm, static_cast<const void*>(agent.data()),
                               std::uint64_t{0}, std::uint8_t{0}, std::uint32_t{0}, std::uint64_t{0}) ||
        !found || (point[0] == 0.0f && point[1] == 0.0f && point[2] == 0.0f))
    {
        return false;
    }
    out = {point[0], point[1], point[2]};
    return true;
}

std::wstring Report()
{
    return std::format(L"{}; player 1's character brought to a further player's talk {}, already near {}, no navmesh point {}, "
                       L"in a field action {}, a shop (not moved) {}, already decided for that press {}, skipped inside a "
                       L"player's scope {}",
                       g_tickCalls[0].IsInstalled() ? L"on" : L"off", g_moved, g_near, g_noPoint, g_busy, g_shops, g_again,
                       g_inScope);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tick     = platform::FindFunction(image, facts::kActTick);
    const auto navPoint = platform::FindFunction(image, facts::kNavPointAlongDir);
    const auto warp     = platform::FindFunction(image, facts::kBattleTeleportFadeIn);
    const auto camReset = platform::FindFunction(image, facts::kFieldCameraResetBehindLeader);
    const auto agent    = image.Rebase(facts::kNavAgentDefault, facts::kNavAgentSize);
    const auto actions  = image.Rebase(facts::kTableAction, 24);
    const auto findRow  = platform::FindFunction(image, facts::kFindRowInTables);
    const auto findNpc  = platform::FindFunction(image, facts::kFindActorByName);  // optional: without it, toward
    const auto named    = image.Rebase(facts::kNamedActorLevel, 8);                 // player 1's character, as before
    if (!tick || !players::field_root::Installed() || !navPoint || !warp || !camReset || !agent || !actions || !findRow)
    {
        platform::Log(L"talk gather: an engine check failed; player 1's character stays where it is when another player "
                      L"starts a talk");
        return false;
    }
    g_tick     = *tick;
    g_agent    = *agent;
    g_navPoint = reinterpret_cast<NavPointFn>(*navPoint);
    g_warp     = reinterpret_cast<WarpFn>(*warp);
    g_camReset = reinterpret_cast<CamResetFn>(*camReset);
    g_actions  = *actions;
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
    control::Register({.name  = L"talk-gather",
                       .shape = control::Shape::Text,
                       .help  = L"player 1's character brought to further players' talks: moved, already near, no navmesh point",
                       .get   = [] { return control::Value{Report()}; }});
    platform::Log(L"talk gather: installed; player 1's character is brought to a talk another player starts when more than "
                  L"8 m away");
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
    Vec3        at{}, presser{};
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
    if (InFieldAction(cloud))
    {
        ++g_busy;
        field_targets::MarkTalkJudged(object);
        platform::LogDebug(L"talk gather: {} started a talk with {}; player 1's character is in a field action: not moved",
                           SeatName(seat), field_targets::NameOf(object));
        return;
    }
    // A navmesh point just beside the presser, as near the NPC as the presser stands: to the side of the presser's line
    // to the NPC that faces where player 1's character stands, else the other side, else behind the presser. Without
    // the NPC's place: toward player 1's character, else either side of that line.
    // The NPC's place: where the pressed object stood by its target entry at the press (a talk target carries its state
    // object's name, which the character lookup by name does not know: none found live 2026-10-26), else that lookup
    // (the lookup is not tried after a noted place: the entry is the surer source). A place far off (another actor of
    // the same name, or a party member) would turn the side and facing: only one within the game's own "far" of the
    // presser counts.
    const Vec3     toCloud{(at.x - presser.x) / distance, (at.y - presser.y) / distance, 0.0f};
    Vec3           npcAt{};
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
    const bool withNpc = known && actor_place::FlatDistance(npcAt, presser) > 1.0f && actor_place::FlatDistance(npcAt, presser) <= kFarCm;
    if (known && !withNpc)
    {
        npcNote = L"; where the NPC stands is not within 8 m of that character, facing that character";
    }
    std::array<Vec3, 3> tries{toCloud, Vec3{-toCloud.y, toCloud.x, 0.0f}, Vec3{toCloud.y, -toCloud.x, 0.0f}};
    if (withNpc)
    {
        const float span = actor_place::FlatDistance(npcAt, presser);
        const Vec3  toNpc{(npcAt.x - presser.x) / span, (npcAt.y - presser.y) / span, 0.0f};
        const Vec3  left{-toNpc.y, toNpc.x, 0.0f};
        const bool  cloudLeft = left.x * toCloud.x + left.y * toCloud.y >= 0.0f;
        const Vec3  nearSide  = cloudLeft ? left : Vec3{-left.x, -left.y, 0.0f};
        tries                 = {nearSide, Vec3{-nearSide.x, -nearSide.y, 0.0f}, Vec3{-toNpc.x, -toNpc.y, 0.0f}};
    }
    Vec3        point{};
    std::size_t used = tries.size();
    for (std::size_t i = 0; i < tries.size(); ++i)
    {
        if (NavPoint(presser, tries[i], point))
        {
            used = i;
            break;
        }
    }
    if (used == tries.size())
    {
        ++g_noPoint;
        field_targets::MarkTalkJudged(object);
        platform::LogDebug(L"talk gather: {} started a talk with {}; player 1's character is {:.1f} m away, but no navmesh "
                           L"point was found beside {}: not moved",
                           SeatName(seat), field_targets::NameOf(object), distance / 100.0f, SeatName(seat));
        return;
    }
    field_targets::MarkTalkJudged(object);  // decided: a later act of the talk never moves it again
    // Facing the NPC (else the presser): FRotator (pitch, yaw, roll) in degrees.
    const Vec3  face        = withNpc ? npcAt : presser;
    const float location[3] = {point.x, point.y, point.z};
    const float rotation[3] = {0.0f, std::atan2(face.y - point.y, face.x - point.x) * 180.0f / kPi, 0.0f};
    if (!platform::CallGuarded(g_warp, cloud, static_cast<const float*>(location), static_cast<const float*>(rotation),
                               static_cast<const float*>(location), true))
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
    static constexpr const wchar_t* kSpots[2][3] = {{L"toward where it stood", L"to one side", L"to the other side"},
                                                    {L"on its side", L"on the far side", L"behind"}};
    platform::Log(L"talk gather: {} started a talk with {}; player 1's character was {:.1f} m away and is brought beside {}'s "
                  L"character ({}{})",
                  SeatName(seat), field_targets::NameOf(object), distance / 100.0f, SeatName(seat), kSpots[withNpc ? 1 : 0][used],
                  npcNote);
    if (platform::Enabled(platform::Level::Debug))
    {
        // The move ran inside the game's lock set (RE-talk-far.md §6): the lock and the channel's step after it.
        const std::uint32_t lock = players::field_root::ControlLockMask();
        std::uint8_t        step = 0;
        Read(static_cast<const std::uint8_t*>(g_channel) + facts::kActChannelStep, step);
        platform::LogDebug(L"talk gather: after the move the field lock is {:#x}, the act's step {}", lock, step);
    }
}

}  // namespace lss::world::talk_gather
