#include "battle/encounter_watch.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_root.h"
#include "players/party_seat.h"
#include "players/seats.h"
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

using GroupTickFn = bool (*)(void* group, float dt);
using InAreaFn    = bool (*)(void* character, std::uint64_t areaName, std::uint64_t kind, const float* probe);

constexpr int           kPlayers   = 1 + players::kSeatsBuilt;
constexpr std::size_t   kGroups    = 64;   // groups remembered at once (a level holds fewer)
constexpr std::uint64_t kAskMs     = 500;  // a group's area asked at most this often
constexpr std::uint64_t kSeenMs    = 1000; // the reading's "seen in the last second"
constexpr std::uint8_t  kUnread    = 0xFF;

// Set at install, then only read.
std::uintptr_t g_tick = 0;
InAreaFn       g_inArea = nullptr;

// Game thread.
platform::EntryRedirect g_entry;
bool                    g_placed = false;

/// A group as last seen: its state and which players' characters stood in its battle area (bit p-1 for player p).
struct Group
{
    const void*   group = nullptr;
    std::uint64_t name = 0, askedAt = 0, seenAt = 0;
    std::uint8_t  state = kUnread;
    unsigned      inside = 0;
};
std::array<Group, kGroups> g_groups{};
std::uint32_t              g_changes = 0, g_moves = 0, g_steps = 0;

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
    std::wstring        text = std::format(L"{}; group steps {}, state changes {}, area moves {}", g_entry.IsInstalled() ? L"on" : L"off",
                                           g_steps, g_changes, g_moves);
    for (const Group& g : g_groups)
    {
        if (g.group != nullptr && now - g.seenAt <= kSeenMs)
        {
            text += std::format(L"; {} {} ({})", world::field_targets::NameOf(g.name), StateName(g.state), Who(g.inside));
        }
    }
    return text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tick   = platform::FindFunction(image, facts::kEncountGroupTick);
    const auto inArea = platform::FindFunction(image, facts::kCharInBattleAreaOfGroup);
    if (!tick || !inArea || !players::field_root::Installed())
    {
        platform::Log(L"encounter watch: an engine check failed; the field encounter is not watched");
        return false;
    }
    g_inArea = reinterpret_cast<InAreaFn>(*inArea);
    g_tick   = *tick;  // last: Tick places the redirect once it is set
    control::Register({.name  = L"encounter-watch",
                       .shape = control::Shape::Text,
                       .help  = L"the field encounter's groups seen in the last second: their state and which players' characters "
                                L"stand in their battle area (the game watches player 1's alone)",
                       .get   = [] { return control::Value{Reading()}; }});
    return true;
}

void Remove() noexcept
{
    g_entry.Restore();
}

void Tick() noexcept
{
    if (g_placed || g_tick == 0)
    {
        return;
    }
    g_placed = true;
    // The game thread alone runs the step, and it is here, outside it.
    if (!g_entry.Install(g_tick, std::span(facts::kEncountGroupTickEntry.data(), facts::kEncountGroupTickFirst),
                         reinterpret_cast<std::uintptr_t>(&GroupTick)))
    {
        platform::Log(L"encounter watch: the group step could not be redirected; the field encounter is not watched");
        return;
    }
    platform::Log(L"encounter watch: installed; each field enemy group's state change is logged with which players' characters "
                  L"stand in its battle area");
}

}  // namespace lss::battle::encounter_watch
