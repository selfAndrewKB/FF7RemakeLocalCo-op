// LocalSplitScreen for FINAL FANTASY VII REMAKE: the UE4SS entry point.
//
// Verifies every engine fact it relies on, installs, and turns the player-intent keys into joins and leaves
// on the game thread (the engine tick's pre-callback), so they never land inside a frame's drawing. A retry, load,
// jump or return to the title takes player 2 out first and brings it back after (players/field_exit.h). While the screen
// is split the HUD is laid out for each half's shape (hud/overlay_layout.h, hud/root_placement.h).
//   Ctrl+F7  player 2 joins with a controller and camera manager of its own
//   Ctrl+F8  player 2 leaves
//   Ctrl+F9  PROBE: player 2 takes Aerith through the game's leader funnel, or gives her back
//   Ctrl+F6  log every local player's controller, camera manager and camera actor
// The same intents, and every module's switches, are commands on the console (control/remote.h), driven from
// the host by tools/game.py.

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include "battle/command_menu_tick.h"
#include "battle/command_menu_two.h"
#include "battle/lock_camera_two.h"
#include "battle/member_rates.h"
#include "battle/party_switch.h"
#include "battle/targeting_two.h"
#include "control/registry.h"
#include "control/remote.h"
#include "hud/hud_probe.h"
#include "hud/overlay_layout.h"
#include "hud/root_placement.h"
#include "hud/image_entries.h"
#include "hud/marker_replay.h"
#include "hud/minimap_two.h"
#include "hud/second_image.h"
#include "hud/ui_targets.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/own_controller.h"
#include "players/pad_buttons.h"
#include "players/scopes.h"
#include "players/player_two_camera.h"
#include "players/game_state.h"
#include "players/session.h"
#include "players/split_render.h"
#include "engine_facts.h"
#include "render/ao_origin.h"
#include "render/bloom_origin.h"
#include "render/distortion_apply.h"

#include <cstdint>
#include <optional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#define LOCALSPLITSCREEN_API __declspec(dllexport)

namespace {

constexpr const wchar_t* kVersion = L"0.16.0-lock-camera-leader";  // the one version string (the log and UE4SS)

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

using lss::control::Outcome;
using lss::control::Shape;
using lss::control::Value;

/// The input holds the console can make: at the mod's own per-pad stick layer, so a scripted stick takes the
/// path a real pad takes.
class ModHolds final : public lss::control::Holds
{
public:
    explicit ModHolds(lss::players::Session& session) noexcept : session_(session) {}

