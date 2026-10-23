#include "players/own_controller.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"

#include <cstdint>
#include <cstring>
#include <span>

namespace lss::players::own_controller {

namespace {

using platform::ReadPointer;

using BeginPlayFn = void (*)(void* cameraManager);

/// Raised inside the spawn when player 2's camera manager cannot be made safe; the join's structured-
/// exception guard around CreatePlayer catches it and reports the join as failed.
constexpr DWORD kJoinRefused = 0xE0535032;  // an application code ('SP2')

// Set once at install, then only read.
platform::VTableEntry g_slot;
BeginPlayFn           g_engineBeginPlay        = nullptr;
char*                 g_cameraActorName        = nullptr;
std::uintptr_t        g_playerControllerVTable = 0;
std::uintptr_t        g_cameraManagerVTable    = 0;

// Game thread only: set by JoinScope for the length of CreatePlayer, and the spawn (with the camera
// manager's BeginPlay) runs synchronously inside that call. BeginPlay of other camera managers reads it
// only to see that no join is under way.
void*  g_joiningPlayerOne = nullptr;
int    g_joiningId        = 0;  // the joining player's controller id: its camera actor's name
Report g_report{};

std::uintptr_t VTableOf(const void* object) noexcept
{
    std::uintptr_t vtable = 0;
    return object != nullptr && platform::CopyGuarded(&vtable, object, sizeof(vtable)) ? vtable : 0;
}

/// Makes sure PCOwner is set, filling it from Owner when it is still null. The owner must be an End
/// player controller that has no other camera manager (the engine publishes this one to it right after
/// the spawn). False if PCOwner stays null.
bool FillOwner(void* cameraManager) noexcept
{
    if (ReadPointer(cameraManager, facts::kCameraManagerPcOwner) != nullptr)
    {
        return true;
    }
    void*       owner         = ReadPointer(cameraManager, facts::kActorOwner);
    const void* ownersManager = ReadPointer(owner, facts::kControllerCameraManager);
    if (VTableOf(owner) != g_playerControllerVTable || (ownersManager != nullptr && ownersManager != cameraManager))
    {
        g_report.ownerRefused = true;
        return false;
    }
    if (!platform::WriteGuarded(static_cast<std::uint8_t*>(cameraManager) + facts::kCameraManagerPcOwner, &owner,
                                sizeof(owner)))
    {
        g_report.ownerRefused = true;
        return false;
    }
    g_report.ownerFilled = true;
    return true;
}

/// Writes the camera actor's name text. The text lives in the image's read-only data, so the page is made
/// writable for the store and put back at once (if putting the protection back fails, the page just stays
/// writable, which is harmless to the game).
bool WriteName(const std::array<char, 14>& text) noexcept
{
    DWORD previous = 0;
    if (!VirtualProtect(g_cameraActorName, text.size(), PAGE_READWRITE, &previous))
    {
        return false;
    }
    std::memcpy(g_cameraActorName, text.data(), text.size());
    DWORD ignored = 0;
    VirtualProtect(g_cameraActorName, text.size(), previous, &ignored);
    return true;
}

/// Runs the engine's BeginPlay with player 2's camera-actor name. The name is put back on every way out,
/// including a structured exception raised inside the engine (the join's guard further up catches it and
/// the game carries on, so every later camera manager must find the name intact). This function holds no
/// objects with destructors, so the __try/__finally never has to interleave with C++ unwinding.
void BeginPlayWithOwnName(void* cameraManager)  // not noexcept: the exception must pass through
{
    __try
    {
        g_engineBeginPlay(cameraManager);
    }
    __finally
    {
        WriteName(facts::kEndCameraActorText);
    }
}

/// Stops the join from inside the spawn: the engine's BeginPlay would read a null PCOwner, or would
/// replace player 1's camera actor. The exception travels to the join's guard, which reports the failure.
[[noreturn]] void RefuseJoin(const wchar_t* why)  // not noexcept: the exception must pass through
{
    platform::Log(L"join: player 2's camera manager cannot begin play safely ({}); the join is stopped", why);
    RaiseException(kJoinRefused, 0, 0, nullptr);
    __assume(false);
}

/// Same signature as AEndPlayerCameraManager::BeginPlay.
void BeginPlay(void* cameraManager)
{
    void* playerOne = g_joiningPlayerOne;
    void* owner     = ReadPointer(cameraManager, facts::kActorOwner);
    if (playerOne == nullptr || owner == nullptr || owner == playerOne)
    {
        g_engineBeginPlay(cameraManager);
        return;
    }

    g_report               = {};
    g_report.cameraManager = cameraManager;
    if (!FillOwner(cameraManager))
    {
        RefuseJoin(L"its owner is not a fitting End player controller");
    }
    if (!WriteName(facts::FurtherCameraActorText(g_joiningId)))
    {
        RefuseJoin(L"the camera actor's name could not be changed");
    }
    g_report.renamed = true;
    BeginPlayWithOwnName(cameraManager);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto text = std::span(reinterpret_cast<const std::uint8_t*>(facts::kEndCameraActorText.data()),
                                facts::kEndCameraActorText.size());
    if (!image.BytesMatch(facts::kEndCameraActorName, text))
    {
        platform::Log(L"check failed: the camera actor's name text is not \"EndCameraActor\"");
        return false;
    }
    const auto name       = image.Rebase(facts::kEndCameraActorName, facts::kEndCameraActorText.size());
    const auto engineFn   = platform::FindFunction(image, facts::kCameraManagerBeginPlay.target);
    const auto controller = image.Rebase(facts::kEndPlayerControllerVTable);
    const auto camera     = image.Rebase(facts::kEndCameraManagerVTable);
    if (!name || !engineFn || !controller || !camera)
    {
        return false;
    }
    g_cameraActorName        = reinterpret_cast<char*>(*name);
    g_playerControllerVTable = *controller;
    g_cameraManagerVTable    = *camera;
    g_engineBeginPlay        = reinterpret_cast<BeginPlayFn>(*engineFn);
    return platform::ReplaceSlot(image, facts::kCameraManagerBeginPlay, reinterpret_cast<std::uintptr_t>(&BeginPlay),
                                 g_slot);
}

void Remove() noexcept
{
    if (!g_slot.Restore())
    {
        platform::Log(L"could not restore the camera manager's BeginPlay slot; the replacement stays in place");
    }
}

bool CanRename() noexcept
{
    return g_cameraActorName != nullptr && WriteName(facts::kEndCameraActorText);
}

bool UsesNativeClasses(const void* playerOneController) noexcept
{
    return playerOneController != nullptr && VTableOf(playerOneController) == g_playerControllerVTable &&
           VTableOf(ReadPointer(playerOneController, facts::kControllerCameraManager)) == g_cameraManagerVTable;
}

JoinScope::JoinScope(void* playerOneController, int controllerId) noexcept
{
    g_report           = {};
    g_joiningPlayerOne = playerOneController;
    g_joiningId        = controllerId;
}

JoinScope::~JoinScope()
{
    g_joiningPlayerOne = nullptr;
    g_joiningId        = 0;
}

Report JoinScope::Result() const noexcept
{
    return g_report;
}

}  // namespace lss::players::own_controller
