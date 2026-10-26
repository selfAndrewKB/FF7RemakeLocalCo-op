#include "players/field_root.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

namespace lss::players::field_root {

namespace {

using platform::Read;
using platform::ReadPointer;

using GetPartyLeaderFn = void* (*)(void* partyManager, std::uint8_t selector);

// Set once at install (the game's own globals, live), then only read.
std::uint8_t* const* g_root         = nullptr;  // the global holding the field root pointer
void* const*         g_battleSystem = nullptr;  // the global holding the battle system pointer
void* const*         g_partyManager = nullptr;  // the global holding the party manager pointer
GetPartyLeaderFn     g_getLeader    = nullptr;
bool                 g_installed    = false;

void* PartyManager() noexcept
{
    void* manager = nullptr;
    return g_installed && Read(g_partyManager, manager) ? manager : nullptr;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto root   = image.Rebase(facts::kEndGameRootGlobal, sizeof(void*));
    const auto system = image.Rebase(facts::kBattleSystemGlobal, sizeof(void*));
    const auto party  = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    const auto leader = platform::FindFunction(image, facts::kGetPartyLeader);
    if (!root || !system || !party || !leader)
    {
        platform::Log(L"field root: a global or the party leader getter is not as the research read it; the modules that need "
                      L"the field root, the battle system or the party leader stand aside");
        return false;
    }
    g_root         = reinterpret_cast<std::uint8_t* const*>(*root);
    g_battleSystem = reinterpret_cast<void* const*>(*system);
    g_partyManager = reinterpret_cast<void* const*>(*party);
    g_getLeader    = reinterpret_cast<GetPartyLeaderFn>(*leader);
    g_installed    = true;
    return true;
}

bool Installed() noexcept
{
    return g_installed;
}

std::uint8_t* Root() noexcept
{
    std::uint8_t* root = nullptr;
    return g_installed && Read(g_root, root) ? root : nullptr;
}

std::uint8_t* StateManager() noexcept
{
    return static_cast<std::uint8_t*>(ReadPointer(Root(), facts::kRootStateManager));
}

bool ControlLock(std::uint32_t& mask, std::uint32_t& counter) noexcept
{
    const std::uint8_t* manager = StateManager();
    std::uint32_t       m = 0, c = 0;
    if (manager == nullptr || !Read(manager + facts::kFieldControlLockMask, m) || !Read(manager + facts::kFieldControlLockCounter, c))
    {
        return false;
    }
    mask    = m;
    counter = c;
    return true;
}

std::uint32_t ControlLockMask() noexcept
{
    const std::uint8_t* manager = StateManager();
    std::uint32_t       mask    = 0;
    return manager != nullptr && Read(manager + facts::kFieldControlLockMask, mask) ? mask : 0;
}

std::uint8_t* CharaOwner() noexcept
{
    return static_cast<std::uint8_t*>(ReadPointer(Root(), facts::kEndGameRootCharaOwner));
}

bool InBattle() noexcept
{
    const std::uint8_t* owner = CharaOwner();
    std::uint8_t        state = 0;
    return owner != nullptr && Read(owner + facts::kRootFieldBattleState, state) && state == facts::kFieldBattleStateBattle;
}

void* BattleSystem() noexcept
{
    void* system = nullptr;
    return g_installed && Read(g_battleSystem, system) ? system : nullptr;
}

void* PartyLeader(std::uint8_t selector) noexcept
{
    void* const manager = PartyManager();
    void*       leader  = nullptr;
    return manager != nullptr && g_getLeader != nullptr && platform::CallGuarded(g_getLeader, leader, manager, selector) ? leader
                                                                                                                         : nullptr;
}

}  // namespace lss::players::field_root
