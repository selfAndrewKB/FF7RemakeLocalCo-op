#include "players/session.h"
#include "players/unreal_call.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_exit.h"
#include "players/field_hold.h"
#include "players/field_root.h"
#include "players/game_state.h"
#include "players/lead_gate.h"
#include "players/local_player_list.h"
#include "players/own_controller.h"
#include "players/party_events.h"
#include "players/party_member.h"
#include "players/party_rank_rules.h"
#include "players/party_seat.h"
#include "players/seat_camera.h"
#include "players/seat_objects.h"
#include "players/weak_object.h"
#include "battle/arena_warp.h"
#include "battle/command_menu.h"
#include "battle/targeting.h"
#include "battle/lock_camera.h"
#include "world/run_catch_up.h"
#include "world/talk_gather.h"
#include "world/traversal_action.h"
#include "views/scene_camera.h"
#include "players/pad_buttons.h"
#include "players/scopes.h"
#include "players/split_render.h"

#include <Unreal/Core/Containers/FString.hpp>

#include <cstdint>
#include <format>
#include <new>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::players {

namespace {

using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

/// The field is back after a world change (RE-lifecycle-world.md §8): play state, the steady step, running. The
/// entry's step 12 re-reconciles every non-leader controller, so a join before step 13 would be undone.
constexpr std::uint8_t kStatePlay  = 6;
constexpr std::uint8_t kStateLoading = 5;
constexpr std::uint8_t kStateLeaving = 9;
constexpr std::int32_t kStepSteady = 13;

/// How long the field must stay back, with player 1 on a character, before a seat rejoins; and how many joins
/// may fail before the rejoin is left to Ctrl+F7. Timed, not counted in ticks: loads tick uncapped.
constexpr std::chrono::seconds kRejoinSteady{2};
constexpr int                  kRejoinFailures = 3;
/// A refused seat (the funnel did not take the member) is tried again after this many ticks; the party order is
/// checked under a held member every this many ticks (cheap guarded reads).
constexpr std::uint32_t kSeatRetryTicks  = 60;
constexpr std::uint32_t kSwapGraceTicks  = 30;  // after a leader change: the seat's next character may still be held for a moment
constexpr std::uint32_t kFollowSlotTicks = 10;

UObject* Field(const void* object, std::uint32_t offset) noexcept
{
    UObject* value = nullptr;
    return object != nullptr && platform::CopyGuarded(&value, static_cast<const std::uint8_t*>(object) + offset,
                                                      sizeof(value))
               ? value
               : nullptr;
}

using local_player_list::Describe;
using local_player_list::GameInstance;

// UE4SS's object lookups can throw (a malformed path, or running out of memory), so every function here
// that makes one catches: an exception escaping a noexcept function would end the game.

RC::Unreal::TArray<UObject*>* LocalPlayers(UObject* gameInstance) noexcept
try
{
    return gameInstance == nullptr
               ? nullptr
               : gameInstance->GetValuePtrByPropertyNameInChain<RC::Unreal::TArray<UObject*>>(STR("LocalPlayers"));
}
catch (...)
{
    return nullptr;
}

bool Contains(RC::Unreal::TArray<UObject*>& players, UObject* player) noexcept
{
    for (std::int32_t i = 0; i < players.Num(); ++i)
    {
        if (players[i] == player)
        {
            return true;
        }
    }
    return false;
}

/// Calls a reflected function with no parameters (e.g. AActor.K2_DestroyActor) on `target`.
bool CallNoParams(UObject* target, const wchar_t* path) noexcept
try
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
    if (target == nullptr || function == nullptr || function->GetParmsSize() != 0)
    {
        return false;
    }
    return GuardedProcessEvent(target, function, nullptr);
}
catch (...)
{
    return false;
}

}  // namespace

void Session::Seat::ResetMember() noexcept
{
    memberWeak[0] = -1;
    memberWeak[1] = 0;
    handedToAi      = false;
    holdHand        = false;
    keptThroughLock = false;
    returnWaitLogged = false;  // the next member's "stays with its AI" line is its own
    hold            = party_member::Wait::None;
    joinedInHold    = false;
}

Session::Session() noexcept
{
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        seats_[seat].index = seat;
    }
}

bool Session::Install(const platform::GameImage& image) noexcept
{
    const auto remove = platform::FindFunction(image, facts::kRemoveLocalPlayer);
    if (!remove || !own_controller::Install(image))
    {
        return false;
    }
    removeLocalPlayer_ = *remove;
    cameraInstalled_   = seat_camera::Install(image);  // optional: without it the seat's camera stays still
    seatInstalled_     = party_seat::Install(image) && party_member::Install(image);  // optional: without it the seat has no member
    split_render::Install(image);                            // optional: without it the views keep the split rule's reduced effects
    fieldExitInstalled_ = field_exit::Install(image, &Session::OnFieldExit, this);  // without it, Tick's step check alone
    if (seatInstalled_)
    {
        // Without it the periodic check alone follows the party (one tick late at a leader change).
        eventsInstalled_ = party_events::Install(image, {.leaderChanging   = &Session::OnLeaderChanging,
                                                         .streamFinishing  = &Session::OnStreamFinishing,
                                                         .streamFinished   = &Session::OnStreamFinished,
                                                         .leaderHandedToAi = &Session::OnLeaderHandedToAi,
                                                         .leaderReturned   = &Session::OnLeaderReturned,
                                                         .context          = this});
    }
    return true;
}

void Session::Remove() noexcept
{
    if (fieldExitInstalled_)
    {
        field_exit::Remove();
        fieldExitInstalled_ = false;
    }
    if (eventsInstalled_)
    {
        party_events::Remove();
        eventsInstalled_ = false;
    }
}

int Session::Count() const noexcept
{
    int count = 0;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        count += seats_[seat].localPlayer != nullptr ? 1 : 0;
    }
    return count;
}

std::uint32_t Session::SeatIds() const noexcept
{
    std::uint32_t ids = 1;  // player 1's id 0
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        ids |= seats_[seat].localPlayer != nullptr ? 1u << seats_[seat].ControllerId() : 0u;
    }
    return ids;
}

UObject* Session::Controller(const Seat& seat) const noexcept
{
    return Field(seat.localPlayer, facts::kLocalPlayerController);
}

void Session::EndMemberModules(const Seat& seat) noexcept
{
    battle::targeting::OnLeave(seat.index);
    battle::lock_camera::OnLeave(seat.index);
    battle::command_menu::OnLeave(seat.index);
}

void Session::OnFieldExit(void* self, const wchar_t* caller) noexcept
{
    auto* session = static_cast<Session*>(self);
    if (session->Count() == 0)
    {
        return;
    }
    if (GetCurrentThreadId() != session->gameThread_)
    {
        platform::Log(L"field exit: requested off the game thread ({}); the players are left to the field-step check", caller);
        return;
    }
    session->LeaveForWorldChange(caller);
}

