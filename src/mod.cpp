// LocalSplitScreen for FINAL FANTASY VII REMAKE: the UE4SS entry point.
//
// Verifies every engine fact it relies on, installs, and turns the player-intent keys into joins and leaves
// on the game thread (the engine tick's pre-callback), so they never land inside a frame's drawing. A retry, load,
// jump or return to the title takes the further players out first and brings them back after (players/field_exit.h).
// While the screen is split the HUD is laid out for each view's shape (hud/overlay_layout.h, hud/root_placement.h).
//   Ctrl+F7        player 2 joins with a controller and camera manager of its own, and takes its party member at once
//   Ctrl+F8        player 2 leaves
//   Ctrl+Shift+F7  player 3 joins (the third row of the party order), Ctrl+Shift+F8 player 3 leaves
//   Ctrl+F6        log every local player's controller, camera manager and camera actor
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
#include "hud/overlay_pass.h"
#include "hud/root_placement.h"
#include "hud/field_icons.h"
#include "hud/outside_icons.h"
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
#include "players/weak_object.h"
#include "players/game_state.h"
#include "players/lead_gate.h"
#include "players/session.h"
#include "players/split_render.h"
#include "engine_facts.h"
#include "render/ao_origin.h"
#include "render/bloom_origin.h"
#include "render/shadows_per_view.h"
#include "render/distortion_apply.h"
#include "render/movie_frame.h"
#include "render/ssr_origin.h"
#include "render/sun_cascades.h"
#include "render/family_boundary.h"
#include "render/fog_volume_watch.h"
#include "render/occlusion_exempt.h"
#include "render/one_family.h"
#include "render/history_cut.h"
#include "render/lighting_volume.h"
#include "views/column_layout.h"
#include "views/column_rules.h"
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
#include <format>
#include <optional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#define LOCALSPLITSCREEN_API __declspec(dllexport)

namespace {

constexpr const wchar_t* kVersion = L"0.79.2-panel-after-rejoin";
static_assert(lss::views::column_rules::kMaxColumns == lss::hud::overlay_pass::kMaxColumns, "the columns the views and the HUD count");  // the one version string (the log and UE4SS)

using lss::platform::Log;
namespace Hook = RC::Unreal::Hook;

bool GameHasFocus() noexcept
{
    DWORD owner = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &owner);
    return owner == GetCurrentProcessId();
}

/// True on the tick a Ctrl+key (with Shift when `shift`, without it otherwise) goes down; `wasDown` carries the state
/// between ticks.
bool Pressed(int virtualKey, bool& wasDown, bool shift = false) noexcept
{
    const bool down    = GameHasFocus() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
                         ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) == shift && (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown            = down;
    return pressed;
}

using lss::control::Outcome;
using lss::control::Shape;
using lss::control::Value;

constexpr int kSeatOne   = 1;  // player 2: the `join`, `leave` and p2.* commands (plan/seats.md step 7 may rename them)
constexpr int kSeatThree = 2;  // player 3: `p3.join`, `p3.leave` and p3.*

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

