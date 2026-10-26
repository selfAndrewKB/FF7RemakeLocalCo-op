#include "battle/battle_warps.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace lss::battle::battle_warps {

namespace {

using platform::Read;

using GetPlayerActorFn = void* (*)(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate);
using NeedsWarpFn      = bool (*)(void* unused, void* member, void* leader, void* groups);

enum class Mode : std::uint8_t
{
    PlayersStay,  // every player's own character stays; the others are warped
    Off,          // nobody is warped
    Game,         // the game's warps for everyone
};

constexpr std::array<const wchar_t*, 3> kModeWords{L"players-stay", L"off", L"game"};

// Set once at install.
std::uintptr_t         g_getActor  = 0;
std::uintptr_t         g_victory   = 0;
std::uintptr_t         g_escape    = 0;
platform::CallRedirect g_startCall;
platform::CallRedirect g_victoryCall;
platform::CallRedirect g_escapeCall;

// Game thread.
Mode g_mode = Mode::PlayersStay;

const wchar_t* ModeWord(Mode mode) noexcept
{
    return kModeWords[static_cast<std::size_t>(mode)];
}

/// The seat (1..) whose player controls `member` (or member type `type`), else 0. A seat's member stays bound while
/// the game hands it to its AI for a field hold, so it counts as the player's then too. The start matches the type as
/// well (the loop fetches by type); the end has only the character, so a member fetched again under a new pointer would
/// not match there [not seen: the seat rebinds on a new actor].
int SeatOf(const void* member, std::uint8_t type) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if ((member != nullptr && players::scopes::Member(seat) == member) ||
            (type != facts::kPlayerTypeNone && players::scopes::MemberType(seat) == type))
        {
            return seat;
        }
    }
    return 0;
}

/// Whether the setting keeps this member where it is.
bool Stays(int seat) noexcept
{
    return g_mode == Mode::Off || (g_mode == Mode::PlayersStay && seat != 0);
}

/// Who the member is, for the log line (with WhoseRest).
const wchar_t* Whose(int seat) noexcept
{
    return seat != 0 ? players::SeatName(seat) : L"a member no player controls";
}
const wchar_t* WhoseRest(int seat) noexcept
{
    return seat != 0 ? L"'s character" : L"";
}

/// The flat distance in metres between two actors' roots (the game's warp thresholds measure flat, RE §1.6), or -1 if
/// either cannot be read.
float FlatMetres(void* a, void* b) noexcept
{
    std::array<float, 3> at{}, bt{};
    void*                aRoot = a != nullptr ? platform::ReadPointer(a, facts::kActorRootComponent) : nullptr;
    void*                bRoot = b != nullptr ? platform::ReadPointer(b, facts::kActorRootComponent) : nullptr;
    if (aRoot == nullptr || bRoot == nullptr ||
        !Read(static_cast<const std::uint8_t*>(aRoot) + facts::kComponentWorldLocation, at) ||
        !Read(static_cast<const std::uint8_t*>(bRoot) + facts::kComponentWorldLocation, bt))
    {
        return -1.0f;
    }
    const float dx = at[0] - bt[0], dy = at[1] - bt[1];
    return std::sqrt(dx * dx + dy * dy) / 100.0f;
}

/// The battle start's member loop, at its fetch of member `type`: null keeps the member where it is (the game's own
/// "no actor" branch). Every call is logged at Debug, the members the game warps too.
void* StartMember(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate) noexcept
{
    const auto get    = reinterpret_cast<GetPlayerActorFn>(g_getActor);
    void*      member = get(partyManager, variant, type, alternate);
    if (member == nullptr)
    {
        return nullptr;
    }
    const int  seat  = SeatOf(member, type);
    const bool stays = Stays(seat);
    if (platform::Enabled(platform::Level::Debug))
    {
        std::uint8_t leaderType = facts::kPlayerTypeNone;
        void* const  leader = Read(static_cast<const std::uint8_t*>(partyManager) + facts::kPartyLeaderType, leaderType)
                                  ? get(partyManager, 0, leaderType, false)
                                  : nullptr;
        platform::LogDebug(L"battle warps: the battle starts; {}{} (type {}), {:.1f} m from the leader: {}", Whose(seat),
                           WhoseRest(seat), type, FlatMetres(member, leader),
                           stays ? L"left where it is" : L"the game's warp");
    }
    return stays ? nullptr : member;
}