void Session::LeaveForWorldChange(const wchar_t* why) noexcept
{
    // Each seat's column first, before any leaves (the list closes up behind a leave): the rejoin restores that order.
    auto* players = LocalPlayers(gameInstance_);
    for (int index = 1; index <= kSeatsBuilt; ++index)
    {
        Seat& seat     = seats_[index];
        seat.lastColumn = kNoColumn;
        for (std::int32_t i = 1; players != nullptr && seat.localPlayer != nullptr && i < players->Num(); ++i)
        {
            seat.lastColumn = (*players)[i] == seat.localPlayer ? i : seat.lastColumn;
        }
    }
    // The highest seat first (any order works: RemoveLocalPlayer takes a player from anywhere in the list).
    for (int index = kSeatsBuilt; index >= 1; --index)
    {
        Seat& seat   = seats_[index];
        seat.standIn = facts::kPlayerTypeNone;  // a new world: every seat back to its own row, out or in
        if (seat.localPlayer == nullptr)
        {
            continue;
        }
        platform::Log(L"world change ({}): {} leaves first{}", why, seat.Name(), Seated(seat.index) ? L" and gives its member back" : L"");
        if (!TakeOut(seat, true))
        {
            worldChangeStuck_ = true;
            platform::Log(L"world change: {} could NOT be taken out; the game tears the field down with it in", seat.Name());
            continue;
        }
        seat.rejoin = Rejoin{.pending = true};
        if (mirror_.fillAtLeave != nullptr)
        {
            // The column held through the loading screen (the seat's column, once per seat that leaves).
            mirror_.fillAtLeave(std::format(L"{} left for a world change", seat.Name()).c_str(), 1 + Count(), SeatIds());
        }
        platform::Log(L"world change: {} joins again once the field is back ({} cancels)", seat.Name(), LeaveKey(seat.index));
    }
}

void Session::Tick() noexcept
try
{
    gameThread_                     = GetCurrentThreadId();
    ++ticks_;
    // The instance lives as long as the game; found once (a UE4SS object walk, never per tick: one try a second until
    // it exists), so LocalPlayer() answers before any join: the HUD's two-view layout under the mirror player read
    // every view through it and saw none in a session without a join (the user, 2026-10-22 10:13: markers drifting).
    if (gameInstance_ == nullptr && ticks_ % 60 == 1)
    {
        gameInstance_ = GameInstance();
    }
    if (eventsInstalled_)
    {
        party_events::Tick();  // the entry redirects, placed from here (outside the functions, on their thread)
    }
    const game_state::Snapshot game = game_state::Current();
    const bool steady = game.valid && game.hasRoot && game.running && game.step == kStepSteady;
    if (steady)
    {
        worldChangeStuck_ = false;
    }
    auto* players = LocalPlayers(gameInstance_);  // one lookup a tick, for every seat
    // Player 1 home (the game's leader is the field leader): no stand-in can hold, whatever event was missed (a retry,
    // a reload, a leader change not heard). The leader change's own clear is the fast path.
    const std::uint8_t leaderType = party_member::GameLeaderType();
    const bool         home       = leaderType != facts::kPlayerTypeNone && leaderType == party_member::FieldLeaderType();
    for (int index = 1; index <= kSeatsBuilt; ++index)
    {
        if (home && seats_[index].standIn != facts::kPlayerTypeNone)
        {
            platform::LogDebug(L"{}'s stand-in (type {}) dropped: player 1 is on the field leader", seats_[index].Name(),
                               seats_[index].standIn);
            seats_[index].standIn = facts::kPlayerTypeNone;
        }
    }
    for (int index = 1; index <= kSeatsBuilt; ++index)
    {
        Seat& seat = seats_[index];
        if (seat.localPlayer == nullptr)
        {
            TryRejoin(seat);  // after a world change, or for the member the seat waits on
            continue;
        }
        if (players != nullptr && !Contains(*players, seat.localPlayer))
        {
            platform::Log(L"{} was removed by the engine; the mod lets go of it", seat.Name());
            Forget(seat);
            continue;
        }
        // The fallback: the field is leaving (running cleared, or an exit step) and the request was not seen. Every
        // seat goes out (the tick's remaining seats with it).
        if (!worldChangeStuck_ && game.valid && game.hasRoot && (!game.running || game.step > kStepSteady))
        {
            platform::Log(L"field exit not seen at the request (state {}, step {}, running {})", game_state::StateName(game.state),
                          game.step, game.running ? L"on" : L"off");
            LeaveForWorldChange(L"the field-step check");
            return;
        }
        if (!seatInstalled_ || !steady || game.state != kStatePlay)
        {
            continue;
        }
        if (!Seated(seat.index))
        {
            // After a release (the row changed, or its member left): the row's member now, or out until one is free.
            // An unreadable party is a tick's thing, not a wait.
            party_member::Choice choice;
            if (!TrySeat(seat, choice) && !Seated(seat.index) && ticks_ >= seat.seatRetryAt && ticks_ >= seat.seatGraceUntil &&
                choice.wait != party_member::Wait::Unread)
            {
                LogWait(seat, choice, L"is in without a member");
                LeaveToAwait(seat, party_member::Describe(choice.wait));
            }
        }
        else if (ticks_ % kFollowSlotTicks == 0)
        {
            FollowSlot(seat);
        }
    }
}
catch (...)
{
    platform::Log(L"session tick: an engine lookup raised an exception");
}