    Outcome HoldStick(std::int32_t player, const float (&axes)[4]) override
    {
        if (!session_.HasPlayerTwo())
        {
            return {false, L"stick holds act only while player 2 is in"};
        }
        lss::players::player_two_camera::HoldStick(static_cast<unsigned>(player - 1), axes);
        return {};
    }
    void ReleaseStick(std::int32_t player) override
    {
        lss::players::player_two_camera::ReleaseStick(static_cast<unsigned>(player - 1));
    }

private:
    lss::players::Session& session_;
};

/// The session's commands and readings on the console. The render modules register their own switches.
void RegisterSessionCommands(lss::players::Session& session)
{
    using lss::control::Register;
    Register({.name = L"join", .help = L"player 2 joins with a controller and camera manager of its own (Ctrl+F7)",
              .set  = [&session](const Value&) {
                  session.Join();
                  return session.HasPlayerTwo() ? Outcome{} : Outcome{false, L"player 2 did not join (see the log)"};
              }});
    Register({.name = L"leave", .help = L"player 2 leaves (Ctrl+F8)", .set = [&session](const Value&) {
                  session.Leave();
                  return session.HasPlayerTwo() ? Outcome{false, L"player 2 is still in (see the log)"} : Outcome{};
              }});
    Register({.name  = L"seat",
              .shape = Shape::OnOff,
              .help  = L"PROBE: player 2 holds Aerith through the game's leader funnel, or gives her back (Ctrl+F9)",
              .set   = [&session](const Value& value) {
                  const bool seated = std::get<bool>(value);
                  if (!session.HasPlayerTwo())
                  {
                      return Outcome{false, L"player 2 is not in"};
                  }
                  if (session.Seated() != seated)
                  {
                      session.ToggleSeat();
                  }
                  return session.Seated() == seated ? Outcome{} : Outcome{false, L"the seat did not change (see the log)"};
              },
              .get = [&session] { return Value{session.Seated()}; }});
    Register({.name = L"p2.in", .shape = Shape::OnOff, .help = L"player 2 is in",
              .get = [&session] { return Value{session.HasPlayerTwo()}; }});
    Register({.name = L"p2.rejoin", .shape = Shape::OnOff,
              .help = L"player 2 was taken out by a world change and joins again once the field is back ('leave' cancels)",
              .get  = [&session] { return Value{session.RejoinPending()}; }});
    Register({.name  = L"p2.state",
              .shape = Shape::Word,
              .words = {L"out", L"in", L"playing"},
              .help  = L"out, in (no member), or playing (holds a member)",
              .get   = [&session] {
                  return Value{std::wstring(!session.HasPlayerTwo() ? L"out" : session.Seated() ? L"playing" : L"in")};
              }});
    Register({.name  = L"showflags",
              .shape = Shape::Hex,
              .max   = 0xFFFFFFFF,
              .help  = L"the show flags forced on in both halves while player 2 is in (0 = the engine's split rule; "
                       L"players/split_render.h)",
              .set   = [&session](const Value& value) {
                  lss::players::split_render::SetMask(static_cast<std::uint32_t>(std::get<std::int64_t>(value)),
                                                      session.HasPlayerTwo());
                  return Outcome{};
              },
              .get = [] { return Value{static_cast<std::int64_t>(lss::players::split_render::Mask())}; }});
    Register({.name = L"console", .shape = Shape::Text, .help = L"an engine console command, as typed in the console",
              .set  = [](const Value& value) {
                  const auto& command = std::get<std::wstring>(value);
                  Log(L"console command '{}'", command);
                  return lss::players::Session::Console(command.c_str())
                             ? Outcome{}
                             : Outcome{false, L"the console command could not be called"};
              }});
    Register({.name = L"report", .help = L"log every local player's controller, pawn, camera and call counts (Ctrl+F6)",
              .set  = [&session](const Value&) {
                  session.Status();
                  return Outcome{};
              }});
    Register({.name = L"quit", .help = L"quit the game through the engine (player 2 must be out)",
              .set  = [&session](const Value&) {
                  if (session.HasPlayerTwo())
                  {
                      return Outcome{false, L"player 2 is still in ('leave' first)"};
                  }
                  return lss::players::Session::Console(L"quit") ? Outcome{}
                                                                  : Outcome{false, L"the console command could not be called"};
              }});
    Register({.name = L"version", .shape = Shape::Text, .help = L"the mod's version",
              .get  = [] { return Value{std::wstring(kVersion)}; }});
}

class LocalSplitScreenMod final : public RC::CppUserModBase
{
public:
    LocalSplitScreenMod()
    {
        ModName        = STR("LocalSplitScreen");
        ModVersion     = kVersion;
        ModDescription = STR("Two-player split-screen for FF7 Remake");
        ModAuthors     = STR("Andrew");
    }