/// The battle end's "does this member need the warp back" (victory or escape): false keeps it where it is. Every call
/// is logged at Debug with the game's own answer for the members it decides.
bool EndMember(std::uintptr_t original, const wchar_t* how, void* unused, void* member, void* leader, void* groups) noexcept
{
    const int  seat   = SeatOf(member, facts::kPlayerTypeNone);
    const bool stays  = Stays(seat);
    const bool warped = !stays && reinterpret_cast<NeedsWarpFn>(original)(unused, member, leader, groups);
    if (platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"battle warps: the battle ends ({}); {}{}, {:.1f} m from the leader: {}", how, Whose(seat),
                           WhoseRest(seat), FlatMetres(member, leader),
                           stays ? L"left where it is" : (warped ? L"the game warps it back" : L"the game leaves it (near)"));
    }
    return warped;
}

bool VictoryMember(void* unused, void* member, void* leader, void* groups) noexcept
{
    return EndMember(g_victory, L"a victory", unused, member, leader, groups);
}

bool EscapeMember(void* unused, void* member, void* leader, void* groups) noexcept
{
    return EndMember(g_escape, L"an escape", unused, member, leader, groups);
}

void SetMode(Mode mode, const wchar_t* why)
{
    g_mode = mode;
    switch (mode)
    {
        case Mode::Game:
            platform::Log(L"battle warps: the game warps the whole party at a battle's start and end ({})", why);
            break;
        case Mode::PlayersStay:
            platform::Log(L"battle warps: every player's own character stays where it is at a battle's start and end; the "
                          L"game warps the members no player controls ({})",
                          why);
            break;
        case Mode::Off:
            platform::Log(L"battle warps: no party member is warped at a battle's start or end ({})", why);
            break;
    }
}

/// The mode named by `word`, or empty.
std::optional<Mode> ModeOf(const std::wstring& word) noexcept
{
    for (std::size_t i = 0; i < kModeWords.size(); ++i)
    {
        if (word == kModeWords[i])
        {
            return static_cast<Mode>(i);
        }
    }
    return std::nullopt;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto getActor     = platform::FindFunction(image, facts::kGetPlayerActor);
    const auto victory      = platform::FindFunction(image, facts::kBattleFinishNeedsWarpVictory);
    const auto escape       = platform::FindFunction(image, facts::kBattleFinishNeedsWarpEscape);
    const auto startCall    = image.Rebase(facts::kBattleInMemberCall, 5);
    const auto victoryCall  = image.Rebase(facts::kBattleFinishVictoryCall, 5);
    const auto escapeCall   = image.Rebase(facts::kBattleFinishEscapeCall, 5);
    if (!getActor || !victory || !escape || !startCall || !victoryCall || !escapeCall)
    {
        platform::Log(L"battle warps: an engine check failed; the game warps the whole party at a battle's start and end");
        return false;
    }
    g_getActor = *getActor;
    g_victory  = *victory;
    g_escape   = *escape;

    const std::wstring setting = platform::settings::Read(L"Battle", L"battle-warps", L"");
    const auto         mode    = ModeOf(setting);
    if (!setting.empty() && !mode)
    {
        platform::LogWarning(L"battle warps: LocalSplitScreen.ini [Battle] battle-warps = '{}' is not game, players-stay or "
                             L"off; read as players-stay",
                             setting);
    }
    g_mode = mode.value_or(Mode::PlayersStay);  // set before the redirects are placed

    // All three calls lie inside one code block each (RE §3.1-§3.2).
    if (!g_startCall.Install(*startCall, g_getActor, reinterpret_cast<std::uintptr_t>(&StartMember)) ||
        !g_victoryCall.Install(*victoryCall, g_victory, reinterpret_cast<std::uintptr_t>(&VictoryMember)) ||
        !g_escapeCall.Install(*escapeCall, g_escape, reinterpret_cast<std::uintptr_t>(&EscapeMember)))
    {
        g_escapeCall.Restore();
        g_victoryCall.Restore();
        g_startCall.Restore();
        platform::Log(L"battle warps: a call could not be redirected; the game warps the whole party at a battle's start and "
                      L"end");
        return false;
    }
    SetMode(g_mode, mode             ? L"the settings file"
                    : setting.empty() ? L"the default; LocalSplitScreen.ini has no [Battle] battle-warps"
                                      : L"the default; the file's value is not valid");
    control::Register({.name  = L"battle-warps",
                       .shape = control::Shape::Word,
                       .words = {kModeWords.begin(), kModeWords.end()},
                       .help  = L"who the game warps at a battle's start and end: players-stay (every player's own character "
                                L"stays where it is; the default), off (nobody is warped), game (the whole party)",
                       .set   = [](const control::Value& v) {
                           if (const auto mode = ModeOf(std::get<std::wstring>(v)))
                           {
                               SetMode(*mode, L"the console");
                           }
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{std::wstring(ModeWord(g_mode))}; }});
    return true;
}

void Remove() noexcept
{
    g_escapeCall.Restore();
    g_victoryCall.Restore();
    g_startCall.Restore();
}

}  // namespace lss::battle::battle_warps
