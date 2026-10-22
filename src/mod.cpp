// LocalSplitScreen for FINAL FANTASY VII REMAKE: the UE4SS entry point.
//
// Verifies every engine fact it relies on, installs, and turns the player-intent keys into joins and leaves
// on the game thread (the engine tick's pre-callback), so they never land inside a frame's drawing. A retry, load,
// jump or return to the title takes player 2 out first and brings it back after (players/field_exit.h). While the screen
// is split the HUD is laid out for each half's shape (hud/overlay_layout.h, hud/root_placement.h).
//   Ctrl+F7  player 2 joins with a controller and camera manager of its own, and takes its party member at once
//   Ctrl+F8  player 2 leaves
//   Ctrl+F6  log every local player's controller, camera manager and camera actor
// The same intents, and every module's switches, are commands on the console (control/remote.h), driven from
// the host by tools/game.py.

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include "battle/command_camera_two.h"
#include "battle/command_menu_tick.h"
#include "battle/command_menu_two.h"
#include "battle/enemy_coin.h"
#include "battle/enemy_leader.h"
#include "battle/lock_camera_two.h"
#include "battle/member_rates.h"
#include "battle/party_switch.h"
#include "battle/sequence_camera_two.h"
#include "battle/target_cursor_two.h"
#include "battle/targeting_two.h"
#include "control/registry.h"
#include "control/remote.h"
#include "hud/hud_probe.h"
#include "hud/overlay_layout.h"
#include "hud/root_placement.h"
#include "hud/field_icons.h"
#include "hud/image_entries.h"
#include "hud/marker_replay.h"
#include "hud/minimap_two.h"
#include "hud/second_image.h"
#include "hud/ui_targets.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "players/own_controller.h"
#include "players/pad_buttons.h"
#include "players/pad_presence.h"
#include "players/scopes.h"
#include "players/camera_override_two.h"
#include "players/nav_area_two.h"
#include "players/operator_battle_mode.h"
#include "players/player_two_camera.h"
#include "players/game_state.h"
#include "players/lead_gate.h"
#include "players/session.h"
#include "players/split_render.h"
#include "engine_facts.h"
#include "render/ao_origin.h"
#include "render/bloom_origin.h"
#include "render/distortion_apply.h"
#include "render/movie_frame.h"
#include "views/letterbox_patch.h"
#include "views/display_align.h"
#include "views/mirror_player.h"
#include "views/scene_camera_two.h"
#include "views/window_span.h"
#include "world/anim_two_views.h"
#include "world/env_sets_two.h"
#include "world/npc_crowd.h"
#include "world/npc_detail.h"
#include "world/npc_senses.h"
#include "world/party_fade.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#define LOCALSPLITSCREEN_API __declspec(dllexport)

namespace {

constexpr const wchar_t* kVersion = L"0.51.0-command-tick-by-seat";  // the one version string (the log and UE4SS)

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

constexpr int kSeatOne = 1;  // the p2.* readings and intents name seat 1 (plan/seats.md step 7 generalises them)

/// The input holds the console can make: at the mod's own per-pad stick layer, so a scripted stick takes the
/// path a real pad takes.
class ModHolds final : public lss::control::Holds
{
public:
    explicit ModHolds(lss::players::Session& session) noexcept : session_(session) {}