void Session::TryRejoin(Seat& seat) noexcept
try
{
    if (!seat.rejoin.pending && !seat.awaiting)
    {
        return;
    }
    const game_state::Snapshot game = game_state::Current();
    auto*                      players = LocalPlayers(gameInstance_);
    // Player 1 on a character also keeps the title screen out (a field in the play state too, with no character).
    // The list holds player 1, the seats in and the mirrors (which hold the empty columns). After a world change the
    // seats join again in the order of the columns they held, so each takes back its own monitor: a seat waits while
    // another seat still to rejoin held an earlier column (a seat that held none, after every seat that did; ties by
    // seat).
    bool lowerPending = false;
    for (int other = 1; seat.rejoin.pending && other <= kSeatsBuilt; ++other)
    {
        const Seat& o = seats_[other];
        const bool  earlier = o.lastColumn < seat.lastColumn || (o.lastColumn == seat.lastColumn && other < seat.index);
        lowerPending = lowerPending || (other != seat.index && o.rejoin.pending && o.localPlayer == nullptr && earlier);
    }
    const bool ready = game.valid && game.state == kStatePlay && game.hasRoot && game.running && game.step == kStepSteady &&
                       players != nullptr && players->Num() == 1 + Count() + Mirrors() && !lowerPending &&
                       Field(Field((*players)[0], facts::kLocalPlayerController), facts::kControllerPawn) != nullptr;
    const auto now = std::chrono::steady_clock::now();
    if (!ready)
    {
        seat.rejoin.steady = false;
        // The field left under the wait (or cannot be read): the join after it settles as after any world change. A
        // pause, a menu or a battle step leaves the field running: the wait goes on and joins once it is ready again.
        if (seat.awaiting && !seat.rejoin.pending &&
            (!game.valid || !game.hasRoot || !game.running || game.state == kStateLoading || game.state == kStateLeaving))
        {
            seat.rejoin.pending = true;
        }
        return;
    }
    if (seat.rejoin.pending)
    {
        if (!seat.rejoin.steady)
        {
            seat.rejoin.steady = true;
            seat.rejoin.since  = now;
            platform::Log(L"rejoin: the field is back ({}); {} joins in {} s if it stays", game.location, seat.Name(),
                          kRejoinSteady.count());
            return;
        }
        if (now - seat.rejoin.since < kRejoinSteady)
        {
            return;
        }
    }
    const bool wasAwaiting = seat.awaiting;
    if (seat.awaiting)
    {
        // The member first: nothing is created for the seat until the row's member is free (logged once per reason).
        if (ticks_ < seat.seatRetryAt)
        {
            return;  // the seat funnel refused the member last time; not tried again before its retry tick
        }
        UObject* const playerOneController = Field((*players)[0], facts::kLocalPlayerController);
        SettleRank(seat, playerOneController);
        const party_member::Choice choice = party_member::Choose(seat.index, playerOneController, nullptr, false, seat.standIn);
        LogFormation(seat, choice);
        if (choice.member == nullptr)
        {
            // After a world change the reason is said again even when unchanged: the field is a new one.
            LogWait(seat, choice, L"joins once its member is free", seat.rejoin.pending);
            seat.rejoin = Rejoin{};  // the field has settled; the wait goes on without the rejoin's delay
            return;
        }
        seat.awaiting = false;  // Join sets it again if the member is gone by then
    }
    const bool afterWorldChange = seat.rejoin.pending;
    JoinNow(seat.index);
    if (seat.localPlayer == nullptr)
    {
        if (seat.awaiting)
        {
            seat.rejoin = Rejoin{};  // the join found the member not free after all: the wait goes on
            return;
        }
        // The join itself failed (logged above): a few more tries, each tick for a wait, after the settle for a rejoin.
        seat.rejoin.steady = false;
        seat.awaiting      = wasAwaiting;
        if (++seat.rejoin.failures >= kRejoinFailures)
        {
            seat.rejoin   = Rejoin{};
            seat.awaiting = false;
            platform::Log(L"{}: {} did not join in {} tries (see above); press {} to join",
                          afterWorldChange ? L"rejoin" : L"the wait for a member", seat.Name(), kRejoinFailures, JoinKey(seat.index));
        }
        return;
    }
    seat.rejoin = Rejoin{};
    if (afterWorldChange)
    {
        platform::Log(L"rejoin: {} is back", seat.Name());
    }
}
catch (...)
{
    platform::Log(L"rejoin: an engine lookup raised an exception");
}

void Session::LeaveToAwait(Seat& seat, const wchar_t* why) noexcept
{
    platform::Log(L"{} leaves: {}; it joins again once its member is free ({} stops the wait)", seat.Name(), why, LeaveKey(seat.index));
    if (!TakeOut(seat, false))
    {
        platform::Log(L"{} could not leave (see above); it stays in without a member", seat.Name());
        return;
    }
    seat.awaiting = true;  // the wait's reason stays logged: the join's wait line names it again only when it changes
}

void Session::LogWait(Seat& seat, const party_member::Choice& choice, const wchar_t* who, bool again) noexcept
{
    seat.lastWait           = choice.wait;
    const std::uint32_t key = 1u | static_cast<std::uint32_t>(choice.type) << 8 | static_cast<std::uint32_t>(choice.wait) << 16 |
                              static_cast<std::uint32_t>(choice.ordinal + 1) << 24;
    if (key == seat.waitLogged && !again)
    {
        return;
    }
    if (seat.waitLogged == 0)
    {
        seat.awaitSince = ticks_;
    }
    seat.waitLogged = key;
    platform::Log(L"{} {} (row {} of the party order, type {}): {}; the game's party order chooses it, so a party change ends the wait",
                  seat.Name(), who, choice.ordinal, choice.type, party_member::Describe(choice.wait));
    if (party_member::ScriptHold(choice.wait) || choice.wait == party_member::Wait::NoFieldAi ||
        choice.wait == party_member::Wait::LeaderHanded || choice.wait == party_member::Wait::PushButtonEvent)
    {
        // The member's AI and the lock mask as the take test saw them (the actor of the row, read again).
        auto* players = LocalPlayers(gameInstance_);
        if (players != nullptr && players->Num() >= 1)
        {
            platform::Log(L"  the row's member: {}",
                          party_member::AiDetail(party_member::RowActor(seat.index, Field((*players)[0], facts::kLocalPlayerController), seat.standIn)));
        }
    }
}

void Session::LogFormation(Seat& seat, const party_member::Choice& choice) noexcept
{
    // The wait's own line names only the seat's row and its reason, so a formation that changes without changing
    // them (Barret entering the party order, the game's leader moving) was invisible in the 2026-10-22 10:30 log.
    if (choice.formation == 0 || choice.formation == seat.formationLogged)
    {
        return;
    }
    seat.formationLogged = choice.formation;
    platform::LogDebug(L"party member: the formation under the wait reads types {} {} {}, field leader type {} ({}'s row: ordinal {} type {}: {})",
                       choice.formation & 0xFF, (choice.formation >> 8) & 0xFF, (choice.formation >> 16) & 0xFF,
                       choice.formation >> 24, seat.Name(), choice.ordinal, choice.type, party_member::Describe(choice.wait));
}

void Session::Forget(Seat& seat) noexcept
{
    seat.localPlayer = nullptr;
    seat.standIn     = facts::kPlayerTypeNone;  // out of the game: back to its own row when it joins again
    EndMemberModules(seat);
    scopes::Unbind(seat.index);
    seat_objects::Forget(seat.index);
    pad_buttons::SetPads(SeatIds());  // the split effects' mask follows the further views by the tick (split_render::Tick)
    if (cameraInstalled_)
    {
        seat_camera::Take(seat.index);  // forgotten, not destroyed: it belonged to the old world
    }
    if (seatInstalled_)
    {
        party_seat::Forget(seat.index);
    }
    seat.ResetMember();
    seat.waitLogged      = 0;
    seat.lastWait        = party_member::Wait::None;
    seat.formationLogged = 0;
}

UObject* Session::PlayerOneController() const noexcept
{
    return Field(LocalPlayer(0), facts::kLocalPlayerController);
}

std::uint32_t Session::RanksHeldBesides(int index) const noexcept
{
    std::uint32_t held = 0;
    for (int other = 1; other <= kSeatsBuilt; ++other)
    {
        const Seat& o = seats_[other];
        if (other != index && (o.localPlayer != nullptr || o.awaiting || o.rejoin.pending))
        {
            const int rank = party_member::Rank(other);
            held |= rank >= 1 && rank < 32 ? 1u << rank : 0u;
        }
    }
    return held;
}

void Session::SettleRank(Seat& seat, void* playerOneController) noexcept
{
    const int count = party_member::Ranks(playerOneController);
    const int rank  = party_member::Rank(seat.index);
    if (count < 1 || rank <= count)
    {
        return;
    }
    if (const int lowest = party_rank_rules::Lowest(RanksHeldBesides(seat.index), count); lowest != 0)
    {
        party_member::SetRank(seat.index, lowest);
        platform::Log(L"{}'s place in the party order (rank {}) is gone with the party's {} free row(s): it takes rank {}",
                      seat.Name(), rank, count, lowest);
    }
}