/// One seat's intents and readings: `join`/`leave` (or the prefixed ones) and `<prefix>.member`, `.in`, `.rejoin`,
/// `.state`.
void RegisterSeatCommands(lss::players::Session& session, int seat, const wchar_t* prefix, const wchar_t* join, const wchar_t* leave)
{
    using lss::control::Register;
    const std::wstring name = lss::players::SeatName(seat);
    Register({.name = join,
              .help = std::format(L"{} joins on its member (the party order's row {}) with a controller and camera manager of its own ({}); "
                                  L"with the member not free, nothing is made yet and {} joins once it is ({}.state awaiting)",
                                  name, seat + 1, lss::players::JoinKey(seat), name, prefix),
              .set  = [&session, seat, name, leave](const Value&) {
                  session.Join(seat);
                  return session.In(seat) ? Outcome{}
                         : session.Awaiting(seat)
                             ? Outcome{true, std::format(L"{} joins once its member is free (see the log; '{}' stops the wait)", name, leave)}
                             : Outcome{false, std::format(L"{} did not join (see the log)", name)};
              }});
    Register({.name = leave,
              .help = std::format(L"{} leaves, or stops waiting for a member ({})", name, lss::players::LeaveKey(seat)),
              .set  = [&session, seat, name](const Value&) {
                  session.Leave(seat);
                  return session.In(seat) ? Outcome{false, std::format(L"{} is still in (see the log)", name)} : Outcome{};
              }});
    Register({.name = std::format(L"{}.member", prefix), .shape = Shape::Text,
              .help = std::format(L"the member {} plays (its type and row of the party order), or why it waits for one", name),
              .get  = [&session, seat] { return Value{session.MemberReading(seat)}; }});
    Register({.name = std::format(L"{}.in", prefix), .shape = Shape::OnOff, .help = std::format(L"{} is in", name),
              .get = [&session, seat] { return Value{session.In(seat)}; }});
    Register({.name = std::format(L"{}.rejoin", prefix), .shape = Shape::OnOff,
              .help = std::format(L"{} was taken out by a world change and joins again once the field is back ('{}' cancels)", name, leave),
              .get  = [&session, seat] { return Value{session.RejoinPending(seat)}; }});
    Register({.name  = std::format(L"{}.state", prefix),
              .shape = Shape::Word,
              .words = {L"out", L"awaiting", L"rejoining", L"in", L"playing", L"handed"},
              .help  = L"out, awaiting (asked for; joins once its member is free), rejoining (taken out by a world change; joins "
                       L"again once the field is back), in (between a seat and the next, momentary), playing (holds its member), "
                       L"or handed (its member is with its AI while the game's leader is: a scene, a field action, the lock; or "
                       L"for the member's own hold: a script, an animation, a guide lead; the seat is kept)",
              .get   = [&session, seat] { return Value{std::wstring(lss::players::StateName(session.State(seat)))}; }});
}

