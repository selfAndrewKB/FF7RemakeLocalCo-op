#include "battle/party_switch.h"

#include "battle/command_menu_tick.h"
#include "battle/member_state.h"
#include "battle/party_switch_rules.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "platform/name_text.h"
#include "platform/verified_code.h"
#include "players/field_root.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace lss::battle::party_switch {

namespace {

using GetPlayerActorFn   = void* (*)(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate);
using CycleLeaderFn      = std::uint64_t (*)(void* system, std::uint64_t direction, std::uint64_t forced, std::uint64_t input);
using KnockedOutByTypeFn = bool (*)(void* unused, std::uint8_t type);
using KeyPressedFn       = bool (*)(const void* key, void* widget);
using InputWidgetFn      = void* (*)(void* controller);
using SaveDataFn         = void* (*)();

constexpr std::uint8_t kTypeNone = facts::kPlayerTypeNone;

/// One of the d-pad's keys as the game's poll reads them: its FKey global and the name it must carry.
struct DpadKey
{
    std::uintptr_t at       = 0;
    const wchar_t* name     = nullptr;
    bool           sideways = false;  // Left/Right: a member switch unless the save data gives them the targets
};

// The poll's knocked-out gate (set at install, then only read).
platform::CallRedirect g_pollGate;
KeyPressedFn           g_keyPressed  = nullptr;
InputWidgetFn          g_inputWidget = nullptr;
SaveDataFn             g_saveData    = nullptr;
std::uintptr_t         g_commandSlot = 0;  // the ticking command controller's slot (a seat's inside its scopes)
std::array<DpadKey, 4> g_dpadKeys{};
int                    g_dpadKeysChecked = 0;  // game thread: 0 not yet, 1 named as the research read them, -1 not
constexpr int          kDpadKeyLookups   = 600;  // frames the name table may still be silent before the check gives up
int                    g_dpadKeyLookups  = 0;

std::uintptr_t         g_target        = 0;
std::uintptr_t         g_canSwitchSite = 0;
std::uintptr_t         g_koSite        = 0;  // the KO tick's forced call (crosses its block: placed from Tick)
std::uintptr_t         g_cycleLeader   = 0;
platform::CallRedirect g_cycleCall;
platform::CallRedirect g_canSwitchCall;
platform::CallRedirect g_koCall;
std::uintptr_t         g_dpadSites[2]{};  // the d-pad's two calls of the cycle (next, previous): cross their blocks, placed from Tick
platform::CallRedirect g_dpadCalls[2];
bool                   g_placed = false;

// A forced switch refused since the last Tick (written by the hook on the dead-state tick's thread, the game thread
// [ASSUMPTION: the component tick; atomic in case it is a worker]), and on the game thread whether the downed
// leader's episode is logged as running and for how many quiet ticks it has not been refused (its end is logged
// after kQuietTicks, so a pause or a skipped tick does not end it).
constexpr int     kQuietTicks = 30;
std::atomic<bool> g_koRefusedNow{false};
bool              g_koRefusing = false;
int               g_koQuiet    = 0;

// Game thread.
std::uint8_t g_lastRefused = kTypeNone;
int          g_lastSeat    = -1;

/// Whether the switch running now must pass over member `type`, another human's: inside a seat's tick, player 1's
/// leader and every other seat's member; outside it (player 1's switch, the game's KO auto-switch), every seat's
/// member.
bool Excluded(std::uint8_t type, int ticking) noexcept
{
    if (type == kTypeNone)
    {
        return false;
    }
    if (ticking >= 1 && type == players::scopes::OtherLeaderType(ticking))
    {
        return true;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (seat != ticking && players::scopes::MemberType(seat) == type)
        {
            return true;
        }
    }
    return false;
}

void* Candidate(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate)
{
    const int seat = command_menu_tick::TickingSeat();  // 0: player 1's switch
    if (Excluded(type, seat))
    {
        if (g_lastRefused != type || g_lastSeat != seat)
        {
            g_lastRefused = type;
            g_lastSeat    = seat;
            platform::LogDebug(L"party switch: {}'s switch passes over member type {} (another player's)", players::SeatName(seat), type);
        }
        return nullptr;
    }
    return reinterpret_cast<GetPlayerActorFn>(g_target)(partyManager, variant, type, alternate);
}

/// The KO'd leader's forced switch: refused in co-op, so player 1 stays on the downed leader until a revive (the
/// game's own "nobody to take" result: no leader write, no retry, no re-target). Single player: the game's switch.
std::uint64_t KoAutoSwitch(void* system, std::uint64_t direction, std::uint64_t forced, std::uint64_t input)
{
    if (players::AnyFurtherMember())
    {
        g_koRefusedNow.store(true, std::memory_order_relaxed);
        platform::LogTrace(L"party switch: the downed leader's forced switch refused");
        return 0;
    }
    return reinterpret_cast<CycleLeaderFn>(g_cycleLeader)(system, direction, forced, input);
}

/// The leader whose switch runs now (a seat's member inside its tick, the party manager's leader slot being the seat's
/// there; player 1's character otherwise) lies knocked out. An unreadable state is not down: the game's gate stays.
bool LeaderDown() noexcept
{
    return member_state::KnockedOut(players::field_root::PartyLeader(facts::kLeaderCurrent), false);
}

int g_cyclingSeat = 0;  // the seat whose d-pad cycle runs now (game thread), 0 outside one and for player 1's

/// The d-pad's call of the cycle, player 1's and, inside its tick, each seat's: the game's cycle, run as its forced switch
/// runs it when in co-op the pressing player's character lies knocked out (party_switch_rules::ForcedCycle).
std::uint64_t RequestedCycle(void* system, std::uint64_t direction, std::uint64_t forced, std::uint64_t input)
{
    const int           seat  = command_menu_tick::TickingSeat();  // 0: player 1's press
    const bool          coop  = seat != 0 || players::AnyFurtherMember();
    const bool          down  = coop && LeaderDown();
    const std::uint64_t force = party_switch_rules::ForcedCycle(coop, down) ? 1 : forced;
    g_cyclingSeat             = seat;
    const std::uint64_t taken = reinterpret_cast<CycleLeaderFn>(g_cycleLeader)(system, direction, force, input);
    g_cyclingSeat             = 0;
    if (!coop)
    {
        return taken;
    }
    if (down)
    {
        platform::Log(L"party switch: {}'s character is down; their own switch {} (the game's two-standing gate dropped, as its own "
                      L"forced switch drops it)",
                      players::SeatName(seat), taken != 0 ? L"takes another member" : L"finds nobody to take");
    }
    else
    {
        platform::LogDebug(L"party switch: {} asked to switch to the {} member: {}", players::SeatName(seat),
                           direction != 0 ? L"next" : L"previous", taken != 0 ? L"taken" : L"refused by the game's cycle");
    }
    return taken;
}

/// The d-pad keys' globals carry the names the research read, checked on the game thread until the engine's name table
/// answers (a name it cannot give yet is asked again, for a while); another name leaves the poll's gate the game's.
bool DpadKeysNamed() noexcept
{
    if (g_dpadKeysChecked == 0)
    {
        g_dpadKeysChecked = 1;
        for (const DpadKey& key : g_dpadKeys)
        {
            std::uint64_t bits = 0;
            const bool    read = key.at != 0 && platform::Read(reinterpret_cast<const void*>(key.at), bits);
            const auto    name = read ? platform::NameText(bits) : std::wstring();
            if (read && name.empty() && ++g_dpadKeyLookups < kDpadKeyLookups)
            {
                g_dpadKeysChecked = 0;  // asked again next frame
                return false;
            }
            if (name != key.name)
            {
                g_dpadKeysChecked = -1;
                platform::Log(L"party switch: the key global for {} {}; a knocked-out player's d-pad stays unheard by the game's poll", key.name,
                              name.empty() ? L"could not be read" : L"is not as the research read it");
                break;
            }
        }
    }
    return g_dpadKeysChecked == 1;
}

/// A d-pad press this frame in the ticking controller's command menu, read as the game's poll reads it (Left and Right
/// count while the save data has them switch members).
bool DpadPressed(void* widget) noexcept
{
    bool  membersSideways = true;  // the game's rule: only the option byte at 1 gives Left and Right the targets
    void* save            = nullptr;
    if (platform::CallGuarded(g_saveData, save) && save != nullptr)
    {
        std::uint8_t targets = 0;
        membersSideways = !(platform::Read(static_cast<const std::uint8_t*>(save) + facts::kSaveDataSidewaysTargets, targets) && targets == 1);
    }
    for (const DpadKey& key : g_dpadKeys)
    {
        bool pressed = false;
        if ((membersSideways || !key.sideways) &&
            platform::CallGuarded(g_keyPressed, pressed, reinterpret_cast<const void*>(key.at), widget) && pressed)
        {
            return true;
        }
    }
    return false;
}

/// The poll's knocked-out test on the ticking controller's leader: the game's answer, except in co-op on a frame that
/// player presses the d-pad, when the leader counts as standing so the game's own poll writes the switch request (the
/// cycle then runs forced, above). The other keys of that frame reach the body only when no member can be taken.
bool KoPollGate(void* unused, std::uint8_t type)
{
    const bool down = reinterpret_cast<KnockedOutByTypeFn>(g_pollGate.Original())(unused, type);
    const int  seat = command_menu_tick::TickingSeat();  // 0: player 1's controller
    if (!down || (seat == 0 && !players::AnyFurtherMember()) || !DpadKeysNamed())
    {
        return down;
    }
    void* controller = platform::ReadPointer(reinterpret_cast<const void*>(g_commandSlot), 0);
    void* widget     = nullptr;
    if (controller == nullptr || !platform::CallGuarded(g_inputWidget, widget, controller) || widget == nullptr || !DpadPressed(widget))
    {
        return down;
    }
    platform::LogDebug(L"party switch: {}'s character is down and the d-pad is pressed: the game's poll reads this frame's keys (the switch "
                       L"request is its own)",
                       players::SeatName(seat));
    return false;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto target    = platform::FindFunction(image, facts::kGetPlayerActor);
    const auto cycle     = image.Rebase(facts::kCycleLeaderCandidateCall, 5);
    const auto canSwitch = image.Rebase(facts::kCanSwitchCandidateCall, 5);
    const auto koSite    = image.Rebase(facts::kKoAutoSwitchCall, 5);
    const auto cycleFn   = platform::FindFunction(image, facts::kCycleLeader);
    const auto dpadNext  = image.Rebase(facts::kDpadSwitchNextCall, 5);
    const auto dpadPrev  = image.Rebase(facts::kDpadSwitchPreviousCall, 5);
    if (koSite && cycleFn)
    {
        g_koSite      = *koSite;
        g_cycleLeader = *cycleFn;
    }
    if (cycleFn && dpadNext && dpadPrev)
    {
        g_dpadSites[0] = *dpadNext;
        g_dpadSites[1] = *dpadPrev;
    }
    if (!target || !cycle || !canSwitch)
    {
        platform::Log(L"party switch: an engine check failed; a player may switch onto the other's member");
        return false;
    }
    g_target        = *target;
    g_canSwitchSite = *canSwitch;
    if (!g_cycleCall.Install(*cycle, g_target, reinterpret_cast<std::uintptr_t>(&Candidate)))
    {
        platform::Log(L"party switch: the cycle's candidate call could not be redirected; a player may switch onto the other's member");
        return false;
    }
    // The poll's knocked-out gate: without it a knocked-out player's d-pad goes unheard (the switch above then never asked).
    // Its readers are set before the redirect goes live (the poll runs every frame on the game thread).
    const auto gateSite    = image.Rebase(facts::kKoPollGateCall, 5);
    const auto knockedOut  = platform::FindFunction(image, facts::kMemberKnockedOutByType);
    const auto keyPressed  = platform::FindFunction(image, facts::kCommandKeyPressed);
    const auto inputWidget = platform::FindFunction(image, facts::kCommandInputWidget);
    const auto saveData    = platform::FindFunction(image, facts::kSaveDataGet);
    const auto commandSlot = image.Rebase(facts::kCommandCtrlSlotGlobal, sizeof(void*));
    g_dpadKeys             = {DpadKey{image.Rebase(facts::kKeyDPadUp, 8).value_or(0), L"Gamepad_DPad_Up", false},
                              DpadKey{image.Rebase(facts::kKeyDPadDown, 8).value_or(0), L"Gamepad_DPad_Down", false},
                              DpadKey{image.Rebase(facts::kKeyDPadLeft, 8).value_or(0), L"Gamepad_DPad_Left", true},
                              DpadKey{image.Rebase(facts::kKeyDPadRight, 8).value_or(0), L"Gamepad_DPad_Right", true}};
    const bool gateFound = gateSite && knockedOut && keyPressed && inputWidget && saveData && commandSlot;
    if (gateFound)
    {
        g_keyPressed  = reinterpret_cast<KeyPressedFn>(*keyPressed);
        g_inputWidget = reinterpret_cast<InputWidgetFn>(*inputWidget);
        g_saveData    = reinterpret_cast<SaveDataFn>(*saveData);
        g_commandSlot = *commandSlot;
    }
    if (!gateFound || !g_pollGate.Install(*gateSite, *knockedOut, reinterpret_cast<std::uintptr_t>(&KoPollGate)))
    {
        g_keyPressed  = nullptr;
        g_inputWidget = nullptr;
        g_saveData    = nullptr;
        g_commandSlot = 0;
        platform::Log(L"party switch: the input poll's knocked-out gate could not be redirected; a knocked-out player's d-pad stays unheard");
    }
    return true;
}

int CyclingSeat() noexcept
{
    return g_cyclingSeat;
}

void Remove() noexcept
{
    g_pollGate.Restore();
    g_canSwitchCall.Restore();
    g_cycleCall.Restore();
    // The KO tick's crossing call and the d-pad's two stay (only the thread that runs them could put them back); the DLL
    // is pinned for them (platform/call_redirect.h), and each passes the game's call through with no further player in.
}

void Tick() noexcept
{
    const bool refused = g_koRefusedNow.exchange(false, std::memory_order_relaxed);
    g_koQuiet          = refused ? 0 : g_koQuiet + 1;
    if (refused && !g_koRefusing)
    {
        g_koRefusing = true;
        platform::LogDebug(L"party switch: player 1's leader is down; player 1 stays on it until a revive (co-op: no forced switch)");
    }
    else if (g_koRefusing && g_koQuiet >= kQuietTicks)
    {
        g_koRefusing = false;
        platform::LogDebug(L"party switch: the downed leader's forced switch has not run for {} ticks (revived, the battle ended, or single player)",
                           kQuietTicks);
    }
    if (g_placed || g_canSwitchSite == 0)
    {
        return;
    }
    g_placed = true;
    if (g_canSwitchCall.InstallFromOwnThread(g_canSwitchSite, g_target, reinterpret_cast<std::uintptr_t>(&Candidate)))
    {
        platform::Log(L"party switch: installed; neither player's switch lands on the other's member");
    }
    else
    {
        platform::Log(L"party switch: CanSwitch's candidate call could not be redirected; a manual switch may count the other's member");
    }
    if (g_koSite != 0 && g_koCall.InstallFromOwnThread(g_koSite, g_cycleLeader, reinterpret_cast<std::uintptr_t>(&KoAutoSwitch)))
    {
        platform::Log(L"party switch: in co-op a downed leader's forced switch is refused; every player stays on their character until revived");
    }
    else
    {
        platform::Log(L"party switch: the KO tick's forced switch could not be redirected; a downed player 1 is moved to a standing AI ally");
    }
    // The d-pad's two calls together, or neither: one direction dropping the gate and the other not would puzzle a player.
    bool dpad = g_cycleLeader != 0;
    for (std::size_t i = 0; dpad && i < 2; ++i)
    {
        dpad = g_dpadSites[i] != 0 &&
               g_dpadCalls[i].InstallFromOwnThread(g_dpadSites[i], g_cycleLeader, reinterpret_cast<std::uintptr_t>(&RequestedCycle));
    }
    if (dpad)
    {
        platform::Log(L"party switch: a downed player's own d-pad switch takes a standing AI member (the game's two-standing gate dropped "
                      L"in co-op{})",
                      g_pollGate.IsInstalled() ? L"; the input poll hears the d-pad of a knocked-out player" : L"");
    }
    else
    {
        g_dpadCalls[0].Restore();  // a crossing call put back from the thread that placed it, between the ticks
        g_pollGate.Restore();      // without the forced cycle, an opened poll would only let the other keys of that frame reach the body
        platform::Log(L"party switch: the d-pad's calls could not be redirected; a downed player's switch stays the game's refusal, their "
                      L"d-pad unheard by the game's poll");
    }
}

}  // namespace lss::battle::party_switch