void Session::Join(int index) noexcept
{
    if (SeatBuilt(index))
    {
        Seat& seat = seats_[index];
        if (seat.localPlayer == nullptr && !seat.awaiting && !seat.rejoin.pending)
        {
            // The next party member no other player plays (party_rank_rules::Lowest; none free: its own number, and the
            // member test below says why it waits).
            const int lowest = party_rank_rules::Lowest(RanksHeldBesides(index), party_rank_rules::kMaxRanks);
            const int rank   = lowest != 0 ? lowest : index;
            if (rank != party_member::Rank(index))
            {
                platform::Log(L"{} takes rank {} of the party order (the next member no other player plays)", seat.Name(), rank);
            }
            party_member::SetRank(index, rank);
        }
    }
    JoinNow(index);
}

bool Session::CanStepMember(int index) const noexcept
{
    // Seated and playing (not handed to its AI for a hold), in the field: a member swap mid-fight is unbuilt ground.
    if (!In(index) || !Seated(index) || HandedToAi(index) || seats_[index].standIn != facts::kPlayerTypeNone ||
        field_root::InBattle())
    {
        return false;
    }
    const int rank = party_member::Rank(index);
    const int next = party_rank_rules::Step(rank, 1, RanksHeldBesides(index), party_member::Ranks(PlayerOneController()));
    return next != rank;
}

bool Session::StepMember(int index, int dir) noexcept
{
    if (!CanStepMember(index))
    {
        return false;
    }
    const int rank  = party_member::Rank(index);
    const int count = party_member::Ranks(PlayerOneController());
    const int next  = party_rank_rules::Step(rank, dir, RanksHeldBesides(index), count);
    if (next == rank)
    {
        return false;
    }
    party_member::SetRank(index, next);
    platform::Log(L"{} steps {} to rank {} of {} in the party order: its member is given back and the new row's seated",
                  seats_[index].Name(), dir >= 0 ? L"right" : L"left", next, count);
    return true;
}

void* Session::RowActor(int index) const noexcept
{
    return SeatBuilt(index) ? party_member::RowActor(index, PlayerOneController(), seats_[index].standIn) : nullptr;
}

void Session::JoinNow(int index) noexcept
try
{
    if (!SeatBuilt(index))
    {
        platform::Log(L"join refused: seat {} is not built ({} built)", index, kSeatsBuilt);
        return;
    }
    Seat& seat = seats_[index];
    if (seat.localPlayer != nullptr)
    {
        platform::Log(L"{} is already in", seat.Name());
        return;
    }
    if (seat.awaiting)
    {
        platform::Log(L"{} already waits for its member to be free (see above; {} stops the wait)", seat.Name(), LeaveKey(seat.index));
        return;
    }
    if (!own_controller::CanRename())
    {
        platform::Log(L"join refused: the camera actor's name text cannot be changed (page protection refused)");
        return;
    }
    UObject* gameInstance = GameInstance();
    auto*    players      = LocalPlayers(gameInstance);
    const std::int32_t mirrors = Mirrors();  // the mirror player holds the column the seat takes (views/mirror_player.h)
    // The list holds player 1 and the seats in, then the mirrors; the seat is appended after the players, so its
    // column is the next one (with player 2 out, player 3 takes column 1).
    const std::int32_t playersIn = 1 + Count();
    if (players == nullptr || players->Num() != playersIn + mirrors)
    {
        platform::Log(L"join refused: expected exactly {} local player(s){}, found {}", playersIn,
                      mirrors != 0 ? L" plus the mirror" : L"", players == nullptr ? -1 : players->Num());
        return;
    }
    UObject* playerOneController = Field((*players)[0], facts::kLocalPlayerController);
    if (Field(playerOneController, facts::kControllerPawn) == nullptr)
    {
        platform::Log(L"join refused: player 1 controls no character (title screen or a load); join in the field");
        return;
    }
    if (!own_controller::UsesNativeClasses(playerOneController))
    {
        platform::Log(L"join refused: player 1's controller or camera manager is not the End class the research read ({})",
                      Describe(playerOneController));
        return;
    }
    // The member first (the user, 2026-10-21): no controller and camera for the seat until the row's member is free.
    // The instance is kept for the wait: found once, it lives as long as the game.
    gameInstance_ = gameInstance;
    if (seatInstalled_)
    {
        const party_member::Choice choice = party_member::Choose(seat.index, playerOneController, nullptr, false, seat.standIn);
        if (choice.member == nullptr)
        {
            seat.awaiting = true;
            LogWait(seat, choice, L"joins once its member is free");
            return;
        }
    }
    // The mirror player leaves its column inside the join, so the seat takes it on the same tick.
    if (mirrors != 0)
    {
        mirror_.clear(std::format(L"{} joins", seat.Name()).c_str());
        if (players->Num() != playersIn)
        {
            platform::Log(L"join refused: the mirror player did not leave its column ({} local players)", players->Num());
            return;
        }
    }
    const local_player_list::Added added = local_player_list::Add(playerOneController, seat.ControllerId());
    if (!added.called && added.localPlayer == nullptr && !added.managerSeen)
    {
        platform::Log(L"join refused: UGameplayStatics.CreatePlayer is not the function the mod expects, or raised at once");
    }
    platform::Log(L"join: {}", added.Text(std::format(L"{}'s", seat.Name()).c_str()));

    // The engine's list decides what happened: a player added before an exception must still be tracked,
    // so it can be removed before anything logs it in again.
    if (players->Num() != playersIn + 1)
    {
        platform::Log(L"join failed: the engine did not add a local player (count now {})", players->Num());
        return;
    }
    seat.localPlayer = (*players)[playersIn];  // the engine appends the new player
    seat.awaiting    = false;
    seat.rejoin      = Rejoin{};  // a join by hand during a rejoin's settle ends the rejoin
    seat.lastColumn  = kNoColumn;
    if (playersIn != seat.index)
    {
        platform::Log(L"join: {} takes column {} (the players before it in the list: {})", seat.Name(), playersIn + 1, playersIn);
    }

    UObject* seatController = Controller(seat);
    UObject* managerOne     = Field(playerOneController, facts::kControllerCameraManager);
    UObject* managerSeat    = Field(seatController, facts::kControllerCameraManager);
    const bool filledOne    = local_player_list::FillView(managerOne);
    const bool filledSeat   = local_player_list::FillView(managerSeat);
    platform::Log(L"join: {} is in; views filled: player 1 {}, {} {}", seat.Name(), filledOne ? L"yes" : L"NO", seat.Name(),
                  filledSeat ? L"yes" : L"NO");
    split_render::OnJoin();
    pad_buttons::SetPads(SeatIds());  // from here the seat's pad's buttons are its own
    scopes::BindController(seat.index, seatController);
    seat_objects::BindController(seat.index, seatController);  // the one registry every module asks
    seat.seatRetryAt = 0;
    if (seatInstalled_)
    {
        party_member::Choice choice;
        if (!TrySeat(seat, choice) && !Seated(seat.index) && ticks_ >= seat.seatRetryAt && choice.wait != party_member::Wait::Unread)
        {
            // The member checked free a moment ago is not any more (a funnel ran in between): out, and the wait. A
            // seat the funnel refused is not this: the player stays in and the seat is tried again at its retry tick.
            LogWait(seat, choice, L"joined but its member is not free any more");
            LeaveToAwait(seat, party_member::Describe(choice.wait));
        }
    }
    else
    {
        Report();
    }
}
catch (...)
{
    platform::Log(L"join: an engine lookup raised an exception; seat {} {}", index,
                  SeatBuilt(index) && seats_[index].localPlayer != nullptr ? L"is tracked" : L"is not in");
}

