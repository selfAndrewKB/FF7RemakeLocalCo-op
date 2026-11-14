#include "battle/seat_toad.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/entry_redirect.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/actor_fields.h"
#include "players/field_root.h"
#include "players/party_seat.h"
#include "players/scopes.h"
#include "players/seat_camera.h"
#include "players/seats.h"
#include "world/actor_place.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <string>

namespace lss::battle::seat_toad {

namespace {

using players::SeatName;

using SwapFn      = std::uint64_t (*)(void* manager, std::uint8_t type);
using AddStatusFn = std::uint64_t (*)(void* component, std::uint8_t status, std::uint8_t level, float scale, std::uint8_t force);

constexpr unsigned kSwapContexts = players::scopes::Primary | players::scopes::Leader | players::scopes::UiSlot;
constexpr float    kGameTimeScale = 1.0f;  // the scale the game's own status sources pass

// Set at install, then only read.
std::uintptr_t g_apply = 0, g_cure = 0;
AddStatusFn    g_addStatus = nullptr;

// Game thread.
platform::EntryRedirect  g_applyEntry, g_cureEntry;
platform::GameThreadMark g_thread;
bool                     g_placed   = false;
int                      g_toadSeat = 0;
SwappedFn                g_swapped  = nullptr;
void*                    g_context  = nullptr;

/// After a swap in a seat's place, the seat's camera read once a second for a few seconds: whether its operator owns the
/// character the controller now holds, ticks, has finished the leader-change move, and in which state, and where the
/// seat's view is against that character.
struct CameraWatch
{
    int                                   seat    = 0;  // 0: no watch
    int                                   sample  = 0;
    std::chrono::steady_clock::time_point nextAt;
    std::uint32_t                         ticks   = 0;  // the operator's tick count at the last sample
};
constexpr int                       kWatchSamples = 6;
constexpr std::chrono::milliseconds kWatchSpacing{1000};
CameraWatch                         g_watch;

void SampleCamera() noexcept
{
    const int           seat       = g_watch.seat;
    const auto*         op         = static_cast<const std::uint8_t*>(players::seat_camera::Operator(seat));
    void* const         controller = players::scopes::Controller(seat);
    void* const         character  = controller != nullptr ? players::actor_fields::LeaderCharacter(controller) : nullptr;
    void*               owner      = nullptr;
    std::uint8_t        state[2]   = {0xFF, 0xFF};
    std::uint8_t        moving     = 0xFF;
    const bool          ownerRead  = op != nullptr && platform::Read(op + facts::kOperatorOwnerChara, owner);
    if (op != nullptr)
    {
        platform::CopyGuarded(state, op + facts::kOperatorCameraState, sizeof(state));
        if (const auto* personal = static_cast<const std::uint8_t*>(platform::ReadPointer(op, facts::kOperatorPersonalLeaderChange)))
        {
            platform::Read(personal + facts::kCameraFlavorActive, moving);
        }
    }
    float       view[3] = {}, at[3] = {};
    const auto* manager = static_cast<const std::uint8_t*>(controller != nullptr ? players::actor_fields::CameraManager(controller) : nullptr);
    const bool  placed  = manager != nullptr && platform::CopyGuarded(view, manager + facts::kCameraManagerPovLocation, sizeof(view)) &&
                         character != nullptr && world::actor_place::ActorLocation(character, at);
    const std::uint32_t ticks  = players::seat_camera::OperatorTicks(seat);
    const auto          number = [](std::uint8_t value) { return value == 0xFF ? std::wstring(L"unread") : std::to_wstring(value); };
    platform::LogDebug(L"seat toad: {}'s camera {} s after the swap: its operator's owner {}; operator ticks +{}; the leader-change move "
                       L"{}; camera state {} (next {}); its view {}",
                       SeatName(seat), g_watch.sample,
                       !ownerRead ? L"unread" : owner == character ? L"the character the controller holds" : L"NOT the character the controller holds",
                       ticks - g_watch.ticks, moving == 0 ? L"done" : moving == 1 ? L"still running" : L"unread", number(state[0]),
                       number(state[1]),
                       placed ? std::format(L"{:.1f} m from the character", std::hypot(view[0] - at[0], view[1] - at[1], view[2] - at[2]) / 100)
                              : std::wstring(L"unread"));
    g_watch.ticks = ticks;
}

/// The seat marked as the one whose swap runs, for the game call's length.
class ToadPass
{
public:
    explicit ToadPass(int seat) noexcept { g_toadSeat = seat; }
    ~ToadPass() { g_toadSeat = 0; }
    ToadPass(const ToadPass&)            = delete;
    ToadPass& operator=(const ToadPass&) = delete;
};

/// The seat whose player holds member `type` now (seated, and the member on its controller, not with its AI), else 0.
int SeatPlaying(std::uint8_t type) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::party_seat::Seated(seat) && players::party_seat::MemberType(seat) == type &&
            players::actor_fields::PlayerSlot(players::party_seat::Member(seat)) != nullptr)
        {
            return seat;
        }
    }
    return 0;
}

