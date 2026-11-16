#include "battle/enemy_coin.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "battle/coin_choice.h"
#include "battle/coin_table.h"
#include "battle/enemy_leader.h"
#include "battle/enemy_stall.h"
#include "battle/member_state.h"
#include "battle/enemy_watch.h"
#include "battle/type_answer_code.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/code_memory.h"
#include "platform/entry_redirect.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/steady_ms.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/weak_object.h"
#include "players/scopes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>

namespace lss::battle::enemy_coin {

namespace {

using platform::Read;
using platform::ReadPointer;
using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;
using ExecFn             = RC::Unreal::UnrealScriptFunction;  // (context, FFrame&, result)

constexpr int         kSeats   = players::scopes::kMaxPlayers;  // seat 0 = player 1 (the game's leader)
constexpr std::size_t kFurther = kSeats - 1;                    // seats 1..3

using GateFn           = char (*)(void* controller);
using PmGetFn          = void* (*)();
using GetPartyLeaderFn = void* (*)(void* partyManager, std::uint8_t variant);
using GetPlayerActorFn = void* (*)(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate);
using IsAFn            = bool (*)(void* object, void* b, void* c);
using IsPartySideFn    = bool (*)(void* component);
using PawnCompFn       = void* (*)(void* controller);
using OwnerFn          = void* (*)(void* component);
using BattleStartFn    = void (*)(void* component);
using HitApplyFn       = char (*)(void* context, void* hit);
using HitApplyBFn      = void (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                             std::uint64_t);
using SensingFn        = void (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                           std::uint64_t, std::uint64_t);
using FollowTickFn     = std::uint64_t (*)(void* controller, float deltaSeconds);  // the game's returns a char in al
using StartRollingFn   = std::int64_t (*)(void* mine, int index, int count);
using EvalPointFn      = std::int64_t (*)(void* move, void* candidates, float* point);  // integer registers only (its call site, IDA)
using SelectMoveTypeFn = std::int64_t (*)(void* move);
using LeaderCharFn     = void* (*)();

// The remaining leader reads (RE-enemy-coin-remaining-reads.md), each counted per player its answer named.
enum ReadKind : std::size_t
{
    kMineFollow,
    kMineArea,
    kMineTrigger,
    kMineGoal,
    kCoreFollow,
    kTornadoFollow,
    kFlyerPoint,
    kHeightChase,
    kReadKinds,
};
// In ReadKind's order (one name per kind, by the array's size).
constexpr std::array<const wchar_t*, kReadKinds> kReadNames{
    L"a mine's follow", L"a mine's area test", L"a mine's rolling trigger", L"a mine's rolling goal",
    L"a charge core's follow", L"a water tornado's follow", L"a flyer's point", L"the height chase"};

// The script natives, in the order of facts::kBattleCoinNativeExecs. Member: the context is the enemy and the leader
// read inside answers. Caller: the script's object is the enemy (a library native); the read inside answers. The
// party API's two write their result straight, so the result is replaced after the game's call.
enum class Kind : std::uint8_t
{
    Member,
    Caller,
    LeaderApi,
    LeaderTypeApi,
};
struct Native
{
    const wchar_t* path;
    Kind           kind;
};
constexpr std::size_t                 kNativeCount = facts::kBattleCoinNativeExecs.size();
constexpr std::array<Native, kNativeCount> kNatives{{
    {L"/Script/EndGame.EndBattleAIController:GetLeader", Kind::Member},
    {L"/Script/EndGame.EndBattleAIController:SetTargetByType", Kind::Member},
    {L"/Script/EndGame.EndBattleAIController:GetActionCharacter", Kind::Member},
    {L"/Script/EndGame.EndBattleAIController:GetSyncActionCharacter", Kind::Member},
    {L"/Script/EndGame.EndBattleAIController:GetSpecialStatusChangeCharacter", Kind::Member},
    {L"/Script/EndGame.EndBattleAIController:GetStatusChangeCharacter", Kind::Member},
    {L"/Script/EndGame.EndBattleAIController:HasStatusChangeEsuna", Kind::Member},
    {L"/Script/EndGame.EndBattleAIController:HasStatusChangeDeSpell", Kind::Member},
    {L"/Script/EndGame.EndBattleAIController:HasStatusChange", Kind::Member},
    {L"/Script/EndGame.EndBattleAPI:GetExpectationCountInConeAreaByLocation", Kind::Caller},
    {L"/Script/EndGame.EndBattleAPI:GetExpectationCountInConeArea", Kind::Caller},
    {L"/Script/EndGame.EndBattleAPI:GetExpectationCountInCapsuleAreaByLocation", Kind::Caller},
    {L"/Script/EndGame.EndBattleAPI:GetExpectationCountInCapsuleArea", Kind::Caller},
    {L"/Script/EndGame.EndBattleAPI:IsAIPC", Kind::Caller},
    {L"/Script/EndGame.EndPartyAPI:GetPartyLeader", Kind::LeaderApi},
    {L"/Script/EndGame.EndPartyAPI:GetPartyLeaderType", Kind::LeaderTypeApi},
}};
constexpr const wchar_t* kAiControllerClass = L"/Script/EndGame.EndBattleAIController";

// Set at install, then only read.
platform::EntryRedirect g_gate, g_mineGate, g_hitApply;
platform::CallRedirect  g_opening, g_hate, g_isAipc, g_eqs, g_area, g_hitApplyB, g_sensing, g_scriptLeader;
std::array<platform::CallRedirect, 2> g_tie, g_expect, g_scriptActor;
platform::VTableEntry   g_battleStart;
platform::VTableEntry   g_mineTick, g_coreTick, g_tornadoTick;
platform::CallRedirect  g_mineFollow, g_mineArea, g_mineTrigger, g_mineGoal, g_coreFollow, g_tornadoFollow, g_flyerPoint, g_heightChase;
platform::CallRedirect  g_renoRolling, g_flyerEval, g_selectMove;
LeaderCharFn            g_getLeaderChar = nullptr;
PmGetFn                 g_pmGet       = nullptr;
GetPartyLeaderFn        g_getLeader   = nullptr;
GetPlayerActorFn        g_getActor    = nullptr;
IsPartySideFn           g_isPartySide = nullptr;
PawnCompFn              g_pawnComp    = nullptr;
std::array<std::uintptr_t, kNativeCount>       g_execs{};      // the live exec addresses IDA named
std::array<std::atomic<ExecFn>, kNativeCount>  g_originals{};  // null until swapped (or found changed)
std::array<UFunction*, kNativeCount>           g_functions{};  // game thread: the swapped ones
std::atomic<const UClass*>                     g_aiClass{nullptr};
bool                                           g_swapsDone = false;  // game thread
unsigned                                       g_swapTries = 0;      // game thread

// The blocks answering the inlined leader-type tests (type_answer_code.h): one per further seat, +0x233 its member's
// type, 9 (NONE) while the seat holds no member. Written on the game thread, read by the game's code on any thread.
alignas(16) std::uint8_t g_fakes[kFurther][facts::kPartyManagerBytes]{};

// The seats as the game thread last saw them, for any thread.
std::array<std::atomic<void*>, kSeats>        g_members{};
std::array<std::atomic<void*>, kSeats>        g_controllers{};
std::atomic<unsigned>                         g_seated{1u};  // bit per seat with a member (bit 0: the game's leader)
std::atomic<bool>                             g_on{false};
std::atomic<bool>                             g_enabled{true};
std::atomic<bool>                             g_areaOn{true};  // `member-area`: the area answer alone (an A/B)

// The enemy whose script native, opening or follow tick is running on this thread (null: none, or not an enemy).
thread_local void*         t_enemy = nullptr;
thread_local void*         t_starting = nullptr;  // the enemy component whose battle start runs (StartingComponent)
thread_local std::uint64_t t_rng   = 0;
// The move struct whose flyer point evaluation or move type choice runs on this thread (its first field: the
// controller), and whether Reno's finishing roll started the roll running now.
thread_local void* t_move        = nullptr;
thread_local bool  t_renoRolling = false;

coin_table::CoinTable g_coins;

struct Counts
{
    std::array<std::atomic<unsigned>, kSeats> tosses{};         // per player named
    std::atomic<unsigned>                     gamesOwn{};       // tosses with no further player: the game's own answer
    std::atomic<unsigned>                     retosses{};       // the coin's player went down
    std::array<std::atomic<unsigned>, kSeats> scriptAnswers{};  // the three script reads and the collector, per player
    std::atomic<unsigned>                     openingAnswers{};
    std::atomic<unsigned>                     expectationAnswers{};
    std::array<std::atomic<unsigned>, kSeats> tieAnswers{};     // the hate pick's tie-break reads (two sites), per coin's player
    std::atomic<unsigned>                     apiAnswers{};
    std::atomic<unsigned>                     eqsKept{};
    std::atomic<unsigned>                     areasRefreshed{};  // a human's member's area query tick let through
    std::atomic<unsigned>                     hitsScoped{};
    std::atomic<unsigned>                     sensingsScoped{};
    std::atomic<unsigned>                     offThread{};  // hits or sensings off the game thread: left as the game's
    std::array<std::array<std::atomic<unsigned>, kSeats>, kReadKinds> reads{};  // the remaining reads, per player named
};
Counts g_counts;                                 // since the last summary
Counts g_totals;                                 // since install
std::chrono::steady_clock::time_point g_lastSummary{};
std::chrono::steady_clock::time_point g_lastSwapTry{};
bool                                  g_offThreadLogged = false;
std::array<bool, 3>                   g_scopedLogged{};

unsigned Take(std::atomic<unsigned>& counter) noexcept
{
    return counter.exchange(0, std::memory_order_relaxed);
}

void Count(std::atomic<unsigned>& since, std::atomic<unsigned>& total) noexcept
{
    since.fetch_add(1, std::memory_order_relaxed);
    total.fetch_add(1, std::memory_order_relaxed);
}

std::uint32_t Seconds() noexcept
{
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// A fair bit per toss, per thread (xorshift seeded from the thread and the clock).
unsigned Random() noexcept
{
    if (t_rng == 0)
    {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        t_rng = (static_cast<std::uint64_t>(GetCurrentThreadId()) + 1) * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint64_t>(counter.QuadPart);
        t_rng |= 1;
    }
    t_rng ^= t_rng << 13;
    t_rng ^= t_rng >> 7;
    t_rng ^= t_rng << 17;
    return static_cast<unsigned>(t_rng >> 32);
}

bool Tossing() noexcept
{
    return g_on.load(std::memory_order_acquire) && g_enabled.load(std::memory_order_relaxed) &&
           (g_seated.load(std::memory_order_relaxed) & ~1u) != 0;
}

/// `seat`'s member stands in the battle (its component not in the KO state; unreadable: not standing). Any thread, guarded.
bool Standing(int seat) noexcept
{
    return !member_state::KnockedOut(g_members[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed), true);
}

/// The battle component of the controller's pawn if it is an enemy's (not on the party side); else null. Any thread.
void* EnemyComponent(void* controller) noexcept
{
    void* component = nullptr;
    bool  party     = true;
    return controller != nullptr && platform::CallGuarded(g_pawnComp, component, controller) && component != nullptr &&
                   platform::CallGuarded(g_isPartySide, party, component) && !party
               ? component
               : nullptr;
}

bool IsEnemy(void* controller) noexcept
{
    return EnemyComponent(controller) != nullptr;
}

/// A further seat's controller (never player 1's).
bool IsFurtherController(const void* controller) noexcept
{
    for (std::size_t seat = 1; seat < kSeats; ++seat)
    {
        if (controller != nullptr && g_controllers[seat].load(std::memory_order_relaxed) == controller)
        {
            return true;
        }
    }
    return false;
}

/// The further seat holding `member`, or 0.
int SeatOfMember(const void* member) noexcept
{
    for (std::size_t seat = 1; seat < kSeats; ++seat)
    {
        if (member != nullptr && g_members[seat].load(std::memory_order_relaxed) == member)
        {
            return static_cast<int>(seat);
        }
    }
    return 0;
}

const wchar_t* Because(coin_choice::Why why) noexcept
{
    switch (why)
    {
    case coin_choice::Why::Coin:
        return L"the coin";
    case coin_choice::Why::OnlyEligible:
        return L"the only one standing";
    case coin_choice::Why::NeitherEligible:
        return L"nobody stands";
    case coin_choice::Why::NoFurtherPlayer:
        break;
    }
    return L"no further player";
}

/// When a coin is tossed.
enum class Occasion : std::uint8_t
{
    FirstQuestion,
    MemberDown,
    Decision,
};

const wchar_t* OccasionText(Occasion when) noexcept
{
    switch (when)
    {
    case Occasion::FirstQuestion:
        return L"first question";
    case Occasion::MemberDown:
        return L"its player's member is down";
    case Occasion::Decision:
        break;
    }
    return L"decision";
}

std::uint32_t NowMs32() noexcept
{
    return static_cast<std::uint32_t>(platform::NowMs());
}

/// Tosses `controller`'s coin and keeps it: the player it names (1..4), or 0 (the game's own answer). Its Debug line
/// is written unless it says what the enemy's last one said within a second (coin_table's repeat rule: then a Trace
/// line, and the next differing line counts the repeats).
int Toss(void* controller, Occasion when) noexcept
{
    const auto key      = reinterpret_cast<std::uintptr_t>(controller);
    const auto seated   = g_seated.load(std::memory_order_relaxed);
    unsigned   eligible = 0;
    for (int seat = 0; seat < kSeats; ++seat)
    {
        if ((seated & (1u << seat)) != 0 && Standing(seat))
        {
            eligible |= 1u << seat;
        }
    }
    const coin_choice::Choice choice = coin_choice::Choose(seated, eligible, Random());
    if (choice.player == coin_choice::kGamesOwn)
    {
        g_coins.Clear(key);
        Count(g_counts.gamesOwn, g_totals.gamesOwn);
        return 0;
    }
    const int player = choice.player + 1;
    g_coins.Set(key, player, Seconds());
    Count(g_counts.tosses[static_cast<std::size_t>(choice.player)], g_totals.tosses[static_cast<std::size_t>(choice.player)]);
    const auto said = static_cast<std::uint32_t>(player) | static_cast<std::uint32_t>(choice.why) << 3 |
                      static_cast<std::uint32_t>(when) << 6;
    unsigned repeated = 0;
    if (g_coins.Repeat(key, said, NowMs32(), repeated))
    {
        platform::LogTrace(L"enemy coin: enemy {:#x} tossed again, as before (player {}; {}; {})", key, player, OccasionText(when),
                           Because(choice.why));
    }
    else if (repeated > 0)
    {
        platform::LogDebug(L"enemy coin: enemy {:#x} counts player {} as the player until its next action ({}; {}) (the toss before "
                           L"it repeated {} times within a second each)",
                           key, player, OccasionText(when), Because(choice.why), repeated);
    }
    else
    {
        platform::LogDebug(L"enemy coin: enemy {:#x} counts player {} as the player until its next action ({}; {})", key, player,
                           OccasionText(when), Because(choice.why));
    }
    return player;
}

/// The coin `controller` holds: tossed if it has none, tossed again if its player's member is down.
int CoinOf(void* controller) noexcept
{
    const auto key  = reinterpret_cast<std::uintptr_t>(controller);
    const int  held = g_coins.PlayerOf(key);
    if (held == 0)
    {
        return Toss(controller, Occasion::FirstQuestion);
    }
    if (!Standing(held - 1))
    {
        Count(g_counts.retosses, g_totals.retosses);
        return Toss(controller, Occasion::MemberDown);
    }
    return held;
}

/// A further player's member for a coin naming it; null for player 1 (the game's own answer) or none.
void* MemberOf(int player) noexcept
{
    return player >= 2 ? g_members[static_cast<std::size_t>(player - 1)].load(std::memory_order_relaxed) : nullptr;
}

/// The answer for an enemy whose coin names no further player: the game's, unless the game's leader lies down (in
/// co-op its forced switch is refused) while another member stands; then that member (battle/enemy_leader), as the
/// single-player game's switch would have made it the leader.
void* GamesOrInstead(void* game) noexcept
{
    void* const instead = enemy_leader::Instead();
    return instead != nullptr ? instead : game;
}

// --- The toss: the decision gate ---------------------------------------------------------------------------------

char GateFor(platform::EntryRedirect& redirect, void* controller) noexcept
{
    const bool tossing  = Tossing();
    const bool recording = enemy_watch::Recording();
    void*      enemy     = tossing || recording ? EnemyComponent(controller) : nullptr;
    if (enemy != nullptr)
    {
        const auto    key    = reinterpret_cast<std::uintptr_t>(controller);
        std::uint64_t action = 0;
        const bool    read   = Read(static_cast<const std::uint8_t*>(controller) + facts::kBattleAICurrentAction, action);
        if (recording)
        {
            enemy_watch::Observe(controller, enemy, read ? action : 0);
        }
        if (tossing && read)
        {
            if (action != 0)
            {
                g_coins.Mark(key);  // this coin's action runs; the next idle gate tosses again
            }
            else if (g_coins.PlayerOf(key) == 0 || g_coins.Marked(key))
            {
                Toss(controller, Occasion::Decision);
            }
            enemy_stall::BeforeGate(controller, enemy, action);  // a stalled hate enemy: the gate's own update picks again
        }
    }
    return reinterpret_cast<GateFn>(redirect.Original())(controller);
}

char Gate(void* controller)
{
    return GateFor(g_gate, controller);
}

/// The mines' (and charge cores') gate: the toss, then the game's gate with the enemy recorded for its area read
/// (also when the gate is reached outside the follow tick).
char MineGate(void* controller)
{
    void* const previous = t_enemy;
    if (Tossing() && IsEnemy(controller))
    {
        t_enemy = controller;
    }
    const char open = GateFor(g_mineGate, controller);
    t_enemy         = previous;
    return open;
}

// --- The opening target ------------------------------------------------------------------------------------------

/// The enemy component's battle start: the enemy recorded for its leader read below.
void BattleStart(void* component)
{
    void* previous = t_enemy;
    void* enemy    = nullptr;
    if (Tossing())
    {
        void** vtable = nullptr;
        void*  owner  = nullptr;
        OwnerFn getOwner = nullptr;
        if (Read(component, vtable) && Read(vtable + 4, getOwner) && platform::CallGuarded(getOwner, owner, component))
        {
            enemy = players::actor_fields::BattleAi(owner);
        }
    }
    void* const starting = t_starting;
    t_enemy              = enemy;
    t_starting           = component;
    reinterpret_cast<BattleStartFn>(g_battleStart.Original())(component);
    t_starting = starting;
    t_enemy    = previous;
}

void* OpeningLeader(void* partyManager, std::uint8_t variant)
{
    void* const game = reinterpret_cast<GetPartyLeaderFn>(g_opening.Original())(partyManager, variant);
    if (t_enemy == nullptr)
    {
        return game;
    }
    const int player = CoinOf(t_enemy);
    if (void* member = MemberOf(player); member != nullptr)
    {
        Count(g_counts.openingAnswers, g_totals.openingAnswers);
        platform::LogDebug(L"enemy coin: enemy {:#x} opens on player {}'s member", reinterpret_cast<std::uintptr_t>(t_enemy), player);
        return member;
    }
    return game;
}

// --- The remaining reads: mines, charge cores, water tornadoes, flyers, the height chase -------------------------

/// The game's answer to one of the remaining leader reads, or the coin's member when `enemy`'s coin names a further
/// player. Per-tick reads: Trace only.
void* AnswerRead(void* game, ReadKind kind, void* enemy) noexcept
{
    if (enemy == nullptr)
    {
        return game;
    }
    const int player = CoinOf(enemy);
    if (player >= 1)
    {
        Count(g_counts.reads[kind][static_cast<std::size_t>(player - 1)], g_totals.reads[kind][static_cast<std::size_t>(player - 1)]);
    }
    void* member = MemberOf(player);
    platform::LogTrace(L"enemy coin: {} answered player {} for enemy {:#x}", kReadNames[kind], player == 0 ? 1 : player,
                       reinterpret_cast<std::uintptr_t>(enemy));
    return member != nullptr ? member : GamesOrInstead(game);
}

/// The enemy whose move struct `t_move` names (its first field is the controller), or null.
void* EnemyOfMove() noexcept
{
    void* const controller = t_move != nullptr ? ReadPointer(t_move, 0) : nullptr;
    return controller != nullptr && IsEnemy(controller) ? controller : nullptr;
}

/// A follow tick (the mine, charge core and water tornado controllers' slot +0x408): the controller recorded for the
/// reads inside, the game's tick unchanged. The base battle AI tick runs inside, so the script reads in it answer the
/// same enemy's coin, as they do through the swapped natives.
std::uint64_t FollowTick(const platform::VTableEntry& slot, void* controller, float deltaSeconds) noexcept
{
    void* const previous = t_enemy;
    t_enemy              = Tossing() && IsEnemy(controller) ? controller : nullptr;
    const auto result    = reinterpret_cast<FollowTickFn>(slot.Original())(controller, deltaSeconds);
    t_enemy              = previous;
    return result;
}

std::uint64_t MineTick(void* controller, float deltaSeconds)
{
    return FollowTick(g_mineTick, controller, deltaSeconds);
}

std::uint64_t CoreTick(void* controller, float deltaSeconds)
{
    return FollowTick(g_coreTick, controller, deltaSeconds);
}

std::uint64_t TornadoTick(void* controller, float deltaSeconds)
{
    return FollowTick(g_tornadoTick, controller, deltaSeconds);
}

void* MineFollowActor(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate)
{
    return AnswerRead(g_getActor(partyManager, variant, type, alternate), kMineFollow, t_enemy);
}

void* MineAreaActor(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate)
{
    return AnswerRead(g_getActor(partyManager, variant, type, alternate), kMineArea, t_enemy);
}

void* MineTriggerActor(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate)
{
    return AnswerRead(g_getActor(partyManager, variant, type, alternate), kMineTrigger, t_enemy);
}

/// The rolling goal: read once per roll start, so each answer is a Debug line.
void* MineGoalActor(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate)
{
    void* const game   = g_getActor(partyManager, variant, type, alternate);
    void* const answer = AnswerRead(game, kMineGoal, t_enemy);
    if (t_enemy != nullptr)
    {
        const int player = answer != game ? SeatOfMember(answer) + 1 : 1;
        platform::LogDebug(L"enemy coin: mine {:#x} rolls at player {}'s member ({})", reinterpret_cast<std::uintptr_t>(t_enemy), player,
                           t_renoRolling ? L"Reno's finishing roll" : L"its group's trigger or its own roll");
    }
    return answer;
}

void* CoreFollowActor(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate)
{
    return AnswerRead(g_getActor(partyManager, variant, type, alternate), kCoreFollow, t_enemy);
}

void* TornadoFollowActor(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate)
{
    return AnswerRead(g_getActor(partyManager, variant, type, alternate), kTornadoFollow, t_enemy);
}

void* FlyerPointActor(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate)
{
    return AnswerRead(g_getActor(partyManager, variant, type, alternate), kFlyerPoint, Tossing() ? EnemyOfMove() : nullptr);
}

void* HeightChaseLeader()
{
    return AnswerRead(g_getLeaderChar(), kHeightChase, Tossing() ? EnemyOfMove() : nullptr);
}

/// Reno's finishing roll starts each mine's roll with nothing recorded: that mine's own coin answers its goal.
std::int64_t RenoStartsRolling(void* mine, int index, int count)
{
    const auto original = reinterpret_cast<StartRollingFn>(g_renoRolling.Original());
    if (t_enemy != nullptr || !Tossing() || !IsEnemy(mine))
    {
        return original(mine, index, count);
    }
    t_enemy              = mine;
    t_renoRolling        = true;
    const auto result    = original(mine, index, count);
    t_renoRolling        = false;
    t_enemy              = nullptr;
    return result;
}

/// The flyers' point evaluation and the move type choice: the move struct recorded for the read inside.
std::int64_t FlyerEvalPoint(void* move, void* candidates, float* point)
{
    void* const previous = t_move;
    t_move               = Tossing() ? move : nullptr;
    const auto result    = reinterpret_cast<EvalPointFn>(g_flyerEval.Original())(move, candidates, point);
    t_move               = previous;
    return result;
}

std::int64_t SelectMoveType(void* move)
{
    void* const previous = t_move;
    t_move               = Tossing() ? move : nullptr;
    const auto result    = reinterpret_cast<SelectMoveTypeFn>(g_selectMove.Original())(move);
    t_move               = previous;
    return result;
}

// --- The script natives' reads -----------------------------------------------------------------------------------

/// The game's answer to a script's "the player" read, or the coin's member when the calling enemy's coin names a
/// further player.
void* AnswerScript(void* game, const wchar_t* what) noexcept
{
    if (t_enemy == nullptr)
    {
        return game;
    }
    const int player = CoinOf(t_enemy);
    if (player >= 1)
    {
        Count(g_counts.scriptAnswers[static_cast<std::size_t>(player - 1)], g_totals.scriptAnswers[static_cast<std::size_t>(player - 1)]);
    }
    void* member = MemberOf(player);
    platform::LogTrace(L"enemy coin: {} answered player {} for enemy {:#x}", what, player == 0 ? 1 : player,
                       reinterpret_cast<std::uintptr_t>(t_enemy));
    return member != nullptr ? member : GamesOrInstead(game);
}

void* ScriptLeader(void* partyManager, std::uint8_t variant)
{
    return AnswerScript(g_getLeader(partyManager, variant), L"GetLeader");
}

void* ScriptActor(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate)
{
    return AnswerScript(g_getActor(partyManager, variant, type, alternate), L"the player actor read");
}

/// PartyManager_Get at the expectation counts' leader-type read: a block carrying the coin's human's type.
void* PartyManagerForExpectation()
{
    if (t_enemy != nullptr)
    {
        const int player = CoinOf(t_enemy);
        if (player >= 2)
        {
            Count(g_counts.expectationAnswers, g_totals.expectationAnswers);
            return g_fakes[player - 2];
        }
    }
    return g_pmGet();
}

/// PartyManager_Get at the hate pick's two tie-break reads, given the picking enemy's controller: "the player" there is
/// the enemy's coin human alone, the game's rule with one leader. A tie between humans goes to that one, and a tie
/// between the others to the game's own coin byte; with every human answered "the player", the last tied human in party
/// order won every tie (player 1, first, never did; research/RE-enemy-hate-three-humans.md).
void* TieAnswer(void* controller) noexcept
{
    if (Tossing() && controller != nullptr)
    {
        const int player = CoinOf(controller);
        if (player >= 1 && player <= kSeats)
        {
            Count(g_counts.tieAnswers[static_cast<std::size_t>(player - 1)], g_totals.tieAnswers[static_cast<std::size_t>(player - 1)]);
        }
        if (player >= 2)
        {
            return g_fakes[player - 2];
        }
    }
    return g_pmGet();
}

/// `IsA_EndBattleAIController` at the enemy EQS target context: a human's controller counts.
bool EqsIsA(void* controller, void* b, void* c)
{
    if (g_on.load(std::memory_order_acquire) && g_enabled.load(std::memory_order_relaxed) && IsFurtherController(controller))
    {
        Count(g_counts.eqsKept, g_totals.eqsKept);
        return true;
    }
    return reinterpret_cast<IsAFn>(g_eqs.Original())(controller, b, c);
}

/// `IsA_EndBattleAIController` at the battle gate of a character's own area queries: a human's member refreshes its
/// area as an AI ally does, so an enemy targeting it finds the points around it (RE-enemy-circling.md).
bool AreaIsA(void* controller, void* b, void* c)
{
    if (g_on.load(std::memory_order_acquire) && g_areaOn.load(std::memory_order_relaxed) && IsFurtherController(controller))
    {
        Count(g_counts.areasRefreshed, g_totals.areasRefreshed);
        return true;
    }
    return reinterpret_cast<IsAFn>(g_area.Original())(controller, b, c);
}

/// A native called from an enemy's script: the enemy recorded on this thread for the reads inside; the party API's
/// results replaced by the coin's.
template <std::size_t I>
void Exec(UObject* context, RC::Unreal::FFrame& frame, void* result)
{
    const ExecFn original = g_originals[I].load(std::memory_order_acquire);
    void*        enemy    = nullptr;
    if (Tossing())
    {
        void* candidate = kNatives[I].kind == Kind::Member ? static_cast<void*>(context) : ReadPointer(&frame, facts::kFrameObject);
        const auto* const aiClass = g_aiClass.load(std::memory_order_acquire);
        const bool controller = kNatives[I].kind == Kind::Member ||
                                (candidate != nullptr && aiClass != nullptr && static_cast<UObject*>(candidate)->IsA(aiClass));
        if (controller && IsEnemy(candidate))
        {
            enemy = candidate;
        }
    }
    void* const previous = t_enemy;
    t_enemy              = enemy;
    original(context, frame, result);
    if constexpr (kNatives[I].kind == Kind::LeaderApi || kNatives[I].kind == Kind::LeaderTypeApi)
    {
        if (enemy != nullptr && result != nullptr)
        {
            const int    player      = CoinOf(enemy);
            std::uint8_t insteadType = facts::kPlayerTypeNone;
            void* const  member      = MemberOf(player);
            void* const  instead     = member == nullptr ? enemy_leader::Instead(&insteadType) : nullptr;
            if (member != nullptr || instead != nullptr)
            {
                Count(g_counts.apiAnswers, g_totals.apiAnswers);
                if constexpr (kNatives[I].kind == Kind::LeaderApi)
                {
                    void* const answer = member != nullptr ? member : instead;
                    std::memcpy(result, &answer, sizeof(answer));
                }
                else
                {
                    const std::uint8_t type = member != nullptr ? g_fakes[player - 2][type_answer_code::kTypeOffset] : insteadType;
                    std::memcpy(result, &type, sizeof(type));
                }
                platform::LogTrace(L"enemy coin: {} answered {} for enemy {:#x}", kNatives[I].path,
                                   member != nullptr ? std::format(L"player {}", player) : std::wstring(L"the standing member in the downed leader's place"),
                                   reinterpret_cast<std::uintptr_t>(enemy));
            }
        }
    }
    t_enemy = previous;
}

template <std::size_t... I>
constexpr std::array<ExecFn, kNativeCount> ExecTable(std::index_sequence<I...>) noexcept
{
    return {{&Exec<I>...}};
}
constexpr std::array<ExecFn, kNativeCount> kExecs = ExecTable(std::make_index_sequence<kNativeCount>{});

// --- Hits and reaction sensing: the human's scopes ---------------------------------------------------------------

/// Whether a hit or sensing by or on `seat`'s member may run in its scopes now (the game thread only).
bool MayScope(int seat, std::size_t lever, const wchar_t* what) noexcept
{
    if (seat == 0 || !g_on.load(std::memory_order_acquire) || !g_enabled.load(std::memory_order_relaxed))
    {
        return false;
    }
    if (!platform::OnGameThread())
    {
        Count(g_counts.offThread, g_totals.offThread);
        if (!g_offThreadLogged)
        {
            g_offThreadLogged = true;
            platform::LogWarning(L"enemy coin: {} for player {}'s member ran off the game thread; it keeps the game's answers", what, seat + 1);
        }
        return false;
    }
    if (!g_scopedLogged[lever])
    {
        g_scopedLogged[lever] = true;
        platform::LogDebug(L"enemy coin: {} for player {}'s member runs in that player's scopes", what, seat + 1);
    }
    return true;
}

/// The seat whose member a weak pair at `at` names, or 0 (game thread).
int SeatOfWeak(const std::uint8_t* at) noexcept
{
    std::int32_t pair[2]{};
    return Read(at, pair) ? SeatOfMember(players::weak_object::ObjectOf(pair)) : 0;
}

char HitApply(void* context, void* hit)
{
    const auto original = reinterpret_cast<HitApplyFn>(g_hitApply.Original());
    if (Tossing())
    {
        if (!platform::OnGameThread())
        {
            // The weak pairs cannot be resolved here; whoever the hit is on, it keeps the game's answers.
            Count(g_counts.offThread, g_totals.offThread);
            if (!g_offThreadLogged)
            {
                g_offThreadLogged = true;
                platform::LogWarning(L"enemy coin: a hit was applied off the game thread; such hits keep the game's answers");
            }
            return original(context, hit);
        }
        const auto* bytes = static_cast<const std::uint8_t*>(hit);
        int         seat  = SeatOfWeak(bytes + facts::kBattleHitAttackerWeak);
        if (seat == 0)
        {
            seat = SeatOfWeak(bytes + facts::kBattleHitVictimWeak);
        }
        if (MayScope(seat, 0, L"a hit"))
        {
            players::scopes::Scope scope(seat, players::scopes::Leader | players::scopes::Primary);
            if (scope.Held())
            {
                Count(g_counts.hitsScoped, g_totals.hitsScoped);
            }
            return original(context, hit);
        }
    }
    return original(context, hit);
}

void HitApplyB(std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7)
{
    const auto original = reinterpret_cast<HitApplyBFn>(g_hitApplyB.Original());
    int        seat     = SeatOfMember(reinterpret_cast<const void*>(a2));
    if (seat == 0)
    {
        seat = SeatOfMember(reinterpret_cast<const void*>(a4));
    }
    if (Tossing() && MayScope(seat, 1, L"a hit (second form)"))
    {
        players::scopes::Scope scope(seat, players::scopes::Leader | players::scopes::Primary);
        if (scope.Held())
        {
            Count(g_counts.hitsScoped, g_totals.hitsScoped);
        }
        original(a1, a2, a3, a4, a5, a6, a7);
        return;
    }
    original(a1, a2, a3, a4, a5, a6, a7);
}

void Sensing(std::uint64_t attackerComponent, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6,
             std::uint64_t a7, std::uint64_t a8)
{
    const auto original = reinterpret_cast<SensingFn>(g_sensing.Original());
    int        seat     = 0;
    for (std::size_t candidate = 1; candidate < kSeats && seat == 0; ++candidate)
    {
        void* member = g_members[candidate].load(std::memory_order_relaxed);
        if (member != nullptr && players::actor_fields::BattleComponent(member) == reinterpret_cast<void*>(attackerComponent))
        {
            seat = static_cast<int>(candidate);
        }
    }
    if (Tossing() && MayScope(seat, 2, L"the reaction sensing"))
    {
        players::scopes::Scope scope(seat, players::scopes::Leader);
        if (scope.Held())
        {
            Count(g_counts.sensingsScoped, g_totals.sensingsScoped);
        }
        original(attackerComponent, a2, a3, a4, a5, a6, a7, a8);
        return;
    }
    original(attackerComponent, a2, a3, a4, a5, a6, a7, a8);
}

// --- Install helpers ---------------------------------------------------------------------------------------------

/// Places `code` (built, empty when the build failed) near the PartyManager_Get call at `site` and points the call at it.
bool PlaceLeaderTestAnswer(platform::CallRedirect& redirect, std::uintptr_t site, std::span<const std::uint8_t> code,
                           std::uintptr_t partyManagerGet, bool crosses, const wchar_t* what) noexcept
{
    const std::uintptr_t placedCode =
        code.empty() ? 0
                     : platform::PlaceCodeNear(site, code.size(), [&](std::uintptr_t, std::span<std::uint8_t> out) noexcept {
                           std::memcpy(out.data(), code.data(), code.size());
                           return true;
                       });
    const bool placed = placedCode != 0 && (crosses ? redirect.InstallFromOwnThread(site, partyManagerGet, placedCode)
                                                    : redirect.Install(site, partyManagerGet, placedCode));
    if (!placed)
    {
        platform::Log(L"enemy coin: {} could not be answered; it keeps the game's leader test", what);
    }
    return placed;
}

/// Places the leader-type answer for the inlined test at `site` (the member in `reg`) and points the call at it.
bool PlaceTypeAnswer(platform::CallRedirect& redirect, std::uintptr_t site, type_answer_code::Register reg, std::uintptr_t partyManagerGet,
                     bool crosses, const wchar_t* what) noexcept
{
    std::array<std::uint8_t, type_answer_code::kMaxBytes> bytes{};
    const std::size_t length = type_answer_code::Build(bytes, reg, reinterpret_cast<std::uintptr_t>(&g_fakes[0][0]), kFurther, partyManagerGet);
    return PlaceLeaderTestAnswer(redirect, site, std::span<const std::uint8_t>(bytes.data(), length), partyManagerGet, crosses, what);
}

/// Places the hate pick's tie-break answer at `site`: the enemy's controller handed to TieAnswer.
bool PlaceTieAnswer(platform::CallRedirect& redirect, std::uintptr_t site, std::uintptr_t partyManagerGet, bool crosses) noexcept
{
    std::array<std::uint8_t, type_answer_code::kControllerJumpBytes> bytes{};
    const std::size_t length = type_answer_code::BuildControllerJump(bytes, reinterpret_cast<std::uintptr_t>(&TieAnswer));
    return PlaceLeaderTestAnswer(redirect, site, std::span<const std::uint8_t>(bytes.data(), length), partyManagerGet, crosses,
                                 L"the hate pick's tie-break leader test");
}

bool PlaceCall(platform::CallRedirect& redirect, const platform::GameImage& image, std::uintptr_t site, std::uintptr_t target, const void* replacement,
               bool crosses, const wchar_t* what) noexcept
{
    const auto live   = image.Rebase(site, 5);
    const bool placed = live && (crosses ? redirect.InstallFromOwnThread(*live, target, reinterpret_cast<std::uintptr_t>(replacement))
                                         : redirect.Install(*live, target, reinterpret_cast<std::uintptr_t>(replacement)));
    if (!placed)
    {
        platform::Log(L"enemy coin: {} could not be redirected; it keeps the game's answer", what);
    }
    return placed;
}

/// Swaps each native's UFunction pointer whose function exists and still holds the exec IDA named. Game thread.
void TrySwaps() noexcept
{
    if (g_aiClass.load(std::memory_order_relaxed) == nullptr)
    {
        const auto* const found = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, kAiControllerClass);
        g_aiClass.store(found, std::memory_order_release);
        if (found == nullptr)
        {
            if (++g_swapTries == 1)
            {
                platform::LogDebug(L"enemy coin: {} not found yet; trying again each second", kAiControllerClass);
            }
            return;
        }
    }
    std::size_t missing = 0;
    for (std::size_t i = 0; i < kNativeCount; ++i)
    {
        if (g_originals[i].load(std::memory_order_relaxed) != nullptr)
        {
            continue;
        }
        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kNatives[i].path);
        if (function == nullptr)
        {
            ++missing;
            continue;
        }
        const auto current = reinterpret_cast<std::uintptr_t>(function->GetFuncPtr());
        if (g_execs[i] == 0 || current != g_execs[i])
        {
            platform::Log(L"enemy coin: {} holds {:#x}, not the exec read in IDA; it keeps the game's own answer", kNatives[i].path, current);
            g_originals[i].store(reinterpret_cast<ExecFn>(current), std::memory_order_release);  // never swapped
            continue;
        }
        g_originals[i].store(reinterpret_cast<ExecFn>(current), std::memory_order_release);
        function->SetFuncPtr(kExecs[i]);
        g_functions[i] = function;
    }
    if (missing == 0)
    {
        g_swapsDone = true;
        std::size_t swapped = 0;
        for (auto* function : g_functions)
        {
            swapped += function != nullptr ? 1 : 0;
        }
        platform::Log(L"enemy coin: {} of {} script natives run with the calling enemy recorded", swapped, kNativeCount);
    }
    else if (++g_swapTries == 1)
    {
        platform::LogDebug(L"enemy coin: {} script natives not found yet; trying again each second", missing);
    }
}

std::wstring Reading(const Counts& counts) noexcept
{
    std::wstring text =
        std::format(L"tosses P1 {} / P2 {} / P3 {} (no further player {}, re-tossed {}); script answers P1 {} / P2 {} / P3 {}; opening {}, "
                    L"expectation {}, party API {}; EQS kept {}; areas refreshed {}; hits scoped {}, sensings scoped {}, off-thread {}",
                    counts.tosses[0].load(), counts.tosses[1].load(), counts.tosses[2].load(), counts.gamesOwn.load(), counts.retosses.load(),
                    counts.scriptAnswers[0].load(), counts.scriptAnswers[1].load(), counts.scriptAnswers[2].load(), counts.openingAnswers.load(),
                    counts.expectationAnswers.load(), counts.apiAnswers.load(), counts.eqsKept.load(), counts.areasRefreshed.load(),
                    counts.hitsScoped.load(), counts.sensingsScoped.load(), counts.offThread.load());
    if (counts.tieAnswers[0].load() + counts.tieAnswers[1].load() + counts.tieAnswers[2].load() != 0)
    {
        text += std::format(L"; the hate pick's tie reads answered for P1 {} / P2 {} / P3 {}", counts.tieAnswers[0].load(),
                            counts.tieAnswers[1].load(), counts.tieAnswers[2].load());
    }
    for (std::size_t kind = 0; kind < kReadKinds; ++kind)
    {
        const auto& per = counts.reads[kind];  // players 1-3 printed: three is the most the mod seats
        if (per[0].load() + per[1].load() + per[2].load() + per[3].load() != 0)
        {
            text += std::format(L"; {} P1 {} / P2 {} / P3 {}", kReadNames[kind], per[0].load(), per[1].load(), per[2].load());
        }
    }
    return text;
}

void ResetSince() noexcept
{
    for (auto& t : g_counts.tosses) Take(t);
    for (auto& a : g_counts.scriptAnswers) Take(a);
    for (auto& a : g_counts.tieAnswers) Take(a);
    for (auto& kind : g_counts.reads)
    {
        for (auto& r : kind) Take(r);
    }
    Take(g_counts.gamesOwn);
    Take(g_counts.retosses);
    Take(g_counts.openingAnswers);
    Take(g_counts.expectationAnswers);
    Take(g_counts.apiAnswers);
    Take(g_counts.eqsKept);
    Take(g_counts.areasRefreshed);
    Take(g_counts.hitsScoped);
    Take(g_counts.sensingsScoped);
    Take(g_counts.offThread);
}

bool Any(const Counts& counts) noexcept
{
    unsigned sum = counts.gamesOwn.load() + counts.retosses.load() + counts.openingAnswers.load() + counts.expectationAnswers.load() +
                   counts.apiAnswers.load() + counts.eqsKept.load() + counts.areasRefreshed.load() + counts.hitsScoped.load() + counts.sensingsScoped.load() + counts.offThread.load();
    for (std::size_t i = 0; i < kSeats; ++i)
    {
        sum += counts.tosses[i].load() + counts.scriptAnswers[i].load() + counts.tieAnswers[i].load();
        for (const auto& kind : counts.reads)
        {
            sum += kind[i].load();
        }
    }
    return sum != 0;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto pmGet       = platform::FindFunction(image, facts::kPartyManagerGet);
    const auto isA         = platform::FindFunction(image, facts::kIsABattleAIController);
    const auto isPartySide = platform::FindFunction(image, facts::kBattleCompIsPartySide);
    const auto pawnComp    = platform::FindFunction(image, facts::kBattleAIGetPawnComp);
    const auto getLeader   = platform::FindFunction(image, facts::kGetPartyLeader);
    const auto getActor    = platform::FindFunction(image, facts::kGetPlayerActor);
    const auto hitApplyB   = platform::FindFunction(image, facts::kBattleHitApplyB);
    const auto sensing     = platform::FindFunction(image, facts::kBattleReactionSensing);
    const auto gate        = image.Rebase(facts::kBattleAIDecisionGate);
    const auto mineGate    = image.Rebase(facts::kBattleAIMineDecisionGate);
    const auto hitApply    = image.Rebase(facts::kBattleHitApply);
    if (!pmGet || !isA || !isPartySide || !pawnComp || !getLeader || !getActor || !hitApplyB || !sensing || !gate || !mineGate || !hitApply)
    {
        platform::Log(L"enemy coin: an engine check failed; enemies keep naming player 1 as the player");
        return false;
    }
    g_pmGet       = reinterpret_cast<PmGetFn>(*pmGet);
    g_getLeader   = reinterpret_cast<GetPartyLeaderFn>(*getLeader);
    g_getActor    = reinterpret_cast<GetPlayerActorFn>(*getActor);
    g_isPartySide = reinterpret_cast<IsPartySideFn>(*isPartySide);
    g_pawnComp    = reinterpret_cast<PawnCompFn>(*pawnComp);
    for (std::size_t i = 0; i < kNativeCount; ++i)
    {
        const auto exec = image.Rebase(facts::kBattleCoinNativeExecs[i]);
        g_execs[i]      = exec ? *exec : 0;
    }
    for (auto& fake : g_fakes)
    {
        fake[type_answer_code::kTypeOffset] = facts::kPlayerTypeNone;
    }
    g_on.store(true, std::memory_order_release);  // before any lever: a lever may run the moment it is placed

    std::size_t placed = 0;
    // The toss (entry redirects on the gate and the mines' override).
    placed += g_gate.Install(*gate, facts::kBattleAIDecisionGateFirst, reinterpret_cast<std::uintptr_t>(&Gate)) ? 1 : 0;
    placed += g_mineGate.Install(*mineGate, facts::kBattleAIMineDecisionGateFirst, reinterpret_cast<std::uintptr_t>(&MineGate)) ? 1 : 0;
    if (!g_gate.IsInstalled())
    {
        platform::Log(L"enemy coin: the decision gate could not be redirected; coins are tossed at the first question only");
    }
    // The opening target: the slot, then its leader read.
    if (platform::ReplaceSlot(image, facts::kBattleEnemyCompBattleStartSlot, reinterpret_cast<std::uintptr_t>(&BattleStart), g_battleStart))
    {
        placed += PlaceCall(g_opening, image, facts::kBattleOpeningLeaderCall, *getLeader, &OpeningLeader, false, L"the opening target's leader read") ? 2 : 1;
    }
    // The script natives' reads (the swaps follow in Tick).
    placed += PlaceCall(g_scriptLeader, image, facts::kBattleScriptLeaderCall, *getLeader, &ScriptLeader, false, L"the scripts' GetLeader") ? 1 : 0;
    for (std::size_t i = 0; i < g_scriptActor.size(); ++i)
    {
        placed += PlaceCall(g_scriptActor[i], image, facts::kBattleScriptPlayerActorCalls[i], *getActor, &ScriptActor, false, L"a script's player actor read") ? 1 : 0;
    }
    for (std::size_t i = 0; i < g_expect.size(); ++i)
    {
        placed += PlaceCall(g_expect[i], image, facts::kBattleExpectationTypeCalls[i], *pmGet, &PartyManagerForExpectation, false,
                            L"an expectation count's leader type read")
                      ? 1
                      : 0;
    }
    // The player's weight at the inlined tests (on_unreal_init: no battle runs, so the crossing calls can be placed
    // from here).
    const auto hate = image.Rebase(facts::kBattleHateTypeCall, 5);
    placed += hate && PlaceTypeAnswer(g_hate, *hate, type_answer_code::kR14, *pmGet, false, L"the hate rows' leader test") ? 1 : 0;
    for (std::size_t i = 0; i < g_tie.size(); ++i)
    {
        const auto tie = image.Rebase(facts::kBattleTieTypeCalls[i], 5);
        placed += tie && PlaceTieAnswer(g_tie[i], *tie, *pmGet, i == 1) ? 1 : 0;
    }
    const auto isAipc = image.Rebase(facts::kBattleIsAipcTypeCall, 5);
    placed += isAipc && PlaceTypeAnswer(g_isAipc, *isAipc, type_answer_code::kRbx, *pmGet, true, L"IsAIPC's leader test") ? 1 : 0;
    // Membership, hits, sensing.
    placed += PlaceCall(g_eqs, image, facts::kBattleEqsIsACall, *isA, &EqsIsA, false, L"the enemy EQS membership test") ? 1 : 0;
    placed += PlaceCall(g_area, image, facts::kCharacterAreaIsACall, *isA, &AreaIsA, false, L"a character's battle area test") ? 1 : 0;
    placed += g_hitApply.Install(*hitApply, facts::kBattleHitApplyFirst, reinterpret_cast<std::uintptr_t>(&HitApply)) ? 1 : 0;
    placed += PlaceCall(g_hitApplyB, image, facts::kBattleHitApplyBCall, *hitApplyB, &HitApplyB, true, L"the hit apply's second form") ? 1 : 0;
    placed += PlaceCall(g_sensing, image, facts::kBattleReactionSensingCall, *sensing, &Sensing, false, L"the reaction sensing") ? 1 : 0;

    // The remaining reads (RE-enemy-coin-remaining-reads.md): the follow ticks record their enemy for the reads inside;
    // Reno's finishing roll, the flyers' point evaluation and the move type choice record theirs at their one call.
    placed += platform::ReplaceSlot(image, facts::kMineFollowTickSlot, reinterpret_cast<std::uintptr_t>(&MineTick), g_mineTick) ? 1 : 0;
    placed += platform::ReplaceSlot(image, facts::kCoreFollowTickSlot, reinterpret_cast<std::uintptr_t>(&CoreTick), g_coreTick) ? 1 : 0;
    placed += platform::ReplaceSlot(image, facts::kTornadoFollowTickSlot, reinterpret_cast<std::uintptr_t>(&TornadoTick), g_tornadoTick) ? 1 : 0;
    placed += PlaceCall(g_mineFollow, image, facts::kMineFollowActorCall, *getActor, &MineFollowActor, false, L"a mine's follow read") ? 1 : 0;
    placed += PlaceCall(g_mineArea, image, facts::kMineAreaActorCall, *getActor, &MineAreaActor, false, L"a mine's area read") ? 1 : 0;
    placed += PlaceCall(g_mineTrigger, image, facts::kMineTriggerActorCall, *getActor, &MineTriggerActor, false, L"a mine's rolling trigger read") ? 1 : 0;
    placed += PlaceCall(g_mineGoal, image, facts::kMineGoalActorCall, *getActor, &MineGoalActor, false, L"a mine's rolling goal read") ? 1 : 0;
    placed += PlaceCall(g_coreFollow, image, facts::kCoreFollowActorCall, *getActor, &CoreFollowActor, false, L"a charge core's follow read") ? 1 : 0;
    placed += PlaceCall(g_tornadoFollow, image, facts::kTornadoFollowActorCall, *getActor, &TornadoFollowActor, false,
                        L"a water tornado's follow read")
                  ? 1
                  : 0;
    placed += PlaceCall(g_flyerPoint, image, facts::kFlyerPointActorCall, *getActor, &FlyerPointActor, false, L"a flyer's point read") ? 1 : 0;
    const auto startRolling = platform::FindFunction(image, facts::kMineStartRolling);
    const auto evalPoint    = platform::FindFunction(image, facts::kFlyerEvalPoint);
    const auto selectMove   = platform::FindFunction(image, facts::kSelectMoveType);
    const auto leaderChar   = platform::FindFunction(image, facts::kGetLeaderEndCharacter);
    if (startRolling)
    {
        placed += PlaceCall(g_renoRolling, image, facts::kRenoFinishRollingCall, *startRolling, &RenoStartsRolling, false,
                            L"Reno's finishing roll")
                      ? 1
                      : 0;
    }
    if (evalPoint)
    {
        placed += PlaceCall(g_flyerEval, image, facts::kFlyerEvalPointCall, *evalPoint, &FlyerEvalPoint, false, L"the flyers' point evaluation") ? 1 : 0;
    }
    if (selectMove && leaderChar)
    {
        g_getLeaderChar = reinterpret_cast<LeaderCharFn>(*leaderChar);
        // The move type choice's call crosses its block: placed here (on_unreal_init, no battle AI exists yet).
        if (PlaceCall(g_selectMove, image, facts::kSelectMoveTypeCall, *selectMove, &SelectMoveType, true, L"the move type choice"))
        {
            placed += 1 + (PlaceCall(g_heightChase, image, facts::kHeightChaseLeaderCall, *leaderChar, &HeightChaseLeader, false,
                                     L"the height chase's leader read")
                               ? 1
                               : 0);
        }
    }

    control::Register({.name  = L"enemy-coin.counts",
                       .shape = control::Shape::Text,
                       .help  = L"the equal-attention coin's counts since install (battle/enemy_coin.h)",
                       .get   = [] { return control::Value{Reading(g_totals)}; }});
    control::Register({.name  = L"enemy-coin",
                       .shape = control::Shape::OnOff,
                       .help  = L"enemies share their attention between the humans by a coin per decision (battle/enemy_coin.h; on)",
                       .set   = [](const control::Value& value) {
                           g_enabled.store(std::get<bool>(value), std::memory_order_relaxed);
                           platform::Log(L"enemy coin {}", std::get<bool>(value) ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled.load(std::memory_order_relaxed)}; }});
    control::Register({.name  = L"member-area",
                       .shape = control::Shape::OnOff,
                       .help  = L"a human's member keeps the battle area enemies read around their target, as an AI ally does (battle/enemy_coin.h; on)",
                       .set   = [](const control::Value& value) {
                           g_areaOn.store(std::get<bool>(value), std::memory_order_relaxed);
                           platform::Log(L"enemy coin: the members' battle area {}", std::get<bool>(value) ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_areaOn.load(std::memory_order_relaxed)}; }});
    g_lastSummary = std::chrono::steady_clock::now();
    platform::Log(L"enemy coin: installed, {} of 32 levers placed; enemies toss a coin per decision between the humans (the script natives "
                  L"answer once swapped)",
                  placed);
    return true;
}

void Remove() noexcept
{
    g_on.store(false, std::memory_order_release);  // every lever passes the game's own answer through from now on
    for (auto& fake : g_fakes)
    {
        fake[type_answer_code::kTypeOffset] = facts::kPlayerTypeNone;  // the type answers match nobody
    }
    for (std::size_t i = 0; i < kNativeCount; ++i)
    {
        if (g_functions[i] != nullptr)
        {
            g_functions[i]->SetFuncPtr(g_originals[i].load(std::memory_order_relaxed));
            g_functions[i] = nullptr;
        }
    }
    for (auto* call : {&g_heightChase, &g_flyerEval, &g_renoRolling, &g_flyerPoint, &g_tornadoFollow, &g_coreFollow, &g_mineGoal,
                       &g_mineTrigger, &g_mineArea, &g_mineFollow})
    {
        call->Restore();
    }
    g_tornadoTick.Restore();
    g_coreTick.Restore();
    g_mineTick.Restore();
    g_sensing.Restore();
    g_hitApply.Restore();
    g_area.Restore();
    g_eqs.Restore();
    g_hate.Restore();
    g_tie[0].Restore();
    for (auto& call : g_expect)
    {
        call.Restore();
    }
    for (auto& call : g_scriptActor)
    {
        call.Restore();
    }
    g_scriptLeader.Restore();
    g_opening.Restore();
    g_battleStart.Restore();
    g_mineGate.Restore();
    g_gate.Restore();
    // The crossing calls (the second tie-break read, IsAIPC, the hit apply's second form, the move type choice) stay:
    // only the thread that runs them could put them back, and they pass through now.
}

void Tick() noexcept
{
    if (!g_on.load(std::memory_order_relaxed))
    {
        return;
    }
    // The seats: player 1 is the game's leader; further seats hold what the scopes bound.
    const std::uintptr_t manager = players::scopes::PartyManagerAddress();
    std::int32_t         pair[2]{};
    void* leader = manager != 0 && Read(reinterpret_cast<const void*>(manager + facts::kPartyLeaderWeak), pair)
                       ? players::weak_object::ObjectOf(pair)
                       : nullptr;
    g_members[0].store(leader, std::memory_order_relaxed);
    g_controllers[0].store(players::scopes::Controller(0), std::memory_order_relaxed);
    unsigned seated = 1u;
    for (std::size_t seat = 1; seat < kSeats; ++seat)
    {
        void*              member = players::scopes::Member(static_cast<int>(seat));
        const std::uint8_t type   = players::scopes::MemberType(static_cast<int>(seat));
        g_members[seat].store(member, std::memory_order_relaxed);
        g_controllers[seat].store(players::scopes::Controller(static_cast<int>(seat)), std::memory_order_relaxed);
        g_fakes[seat - 1][type_answer_code::kTypeOffset] = member != nullptr ? type : facts::kPlayerTypeNone;
        if (member != nullptr)
        {
            seated |= 1u << seat;
        }
    }
    g_seated.store(seated, std::memory_order_release);

    const auto now = std::chrono::steady_clock::now();
    if (!g_swapsDone && now - g_lastSwapTry >= std::chrono::seconds(1))
    {
        g_lastSwapTry = now;
        TrySwaps();  // once a second until every native is found (an object walk is never per frame)
    }
    if (now - g_lastSummary >= std::chrono::seconds(60))
    {
        g_lastSummary = now;
        if (Any(g_counts))
        {
            platform::LogDebug(L"enemy coin (60 s): {}", Reading(g_counts));
        }
        ResetSince();
    }
}

void* StartingComponent() noexcept
{
    return t_starting;
}

bool StartWatched() noexcept
{
    return g_battleStart.Active();
}

bool Active() noexcept
{
    return Tossing();
}

int HumanSeat(const void* character) noexcept
{
    const unsigned seated = g_seated.load(std::memory_order_acquire);
    for (int seat = 0; character != nullptr && seat < kSeats; ++seat)
    {
        if ((seated & (1u << seat)) != 0 && g_members[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed) == character)
        {
            return seat;
        }
    }
    return -1;
}

bool HumanStands(int seat) noexcept
{
    return seat >= 0 && seat < kSeats && (g_seated.load(std::memory_order_acquire) & (1u << seat)) != 0 && Standing(seat);
}

bool OtherHumanStands(int seat) noexcept
{
    for (int other = 0; other < kSeats; ++other)
    {
        if (other != seat && HumanStands(other))
        {
            return true;
        }
    }
    return false;
}

}  // namespace lss::battle::enemy_coin