void Session::Leave(int index) noexcept
{
    if (!SeatBuilt(index))
    {
        platform::Log(L"leave refused: seat {} is not built ({} built)", index, kSeatsBuilt);
        return;
    }
    Seat& seat = seats_[index];
    if (seat.rejoin.pending)
    {
        seat.rejoin = Rejoin{};
        platform::Log(L"{} no longer rejoins after the world change", seat.Name());
    }
    if (seat.awaiting)
    {
        seat.awaiting   = false;
        seat.waitLogged = 0;
        seat.lastWait   = party_member::Wait::None;
        platform::Log(L"{} no longer waits for a party member", seat.Name());
    }
    if (seat.localPlayer == nullptr)
    {
        platform::Log(L"{} is not in", seat.Name());
        return;
    }
    TakeOut(seat, false);
}

bool Session::TakeOut(Seat& seat, bool worldChange) noexcept
try
{
    auto* players = LocalPlayers(gameInstance_);
    if (players == nullptr || !Contains(*players, seat.localPlayer))
    {
        platform::Log(L"leave: the engine no longer lists {}; letting go without touching it", seat.Name());
        Forget(seat);
        return true;
    }
    // The actors the seat's controller and camera manager spawned, which nothing destroys (the End camera
    // manager has no EndPlay or Destroyed override). Read before the removal destroys their owners.
    UObject*   controller = Controller(seat);
    const auto leftovers  = local_player_list::ReadLeftovers(seat.localPlayer);
    // While the seat's controller and member still exist, and the member still the seat's (the modules end the seat's
    // lock and menu in its own scopes). A leave the funnel refuses below costs one lock and one menu rebuild: the
    // command menu sets the seat up again on its next tick, the lock is pressed again.
    EndMemberModules(seat);
    // The seat's member goes back to its AI first, through the game's leader funnel.
    if (seatInstalled_ && !party_seat::Unseat(seat.index, Field((*players)[0], facts::kLocalPlayerController), controller))
    {
        if (!worldChange)
        {
            platform::Log(L"leave refused: {}'s controller still holds its member; press {} again to retry", seat.Name(), LeaveKey(seat.index));
            return false;
        }
        // The game destroys and rebuilds the party next; removing the controller unpossesses the member on the way.
        platform::Log(L"leave: {}'s controller still holds its member; the world change takes it out regardless", seat.Name());
    }
    // The seat's field camera operator next: its cleanup unbinds the delegates it bound on the seat's
    // controller, which must still exist then.
    DestroyOperator(seat);

    platform::Log(L"leave: {}'s {}", seat.Name(), leftovers.Text());

    bool       raised  = false;
    const bool removed = local_player_list::Remove(gameInstance_, seat.localPlayer, removeLocalPlayer_, raised);
    if (!removed)
    {
        platform::Log(L"leave failed: the engine did not remove {}{}", seat.Name(), raised ? L" (the call raised an exception)" : L"");
        return false;
    }
    seat.localPlayer = nullptr;
    pad_buttons::SetPads(SeatIds());
    if (seatInstalled_)
    {
        party_seat::Forget(seat.index);  // a later player in this seat is a new controller, set up afresh
    }
    seat.ResetMember();
    scopes::Unbind(seat.index);
    seat_objects::Forget(seat.index);
    battle::arena_warp::Forget(seat.index);  // a double click waiting for the next tick is not run for nobody
    const auto gone = local_player_list::DestroyLeftovers(leftovers);
    platform::Log(L"leave: {} removed{}; leftovers destroyed: camera actor {}, battle-talk actor {}", seat.Name(),
                  raised ? L" (the call raised an exception after removing it)" : L"", gone.camera ? L"yes" : L"no",
                  gone.talk ? L"yes" : L"no");
    return true;
}
catch (...)
{
    platform::Log(L"leave: an engine lookup raised an exception");
    return seat.localPlayer == nullptr;
}

bool Session::TrySeat(Seat& seat, party_member::Choice& choice) noexcept
try
{
    auto* players = LocalPlayers(gameInstance_);
    if (seat.localPlayer == nullptr || !seatInstalled_ || players == nullptr || !Contains(*players, seat.localPlayer) ||
        ticks_ < seat.seatRetryAt)
    {
        choice.wait = party_member::Wait::Unread;
        return false;
    }
    UObject* playerOne      = Field((*players)[0], facts::kLocalPlayerController);
    UObject* seatController = Controller(seat);
    choice                  = party_member::Choose(seat.index, playerOne, seatController, false, seat.standIn);
    if (choice.member == nullptr)
    {
        return false;
    }
    if (!party_seat::SeatMember(seat.index, playerOne, seatController, choice.member, choice.type))
    {
        seat.seatRetryAt = ticks_ + kSeatRetryTicks;  // the refusal is logged by the seat; tried again later, the player in
        return false;
    }
    if (!weak_object::WeakPairOf(choice.member, seat.memberWeak))
    {
        seat.memberWeak[0] = -1;
        seat.memberWeak[1] = 0;
    }
    // After a wait: how long the player was without a member (the single view's length).
    platform::Log(L"{} plays member type {} (row {} of the party order){}", seat.Name(), choice.type, choice.ordinal,
                  seat.waitLogged == 0 ? L"" : std::format(L", {} ticks after it began to wait for one", ticks_ - seat.awaitSince));
    seat.waitLogged = 0;
    if (cameraInstalled_ && !seat_camera::Start(seat.index, playerOne, seatController, choice.member))
    {
        platform::Log(L"{} keeps a still camera; each player's sticks are still their own pad's", seat.Name());
    }
    if (party_member::JoinsThroughHold(choice.wait))
    {
        // Taken in a walk its AI makes: handed straight back, on the tick it was seated, as a seated player's member is
        // at the walk's start; the walk starts again from where the member stands (party_member::JoinsThroughHold).
        HandForHold(seat, choice.member, choice.wait, true);
    }
    Report();
    return true;
}
catch (...)
{
    platform::Log(L"seat: an engine lookup raised an exception");
    choice.wait = party_member::Wait::Unread;
    return false;
}

void Session::DestroyOperator(Seat& seat) noexcept
{
    UObject*   operatorSeat = cameraInstalled_ ? static_cast<UObject*>(seat_camera::Take(seat.index)) : nullptr;
    const bool operatorGone = operatorSeat != nullptr && CallNoParams(operatorSeat, STR("/Script/Engine.Actor:K2_DestroyActor"));
    platform::Log(L"{}'s camera operator {}", seat.Name(), operatorSeat == nullptr ? L"was not running" : operatorGone ? L"destroyed" : L"NOT DESTROYED");
}