/// The game's swap, in the place of the seat that plays `type` when there is one. `what` names the swap in the log.
std::uint64_t SwapInSeatPlace(std::uintptr_t original, void* manager, std::uint8_t type, const wchar_t* what)
{
    const auto game = [&] { return reinterpret_cast<SwapFn>(original)(manager, type); };
    const int  seat = g_thread.Here() ? SeatPlaying(type) : 0;
    if (seat == 0 || players::scopes::OpenPlayer(players::scopes::All) != 0)
    {
        if (seat != 0)
        {
            platform::LogDebug(L"seat toad: {}'s character {} inside another player's scope: the game's swap as it stands", SeatName(seat), what);
        }
        return game();
    }
    void* const   controller = players::scopes::Controller(seat);
    void* const   before     = controller != nullptr ? players::actor_fields::LeaderCharacter(controller) : nullptr;
    std::uint64_t result     = 0;
    bool          held       = false;
    {
        const players::scopes::Scope scope(seat, kSwapContexts);
        held = scope.Held();
        if (held)
        {
            const ToadPass pass(seat);
            result = game();
        }
    }
    if (!held)
    {
        platform::Log(L"seat toad: {}'s character {} (member type {}); {}'s scope could not open: the game's swap as it stands "
                      L"(its controller and camera stay where they were)",
                      SeatName(seat), what, type, SeatName(seat));
        return game();
    }
    void* const after = controller != nullptr ? players::actor_fields::LeaderCharacter(controller) : nullptr;
    const bool  moved = after != before && after != nullptr;
    platform::Log(L"seat toad: {}'s character {} (member type {}); the game's swap ran in {}'s place: {}", SeatName(seat), what, type,
                  SeatName(seat),
                  moved ? L"its controller moved onto it (its camera: the watch's lines)"
                        : L"its controller did not move (the game's leader branch did not run for it)");
    if (moved && g_swapped != nullptr)
    {
        g_swapped(g_context, seat, after);  // adopted now: a cure in the same drain finds the toad as the seat's leader
    }
    if (moved)
    {
        g_watch = {.seat = seat, .sample = 0, .nextAt = std::chrono::steady_clock::now(), .ticks = players::seat_camera::OperatorTicks(seat)};
    }
    return result;
}

std::uint64_t Apply(void* manager, std::uint8_t type)
{
    return SwapInSeatPlace(g_applyEntry.Original(), manager, type, L"becomes a toad");
}

std::uint64_t Cure(void* manager, std::uint8_t type)
{
    return SwapInSeatPlace(g_cureEntry.Original(), manager, type, L"is no longer a toad");
}

/// The character `player` (1 the game's leader, 2.. a seat's member) plays now, or null.
void* CharacterOf(int player) noexcept
{
    return player == 1 ? players::field_root::PartyLeader(facts::kLeaderCurrent) : players::party_seat::Member(player - 1);
}

