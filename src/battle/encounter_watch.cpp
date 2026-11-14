#include "battle/encounter_watch.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/actor_fields.h"
#include "players/field_root.h"
#include "players/party_seat.h"
#include "players/seats.h"
#include "world/actor_place.h"
#include "world/field_targets.h"

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string>

#include <Windows.h>

namespace lss::battle::encounter_watch {

namespace {

using platform::Read;

using GroupTickFn   = bool (*)(void* group, float dt);
using GroupUpdateFn = std::uintptr_t (*)(void* group, float dt);  // its return (the lifecycle) passed on untouched
using InAreaFn      = bool (*)(void* character, std::uint64_t areaName, std::uint64_t kind, const float* probe);
using MemberActorFn = void* (*)(void* group, int member);
using HiddenFn      = bool (*)(void* actor);

constexpr int           kPlayers   = 1 + players::kSeatsBuilt;
constexpr std::size_t   kGroups    = 128;  // groups remembered at once (the field holds at most 128 areas; more groups reuse
                                           // the longest unseen slot, and may log a "first seen" again)
constexpr std::uint64_t kAskMs     = 500;  // a group's area asked at most this often
constexpr std::uint64_t kSeenMs    = 1000; // the reading's "seen in the last second"
constexpr std::uint64_t kWaitMs    = 1000; // a waiting group's members read at most this often
constexpr float         kNearM     = 15.0f;  // a player this close to a waiting group's enemy is logged
constexpr float         kFarM      = 25.0f;  // and logged again only after going this far away
constexpr std::uint8_t  kUnread    = 0xFF;

// Set at install, then only read.
std::uintptr_t g_tick = 0, g_update = 0;
InAreaFn       g_inArea = nullptr;
HiddenFn       g_hidden = nullptr;

// Game thread.
platform::EntryRedirect g_entry, g_updateEntry;
bool                    g_placed = false;

/// A group as last seen: its state and which players' characters stood in its battle area (bit p-1 for player p);
/// while it waits, why, and which players came near one of its enemies.
struct Group
{
    const void*   group = nullptr;
    std::uint64_t name = 0, askedAt = 0, seenAt = 0, waitReadAt = 0;
    std::uint8_t  state = kUnread, lifecycle = kUnread;
    unsigned      inside = 0, closeBy = 0;  // closeBy: bit p-1, player p came near one of its enemies while it waited
    std::wstring  why;
};
std::array<Group, kGroups> g_groups{};
std::uint32_t              g_changes = 0, g_moves = 0, g_steps = 0, g_nearWaiting = 0;

const wchar_t* StateName(std::uint8_t state) noexcept
{
    switch (state)
    {
        case 0: return L"idle";
        case 1: return L"caution";
        case 2: return L"engage";
        case 3: return L"battle";
        case 4: return L"escaping";
        case 5: return L"escaped";
        case 6: return L"ended";
        case kUnread: return L"unread";
        default: return L"?";
    }
}

/// Which players' characters stand in the battle area named `area`, by the game's own test (player 1's: the party
/// leader the encounter watches).
unsigned Inside(std::uint64_t area) noexcept
{
    alignas(16) constexpr float zero[4] = {};  // the probe vector, padded and aligned for any vector load
    unsigned                    inside  = 0;
    for (int player = 1; player <= kPlayers; ++player)
    {
        void* const character = players::party_seat::PlayerCharacter(player);
        bool        in        = false;
        if (character != nullptr && platform::CallGuarded(g_inArea, in, character, area, std::uint64_t{0}, zero) && in)
        {
            inside |= 1u << (player - 1);
        }
    }
    return inside;
}

std::wstring Who(unsigned inside)
{
    std::wstring text;
    for (int player = 1; player <= kPlayers; ++player)
    {
        text += std::format(L"{}player {} {}", player == 1 ? L"" : L", ", player, (inside & 1u << (player - 1)) != 0 ? L"in" : L"out");
    }
    return text;
}

Group* Slot(const void* group, std::uint64_t now) noexcept
{
    Group* oldest = &g_groups[0];
    for (Group& g : g_groups)
    {
        if (g.group == group)
        {
            return &g;
        }
        if (g.seenAt < oldest->seenAt)
        {
            oldest = &g;
        }
    }
    *oldest = {group};  // a group not seen for the longest makes room
    oldest->seenAt = now;
    return oldest;
}

void Observe(const void* group, std::uint8_t after) noexcept
try
{
    const auto*         bytes = static_cast<const std::uint8_t*>(group);
    const std::uint64_t now   = GetTickCount64();
    Group&              g     = *Slot(group, now);
    std::uint64_t       area  = 0;
    Read(bytes + facts::kEncountGroupName, g.name);
    const bool changed = after != g.state && g.state != kUnread;
    const bool ask     = changed || now - g.askedAt >= kAskMs;
    g.seenAt           = now;
    if (!ask || !Read(bytes + facts::kEncountGroupAreaName, area))
    {
        g.state = after;
        return;
    }
    const unsigned inside = Inside(area);
    g.askedAt             = now;
    if (g.state == kUnread)
    {
        platform::LogDebug(L"encounter watch: group {} first seen {}; in its battle area {}: {}", world::field_targets::NameOf(g.name),
                           StateName(after), world::field_targets::NameOf(area), Who(inside));
    }
    else if (changed)
    {
        ++g_changes;
        // Idle and caution trade places whenever an enemy loses and finds player 1 again: Debug; the rest are rare.
        const bool sightOnly = after <= 1 && g.state <= 1;
        const auto line      = std::format(L"encounter watch: group {} {} -> {}; in its battle area {}: {}", world::field_targets::NameOf(g.name),
                                           StateName(g.state), StateName(after), world::field_targets::NameOf(area), Who(inside));
        if (sightOnly)
        {
            platform::LogDebug(L"{}", line);
        }
        else
        {
            platform::Log(L"{}", line);
        }
    }
    else if (inside != g.inside && g.state != kUnread)
    {
        ++g_moves;
        platform::LogDebug(L"encounter watch: group {} ({}) battle area {}: {}", world::field_targets::NameOf(g.name), StateName(after),
                           world::field_targets::NameOf(area), Who(inside));
    }
    g.state  = after;
    g.inside = inside;
}
catch (...)
{
}

/// Why a waiting group waits, and how near each player's character is to its nearest counted enemy (0: none placed).
struct Waiting
{
    std::wstring                         why;
    std::array<float, kPlayers>          nearest{};
    std::array<std::uint64_t, kPlayers>  enemy{};  // that enemy's object name
};

/// The game's own lifecycle test, read: the first counted member whose actor is not spawned, has no battle component yet,
/// or is hidden; with none, the group is switched off (its scene object's state).
Waiting WaitOf(void* group) noexcept
try
{
    Waiting       w;
    std::uint32_t count  = 0;
    Read(static_cast<const std::uint8_t*>(group) + facts::kEncountGroupMemberCount, count);
    void* const   vtable = platform::ReadPointer(group, 0);
    const auto    actorOf =
        vtable != nullptr ? reinterpret_cast<MemberActorFn>(platform::ReadPointer(vtable, facts::kEncountGroupMemberActor)) : nullptr;
    std::array<world::actor_place::GameVector, kPlayers> at{};
    std::array<bool, kPlayers>                           placed{};
    for (int player = 1; player <= kPlayers; ++player)
    {
        void* const character = players::party_seat::PlayerCharacter(player);
        placed[player - 1]    = character != nullptr && world::actor_place::ActorLocation(character, at[player - 1]);
    }
    for (std::uint32_t i = 0; i < count && i < facts::kEncountGroupMaxMembers; ++i)
    {
        const auto*   record = static_cast<const std::uint8_t*>(platform::ReadPointer(group, facts::kEncountGroupMembers + 8 * i));
        std::uint8_t  out    = 1;
        std::uint64_t name   = 0;
        if (record == nullptr || !Read(record + facts::kEncountMemberOut, out) || out != 0)
        {
            continue;  // not counted, as the game skips it
        }
        Read(record + facts::kEncountMemberName, name);
        void* actor  = nullptr;
        bool  hidden = false;
        if (actorOf == nullptr || !platform::CallGuarded(actorOf, actor, group, static_cast<int>(i)) || actor == nullptr)
        {
            if (w.why.empty())
            {
                w.why = std::format(L"its enemy {} is not spawned", world::field_targets::NameOf(name));
            }
            continue;
        }
        if (players::actor_fields::BattleComponent(actor) == nullptr)
        {
            if (w.why.empty())
            {
                w.why = std::format(L"its enemy {} has no battle component yet", world::field_targets::NameOf(name));
            }
            continue;
        }
        if (platform::CallGuarded(g_hidden, hidden, actor) && hidden && w.why.empty())
        {
            w.why = std::format(L"its enemy {} is hidden (reason 11 or 14)", world::field_targets::NameOf(name));
        }
        world::actor_place::GameVector there{};
        if (!world::actor_place::ActorLocation(actor, there))
        {
            continue;
        }
        for (int p = 0; p < kPlayers; ++p)
        {
            const float metres = placed[p] ? world::actor_place::FlatDistance(at[p], there) / 100.0f : 0.0f;
            if (placed[p] && (w.nearest[p] == 0.0f || metres < w.nearest[p]))
            {
                w.nearest[p] = metres;
                w.enemy[p]   = name;
            }
        }
    }
    if (w.why.empty())
    {
        w.why = count == 0 ? L"it has no member" : L"every counted enemy is spawned and shown: the group is switched off (its scene object's state)";
    }
    return w;
}
catch (...)
{
    return {L"unread"};
}

/// A group's update ran: its lifecycle, and while it waits (at most once a second) why, and which player came near.
void ObserveUpdate(void* group) noexcept
try
{
    const std::uint64_t now  = GetTickCount64();
    Group&              g    = *Slot(group, now);
    std::uint8_t        life = kUnread;
    Read(static_cast<const std::uint8_t*>(group) + facts::kEncountGroupLifecycle, life);
    Read(static_cast<const std::uint8_t*>(group) + facts::kEncountGroupName, g.name);
    g.seenAt = now;
    if (life != g.lifecycle)
    {
        if (g.lifecycle != kUnread)
        {
            platform::LogDebug(L"encounter watch: group {} lifecycle {} -> {} (0 waiting, 1 stepping)", world::field_targets::NameOf(g.name),
                               g.lifecycle, life);
        }
        g.lifecycle = life;
        g.closeBy   = 0;
    }
    if (life != 0 || now - g.waitReadAt < kWaitMs)
    {
        return;
    }
    g.waitReadAt    = now;
    const Waiting w = WaitOf(group);
    if (w.why != g.why)
    {
        platform::LogDebug(L"encounter watch: group {} waits: {}", world::field_targets::NameOf(g.name), w.why);
        g.why = w.why;
    }
    for (int player = 2; player <= kPlayers; ++player)
    {
        const float    metres = w.nearest[player - 1];
        const unsigned bit    = 1u << (player - 1);
        if (metres > 0.0f && metres <= kNearM && (g.closeBy & bit) == 0)
        {
            g.closeBy |= bit;
            ++g_nearWaiting;
            platform::Log(L"encounter watch: player {} is {:.1f} m from enemy {} of group {} (player 1 {}), and that group waits: {}", player,
                          metres, world::field_targets::NameOf(w.enemy[player - 1]), world::field_targets::NameOf(g.name),
                          w.nearest[0] > 0.0f ? std::format(L"{:.1f} m from its nearest", w.nearest[0]) : std::wstring(L"unplaced"), w.why);
        }
        else if ((metres == 0.0f || metres > kFarM) && (g.closeBy & bit) != 0)
        {
            g.closeBy &= ~bit;
        }
    }
}
catch (...)
{
}

std::uintptr_t GroupUpdate(void* group, float dt)
{
    const auto           original = reinterpret_cast<GroupUpdateFn>(g_updateEntry.Original());
    const std::uintptr_t result   = original(group, dt);
    if (group != nullptr)
    {
        ObserveUpdate(group);
    }
    return result;
}

bool GroupTick(void* group, float dt)
{
    const auto original = reinterpret_cast<GroupTickFn>(g_entry.Original());
    const bool result   = original(group, dt);
    ++g_steps;
    std::uint8_t after = kUnread;
    if (group != nullptr && Read(static_cast<const std::uint8_t*>(group) + facts::kEncountGroupState, after))
    {
        Observe(group, after);
    }
    return result;
}

std::wstring Reading()
{
    const std::uint64_t now  = GetTickCount64();
    std::wstring        text = std::format(L"{}; group steps {}, state changes {}, area moves {}, players near a waiting group's enemy {}",
                                           g_entry.IsInstalled() && g_updateEntry.IsInstalled() ? L"on" : L"off", g_steps, g_changes,
                                           g_moves, g_nearWaiting);
    for (const Group& g : g_groups)
    {
        if (g.group != nullptr && now - g.seenAt <= kSeenMs)
        {
            text += g.lifecycle == 0 ? std::format(L"; {} waiting: {}", world::field_targets::NameOf(g.name), g.why)
                                     : std::format(L"; {} {} ({})", world::field_targets::NameOf(g.name), StateName(g.state), Who(g.inside));
        }
    }
    return text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tick   = platform::FindFunction(image, facts::kEncountGroupTick);
    const auto update = platform::FindFunction(image, facts::kEncountGroupUpdate);
    const auto inArea = platform::FindFunction(image, facts::kCharInBattleAreaOfGroup);
    const auto hidden = platform::FindFunction(image, facts::kEncountMemberHidden);
    if (!tick || !update || !inArea || !hidden || !players::field_root::Installed())
    {
        platform::Log(L"encounter watch: an engine check failed; the field encounter is not watched");
        return false;
    }
    g_inArea = reinterpret_cast<InAreaFn>(*inArea);
    g_hidden = reinterpret_cast<HiddenFn>(*hidden);
    g_update = *update;
    g_tick   = *tick;  // last: Tick places the redirects once it is set
    control::Register({.name  = L"encounter-watch",
                       .shape = control::Shape::Text,
                       .help  = L"the field encounter's groups seen in the last second: their state and which players' characters "
                                L"stand in their battle area (the game watches player 1's alone)",
                       .get   = [] { return control::Value{Reading()}; }});
    return true;
}

void Remove() noexcept
{
    g_updateEntry.Restore();
    g_entry.Restore();
}

void Tick() noexcept
{
    if (g_placed || g_tick == 0)
    {
        return;
    }
    g_placed = true;
    // The game thread alone runs the update and the step, and it is here, outside them.
    if (!g_entry.Install(g_tick, std::span(facts::kEncountGroupTickEntry.data(), facts::kEncountGroupTickFirst),
                         reinterpret_cast<std::uintptr_t>(&GroupTick)) ||
        !g_updateEntry.Install(g_update, std::span(facts::kEncountGroupUpdateEntry.data(), facts::kEncountGroupUpdateFirst),
                               reinterpret_cast<std::uintptr_t>(&GroupUpdate)))
    {
        g_updateEntry.Restore();
        g_entry.Restore();
        platform::Log(L"encounter watch: the group update or step could not be redirected; the field encounter is not watched");
        return;
    }
    platform::Log(L"encounter watch: installed; each field enemy group's state change is logged with which players' characters "
                  L"stand in its battle area, and a player near a waiting group's enemy with why the group waits");
}

}  // namespace lss::battle::encounter_watch