void Session::ReleaseMember(Seat& seat, const wchar_t* why) noexcept
try
{
    auto* players = LocalPlayers(gameInstance_);
    if (seat.localPlayer == nullptr || players == nullptr || !Contains(*players, seat.localPlayer))
    {
        return;
    }
    platform::Log(L"{}'s member (type {}) is {}: given back; the row's member next, or {} leaves to wait for one", seat.Name(),
                  party_seat::MemberType(seat.index), why, seat.Name());
    EndMemberModules(seat);  // the menu is set up again for the next member by its own tick
    DestroyOperator(seat);
    party_seat::Unseat(seat.index, Field((*players)[0], facts::kLocalPlayerController), Controller(seat));
    seat.ResetMember();
}
catch (...)
{
    platform::Log(L"release: an engine lookup raised an exception");
}

void Session::HandForHold(Seat& seat, void* member, party_member::Wait hold, bool joined) noexcept
{
    // The game's own hand (what it does for its leader at an event's start), the seat and the player's camera
    // operator kept: the member's AI runs the script, the player watches it, and the seat takes it back when the
    // hold ends.
    // A join's hand that faulted is retried by the follow check: still a join's, with no catch-up run.
    joined            = joined || seat.joinedInHold;
    seat.joinedInHold = joined;
    if (hold == party_member::Wait::Guide && !joined)
    {
        world::run_catch_up::BeforeGuide(seat.index, member);  // taken past its own story box: brought back for the run
    }
    seat.handedToAi      = party_member::HandToAi(member);
    seat.holdHand        = seat.handedToAi;
    seat.hold            = hold;
    platform::Log(L"{}'s member (type {}) handed to its AI{}: {}: {}; {}'s view stays on it, its controller takes it back "
                  L"when the hold ends{}",
                  seat.Name(), party_seat::MemberType(seat.index), joined ? L" as it joins (the walk goes on)" : L"",
                  party_member::Describe(hold),
                  seat.handedToAi ? L"yes" : L"NO (the hand faulted; tried again)", seat.Name(),
                  hold == party_member::Wait::Guide || hold == party_member::Wait::AtMark
                      ? std::format(L" (lead gate: {})", lead_gate::Reading())
                      : L"");
    if (party_member::InterestOnTop(member))
    {
        // Kept rather than handed, the hold beneath could wait for ever (an interest that clears only at its walk's
        // end, a walk refused while held); handed, its AI runs the walk first. research/RE-interest-ladder-loop.md.
        platform::Log(L"{}'s member has an interest walk on top of that hold: its AI runs the walk first, then the hold",
                      seat.Name());
    }
    platform::LogDebug(L"  the member: {}", party_member::AiDetail(member));
}

void Session::ReturnFromHold(Seat& seat, void* member) noexcept
{
    seat.handedToAi   = false;
    seat.holdHand     = false;
    seat.joinedInHold = false;  // the next hold is the seated player's own
    platform::Log(L"{}'s member (type {}) back on its controller: its hold ended ({}): {}; the member: {}", seat.Name(),
                  party_seat::MemberType(seat.index), party_member::Describe(seat.hold),
                  party_member::ReturnTo(member, Controller(seat)) ? L"yes" : L"NO", party_member::AiDetail(member));
    seat.hold = party_member::Wait::None;
}

void Session::FollowSlot(Seat& seat) noexcept
try
{
    void* member = party_seat::Member(seat.index);
    if (member == nullptr)
    {
        return;
    }
    // Destroyed under the player (a retry rebuilds the party without a field exit): nothing is called on it.
    if (seat.memberWeak[1] != 0 && weak_object::ObjectOf(seat.memberWeak) != member)
    {
        platform::Log(L"{}'s member was destroyed under it (the party rebuilt without a field exit); {} lets go and chooses again",
                      seat.Name(), seat.Name());
        EndMemberModules(seat);
        DestroyOperator(seat);
        party_seat::Forget(seat.index);
        seat.ResetMember();
        seat.standIn = facts::kPlayerTypeNone;  // a rebuilt party: its own row again
        return;
    }
    auto* players = LocalPlayers(gameInstance_);
    if (players == nullptr || players->Num() < 1)
    {
        return;
    }
    if (const wchar_t* why = party_member::WhyNotInSlot(seat.index, member, party_seat::MemberType(seat.index),
                                                       Field((*players)[0], facts::kLocalPlayerController), seat.standIn))
    {
        ReleaseMember(seat, why);
        return;
    }
    if (seat.handedToAi)
    {
        if (seat.holdHand)
        {
            // The hold's end decides: the member free again (the level's next setting, the scene's end), the game's
            // leader with its player, and nothing else holding it.
            if (party_member::HoldWait(member) == party_member::Wait::None)
            {
                ReturnFromHold(seat, member);
            }
            return;
        }
        // A treasure or push-button lock, read now: the member goes back to its player unless the event took it (its
        // cinema mode), cut to its own shot, or its own state holds it; the game's leader stays with its AI for the
        // interaction.
        const std::uint32_t lock = party_member::ControlLockMask();
        if (!party_member::LeaderWithPlayer() && field_hold::InteractionLock(lock) && !views::scene_camera::EventShot())
        {
            if (const wchar_t* why = party_member::WhyHeldByScript(member))
            {
                if (!seat.returnWaitLogged)
                {
                    seat.returnWaitLogged = true;
                    platform::Log(L"{}'s member stays with its AI through the interaction lock ({:#x}): {}", seat.Name(), lock, why);
                }
                return;
            }
            // A failed return leaves the member with its AI and the seat marked so: the next check tries again.
            const bool back       = party_member::ReturnTo(member, Controller(seat));
            seat.handedToAi       = !back;
            seat.keptThroughLock  = back;
            seat.returnWaitLogged = false;
            platform::LogDebug(L"{}'s member back on its controller: the lock ({:#x}) is the interacting character's own (a "
                               L"treasure, a pickup, a switch or a terminal), and the event does not use it: {}",
                               seat.Name(), lock, back ? L"yes" : L"NO");
            return;
        }
        // The game's own state decides the return (RE-script-release.md §4: the event bit alone undid a keep-group
        // hand): its leader back with a player controller. The funnel is heard at its entry; this is the backstop,
        // and the retry while a script still held the member at the game's return.
        if (party_member::LeaderWithPlayer())
        {
            ReturnWithLeader(seat);
        }
        return;
    }
    const party_member::Wait hold = party_member::HoldWait(member);
    // The push-button event answers first while the game's leader is handed (an interaction lock the member stayed
    // through that gained another bit reads as the push-button event when its bit stands): the same backstop. Only while the
    // game's leader is with its AI: a push-button bit standing with the leader on its player would otherwise hand and
    // return the member at every check (no such setter is known; EventControl_Begin hands the leader).
    // A kept member's event cut to its own shot while the lock still stands (slot 18 alone is no reason, v0.111.0).
    const bool eventShot = seat.keptThroughLock && field_hold::InteractionLock(party_member::ControlLockMask()) &&
                           views::scene_camera::EventShot();
    if (hold == party_member::Wait::LeaderHanded || (hold == party_member::Wait::PushButtonEvent && !party_member::LeaderWithPlayer()) ||
        eventShot)
    {
        if (world::traversal_action::PlayerOneMoveHolds(party_member::ControlLockMask()))
        {
            return;  // player 1's own crawl, squeeze or grappling hook only: the member stays its player's (HandWithLeader)
        }
        // The hand funnel's backstop: the game's leader is with its AI and the seat's member is not (a seat that
        // landed inside the hand, after the funnel fired: seen live 2026-10-21, Tifa taken mid-animation), or an
        // interaction lock the member stayed through gained another bit (a scene, a story beat).
        if (eventShot)
        {
            platform::Log(L"the event cut to its own shot (lock {:#x}): {}'s member goes to its AI with the game's leader",
                          party_member::ControlLockMask(), seat.Name());
            seat.keptThroughLock = false;
        }
        else if (seat.keptThroughLock)
        {
            platform::Log(L"the lock grew beyond the interacting character's own ({:#x}): {}'s member goes to its AI with the game's "
                          L"leader",
                          party_member::ControlLockMask(), seat.Name());
            seat.keptThroughLock = false;
        }
        else
        {
            platform::Log(L"the game's leader is with its AI (lock {:#x}) and {}'s member is not: the hand was not heard for it; "
                          L"handed now",
                          party_member::ControlLockMask(), seat.Name());
        }
        HandWithLeader(seat);
        return;
    }
    if (party_member::ScriptHold(hold))
    {
        HandForHold(seat, member, hold);
    }
}
catch (...)
{
    platform::Log(L"follow: an engine lookup raised an exception");
}

