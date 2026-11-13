#include "battle/enemy_leader.h"

#include "battle/member_state.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_root.h"
#include "players/game_globals.h"
#include "players/weak_object.h"
#include "players/scopes.h"
#include "players/seats.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>

namespace lss::battle::enemy_leader {

namespace {

using platform::Read;

using GetPartyLeaderFn = void* (*)(void* partyManager, std::uint8_t variant);
using IterBeginFn      = void (*)(void* iterator, void* registry, void* owner, std::uint8_t categoryBit);
using IterNextFn       = void (*)(void* iterator);
using SetTargetFn      = void (*)(void* component, void* character);
using FreeFn           = void (*)(void* memory);
using GetPlayerActorFn = void* (*)(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate);

constexpr int kAllySeat = -1;  // g_enemySeat: the standing AI ally the game's forced switch would have taken

// The game's registry iterator as BattleSystem_CycleLeader keeps it on its stack: the owner at +8, the index at
// +16, then a TArray with 128 inline 40-byte entries (the character pointer first), its heap pointer, count and
// capacity after them. Zeroed, as the cycle starts it (its first push sets the inline capacity).
constexpr std::size_t kIteratorBytes    = 0x1480;
constexpr std::size_t kIteratorIndex    = 16;
constexpr std::size_t kIteratorEntries  = 24;
constexpr std::size_t kIteratorHeap     = 24 + 5120;
constexpr std::size_t kIteratorCount    = 24 + 5128;
constexpr std::size_t kEntryBytes       = 40;
constexpr std::uint8_t kCategoryBattle  = 2;

// Set at install, then only read.
std::uintptr_t         g_getLeader   = 0;
IterBeginFn            g_iterBegin   = nullptr;
IterNextFn             g_iterNext    = nullptr;
SetTargetFn            g_setTarget   = nullptr;
FreeFn                 g_free        = nullptr;
GetPlayerActorFn       g_getActor    = nullptr;
std::uint8_t**         g_data        = nullptr;  // the data singleton's global (its player status bytes)
platform::CallRedirect g_leaderCall;

// The member enemies follow in place of the game's downed leader (null while the leader stands or nobody does), and
// its player type: written by Tick, read by battle/enemy_coin on any thread.
std::atomic<void*>        g_instead{nullptr};
std::atomic<std::uint8_t> g_insteadType{facts::kPlayerTypeNone};

// Game thread.
void*         g_leader       = nullptr;  // the game's leader last tick
void*         g_enemyLeader  = nullptr;  // the enemy-facing leader last tick (null outside a battle)
int           g_enemySeat    = 0;        // 0: the game's leader; else the seat whose member enemies follow
std::uint32_t g_retargets    = 0;
std::uint32_t g_lastEnemies  = 0;
std::uint32_t g_answers      = 0;        // leader reads answered with the standing human's member
std::uint32_t g_offThread    = 0;
bool          g_offThreadLogged = false;

std::uint8_t* Component(void* character) noexcept
{
    std::uint8_t* component = nullptr;
    return character != nullptr && Read(static_cast<std::uint8_t*>(character) + facts::kCharacterBattleComponent, component)
               ? component
               : nullptr;
}

/// A member in the KO state (or unreadable: nobody to follow).
bool Downed(void* character) noexcept
{
    return member_state::KnockedOut(character, true);
}

void* GameLeader() noexcept
{
    const std::uintptr_t manager = players::scopes::PartyManagerAddress();
    std::int32_t         pair[2]{};
    return manager != 0 && Read(reinterpret_cast<const void*>(manager + facts::kPartyLeaderWeak), pair)
               ? players::weak_object::ObjectOf(pair)
               : nullptr;
}

/// The game's re-target tail of a forced leader change, with `member` as the new leader. The enemies re-targeted.
std::uint32_t Retarget(void* member) noexcept
{
    std::uint8_t* registryOwner = players::field_root::CharaOwner();
    if (registryOwner == nullptr)
    {
        return 0;
    }
    alignas(16) static std::uint8_t iterator[kIteratorBytes];  // game thread only; the game keeps its own on the stack
    std::memset(iterator, 0, sizeof(iterator));
    std::uint32_t enemies = 0;
    bool          faulted = false;
    if (!platform::CallGuarded(g_iterBegin, iterator, registryOwner + facts::kCharaRegistry, registryOwner, kCategoryBattle))
    {
        return 0;
    }
    for (;;)
    {
        std::int32_t index = 0, count = 0;
        std::memcpy(&index, iterator + kIteratorIndex, sizeof(index));
        std::memcpy(&count, iterator + kIteratorCount, sizeof(count));
        if (index < 0 || index >= count)
        {
            break;
        }
        std::uint8_t* heap = nullptr;
        std::memcpy(&heap, iterator + kIteratorHeap, sizeof(heap));
        const std::uint8_t* entries = heap != nullptr ? heap : iterator + kIteratorEntries;
        void*               character = nullptr;
        std::memcpy(&character, entries + kEntryBytes * static_cast<std::size_t>(index), sizeof(character));
        std::uint8_t* component  = Component(character);
        std::uint8_t* controller = nullptr;
        std::uint8_t  state = 0, select = 0;
        if (component != nullptr && Read(component + facts::kBattleCompActionState, state) && state != facts::kBattleCompStateKnockedOut &&
            Read(static_cast<std::uint8_t*>(character) + facts::kCharacterBattleAI, controller) && controller != nullptr &&
            Read(controller + facts::kBattleAITargetSelectType, select) && select == facts::kBattleAITargetSelectFollowsLeader)
        {
            if (platform::CallGuarded(g_setTarget, component, member))
            {
                ++enemies;
            }
            else
            {
                faulted = true;
            }
        }
        ++index;
        std::memcpy(iterator + kIteratorIndex, &index, sizeof(index));
        if (!platform::CallGuarded(g_iterNext, iterator))
        {
            faulted = true;
            break;
        }
    }
    std::uint8_t* heap = nullptr;
    std::memcpy(&heap, iterator + kIteratorHeap, sizeof(heap));
    if (heap != nullptr)
    {
        platform::CallGuarded(g_free, heap);
    }
    if (faulted)
    {
        platform::Log(L"enemy leader: the game's re-target walk RAISED AN EXCEPTION ({} enemies set before it)", enemies);
    }
    return enemies;
}

/// The standing human's member in place of `leader` when it is the game's downed leader, on the game thread.
void* Standing(void* leader, std::uint32_t& counter) noexcept
{
    if (g_enemySeat == 0 || g_enemyLeader == nullptr || leader != g_leader)
    {
        return leader;
    }
    if (!platform::OnGameThread())
    {
        ++g_offThread;
        if (!g_offThreadLogged)
        {
            g_offThreadLogged = true;
            platform::LogWarning(L"enemy leader: a leader read ran off the game thread; it keeps the game's answer");
        }
        return leader;
    }
    ++counter;
    return g_enemyLeader;
}

/// PartyManager_GetPartyLeader at the battle AI tick's read: the standing human's member while the game's leader is down.
void* LeaderForBattleAI(void* partyManager, std::uint8_t variant)
{
    return Standing(reinterpret_cast<GetPartyLeaderFn>(g_getLeader)(partyManager, variant), g_answers);
}

const wchar_t* Describe(int seat) noexcept
{
    return seat == kAllySeat ? L"the standing AI ally"
           : seat == 0       ? L"the game's leader"
           : seat == 1       ? L"player 2's member"
           : seat == 2       ? L"player 3's member"
                             : L"player 4's member";
}

/// The member the game's forced switch would take from the downed leader when no human's member stands: the next
/// deployed slot from the leader's, with wrap-around (the cycle's direction 1), that no human plays and passes the
/// cycle's own candidate test (an actor, a battle component not KO'd, not a battle-support guest; no
/// trap or sync-action test). Null if none. The cycle's tests after its pick (inside the battle area, party side) are
/// not repeated [ASSUMPTION: an ally in the party's battle is inside it]. Game thread.
void* AllyInstead(std::uint8_t& type) noexcept
{
    const std::uintptr_t manager = players::scopes::PartyManagerAddress();
    std::uint8_t         slots[3]{};
    std::uint8_t         leaderType = facts::kPlayerTypeNone;
    const std::uint8_t*  data = g_data != nullptr ? *g_data : nullptr;
    if (manager == 0 || data == nullptr || !Read(reinterpret_cast<const void*>(manager + facts::kPartySlotTypes), slots) ||
        !Read(reinterpret_cast<const void*>(manager + facts::kPartyLeaderType), leaderType))
    {
        return nullptr;
    }
    int from = -1;
    for (int i = 0; i < 3; ++i)
    {
        from = slots[i] == leaderType ? i : from;
    }
    if (from < 0)
    {
        return nullptr;
    }
    for (int step = 1; step < 3; ++step)
    {
        const std::uint8_t candidate = slots[(from + step) % 3];
        std::uint8_t       status    = 0;
        bool               human     = false;
        for (int seat = 1; seat < players::scopes::kMaxPlayers; ++seat)
        {
            human = human || players::scopes::MemberType(seat) == candidate;
        }
        if (candidate == facts::kPlayerTypeNone || candidate >= 8 || human ||
            !Read(data + facts::kDataPlayerStatus + candidate, status) || status == facts::kStatusBattleSupport)
        {
            continue;
        }
        void* actor = nullptr;
        if (platform::CallGuarded(g_getActor, actor, reinterpret_cast<void*>(manager), std::uint8_t{0}, candidate, false) && actor != nullptr &&
            !Downed(actor))
        {
            type = candidate;
            return actor;
        }
    }
    return nullptr;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto getLeader = platform::FindFunction(image, facts::kGetPartyLeader);
    const auto site      = image.Rebase(facts::kBattleAITickLeaderCall, 5);
    const auto iterBegin = platform::FindFunction(image, facts::kCharaRegistryIterBegin);
    const auto iterNext  = platform::FindFunction(image, facts::kCharaRegistryIterNext);
    const auto setTarget = platform::FindFunction(image, facts::kBattleCompSetTargetChar);
    const auto free      = platform::FindFunction(image, facts::kGameFree);
    const auto getActor  = platform::FindFunction(image, facts::kGetPlayerActor);
    const auto data      = players::game_globals::Address(players::game_globals::Global::DataSingleton);
    if (!players::field_root::Installed() || !getLeader || !site || !iterBegin || !iterNext || !setTarget || !free || !getActor || !data)
    {
        platform::Log(L"enemy leader: an engine check failed; enemies keep following a downed leader");
        return false;
    }
    g_getLeader = *getLeader;
    g_iterBegin = reinterpret_cast<IterBeginFn>(*iterBegin);
    g_iterNext  = reinterpret_cast<IterNextFn>(*iterNext);
    g_setTarget = reinterpret_cast<SetTargetFn>(*setTarget);
    g_free      = reinterpret_cast<FreeFn>(*free);
    g_getActor  = reinterpret_cast<GetPlayerActorFn>(*getActor);
    g_data      = reinterpret_cast<std::uint8_t**>(data);
    if (!g_leaderCall.Install(*site, g_getLeader, reinterpret_cast<std::uintptr_t>(&LeaderForBattleAI)))
    {
        platform::Log(L"enemy leader: the battle AI tick's leader read could not be redirected; enemies are re-targeted but get no leader-change event");
    }
    control::Register({.name = L"enemy-leader", .shape = control::Shape::Text,
                       .help = L"whom enemies follow (the game's leader, or while it is down the standing human's member, else in co-op the standing AI ally); "
                               L"re-targets, leader-read answers",
                       .get  = [] {
                           return control::Value{std::format(L"enemies follow {}; re-targets {} (last {} enemies); leader-read answers {}, off-thread {}",
                                                             Describe(g_enemySeat), g_retargets, g_lastEnemies, g_answers, g_offThread)};
                       }});
    platform::Log(L"enemy leader: installed; enemies turn on the standing human's member when the game's leader is down (in co-op, else the "
                  L"standing AI ally)");
    return true;
}

void Remove() noexcept
{
    g_leaderCall.Restore();
}

void Tick() noexcept
{
    if (g_iterBegin == nullptr || players::field_root::BattleSystem() == nullptr)  // not installed, or no battle
    {
        g_leader = g_enemyLeader = nullptr;
        g_enemySeat = 0;
        g_instead.store(nullptr, std::memory_order_relaxed);
        return;
    }
    void* leader = GameLeader();
    if (leader == nullptr)
    {
        g_leader = g_enemyLeader = nullptr;
        g_enemySeat = 0;
        g_instead.store(nullptr, std::memory_order_relaxed);
        return;
    }
    // The enemy-facing leader: the game's while it stands, else the first other human's standing member, else (in
    // co-op, where the game's forced switch is refused) the standing AI ally that switch would have taken.
    void*        enemyLeader = leader;
    int          seat        = 0;
    std::uint8_t type        = facts::kPlayerTypeNone;
    if (Downed(leader))
    {
        for (int candidate = 1; candidate < players::scopes::kMaxPlayers; ++candidate)
        {
            void* member = players::scopes::Member(candidate);
            if (member != nullptr && member != leader && !Downed(member))
            {
                enemyLeader = member;
                seat        = candidate;
                type        = players::scopes::MemberType(candidate);
                break;
            }
        }
        if (seat == 0 && players::AnyFurtherMember())
        {
            if (void* ally = AllyInstead(type); ally != nullptr)
            {
                enemyLeader = ally;
                seat        = kAllySeat;
            }
        }
    }
    const bool leaderSwitched = leader != g_leader && g_leader != nullptr;
    if (enemyLeader != g_enemyLeader && g_enemyLeader != nullptr)
    {
        // A change away from the game's leader: the game's cycle found nobody, the tail runs here. A change back to
        // it with no switch (the member went down, or the leader revived): the same tail, to a standing leader only.
        const bool run = seat != 0 || (!leaderSwitched && !Downed(enemyLeader));
        if (run)
        {
            g_lastEnemies = Retarget(enemyLeader);
            ++g_retargets;
            platform::LogDebug(L"enemy leader: enemies now follow {} ({} follow-the-leader enemies re-targeted{})", Describe(seat),
                               g_lastEnemies, seat != 0 ? L"; the game's leader is down" : L"");
        }
        else
        {
            platform::LogDebug(L"enemy leader: enemies follow {} again (the game's own switch re-targeted them)", Describe(seat));
        }
    }
    g_leader      = leader;
    g_enemyLeader = enemyLeader;
    g_enemySeat   = seat;
    void* const        instead     = seat != 0 ? enemyLeader : nullptr;
    const std::uint8_t insteadType = seat != 0 ? type : facts::kPlayerTypeNone;
    if (instead != g_instead.load(std::memory_order_relaxed) || insteadType != g_insteadType.load(std::memory_order_relaxed))
    {
        g_instead.store(nullptr, std::memory_order_release);  // the pair changes: a reader between the stores reads again
        g_insteadType.store(insteadType, std::memory_order_release);
        g_instead.store(instead, std::memory_order_release);
    }
}

void* Instead(std::uint8_t* type) noexcept
{
    // The member, its type, then the member again: a pair Tick changed in between is read again, so the type always
    // belongs to the member returned.
    for (;;)
    {
        void* const        instead = g_instead.load(std::memory_order_acquire);
        const std::uint8_t kind    = g_insteadType.load(std::memory_order_acquire);
        if (g_instead.load(std::memory_order_acquire) == instead)
        {
            if (type != nullptr)
            {
                *type = kind;
            }
            return instead;
        }
    }
}

}  // namespace lss::battle::enemy_leader
