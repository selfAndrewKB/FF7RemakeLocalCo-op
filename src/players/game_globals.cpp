#include "players/game_globals.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lss::players::game_globals {

namespace {

enum class Holds : std::uint8_t
{
    Pointer,  // the global keeps a pointer to the object
    Object,   // the global is the object
};

struct Entry
{
    Global         global;
    const wchar_t* name;
    std::uintptr_t ida;
    std::size_t    bytes;  // checked inside the image at the rebase
    Holds          holds;
};

constexpr std::size_t kUiSlots = 64;  // EndUISystem's slot array, as far as the modules read it
// The two object globals, as far as the modules read them.
constexpr std::size_t kTargetsBytes    = std::max({std::size_t{facts::kSceneTargetsUiHeight} + sizeof(std::uint32_t),
                                                   std::size_t{facts::kSceneTargetsInGameUI} + sizeof(void*),
                                                   std::size_t{facts::kSceneTargetsCompositeHdr} + sizeof(void*)});
constexpr std::size_t kResolutionBytes = std::max(3 * sizeof(std::int32_t),
                                                  std::size_t{facts::kSystemResolutionWindowMode} + sizeof(std::int32_t));

constexpr std::array<Entry, static_cast<std::size_t>(Global::Count)> kEntries{{
    {Global::Engine, L"GEngine", facts::kGEngine, sizeof(void*), Holds::Pointer},
    {Global::GameMode, L"the game mode", facts::kGameModeGlobal, sizeof(void*), Holds::Pointer},
    {Global::PartyManager, L"the party manager", facts::kPartyManagerGlobal, sizeof(void*), Holds::Pointer},
    {Global::DataSingleton, L"the data singleton", facts::kDataSingletonGlobal, sizeof(void*), Holds::Pointer},
    {Global::SquareInputOwner, L"the SquareInput owner", facts::kSquareInputOwnerGlobal, sizeof(void*), Holds::Pointer},
    {Global::EnvSetManager, L"the environment set manager", facts::kEnvSetManagerGlobal, sizeof(void*), Holds::Pointer},
    {Global::RootOverlay, L"the root overlay", facts::kRootOverlayGlobal, sizeof(void*), Holds::Pointer},
    {Global::DynamicRhi, L"GDynamicRHI", facts::kGDynamicRHI, sizeof(void*), Holds::Pointer},
    {Global::Malloc, L"GMalloc", facts::kGMallocGlobal, sizeof(void*), Holds::Pointer},
    {Global::SceneRenderTargets, L"the scene render targets", facts::kSceneRenderTargetsGlobal, kTargetsBytes, Holds::Object},
    {Global::RhiCommandListImmediate, L"the immediate RHI command list", facts::kGRHICommandListImmediate, sizeof(void*),
     Holds::Object},
    {Global::SystemResolution, L"the system resolution", facts::kSystemResolution, kResolutionBytes, Holds::Object},
    {Global::NameNoneIndex, L"NAME_None's index", facts::kNameNoneIndex, sizeof(std::uint32_t), Holds::Object},
    {Global::OperatorFrameStamp, L"the operator frame stamp", facts::kOperatorFrameStamp, sizeof(std::uint64_t), Holds::Object},
    {Global::TableAction, L"the action tables", facts::kTableAction, 3 * sizeof(void*), Holds::Object},
    {Global::UiSystemSlots, L"EndUISystem's slots", facts::kEndUISystemSlots, kUiSlots * sizeof(void*), Holds::Object},
}};

constexpr bool InEnumOrder() noexcept
{
    for (std::size_t i = 0; i < kEntries.size(); ++i)
    {
        if (static_cast<std::size_t>(kEntries[i].global) != i)
        {
            return false;
        }
    }
    return true;
}
static_assert(InEnumOrder(), "one entry per global, in the enum's order");

// Set once at install, then only read.
std::array<std::uintptr_t, kEntries.size()> g_addresses{};

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    std::wstring missing;
    for (std::size_t i = 0; i < kEntries.size(); ++i)
    {
        g_addresses[i] = image.Rebase(kEntries[i].ida, kEntries[i].bytes).value_or(0);
        if (g_addresses[i] == 0)
        {
            try
            {
                missing += missing.empty() ? kEntries[i].name : std::wstring(L", ") + kEntries[i].name;
            }
            catch (...)
            {
            }
        }
    }
    if (!missing.empty())
    {
        platform::Log(L"game globals: outside the image, not as the research read them: {}; the modules that need them stand aside",
                      missing);
        return false;
    }
    return true;
}

std::uintptr_t Address(Global global) noexcept
{
    const auto index = static_cast<std::size_t>(global);
    return index < g_addresses.size() ? g_addresses[index] : 0;
}

void* Object(Global global) noexcept
{
    const std::uintptr_t address = Address(global);
    if (address == 0)
    {
        return nullptr;
    }
    if (kEntries[static_cast<std::size_t>(global)].holds == Holds::Object)
    {
        return reinterpret_cast<void*>(address);
    }
    return platform::ReadPointer(reinterpret_cast<const void*>(address), 0);
}

std::uintptr_t UiSlotAddress(std::uint32_t slot) noexcept
{
    const std::uintptr_t slots = Address(Global::UiSystemSlots);
    return slots != 0 && slot < kUiSlots ? slots + slot * sizeof(void*) : 0;
}

void* UiSlot(std::uint32_t slot) noexcept
{
    const std::uintptr_t address = UiSlotAddress(slot);
    return address != 0 ? platform::ReadPointer(reinterpret_cast<const void*>(address), 0) : nullptr;
}

void* GameViewportClient() noexcept
{
    return platform::ReadPointer(Object(Global::Engine), facts::kEngineGameViewport);
}

}  // namespace lss::players::game_globals