/// The test action: the game's status setter gives the player's character the Frog status, as an enemy's spell does.
control::Outcome GiveFrog(int player) noexcept
{
    if (!players::field_root::InBattle())
    {
        return {false, L"not in a fight: the toad is a battle status"};
    }
    void* const character = CharacterOf(player);
    void* const component = character != nullptr ? players::actor_fields::BattleComponent(character) : nullptr;
    if (component == nullptr)
    {
        return {false, std::format(L"player {} plays no character with a battle component now", player)};
    }
    std::uint64_t result = 0;
    if (!platform::CallGuarded(g_addStatus, result, component, facts::kStatusFrog, facts::kAddStatusLevelMiddle, kGameTimeScale,
                               std::uint8_t{1}))
    {
        return {false, L"the game's status setter faulted"};
    }
    platform::Log(L"seat toad: the test gave player {}'s character the Frog status at the status table's middle duration (the "
                  L"next battle tick applies it; its timer cures it)",
                  player);
    return {};
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto apply     = platform::FindFunction(image, facts::kToadApply);
    const auto cure      = platform::FindFunction(image, facts::kToadCure);
    const auto addStatus = platform::FindFunction(image, facts::kAddStatus);
    if (!apply || !cure || !addStatus || !players::field_root::Installed())
    {
        platform::Log(L"seat toad: an engine check failed; a toad on player 2's or 3's character leaves that player's camera on the hidden character");
        return false;
    }
    g_apply     = *apply;
    g_cure      = *cure;
    g_addStatus = reinterpret_cast<AddStatusFn>(*addStatus);
    control::Register({.name  = L"toad",
                       .shape = control::Shape::Integer,
                       .min   = 1,
                       .max   = players::kSeatsBuilt + 1,
                       .help  = L"(test, in a fight) give player N's character the Frog status (the status table's middle "
                                L"duration) through the game's own status setter: the toad swap follows on the next battle "
                                L"tick, its timer cures it",
                       .set   = [](const control::Value& value) { return GiveFrog(static_cast<int>(std::get<std::int64_t>(value))); }});
    return true;
}

void Remove() noexcept
{
    g_cureEntry.Restore();
    g_applyEntry.Restore();
    SetListener(nullptr, nullptr);
}

void Tick() noexcept
{
    if (g_watch.seat != 0 && std::chrono::steady_clock::now() >= g_watch.nextAt)
    {
        SampleCamera();
        g_watch.nextAt += kWatchSpacing;
        if (++g_watch.sample >= kWatchSamples)
        {
            g_watch = {};
        }
    }
    if (g_placed || g_apply == 0)
    {
        return;
    }
    g_placed = true;
    g_thread.Set();
    // Both open with three pushes; the game thread alone runs them, and it is here, outside.
    const bool placed = g_applyEntry.Install(g_apply, std::span(facts::kToadApplyEntry.data(), facts::kToadSwapFirst),
                                             reinterpret_cast<std::uintptr_t>(&Apply)) &&
                        g_cureEntry.Install(g_cure, std::span(facts::kToadCureEntry.data(), facts::kToadSwapFirst),
                                            reinterpret_cast<std::uintptr_t>(&Cure));
    if (!placed)
    {
        g_cureEntry.Restore();
        g_applyEntry.Restore();
        platform::Log(L"seat toad: the toad swaps could not be redirected; a toad on player 2's or 3's character leaves that player's "
                      L"camera on the hidden character");
        return;
    }
    platform::Log(L"seat toad: installed; a toad swap of player 2's or 3's character moves that player's controller and camera, as the game's "
                  L"does for player 1");
}

int ToadSeat() noexcept
{
    return g_toadSeat;
}

void SetListener(SwappedFn swapped, void* context) noexcept
{
    g_swapped = swapped;
    g_context = context;
}

}  // namespace lss::battle::seat_toad