    Outcome HoldStick(std::int32_t player, const float (&axes)[4]) override
    {
        if (!session_.In(player - 1))
        {
            return {false, L"stick holds act only while that player is in"};
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
    Register({.name = L"join",
              .help = L"player 2 joins on its member with a controller and camera manager of its own (Ctrl+F7); with the member not "
                      L"free, nothing is made yet and player 2 joins once it is (p2.state awaiting)",
              .set  = [&session](const Value&) {
                  session.Join(kSeatOne);
                  return session.In(kSeatOne) ? Outcome{}
                         : session.Awaiting(kSeatOne)   ? Outcome{true, L"player 2 joins once its member is free (see the log; 'leave' stops the wait)"}
                                                : Outcome{false, L"player 2 did not join (see the log)"};
              }});
    Register({.name = L"leave", .help = L"player 2 leaves, or stops waiting for a member (Ctrl+F8)", .set = [&session](const Value&) {
                  session.Leave(kSeatOne);
                  return session.In(kSeatOne) ? Outcome{false, L"player 2 is still in (see the log)"} : Outcome{};
              }});
    Register({.name = L"party.ai", .shape = Shape::Text,
              .help = L"each deployed member's status, PLAYER slot holder, actor mode and field AI layers (the take test's view)",
              .get  = [&session] { return Value{session.FormationReading()}; }});
    Register({.name = L"p2.member", .shape = Shape::Text,
              .help = L"the member player 2 plays (its type and row of the party order), or why it waits for one",
              .get  = [&session] { return Value{session.MemberReading(kSeatOne)}; }});
    Register({.name = L"p2.in", .shape = Shape::OnOff, .help = L"player 2 is in",
              .get = [&session] { return Value{session.In(kSeatOne)}; }});
    Register({.name = L"p2.rejoin", .shape = Shape::OnOff,
              .help = L"player 2 was taken out by a world change and joins again once the field is back ('leave' cancels)",
              .get  = [&session] { return Value{session.RejoinPending(kSeatOne)}; }});
    Register({.name  = L"p2.state",
              .shape = Shape::Word,
              .words = {L"out", L"awaiting", L"in", L"playing", L"handed"},
              .help  = L"out, awaiting (asked for; joins once its member is free), in (between a seat and the next, momentary), "
                       L"playing (holds its member), or handed (its member is with its AI while the game's leader is: a scene, a "
                       L"field action, the lock; or for the member's own hold: a script, an animation, a guide lead; the seat is kept)",
              .get   = [&session] {
                  return Value{std::wstring(!session.In(kSeatOne) ? (session.Awaiting(kSeatOne) ? L"awaiting" : L"out")
                                            : !session.Seated(kSeatOne)     ? L"in"
                                            : session.HandedToAi(kSeatOne)  ? L"handed"
                                                                    : L"playing")};
              }});
    Register({.name  = L"showflags",
              .shape = Shape::Hex,
              .max   = 0xFFFFFFFF,
              .help  = L"the show flags forced on in both halves while player 2 is in (0 = the engine's split rule; "
                       L"players/split_render.h)",
              .set   = [&session](const Value& value) {
                  lss::players::split_render::SetMask(static_cast<std::uint32_t>(std::get<std::int64_t>(value)),
                                                      session.Count() != 0);
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
                  if (session.Count() != 0)
                  {
                      return Outcome{false, L"a further player is still in ('leave' first)"};
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
        lss::views::mirror_player::Clear(L"the mod unloads");
        lss::views::window_span::Leave(L"the mod unloads");
        lss::views::display_align::Restore();  // the leave's ticks never come: the monitors go back now
        lss::views::scene_camera_two::Remove();
        lss::views::letterbox_patch::Restore();
        lss::hud::hud_probe::Remove();
        lss::hud::root_placement::Remove();
        lss::hud::overlay_layout::Remove();
        lss::hud::marker_replay::Remove();
        lss::hud::field_icons::Remove();
        lss::hud::second_image::Remove();
        lss::hud::minimap_two::Remove();
        lss::hud::image_entries::Remove();
        lss::hud::ui_targets::Remove();
        lss::render::movie_frame::Remove();
        lss::players::pad_buttons::Remove();
        lss::battle::sequence_camera_two::Remove();
        lss::battle::target_cursor_two::Remove();
        lss::battle::enemy_coin::Remove();
        lss::battle::enemy_leader::Remove();
        lss::battle::member_rates::Remove();
        lss::battle::command_camera_two::Remove();
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
        if (installed_ && session_.Count() == 0)
        {
            lss::players::player_two_camera::RemoveInputTick();
            lss::players::nav_area_two::Remove();
            lss::players::operator_battle_mode::Remove();
            lss::players::camera_override_two::Remove();
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
        // Optional: without them split in Borderless keeps the game's letterbox, and the window stays on one monitor.
        lss::views::letterbox_patch::Install(*image);
        lss::views::window_span::Install(*image);
        lss::views::scene_camera_two::Install(*image);  // readings only; fine without
        // Optional: without it a held span shows one view across the row while player 2 is out.
        if (lss::views::mirror_player::Install(*image))
        {
            session_.SetMirrorPlayers({&lss::views::mirror_player::Count, &lss::views::mirror_player::Clear,
                                       &lss::views::mirror_player::FillAtLeave});
        }
        lss::players::pad_buttons::Install(*image);  // optional: without it every pad presses player 1's buttons
        lss::players::pad_presence::Register();      // diagnostics: which XInput users have a pad
        lss::players::lead_gate::Install(*image);    // optional: without it a LEAD on player 2's member stays the player's
        // Player 2's own battle: its command menu, ticked in its scopes, and the switch rules. Each optional.
        if (lss::players::scopes::Install(*image) && lss::battle::command_menu_two::Install(*image))
        {
            lss::battle::command_menu_tick::Install(*image);
            lss::battle::party_switch::Install(*image);
            if (lss::battle::targeting_two::Install(*image))
            {
                lss::battle::target_cursor_two::Install(*image);  // optional: without it both halves show player 1's cursor
            }
            lss::battle::lock_camera_two::Install(*image);
            lss::battle::command_camera_two::Install(*image);  // optional: without it the aim reading is off, the UI scope stays
            lss::battle::member_rates::Install(*image);
            lss::battle::enemy_leader::Install(*image);
            lss::battle::enemy_coin::Install(*image);  // optional: without it enemies name player 1 as the player
            lss::battle::sequence_camera_two::Install(*image);
        }
        lss::players::game_state::Install(*image);  // readings only; fine without
        lss::world::party_fade::Install(*image);    // optional: without it a member near player 2's camera vanishes in both halves
        lss::world::anim_two_views::Install(*image);  // optional: without it characters near player 2 animate by player 1's camera
        lss::world::npc_detail::Install(*image);      // optional: without it NPCs near player 2 pause by player 1's camera
        lss::world::npc_crowd::Install(*image);       // optional: without it the crowd is spawned around player 1 only
        lss::world::npc_senses::Install(*image);      // optional: without it NPCs see nobody beyond player 1's grid
        lss::world::env_sets_two::Install(*image);    // optional: without it only player 1's ground hides the area's actors
        lss::hud::hud_probe::Install(*image);       // logging only; fine without
        lss::hud::overlay_layout::Install(*image);  // optional: without it each half shows the middle of the HUD
        lss::hud::ui_targets::Install(*image);      // optional: without it both halves sample one UI image
        lss::render::movie_frame::Install(*image);  // optional: without it a movie shows in the first view only
        if (lss::hud::image_entries::Install(*image) && lss::hud::minimap_two::Install(*image))  // optional: player 2's image
        {
            if (lss::hud::second_image::Install(*image))
            {
                lss::hud::marker_replay::Install(*image);
                lss::hud::field_icons::Install(*image);
            }
        }
        // The settings file: the one preset today, applied before the first tick.
        lss::views::window_span::SetPreset(lss::platform::settings::Read(L"Views", L"multi-monitor", L"auto"),
                                           lss::platform::settings::Path());
        RegisterSessionCommands(session_);
        remote_.Open();
        const Hook::FCallbackOptions options{false, false, STR("LocalSplitScreen"), STR("player intent")};
        tickCallback_ = Hook::RegisterEngineTickPreCallback(
            [this](Hook::TCallbackIterationData<void>&, RC::Unreal::UEngine*, float, bool) { OnEngineTick(); }, options);
        Log(L"ready: every engine check passed. Ctrl+F7 adds player 2 (it takes the 2nd party slot's member), Ctrl+F8 removes player 2, Ctrl+F6 reports, Ctrl+F2 toggles the window span; with window-span.hold on the mirror player fills the empty column");
    }

private:
    void OnEngineTick() noexcept
    {
        if (Pressed(VK_F7, joinWasDown_))
        {
            session_.Join(kSeatOne);
        }
        if (Pressed(VK_F8, leaveWasDown_))
        {
            session_.Leave(kSeatOne);
        }
        if (Pressed(VK_F6, reportWasDown_))
        {
            session_.Status();
        }
        if (Pressed(VK_F2, spanWasDown_))
        {
            lss::views::window_span::Toggle();
        }
        lss::players::player_two_camera::MaintainInputTick();
        lss::players::game_state::Tick();  // first: the session and the console read this tick's values
        lss::players::pad_presence::Tick();
        session_.Tick();
        const int playersIn = 1 + session_.Count();  // player 1 and the seats in: the columns the players fill
        lss::views::letterbox_patch::Tick(playersIn > 1 || lss::views::window_span::Held());
        lss::views::window_span::Tick(playersIn > 1);
        lss::views::mirror_player::Tick(playersIn, lss::views::window_span::Held() ? lss::views::window_span::Columns() : 0);
        lss::views::scene_camera_two::Tick(CameraManagerOf(0), session_.In(kSeatOne) ? CameraManagerOf(1) : nullptr);  // seat 1 today
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            lss::battle::command_menu_two::Tick(seat, session_.Seated(seat));
        }
        lss::battle::command_menu_tick::Tick();
        lss::battle::party_switch::Tick();
        lss::battle::targeting_two::Tick();
        lss::battle::target_cursor_two::Tick();
        lss::battle::lock_camera_two::Tick();
        lss::battle::member_rates::Tick();
        lss::battle::enemy_leader::Tick();
        lss::battle::enemy_coin::Tick();
        lss::battle::sequence_camera_two::Tick();
        TickWorld();
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

    /// A local player's camera manager (null if unread).
    void* CameraManagerOf(int index) const noexcept
    {
        return lss::platform::ReadPointer(lss::platform::ReadPointer(session_.LocalPlayer(index), lss::facts::kLocalPlayerController),
                                          lss::facts::kControllerCameraManager);
    }

    /// A local player's pawn (player 1's is the character the game centres the NPCs' senses on), or null.
    void* PawnOf(int index) const noexcept
    {
        return lss::platform::ReadPointer(ControllerOf(index), lss::facts::kControllerPawn);
    }

    /// A local player's controller, or null.
    void* ControllerOf(int index) const noexcept
    {
        return lss::platform::ReadPointer(session_.LocalPlayer(index), lss::facts::kLocalPlayerController);
    }

    /// The world around two views (world/): party members kept visible near a camera, characters animated by the
    /// camera that sees them better, the NPCs near player 2 kept awake and spawned around it too, and able to see it.
    void TickWorld() noexcept
    {
        const bool in        = session_.In(kSeatOne);  // seat 1: the world modules take one further player today (plan/seats.md)
        void*      cameraTwo = in ? CameraManagerOf(1) : nullptr;
        void*      memberTwo = in ? lss::players::scopes::Member(1) : nullptr;
        lss::world::party_fade::Tick(in);
        lss::world::anim_two_views::Tick(cameraTwo, memberTwo);
        lss::world::npc_detail::Tick(in ? CameraManagerOf(0) : nullptr, cameraTwo);
        lss::world::npc_crowd::Tick(memberTwo);
        lss::world::npc_senses::Tick(in ? PawnOf(0) : nullptr, memberTwo);
        lss::world::env_sets_two::Tick(memberTwo, in ? ControllerOf(1) : nullptr);
    }

    /// The HUD layout follows the split and each column's view (hud/overlay_pass.h: column 0 is player 1's, the seats'
    /// follow in order, then the mirror's). With the mirror player in and no seat in, the HUD is laid out for player 1's
    /// column and every view samples that one image: the further passes and the per-view targets stay off, since only
    /// a further pass would draw into a second target, leaving the mirror's view empty.
    void TickHud() noexcept
    {
        namespace op = lss::hud::overlay_pass;
        lss::hud::second_image::Columns columns;
        columns.count = std::min(1 + session_.Count(), op::kMaxColumns);  // the columns real players fill
        const bool mirror      = session_.Count() == 0 && lss::views::mirror_player::Count() > 0;
        const int  viewColumns = std::min(columns.count + lss::views::mirror_player::Count(), op::kMaxColumns);
        lss::hud::overlay_layout::ViewShares shares{};
        int                                  read = 0;  // the columns whose views were read, from the first
        for (int column = 0; column < viewColumns && ReadView(column, shares[column]); ++column)
        {
            read = column + 1;
        }
        const bool views = read >= 2;  // a split, or the mirror beside player 1
        const bool split = views && columns.count > 1 && read >= columns.count;
        lss::hud::overlay_layout::Tick(views, shares, read);
        if (mirror && lss::hud::ui_targets::Enabled())
        {
            lss::hud::ui_targets::SetEnabled(false);
            Log(L"HUD targets: off while the mirror player is in and player 2 is out (each column samples player 1's HUD image; "
                L"player 2's image turns them on again at its join)");
        }
        for (int column = 0; split && column < columns.count; ++column)
        {
            columns.localPlayer[column] = session_.LocalPlayer(column);
            columns.controller[column]  = ControllerOf(column);
            // A further column's seat is its index while the seats fill in order (plan/seats.md): its member.
            columns.member[column] = column >= 1 ? lss::players::scopes::Member(column) : nullptr;
        }
        lss::hud::second_image::Tick(split, columns);
        lss::hud::minimap_two::Tick(split && session_.Seated(kSeatOne), columns.controller[1]);
        lss::hud::marker_replay::Tick();
        lss::hud::field_icons::Tick();
        lss::hud::root_placement::Tick(views && lss::hud::overlay_layout::BandApplies());
    }

    // Declaration order matters: holds_ and remote_ hold references into session_, so it comes first.
    lss::players::Session          session_;
    ModHolds                       holds_{session_};
    lss::control::Remote           remote_{holds_};
    bool                  installed_     = false;
    bool                  joinWasDown_   = false;
    bool                  leaveWasDown_  = false;
    bool                  reportWasDown_ = false;
    bool                  spanWasDown_   = false;
    Hook::GlobalCallbackId tickCallback_ = Hook::ERROR_ID;
};

}  // namespace

extern "C"
{
    LOCALSPLITSCREEN_API RC::CppUserModBase* start_mod() { return new LocalSplitScreenMod(); }
    LOCALSPLITSCREEN_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