/// The session's commands and readings on the console. The render modules register their own switches.
void RegisterSessionCommands(lss::players::Session& session)
{
    using lss::control::Register;
    RegisterSeatCommands(session, kSeatOne, L"p2", L"join", L"leave");
    RegisterSeatCommands(session, kSeatThree, L"p3", L"p3.join", L"p3.leave");
    Register({.name = L"party.ai", .shape = Shape::Text,
              .help = L"each deployed member's status, PLAYER slot holder, actor mode and field AI layers (the take test's view)",
              .get  = [&session] { return Value{session.FormationReading()}; }});
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
    Register({.name = L"quit", .help = L"quit the game through the engine (every further player must be out)",
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
        lss::players::split_render::Remove();  // after the mirrors: the mask back to zero so a reinstall finds it free
        lss::views::column_layout::Remove();
        lss::views::window_span::Leave(L"the mod unloads");
        lss::views::display_align::Restore();  // the leave's ticks never come: the monitors go back now
        lss::views::scene_camera_two::Remove();
        lss::views::letterbox_patch::Restore();
        lss::hud::hud_probe::Remove();
        lss::hud::root_placement::Remove();
        lss::hud::overlay_layout::Remove();
        lss::hud::marker_replay::Remove();
        lss::hud::field_icons::Remove();
        lss::hud::outside_icons::Remove();
        lss::hud::second_image::Remove();
        lss::hud::minimap_two::Remove();
        lss::hud::image_entries::Remove();
        lss::hud::ui_targets::Remove();
        lss::render::movie_frame::Remove();
        lss::render::shadows_per_view::Remove();
        lss::render::ssr_origin::Remove();
        lss::render::sun_cascades::Remove();
        lss::render::lighting_volume::Remove();
        lss::render::history_cut::Remove();
        lss::render::one_family::Remove();
        lss::render::occlusion_exempt::Remove();
        lss::render::fog_volume_watch::Remove();
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
        if (image)
        {
            lss::players::weak_object::Install(*image);  // first: every module that keeps a game object between ticks uses it
        }
        if (!image || !session_.Install(*image))
        {
            Log(L"standing aside: an engine check failed (see above); nothing installed, the game runs unmodified");
            return;
        }
        installed_ = true;
        lss::render::ao_origin::Install(*image);  // optional: without it player 2's half keeps the AO ghost
        // Optional: without it a distortion crossing the split line streaks over the other half.
        lss::render::distortion_apply::Install(*image);
        lss::render::shadows_per_view::Install(*image);  // optional: without it one camera decides some shadows for every view
        lss::render::ssr_origin::Install(*image);  // optional: without it a view's reflections read the next column's surfaces
        lss::render::sun_cascades::Install(*image);  // optional: without it each view takes every view's sun cascades
        lss::render::lighting_volume::Install(*image);  // optional: the volume-box test only
        lss::render::history_cut::Install(*image);      // optional: the history-cut test only
        lss::render::fog_volume_watch::Install(*image);  // optional: logging only
        lss::render::occlusion_exempt::Install(*image);  // optional: the occlusion-exempt test only
        // Optional: without them every player is a view of one family, and views after the first lose their fog.
        if (lss::render::family_boundary::Install(*image))
        {
            lss::render::one_family::Install(*image);
        }
        // Optional: without it player 2's bloom would be built from player 1's rows, so bloom stays off in split.
        if (lss::render::bloom_origin::Install(*image))
        {
            lss::players::split_render::SetMask(lss::facts::kSplitDisabledFlags, false);
        }
        // Optional: without them split in Borderless keeps the game's letterbox, and the window stays on one monitor.
        lss::views::letterbox_patch::Install(*image);
        lss::views::window_span::Install(*image);
        lss::views::column_layout::Install(*image);     // optional: without it three local players take the stock quadrants
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
        lss::world::anim_two_views::Install(*image);  // optional: without it characters near a further player animate by player 1's camera
        lss::world::npc_detail::Install(*image);      // optional: without it NPCs near a further player pause by player 1's camera
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
                lss::hud::outside_icons::Install(*image);
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
        Log(L"ready: every engine check passed. Ctrl+F7 adds player 2 (it takes the 2nd party slot's member), Ctrl+F8 removes player 2, "
            L"Ctrl+Shift+F7 adds player 3 (the 3rd slot's member), Ctrl+Shift+F8 removes player 3, Ctrl+F6 reports, Ctrl+F2 toggles "
            L"the window span; with window-span.hold on the mirror player fills the empty columns");
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
        if (Pressed(VK_F7, joinThreeWasDown_, true))
        {
            session_.Join(kSeatThree);
        }
        if (Pressed(VK_F8, leaveThreeWasDown_, true))
        {
            session_.Leave(kSeatThree);
        }
        if (Pressed(VK_F6, reportWasDown_))
        {
            session_.Status();
        }
        if (Pressed(VK_F2, spanWasDown_))
        {
            lss::views::window_span::Toggle();
        }
        // The console's lines (joins and leaves among them) before the session and the column map, as the keys, so
        // this frame's HUD passes never name a player the console just took out.
        remote_.Tick();
        lss::players::player_two_camera::MaintainInputTick();
        lss::players::game_state::Tick();  // first: the session and the console read this tick's values
        lss::players::pad_presence::Tick();
        session_.Tick();
        const int playersIn = 1 + session_.Count();  // player 1 and the seats in: the columns the players fill
        lss::views::letterbox_patch::Tick(playersIn > 1 || lss::views::window_span::Held());
        lss::views::window_span::Tick(playersIn > 1);
        // The columns this frame shows (views/column_rules.h): the players, the span's while held, or the console's.
        const int spanColumns = lss::views::window_span::Held() ? lss::views::window_span::Columns() : 0;
        const int viewColumns = lss::views::column_rules::Wanted(playersIn, spanColumns, lss::views::column_layout::Override());
        const int aloneColumns = lss::views::column_rules::Wanted(1, spanColumns, lss::views::column_layout::Override());
        lss::views::column_layout::Tick(viewColumns, playersIn + lss::views::mirror_player::Count());
        lss::views::mirror_player::Tick(playersIn, session_.SeatIds(), viewColumns, aloneColumns);
        MapColumns();  // after every join, leave and mirror change of this tick: the HUD and this frame's passes read it
        lss::players::split_render::Tick(session_.Count() + lss::views::mirror_player::Count() > 0);
        lss::views::scene_camera_two::SeatManagers seatManagers{};
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            seatManagers[static_cast<std::size_t>(seat)] = ManagerOfSeat(seat);
        }
        lss::views::scene_camera_two::Tick(CameraManagerOf(0), seatManagers);
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            lss::battle::command_menu_two::Tick(seat, session_.Seated(seat));
        }
        lss::battle::command_menu_tick::Tick();
        lss::battle::party_switch::Tick();
        lss::players::operator_battle_mode::Tick();
        lss::battle::targeting_two::Tick();
        lss::battle::target_cursor_two::Tick();
        lss::battle::lock_camera_two::Tick();
        lss::battle::member_rates::Tick();
        lss::battle::enemy_leader::Tick();
        lss::battle::enemy_coin::Tick();
        lss::battle::sequence_camera_two::Tick();
        TickWorld();
        TickHud();
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

    /// A seat's own controller while its player is in (whatever its column), or null.
    void* ControllerOfSeat(int seat) const noexcept
    {
        return lss::platform::ReadPointer(session_.SeatPlayer(seat), lss::facts::kLocalPlayerController);
    }

    /// A seat's own camera manager while its player is in, or null.
    void* ManagerOfSeat(int seat) const noexcept
    {
        return lss::platform::ReadPointer(ControllerOfSeat(seat), lss::facts::kControllerCameraManager);
    }

    /// Which seat fills each column, from the engine's list (a seat's column is its place there: players/seats.h),
    /// for every module that runs per column (hud/overlay_pass.h).
    void MapColumns() noexcept
    {
        lss::hud::overlay_pass::Seats seats;
        for (int column = 1; column < lss::hud::overlay_pass::kMaxColumns; ++column)
        {
            auto* player = session_.LocalPlayer(column);
            for (int seat = 1; player != nullptr && seat <= lss::players::kSeatsBuilt; ++seat)
            {
                if (session_.SeatPlayer(seat) == player)
                {
                    seats.seat[column] = seat;
                }
            }
        }
        lss::hud::overlay_pass::SetSeats(seats);
    }

    /// The world around the views (world/): party members kept visible near a camera, characters animated by the
    /// camera that sees them best, the NPCs near every further player kept awake and spawned around them too, and
    /// able to see them.
    void TickWorld() noexcept
    {
        // Every world module per built seat (plan/seats.md step 3): each told the seat's member, camera or controller
        // while the seat's player is in, then its once-a-tick work.
        bool anyIn = false;
        lss::world::party_fade::Tick(session_.Count());
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            const bool seatIn = session_.In(seat);
            void*      member = seatIn ? lss::players::scopes::Member(seat) : nullptr;
            void*      camera = seatIn ? ManagerOfSeat(seat) : nullptr;
            anyIn             = anyIn || seatIn;
            lss::world::anim_two_views::TickSeat(seat, camera, member);
            lss::world::npc_crowd::TickSeat(seat, member);
            lss::world::npc_senses::TickSeat(seat, member);
            lss::world::npc_detail::TickSeat(seat, camera);
            lss::world::env_sets_two::TickSeat(seat, member, seatIn ? ControllerOfSeat(seat) : nullptr);
        }
        lss::world::anim_two_views::Tick();
        lss::world::npc_crowd::Tick();
        lss::world::npc_senses::Tick(anyIn ? PawnOf(0) : nullptr);
        lss::world::npc_detail::Tick(anyIn ? CameraManagerOf(0) : nullptr);
        lss::world::env_sets_two::Tick();
    }

    /// The HUD layout follows the split and each column's view (hud/overlay_pass.h: column 0 is player 1's, the seats'
    /// follow in the engine list's order, then the mirrors'). With the mirror player in and no seat in, the HUD is laid
    /// out for player 1's
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
        lss::hud::ui_targets::SetPlayerColumns(columns.count);  // a mirror's column samples player 1's HUD image
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
            // A further column's seat from the map (MapColumns): its member.
            const int seat         = op::SeatOf(column);
            columns.member[column] = seat >= 1 ? lss::players::scopes::Member(seat) : nullptr;
        }
        lss::hud::second_image::Tick(split, columns);
        lss::hud::minimap_two::Tick();
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            lss::hud::minimap_two::TickSeat(seat, split && session_.Seated(seat), split ? ControllerOfSeat(seat) : nullptr);
        }
        lss::hud::marker_replay::Tick();
        lss::hud::field_icons::Tick();
        lss::hud::outside_icons::Tick();
        lss::hud::root_placement::Tick(views && lss::hud::overlay_layout::BandApplies());
    }

    // Declaration order matters: holds_ and remote_ hold references into session_, so it comes first.
    lss::players::Session          session_;
    ModHolds                       holds_{session_};
    lss::control::Remote           remote_{holds_};
    bool                  installed_     = false;
    bool                  joinWasDown_   = false;
    bool                  leaveWasDown_  = false;
    bool                  joinThreeWasDown_  = false;
    bool                  leaveThreeWasDown_ = false;
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