void Session::OnLeaderChanging(void* self, void* character) noexcept
{
    auto* session = static_cast<Session*>(self);
    // The game's leader going home (onto the field leader: a battle's end) ends every stand-in: each seat takes its own
    // row again. Otherwise the seat whose member the change takes plays the character player 1 leaves (the user,
    // 2026-10-25: the battle leader option; a forced switch alike), so no other seat is moved.
    const bool         home = party_member::IsFieldLeader(character);
    const std::uint8_t was  = party_member::GameLeaderType();  // player 1's character before the change
    for (int index = 1; index <= kSeatsBuilt; ++index)
    {
        Seat& seat = session->seats_[index];
        if (home && seat.standIn != facts::kPlayerTypeNone)
        {
            platform::Log(L"{} stood in for player 1's character (type {}): the game's leader goes home, {} takes its own row again",
                          seat.Name(), seat.standIn, seat.Name());
            seat.standIn        = facts::kPlayerTypeNone;
            seat.seatGraceUntil = session->ticks_ + kSwapGraceTicks;  // its row's member may be another seat's for a moment
        }
        if (character != nullptr && character == party_seat::Member(seat.index))
        {
            const std::uint8_t own = party_seat::MemberType(seat.index);  // read before the release unseats it
            session->ReleaseMember(seat, L"about to become player 1's leader (the game's leader change)");
            if (!home && was < facts::kPlayerTypeNone && was != own)
            {
                seat.standIn = was;
                platform::Log(L"{} plays player 1's former character (type {}) until the game's leader goes home", seat.Name(), was);
            }
            // The next character is the seat's once the change has let go of it [ASSUMPTION: within the change's call or
            // a tick after; a longer hold still ends in the wait, as before].
            seat.seatGraceUntil = session->ticks_ + kSwapGraceTicks;
        }
    }
}

void Session::OnStreamFinishing(void* self) noexcept
{
    auto* session = static_cast<Session*>(self);
    for (int index = 1; index <= kSeatsBuilt; ++index)
    {
        Seat& seat = session->seats_[index];
        if (party_seat::Member(seat.index) != nullptr && !party_member::InFormation(party_seat::MemberType(seat.index)))
        {
            session->ReleaseMember(seat, L"leaving the deployed party (its status changed)");
        }
    }
}

void Session::OnStreamFinished(void* self) noexcept
{
    auto* session = static_cast<Session*>(self);
    for (int index = 1; index <= kSeatsBuilt; ++index)
    {
        Seat& seat   = session->seats_[index];
        void* member = party_seat::Member(seat.index);
        if (member == nullptr)
        {
            continue;
        }
        // The rebuild typed every member PARTY; the game re-types only its own leader PLAYER.
        platform::LogDebug(L"{}'s member keeps the PLAYER actor type after the formation rebuild: {}", seat.Name(),
                           party_member::ReassertPlayerType(member) ? L"set" : L"NOT set (the setter faulted)");
        session->FollowSlot(seat);
    }
}

void Session::OnLeaderHandedToAi(void* self) noexcept
{
    auto* session = static_cast<Session*>(self);
    world::talk_gather::OnLeaderHanded();  // a further player's talk: player 1's character brought over when far
    for (int index = 1; index <= kSeatsBuilt; ++index)
    {
        session->HandWithLeader(session->seats_[index]);
    }
}

void Session::HandWithLeader(Seat& seat) noexcept
{
    void* member = party_seat::Member(seat.index);
    if (member == nullptr || seat.handedToAi)
    {
        return;
    }
    // A lock set in battle (a boss arena, the colosseum) hands the game's leader there too [UNKNOWN whether the
    // battle AI then runs it]: the member stays the player's until a log shows what the game does with its leader.
    if (party_member::ActorMode(member) == facts::kActorModeBattle)
    {
        platform::LogDebug(L"the game handed its leader to its AI in battle (lock {:#x}); {}'s member stays (unread case)",
                           party_member::ControlLockMask(), seat.Name());
        return;
    }
    // Player 1's crawl, squeeze or grappling hook locks the field for its own leader only (the user, 2026-10-24: a crawl
    // locks only the mover): the member stays its player's, unless a story lock stands with it (the first lock's hand
    // runs before its bits are set: a mask still 0 asks MoverLockNow).
    if (world::traversal_action::PlayerOneMoveHolds(party_member::ControlLockMask()))
    {
        platform::LogDebug(L"player 1's crawl, squeeze or grappling hook handed only player 1's character to its AI (lock {:#x}); {}'s "
                           L"member stays theirs",
                           party_member::ControlLockMask(), seat.Name());
        return;
    }
    // A treasure or push-button lock is the interacting character's alone (field_hold::InteractionLock): a member nothing
    // else holds stays its player's. The first lock's hand usually runs before its bits are set (the mask still 0):
    // then the member goes with the leader and the follow check gives it back a tick later, once the bits are read
    // and the event has put its own actors in the cinema mode.
    const std::uint32_t lock = party_member::ControlLockMask();
    if (field_hold::InteractionLock(lock) && !views::scene_camera::EventShot() && party_member::WhyHeldByScript(member) == nullptr)
    {
        seat.keptThroughLock = true;
        platform::LogDebug(L"{}'s member stays theirs: the lock ({:#x}) is the interacting character's own (a treasure, a pickup, "
                           L"a switch or a terminal)",
                           seat.Name(), lock);
        return;
    }
    seat.handedToAi       = party_member::HandToAi(member);
    seat.returnWaitLogged = false;
    platform::LogDebug(L"{}'s member handed to its AI with the game's leader (lock {:#x}): {}; the member: {}", seat.Name(),
                       party_member::ControlLockMask(), seat.handedToAi ? L"yes" : L"NO", party_member::AiDetail(member));
}

void Session::OnLeaderReturned(void* self) noexcept
{
    auto* session = static_cast<Session*>(self);
    for (int index = 1; index <= kSeatsBuilt; ++index)
    {
        session->seats_[index].keptThroughLock  = false;
        session->seats_[index].returnWaitLogged = false;  // the lock's "stays with its AI" line may have taken it
        session->ReturnWithLeader(session->seats_[index]);
    }
}