    ~LocalSplitScreenMod() override
    {
        if (tickCallback_ != Hook::ERROR_ID)
        {
            Hook::UnregisterCallback(tickCallback_);
        }
        remote_.Close();
        lss::hud::hud_probe::Remove();
        lss::hud::root_placement::Remove();
        lss::hud::overlay_layout::Remove();
        lss::hud::marker_replay::Remove();
        lss::hud::second_image::Remove();
        lss::hud::minimap_two::Remove();
        lss::hud::image_entries::Remove();
        lss::hud::ui_targets::Remove();
        lss::players::pad_buttons::Remove();
        lss::battle::member_rates::Remove();
        lss::battle::lock_camera_two::Remove();
        lss::battle::targeting_two::Remove();
        lss::battle::party_switch::Remove();
        lss::battle::command_menu_tick::Remove();
        lss::battle::command_menu_two::Remove();
        if (installed_)
        {
            session_.Remove();  // the field-exit calls go back whether or not player 2 is in
        }
        // With player 2 in, its camera manager was built through the replacement; the DLL is pinned, so the
        // replacement stays valid. Otherwise the engine's function goes back.
        if (installed_ && !session_.HasPlayerTwo())
        {
            lss::players::player_two_camera::RemoveInputTick();
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
        lss::render::ao_origin::Install(*image);  // optional: without it player 2's half keeps the AO ghost
        // Optional: without it a distortion crossing the split line streaks over the other half.
        lss::render::distortion_apply::Install(*image);
        // Optional: without it player 2's bloom would be built from player 1's rows, so bloom stays off in split.
        if (lss::render::bloom_origin::Install(*image))
        {
            lss::players::split_render::SetMask(lss::facts::kSplitDisabledFlags, false);
        }
        lss::players::pad_buttons::Install(*image);  // optional: without it every pad presses player 1's buttons
        // Player 2's own battle: its command menu, ticked in its scopes, and the switch rules. Each optional.
        if (lss::players::scopes::Install(*image) && lss::battle::command_menu_two::Install(*image))
        {
            lss::battle::command_menu_tick::Install(*image);
            lss::battle::party_switch::Install(*image);
            lss::battle::targeting_two::Install(*image);
            lss::battle::lock_camera_two::Install(*image);
            lss::battle::member_rates::Install(*image);
        }
        lss::players::game_state::Install(*image);  // readings only; fine without
        lss::hud::hud_probe::Install(*image);       // logging only; fine without
        lss::hud::overlay_layout::Install(*image);  // optional: without it each half shows the middle of the HUD
        lss::hud::ui_targets::Install(*image);      // optional: without it both halves sample one UI image
        if (lss::hud::image_entries::Install(*image) && lss::hud::minimap_two::Install(*image))  // optional: player 2's image
        {
            lss::hud::second_image::Install(*image) && lss::hud::marker_replay::Install(*image);
        }
        RegisterSessionCommands(session_);
        remote_.Open();
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
            session_.Status();
        }
        lss::players::player_two_camera::MaintainInputTick();
        lss::players::game_state::Tick();  // first: the session and the console read this tick's values
        session_.Tick();
        lss::battle::command_menu_two::Tick(session_.Seated());
        lss::battle::command_menu_tick::Tick();
        lss::battle::party_switch::Tick();
        lss::battle::targeting_two::Tick();
        lss::battle::lock_camera_two::Tick();
        lss::battle::member_rates::Tick();
        TickHud();
        remote_.Tick();
    }

    /// A local player's view share (ULocalPlayer Origin and Size), read under a guard.
    bool ReadView(int index, lss::hud::overlay_layout::ViewShare& out) const noexcept
    {
        const auto* player = reinterpret_cast<const std::uint8_t*>(session_.LocalPlayer(index));
        return player != nullptr &&
               lss::platform::CopyGuarded(&out.originX, player + lss::facts::kLocalPlayerOrigin, 2 * sizeof(float)) &&
               lss::platform::CopyGuarded(&out.sizeX, player + lss::facts::kLocalPlayerSize, 2 * sizeof(float)) &&
               out.sizeX > 0.0f && out.sizeY > 0.0f && out.sizeX <= 1.0f && out.sizeY <= 1.0f;
    }

    /// The HUD layout follows the split and each player's view.
    void TickHud() noexcept
    {
        lss::hud::overlay_layout::ViewShare one, two;
        const bool split = session_.HasPlayerTwo() && ReadView(0, one) && ReadView(1, two);
        lss::hud::overlay_layout::Tick(split, one, two);
        lss::hud::second_image::Players players;
        for (int i = 0; split && i < 2; ++i)
        {
            players.localPlayer[i] = session_.LocalPlayer(i);
            lss::platform::CopyGuarded(&players.controller[i],
                                       static_cast<const std::uint8_t*>(players.localPlayer[i]) + lss::facts::kLocalPlayerController,
                                       sizeof(void*));
        }
        lss::hud::second_image::Tick(split, players);
        lss::hud::minimap_two::Tick(split && session_.Seated(), players.controller[1]);
        lss::hud::marker_replay::Tick();
        lss::hud::root_placement::Tick(split && lss::hud::overlay_layout::BandApplies());
    }

    // Declaration order matters: holds_ and remote_ hold references into session_, so it comes first.
    lss::players::Session          session_;
    ModHolds                       holds_{session_};
    lss::control::Remote           remote_{holds_};
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
