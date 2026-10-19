#include "battle/enemy_leader.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/player_two_camera.h"
#include "players/scopes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <format>

namespace lss::battle::enemy_leader {

namespace {

using GetPartyLeaderFn = void* (*)(void* partyManager, std::uint8_t variant);
using IterBeginFn      = void (*)(void* iterator, void* registry, void* owner, std::uint8_t categoryBit);
using IterNextFn       = void (*)(void* iterator);
using SetTargetFn      = void (*)(void* component, void* character);
using FreeFn           = void (*)(void* memory);

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
std::uint8_t**         g_root        = nullptr;
std::uintptr_t         g_getLeader   = 0;
IterBeginFn            g_iterBegin   = nullptr;
IterNextFn             g_iterNext    = nullptr;
SetTargetFn            g_setTarget   = nullptr;
FreeFn                 g_free        = nullptr;
void**                 g_system      = nullptr;
platform::CallRedirect g_leaderCall;

// Game thread.
DWORD         g_thread       = 0;
void*         g_leader       = nullptr;  // the game's leader last tick
void*         g_enemyLeader  = nullptr;  // the enemy-facing leader last tick (null outside a battle)
int           g_enemySeat    = 0;        // 0: the game's leader; else the seat whose member enemies follow
std::uint32_t g_retargets    = 0;
std::uint32_t g_lastEnemies  = 0;
std::uint32_t g_answers      = 0;        // leader reads answered with the standing human's member
std::uint32_t g_offThread    = 0;
bool          g_offThreadLogged = false;

template <class T>
bool Read(const void* address, T& out) noexcept
{
    return address != nullptr && platform::CopyGuarded(&out, address, sizeof(T));
}

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
    std::uint8_t state = 0;
    const auto*  component = Component(character);
    return component == nullptr || !Read(component + facts::kBattleCompActionState, state) || state == facts::kBattleCompStateKnockedOut;
}

void* GameLeader() noexcept
{
    const std::uintptr_t manager = players::scopes::PartyManagerAddress();
    std::int32_t         pair[2]{};
    return manager != 0 && Read(reinterpret_cast<const void*>(manager + facts::kPartyLeaderWeak), pair)
               ? players::player_two_camera::ObjectOf(pair)
               : nullptr;
}

/// The game's re-target tail of a forced leader change, with `member` as the new leader. The enemies re-targeted.
std::uint32_t Retarget(void* member) noexcept
{
    std::uint8_t* registryOwner = nullptr;
    if (g_root == nullptr || !Read(g_root, registryOwner) || registryOwner == nullptr ||
        !Read(registryOwner + facts::kEndGameRootCharaOwner, registryOwner) || registryOwner == nullptr)
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

/// PartyManager_GetPartyLeader at the battle AI tick's read: the standing human's member while the game's leader is down.
void* LeaderForBattleAI(void* partyManager, std::uint8_t variant)
{
    void* leader = reinterpret_cast<GetPartyLeaderFn>(g_leaderCall.Original())(partyManager, variant);
    if (g_enemySeat == 0 || g_enemyLeader == nullptr || leader != g_leader)
    {
        return leader;
    }
    if (GetCurrentThreadId() != g_thread)
    {
        ++g_offThread;
        if (!g_offThreadLogged)
        {
            g_offThreadLogged = true;
            platform::LogWarning(L"enemy leader: a battle AI's leader read ran off the game thread; it keeps the game's answer");
        }
        return leader;
    }
    ++g_answers;
    return g_enemyLeader;
}

const wchar_t* Describe(int seat) noexcept
{
    return seat == 0 ? L"the game's leader" : seat == 1 ? L"player 2's member" : seat == 2 ? L"player 3's member" : L"player 4's member";
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto root      = image.Rebase(facts::kEndGameRootGlobal);
    const auto system    = image.Rebase(facts::kBattleSystemGlobal);
    const auto getLeader = platform::FindFunction(image, facts::kGetPartyLeader);
    const auto site      = image.Rebase(facts::kBattleAITickLeaderCall, 5);
    const auto iterBegin = platform::FindFunction(image, facts::kCharaRegistryIterBegin);
    const auto iterNext  = platform::FindFunction(image, facts::kCharaRegistryIterNext);
    const auto setTarget = platform::FindFunction(image, facts::kBattleCompSetTargetChar);
    const auto free      = platform::FindFunction(image, facts::kGameFree);
    if (!root || !system || !getLeader || !site || !iterBegin || !iterNext || !setTarget || !free)
    {
        platform::Log(L"enemy leader: an engine check failed; enemies keep following a downed leader");
        return false;
    }
    g_root      = reinterpret_cast<std::uint8_t**>(*root);
    g_system    = reinterpret_cast<void**>(*system);
    g_getLeader = *getLeader;
    g_iterBegin = reinterpret_cast<IterBeginFn>(*iterBegin);
    g_iterNext  = reinterpret_cast<IterNextFn>(*iterNext);
    g_setTarget = reinterpret_cast<SetTargetFn>(*setTarget);
    g_free      = reinterpret_cast<FreeFn>(*free);
    if (!g_leaderCall.Install(*site, g_getLeader, reinterpret_cast<std::uintptr_t>(&LeaderForBattleAI)))
    {
        platform::Log(L"enemy leader: the battle AI tick's leader read could not be redirected; enemies are re-targeted but get no leader-change event");
    }
    control::Register({.name = L"enemy-leader", .shape = control::Shape::Text,
                       .help = L"whom enemies follow (the game's leader, or the standing human's member while it is down); re-targets, leader-read answers",
                       .get  = [] {
                           return control::Value{std::format(L"enemies follow {}; re-targets {} (last {} enemies); leader-read answers {}, off-thread {}",
                                                             Describe(g_enemySeat), g_retargets, g_lastEnemies, g_answers, g_offThread)};
                       }});
    platform::Log(L"enemy leader: installed; enemies turn on the standing human's member when the game's leader is down");
    return true;
}

void Remove() noexcept
{
    g_leaderCall.Restore();
}

void Tick() noexcept
{
    g_thread = GetCurrentThreadId();
    void* system = nullptr;
    if (g_system == nullptr || !Read(g_system, system) || system == nullptr)
    {
        g_leader = g_enemyLeader = nullptr;
        g_enemySeat = 0;
        return;
    }
    void* leader = GameLeader();
    if (leader == nullptr)
    {
        g_leader = g_enemyLeader = nullptr;
        g_enemySeat = 0;
        return;
    }
    // The enemy-facing leader: the game's while it stands, else the first other human's standing member.
    void* enemyLeader = leader;
    int   seat        = 0;
    if (Downed(leader))
    {
        for (int candidate = 1; candidate < players::scopes::kMaxPlayers; ++candidate)
        {
            void* member = players::scopes::Member(candidate);
            if (member != nullptr && member != leader && !Downed(member))
            {
                enemyLeader = member;
                seat        = candidate;
                break;
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
}

}  // namespace lss::battle::enemy_leader
