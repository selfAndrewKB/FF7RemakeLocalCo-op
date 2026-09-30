// LocalSplitScreen for FINAL FANTASY VII REMAKE: the UE4SS entry point.
//
// Verifies every engine fact it relies on, installs, and turns the player-intent keys into joins and leaves
// on the game thread (the engine tick's pre-callback), so they never land inside a frame's drawing.
//   Ctrl+F7  player 2 joins with a controller and camera manager of its own
//   Ctrl+F8  player 2 leaves
//   Ctrl+F9  PROBE: player 2 takes Aerith through the game's leader funnel, or gives her back
//   Ctrl+F6  log every local player's controller, camera manager and camera actor

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include "platform/image.h"
#include "platform/log.h"
#include "players/own_controller.h"
#include "players/session.h"

#include <optional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#define LOCALSPLITSCREEN_API __declspec(dllexport)

namespace {

constexpr const wchar_t* kVersion = L"0.3.0-seat-probe";

using lss::platform::Log;
namespace Hook = RC::Unreal::Hook;

bool GameHasFocus() noexcept
{
    DWORD owner = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &owner);
    return owner == GetCurrentProcessId();
}

/// True on the tick a Ctrl+key goes down; `wasDown` carries the state between ticks.
bool Pressed(int virtualKey, bool& wasDown) noexcept
{
    const bool down    = GameHasFocus() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
                         (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown            = down;
    return pressed;
}

class LocalSplitScreenMod final : public RC::CppUserModBase
{
public:
    LocalSplitScreenMod()
    {
        ModName        = STR("LocalSplitScreen");
        ModVersion     = STR("0.3.0-seat-probe");
        ModDescription = STR("Two-player split-screen for FF7 Remake");
        ModAuthors     = STR("Andrew");
    }

    ~LocalSplitScreenMod() override
    {
        if (tickCallback_ != Hook::ERROR_ID)
        {
            Hook::UnregisterCallback(tickCallback_);
        }
        // With player 2 in, its camera manager was built through the replacement; the DLL is pinned, so the
        // replacement stays valid. Otherwise the engine's function goes back.
        if (installed_ && !session_.HasPlayerTwo())
        {
            lss::players::own_controller::Remove();
        }
    }

    auto on_unreal_init() -> void override
    {
        Log(L"v{} starting", kVersion);
        const auto image = lss::platform::GameImage::Locate();
        if (!image || !session_.Install(*image))
        {
            Log(L"standing aside: an engine check failed (see above); nothing installed, the game runs unmodified");
            return;
        }
        installed_ = true;
        const Hook::FCallbackOptions options{false, false, STR("LocalSplitScreen"), STR("player intent")};
        tickCallback_ = Hook::RegisterEngineTickPreCallback(
            [this](Hook::TCallbackIterationData<void>&, RC::Unreal::UEngine*, float, bool) { OnEngineTick(); }, options);
        Log(L"ready: every engine check passed. Ctrl+F7 adds player 2, Ctrl+F8 removes player 2, Ctrl+F9 seats Aerith on player 2 (probe), Ctrl+F6 reports");
    }

private:
    void OnEngineTick() noexcept
    {
        if (Pressed(VK_F7, joinWasDown_))
        {
            session_.Join();
        }
        if (Pressed(VK_F8, leaveWasDown_))
        {
            session_.Leave();
        }
        if (Pressed(VK_F9, seatWasDown_))
        {
            session_.ToggleSeat();
        }
        if (Pressed(VK_F6, reportWasDown_))
        {
            session_.Report();
        }
    }

    lss::players::Session session_;
    bool                  installed_     = false;
    bool                  joinWasDown_   = false;
    bool                  leaveWasDown_  = false;
    bool                  reportWasDown_ = false;
    bool                  seatWasDown_   = false;
    Hook::GlobalCallbackId tickCallback_ = Hook::ERROR_ID;
};

}  // namespace

extern "C"
{
    LOCALSPLITSCREEN_API RC::CppUserModBase* start_mod() { return new LocalSplitScreenMod(); }
    LOCALSPLITSCREEN_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