void Session::ReturnWithLeader(Seat& seat) noexcept
try
{
    void* member = party_seat::Member(seat.index);
    if (member == nullptr || !seat.handedToAi || seat.holdHand || seat.localPlayer == nullptr)
    {
        return;  // a hand for the member's own hold ends with the hold (FollowSlot), not with the game's leader
    }
    // The game's leader is back; the member follows once nothing holds it (a scene's own layers can outlast the
    // lock, a guide lead or run written at the scene's end goes to its mark). The periodic check retries while the game's
    // leader stays with its player.
    if (const wchar_t* why = party_member::WhyHeldByScript(member))
    {
        if (!seat.returnWaitLogged)
        {
            seat.returnWaitLogged = true;
            platform::LogDebug(L"the game took its leader back (lock {:#x}) while {}'s member is still {}; it follows when free: {}",
                               party_member::ControlLockMask(), seat.Name(), why, party_member::AiDetail(member));
        }
        return;
    }
    seat.handedToAi = false;
    platform::LogDebug(L"{}'s member back on its controller with the game's leader (lock {:#x}): {}", seat.Name(),
                       party_member::ControlLockMask(), party_member::ReturnTo(member, Controller(seat)) ? L"yes" : L"NO");
}
catch (...)
{
    platform::Log(L"return: an engine lookup raised an exception");
}

void Session::Report() const noexcept
try
{
    auto* players = LocalPlayers(GameInstance());
    if (players == nullptr)
    {
        platform::Log(L"report: no game instance");
        return;
    }
    for (std::int32_t i = 0; i < players->Num(); ++i)
    {
        UObject* player     = (*players)[i];
        UObject* controller = Field(player, facts::kLocalPlayerController);
        UObject* manager    = Field(controller, facts::kControllerCameraManager);
        // The list's place is the column; the player is named by its seat (a mirror holds no seat).
        const wchar_t* name = i == 0 ? SeatName(0) : L"a mirror";
        for (int seat = 1; seat <= kSeatsBuilt; ++seat)
        {
            name = seats_[seat].localPlayer == player && player != nullptr ? SeatName(seat) : name;
        }
        platform::Log(L"column {}: {}: {}", i + 1, name, Describe(player));
        platform::Log(L"  controller {} (pawn {})", Describe(controller), Describe(Field(controller, facts::kControllerPawn)));
        platform::Log(L"  camera manager {} (owner {})", Describe(manager), Describe(Field(manager, facts::kCameraManagerPcOwner)));
        platform::Log(L"  main camera actor {}", Describe(Field(manager, facts::kCameraManagerMainCamera)));
    }
}
catch (...)
{
    platform::Log(L"report: an engine lookup raised an exception");
}

void Session::Status() const noexcept
{
    Report();
    if (Count() != 0 && cameraInstalled_)
    {
        seat_camera::LogCounts();
    }
}

UObject* Session::LocalPlayer(int index) const noexcept
try
{
    auto* players = LocalPlayers(gameInstance_);  // found by Tick within a second of the start
    return players != nullptr && index >= 0 && index < players->Num() ? (*players)[index] : nullptr;
}
catch (...)
{
    return nullptr;
}

bool Session::Seated(int seat) const noexcept
{
    return SeatBuilt(seat) && seatInstalled_ && party_seat::Seated(seat);
}

std::wstring Session::FormationReading() const noexcept
try
{
    auto* players = LocalPlayers(GameInstance());
    if (!seatInstalled_ || players == nullptr || players->Num() < 1)
    {
        return L"no party";
    }
    return party_member::FormationReading(Field((*players)[0], facts::kLocalPlayerController));
}
catch (...)
{
    return L"unread";
}

std::wstring Session::MemberReading(int index) const noexcept
try
{
    if (!SeatBuilt(index))
    {
        return L"no such seat";
    }
    const Seat& seat = seats_[index];
    if (seat.localPlayer == nullptr && !seat.awaiting)
    {
        return L"out";
    }
    if (Seated(seat.index))
    {
        const auto type = party_seat::MemberType(seat.index);
        return seat.holdHand    ? std::format(L"handed type {} ({}; {}'s view on it)", type, party_member::Describe(seat.hold), seat.Name())
               : seat.handedToAi ? std::format(L"handed type {} (with the game's leader, lock {:#x})", type, party_member::ControlLockMask())
                                 : std::format(L"playing type {}", type);
    }
    auto* players = LocalPlayers(gameInstance_);
    if (!seatInstalled_ || players == nullptr || players->Num() < 1)
    {
        return L"waiting (no seat)";
    }
    if (seat.awaiting)
    {
        const party_member::Choice choice =
            party_member::Choose(seat.index, Field((*players)[0], facts::kLocalPlayerController), nullptr, false, seat.standIn);
        return std::format(L"awaiting: row {} type {}: {}", choice.ordinal, choice.type, party_member::Describe(choice.wait));
    }
    if (seat.localPlayer == nullptr)
    {
        return L"waiting (no seat)";
    }
    const party_member::Choice choice =
        party_member::Choose(seat.index, Field((*players)[0], facts::kLocalPlayerController), Controller(seat), false, seat.standIn);
    return std::format(L"waiting: row {} type {}: {}", choice.ordinal, choice.type, party_member::Describe(choice.wait));
}
catch (...)
{
    return L"unread";
}

bool Session::Console(const wchar_t* command) noexcept
try
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:ExecuteConsoleCommand"));
    auto* library = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary"));
    auto* players = LocalPlayers(GameInstance());
    if (function == nullptr || library == nullptr || players == nullptr || players->Num() < 1)
    {
        return false;
    }
    auto* context = function->FindProperty(RC::Unreal::FName(STR("WorldContextObject"), RC::Unreal::FNAME_Find));
    auto* text    = function->FindProperty(RC::Unreal::FName(STR("Command"), RC::Unreal::FNAME_Find));
    constexpr std::int32_t kMaxFrame = 64;
    const std::int32_t     size      = function->GetParmsSize();
    if (context == nullptr || text == nullptr || size > kMaxFrame || context->GetSize() != sizeof(UObject*) ||
        text->GetSize() != sizeof(RC::Unreal::FString) || context->GetOffset_Internal() < 0 ||
        context->GetOffset_Internal() + context->GetSize() > size || text->GetOffset_Internal() < 0 ||
        text->GetOffset_Internal() + text->GetSize() > size ||
        (context->GetOffset_Internal() < text->GetOffset_Internal() + text->GetSize() &&
         text->GetOffset_Internal() < context->GetOffset_Internal() + context->GetSize()))
    {
        return false;
    }
    // SpecificPlayer stays null: the engine then uses player 1's controller, as the console does.
    alignas(16) std::uint8_t frame[kMaxFrame]{};
    *reinterpret_cast<UObject**>(frame + context->GetOffset_Internal()) = Field((*players)[0], facts::kLocalPlayerController);
    auto*      string = new (frame + text->GetOffset_Internal()) RC::Unreal::FString(command);
    const bool called = GuardedProcessEvent(static_cast<UObject*>(library->GetClassDefaultObject()), function, frame);
    string->~FString();
    return called;
}
catch (...)
{
    return false;
}

}  // namespace lss::players
