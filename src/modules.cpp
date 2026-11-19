#include "modules.h"

#include "audio/listeners.h"
#include "battle/arena_warp.h"
#include "battle/battle_rates.h"
#include "battle/battle_warps.h"
#include "battle/command_camera.h"
#include "battle/command_menu_tick.h"
#include "battle/command_menu.h"
#include "battle/enemy_coin.h"
#include "battle/enemy_leader.h"
#include "battle/enemy_stall.h"
#include "battle/summon_seats.h"
#include "battle/enemy_watch.h"
#include "battle/lock_camera.h"
#include "battle/member_rates.h"
#include "battle/menu_grey.h"
#include "battle/party_switch.h"
#include "battle/encounter_watch.h"
#include "battle/seat_toad.h"
#include "battle/sequence_camera.h"
#include "battle/target_cursor.h"
#include "battle/targeting.h"
#include "engine_facts.h"
#include "hud/field_icons.h"
#include "hud/hud_probe.h"
#include "hud/image_entries.h"
#include "hud/lead_mark.h"
#include "hud/marker_replay.h"
#include "hud/minimap.h"
#include "hud/outside_icons.h"
#include "hud/overlay_layout.h"
#include "hud/overlay_pass.h"
#include "hud/root_placement.h"
#include "hud/save_notice.h"
#include "hud/second_image.h"
#include "hud/ui_effect_pass.h"
#include "hud/ui_targets.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "players/ai_move_guard.h"
#include "players/event_move_hand.h"
#include "players/game_state.h"
#include "players/lead_gate.h"
#include "players/nav_area.h"
#include "players/operator_battle_mode.h"
#include "players/pad_buttons.h"
#include "players/pad_presence.h"
#include "players/pad_table.h"
#include "players/player_window.h"
#include "hud/view_prompt.h"
#include "hud/view_window.h"
#include "menu/coop_menu.h"
#include "players/seat_camera.h"
#include "players/seat_responsiveness.h"
#include "players/scopes.h"
#include "players/session.h"
#include "players/split_render.h"
#include "players/split_switch.h"
#include "render/ao_origin.h"
#include "render/area_blend.h"
#include "render/bloom_origin.h"
#include "render/distortion_apply.h"
#include "render/family_boundary.h"
#include "render/frame_capture.h"
#include "render/frame_watch.h"
#include "render/fog_volume_watch.h"
#include "render/history_cut.h"
#include "render/lighting_volume.h"
#include "render/low_health.h"
#include "render/movie_frame.h"
#include "render/occlusion_exempt.h"
#include "render/ribbon_fill.h"
#include "render/motion_history.h"
#include "render/one_family.h"
#include "render/pass_marks.h"
#include "render/shadows_per_view.h"
#include "render/ssr_origin.h"
#include "render/sun_cascades.h"
#include "render/view_marks.h"
#include "views/column_layout.h"
#include "views/display_align.h"
#include "views/letterbox_patch.h"
#include "views/mirror_player.h"
#include "views/scene_camera.h"
#include "views/whole_screen.h"
#include "views/narrow_camera.h"
#include "views/split_fov.h"
#include "views/split_scale.h"
#include "views/window_span.h"
#include "world/anim_views.h"
#include "world/area_levels.h"
#include "world/catch_up_move.h"
#include "world/env_sets.h"
#include "world/level_watch.h"
#include "world/named_actor.h"
#include "world/data_rows.h"
#include "world/field_targets.h"
#include "world/push_events.h"
#include "world/forced_walk.h"
#include "world/npc_crowd.h"
#include "world/npc_detail.h"
#include "world/npc_senses.h"
#include "world/overlap_fade.h"
#include "world/party_fade.h"
#include "world/party_warp.h"
#include "world/referenced_actor.h"
#include "world/search_arrows.h"
#include "world/shop_act.h"
#include "world/run_catch_up.h"
#include "world/state_signal.h"
#include "world/story_boxes.h"
#include "world/stuck_spots.h"
#include "world/talk_gather.h"
#include "world/traversal_action.h"
#include "world/traversal_walkup.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <type_traits>

namespace lss::modules {

namespace {

static_assert(views::column_rules::kMaxColumns == hud::overlay_pass::kMaxColumns, "the columns the views and the HUD count");
static_assert(views::columns::kMaxColumns == hud::overlay_pass::kMaxColumns, "the columns the map and the HUD count");

/// A row's install call: the module's Install returns bool (false: it stands aside) or void, and takes the game
/// image or nothing.
using Installer = bool (*)(const platform::GameImage&) noexcept;

template <auto Install>
bool Call(const platform::GameImage& image) noexcept
{
    if constexpr (std::is_invocable_v<decltype(Install), const platform::GameImage&>)
    {
        if constexpr (std::is_void_v<std::invoke_result_t<decltype(Install), const platform::GameImage&>>)
        {
            Install(image);
            return true;
        }
        else
        {
            return static_cast<bool>(Install(image));
        }
    }
    else
    {
        if constexpr (std::is_void_v<std::invoke_result_t<decltype(Install)>>)
        {
            Install();
            return true;
        }
        else
        {
            return static_cast<bool>(Install());
        }
    }
}

/// A row named as another row's need: the needed module's Install.
template <auto Install>
constexpr Installer Need() noexcept
{
    return &Call<Install>;
}

/// When a row is removed.
enum class Removal : std::uint8_t
{
    Always,          // at the unload, before the session's Remove
    WithPlayersOut,  // at the unload, after the session's Remove, only with every further player out
};

/// One row: the module's name for the log, its install, its removal or none, and the rows it needs installed.
struct Module
{
    const wchar_t*            name;
    Installer                 install;
    void (*remove)() noexcept = nullptr;  // nullptr: no removal; its patches stay in place, pointing into the pinned DLL
    Removal                   removal = Removal::Always;
    std::array<Installer, 2>  needs{};  // the rows that must have installed for this one to (nullptr: none)
};

// The rows that are not one module's Install or Remove as declared.

/// The split scale's install takes the engine console (players/session.h).
bool InstallSplitScale(const platform::GameImage& image) noexcept
{
    return views::split_scale::Install(image, &players::Session::Console);
}

/// With the bloom per view (render/bloom_origin.h), the split's reduced effects go back on (players/split_render.h:
/// the mask raised to the flags the split rule turns off). The session installs split_render itself.
bool SplitFlagsWithBloom(const platform::GameImage&) noexcept
{
    players::split_render::SetMask(facts::kSplitDisabledFlags, false);
    return true;
}

/// The letterbox patch's removal is its restore.
void RestoreLetterbox() noexcept
{
    views::letterbox_patch::Restore();
}

/// The window span's removal: the window back, then the monitors (the leave's ticks never come at the unload).
void LeaveWindowSpan() noexcept
{
    views::window_span::Leave(L"the mod unloads");
    views::display_align::Restore();
}

/// The overlay layout's removal takes the root placement's with it (the placement is the layout's, installed from
/// the layout's rect call on the game thread).
void RemoveOverlayLayout() noexcept
{
    hud::root_placement::Remove();
    hud::overlay_layout::Remove();
}

// The table. The order is the install order; a row's comment says what the module does for the players, as the
// entry point's did. "Optional" rows leave the game as it was without them (their own line says what is missing).
constexpr std::array kModules{
    // logging only: the frame rate, the worst frame, the hitches and which thread takes the frame
    Module{.name = L"frame watch", .install = Call<&render::frame_watch::Install>},
    // logging only: every frame around an Airbuster beam or a frame-rate drop in battle, to a file
    Module{.name = L"frame capture", .install = Call<&render::frame_capture::Install>, .needs = {Need<&render::frame_watch::Install>()}},
    // without it Square Enix's ribbon trails read every point back from GPU memory: tens of ms a frame in a trail-heavy attack
    Module{.name = L"ribbon fill", .install = Call<&render::ribbon_fill::Install>},
    // The render per view: each view's own origins, shadows, reflections, cascades and fades.
    // without it a further player's half keeps the AO ghost
    Module{.name = L"ao origin", .install = Call<&render::ao_origin::Install>},
    // without it a distortion crossing the split line streaks over the other half
    Module{.name = L"distortion apply", .install = Call<&render::distortion_apply::Install>},
    // without it one camera decides some shadows for every view
    Module{.name = L"shadows per view", .install = Call<&render::shadows_per_view::Install>, .remove = &render::shadows_per_view::Remove},
    // its site is the shadows' call: without it every view takes player 1's area fades
    Module{.name = L"area blend", .install = Call<&render::area_blend::Install>},
    // without it a view's reflections read the next column's surfaces
    Module{.name = L"ssr origin", .install = Call<&render::ssr_origin::Install>, .remove = &render::ssr_origin::Remove},
    // without it each view takes every view's sun cascades
    Module{.name = L"sun cascades", .install = Call<&render::sun_cascades::Install>, .remove = &render::sun_cascades::Remove},
    // the volume-box test only
    Module{.name = L"lighting volume", .install = Call<&render::lighting_volume::Install>, .remove = &render::lighting_volume::Remove},
    // the history-cut test only
    Module{.name = L"history cut", .install = Call<&render::history_cut::Install>, .remove = &render::history_cut::Remove},
    // logging only
    Module{.name = L"fog volume watch", .install = Call<&render::fog_volume_watch::Install>, .remove = &render::fog_volume_watch::Remove},
    // the occlusion-exempt test only
    Module{.name = L"occlusion exempt", .install = Call<&render::occlusion_exempt::Install>, .remove = &render::occlusion_exempt::Remove},
    // without these two every player is a view of one family, and views after the first lose their fog
    Module{.name = L"family boundary", .install = Call<&render::family_boundary::Install>},
    // without it players after the first lose the motion of moving things (smear, dithered hair)
    Module{.name = L"motion history", .install = Call<&render::motion_history::Install>},
    // a dev switch for GPU profilers (off at start): named GPU events around each player's view family
    Module{.name = L"view marks", .install = Call<&render::view_marks::Install>},
    Module{.name = L"pass marks", .install = Call<&render::pass_marks::Install>},
    Module{.name = L"one family", .install = Call<&render::one_family::Install>, .remove = &render::one_family::Remove,
           .needs = {Need<&render::family_boundary::Install>()}},
    // without it a further player's bloom would be built from player 1's rows, so bloom stays off in split
    Module{.name = L"bloom origin", .install = Call<&render::bloom_origin::Install>},
    Module{.name = L"split flags with bloom", .install = &SplitFlagsWithBloom, .needs = {Need<&render::bloom_origin::Install>()}},

    // The views: the window, the columns, the mirrors, the scene camera.
    // without it split in Borderless keeps the game's letterbox
    Module{.name = L"letterbox patch", .install = Call<&views::letterbox_patch::Install>, .remove = &RestoreLetterbox},
    // without it the window stays on one monitor
    Module{.name = L"window span", .install = Call<&views::window_span::Install>, .remove = &LeaveWindowSpan},
    // without it three local players take the stock quadrants
    Module{.name = L"column layout", .install = Call<&views::column_layout::Install>, .remove = &views::column_layout::Remove},
    // without it split at a resolution below the window leaves each view's outer part undrawn
    Module{.name = L"split scale", .install = &InstallSplitScale, .remove = &views::split_scale::Remove},
    // without it a view wider than the game's image keeps the single-player width and half its height
    Module{.name = L"split FOV", .install = Call<&views::split_fov::Install>, .remove = &views::split_fov::Remove},
    Module{.name = L"narrow camera", .install = Call<&views::narrow_camera::Install>, .remove = &views::narrow_camera::Remove},
    // readings only; fine without
    Module{.name = L"scene camera", .install = Call<&views::scene_camera::Install>, .remove = &views::scene_camera::Remove},
    // without it a held span shows one view across the row while no further player is in (the entry point clears the mirrors
    // first at the unload, before this table's removal)
    Module{.name = L"mirror player", .install = Call<&views::mirror_player::Install>},

    // The players' pads, the LEAD gate, the game's single slots answered per seat.
    // without it every pad presses player 1's buttons
    Module{.name = L"pad buttons", .install = Call<&players::pad_buttons::Install>, .remove = &players::pad_buttons::Remove},
    // without it players 2 and 3's cameras turn at player 1's Responsiveness
    Module{.name = L"seat responsiveness", .install = Call<&players::seat_responsiveness::Install>},
    // diagnostics: which XInput users have a pad
    Module{.name = L"pads reading", .install = Call<&players::pad_presence::Register>},
    // without it each pad is the player with its XInput number, whatever order Windows and Steam Input gave them
    Module{.name = L"pad table", .install = Call<&players::pad_table::Install>, .remove = &players::pad_table::Remove},
    // without it no prompt can be drawn in a player's own view (the player windows')
    Module{.name = L"view prompts", .install = Call<&hud::view_prompt::Install>, .remove = &hud::view_prompt::Remove},
    // without it the player windows are drawn in the engine's own font (the view prompts) instead of the game's window
    Module{.name = L"view windows", .install = Call<&hud::view_window::Install>, .remove = &hud::view_window::Remove},
    // without it the title screen does not say which controller plays player 1 (the game's save-data notice)
    Module{.name = L"save notice", .install = Call<&hud::save_notice::Install>, .remove = &hud::save_notice::Remove},
    // without it the pads can be changed only from the console (pad-table)
    Module{.name = L"player windows", .install = Call<&players::player_window::Install>, .remove = &players::player_window::Remove,
           .needs = {Need<&players::pad_table::Install>(), Need<&hud::view_prompt::Install>()}},
    // without it the mod's options are set only from the console and the settings file
    Module{.name = L"Co-op menu", .install = Call<&menu::coop_menu::Install>, .remove = &menu::coop_menu::Remove},
    // without it nothing finds a character by its name: the environment sets and the scripted-move hand stand aside, and
    // player 1's character is brought toward where it stood for another player's talk instead of beside the presser
    Module{.name = L"named actor", .install = Call<&world::named_actor::Install>},
    // without it nothing reads the level's state data: the lead gate and the state triggers from the console stand aside
    Module{.name = L"data rows", .install = Call<&world::data_rows::Install>},
    // without it a LEAD or a run to a mark on a further player's member stays the player's
    Module{.name = L"lead gate", .install = Call<&players::lead_gate::Install>, .needs = {Need<&world::data_rows::Install>()}},
    // without it an ambient interest walk hands a further player's member to its AI (which can loop on a ladder)
    Module{.name = L"AI move guard", .install = Call<&players::ai_move_guard::Install>, .remove = &players::ai_move_guard::Remove},
    // without it a further player's member goes to its AI after the level writes a scripted one-shot move on it, which ends
    // the move at once (a scripted entry or gesture cut short)
    Module{.name = L"event move hand", .install = Call<&players::event_move_hand::Install>, .remove = &players::event_move_hand::Remove,
           .needs = {Need<&world::named_actor::Install>()}},
    // the game's single slots answered for a seat for one call: the battle modules need it
    Module{.name = L"scopes", .install = Call<&players::scopes::Install>},

    // Each further player's own battle: its command menu, ticked in its scopes, and the switch rules.
    // without it the game warps the whole party
    Module{.name = L"battle warps", .install = Call<&battle::battle_warps::Install>, .remove = &battle::battle_warps::Remove,
           .needs = {Need<&players::scopes::Install>()}},
    // a further player's command controller: built, follows the game's
    Module{.name = L"command menu", .install = Call<&battle::command_menu::Install>, .remove = &battle::command_menu::Remove,
           .needs = {Need<&players::scopes::Install>()}},
    // ticked in battle; a further player's attack, guard and dodge
    Module{.name = L"command menu tick", .install = Call<&battle::command_menu_tick::Install>, .remove = &battle::command_menu_tick::Remove,
           .needs = {Need<&battle::command_menu::Install>()}},
    // without it an open menu greys the screen with the slow-down off
    Module{.name = L"menu grey", .install = Call<&battle::menu_grey::Install>, .remove = &battle::menu_grey::Remove,
           .needs = {Need<&battle::command_menu_tick::Install>()}},
    // without it one player's low health lights the red edge in every view (its views' site is the shadows' call)
    Module{.name = L"low health", .install = Call<&render::low_health::Install>, .remove = &render::low_health::Remove,
           .needs = {Need<&battle::command_menu_tick::Install>(), Need<&render::shadows_per_view::Install>()}},
    // without it a player locked outside a fight stays there
    Module{.name = L"arena warp", .install = Call<&battle::arena_warp::Install>, .needs = {Need<&battle::command_menu_tick::Install>()}},
    // player 1's switch skips a further player's member
    Module{.name = L"party switch", .install = Call<&battle::party_switch::Install>, .remove = &battle::party_switch::Remove,
           .needs = {Need<&battle::command_menu::Install>()}},
    // logging only: what the field encounter's groups see, for the option "enemies notice players 2 and 3"
    Module{.name = L"encounter watch", .install = Call<&battle::encounter_watch::Install>, .remove = &battle::encounter_watch::Remove},
    // without it a toad on player 2's or 3's character leaves that player's camera on the hidden character
    Module{.name = L"seat toad", .install = Call<&battle::seat_toad::Install>, .remove = &battle::seat_toad::Remove,
           .needs = {Need<&players::scopes::Install>()}},
    // a further player's targeting: the game's tick run again for it
    Module{.name = L"targeting", .install = Call<&battle::targeting::Install>, .remove = &battle::targeting::Remove,
           .needs = {Need<&battle::command_menu::Install>()}},
    // without it both halves show player 1's cursor
    Module{.name = L"target cursor", .install = Call<&battle::target_cursor::Install>, .remove = &battle::target_cursor::Remove,
           .needs = {Need<&battle::targeting::Install>()}},
    // the lock camera on each player's own view
    Module{.name = L"lock camera", .install = Call<&battle::lock_camera::Install>, .remove = &battle::lock_camera::Remove,
           .needs = {Need<&battle::command_menu::Install>()}},
    // without it the aim reading is off, the UI scope stays
    Module{.name = L"command camera", .install = Call<&battle::command_camera::Install>, .remove = &battle::command_camera::Remove,
           .needs = {Need<&battle::command_menu::Install>()}},
    // without it only player 1's menu offers Summon
    Module{.name = L"summon seats", .install = Call<&battle::summon_seats::Install>, .remove = &battle::summon_seats::Remove,
           .needs = {Need<&battle::command_menu::Install>()}},
    // a further player's member at the player rates (ATB, limit)
    Module{.name = L"member rates", .install = Call<&battle::member_rates::Install>, .remove = &battle::member_rates::Remove,
           .needs = {Need<&battle::command_menu::Install>()}},
    // the game's own leader reads for an enemy follow the coin
    Module{.name = L"enemy leader", .install = Call<&battle::enemy_leader::Install>, .remove = &battle::enemy_leader::Remove,
           .needs = {Need<&battle::command_menu::Install>()}},
    // without it enemies name player 1 as the player
    Module{.name = L"enemy coin", .install = Call<&battle::enemy_coin::Install>, .remove = &battle::enemy_coin::Remove,
           .needs = {Need<&battle::command_menu::Install>()}},
    // without it every enemy's HP, stagger gauge and damage and every award are the game's (the battle's rates); after the
    // coin, whose battle-start slot names the enemy being started
    Module{.name = L"battle rates", .install = Call<&battle::battle_rates::Install>, .remove = &battle::battle_rates::Remove,
           .needs = {Need<&battle::enemy_coin::Install>()}},
    // without it an enemy aimed at a far human it cannot reach waits until that human comes near
    Module{.name = L"enemy stall", .install = Call<&battle::enemy_stall::Install>, .remove = &battle::enemy_stall::Remove,
           .needs = {Need<&battle::enemy_coin::Install>()}},
    // logging only: fed by the coin's decision gate and the stall's ready test
    Module{.name = L"enemy watch", .install = Call<&battle::enemy_watch::Install>, .remove = &battle::enemy_watch::Remove,
           .needs = {Need<&battle::enemy_coin::Install>()}},
    // battle camera sequences on each player's own view
    Module{.name = L"sequence camera", .install = Call<&battle::sequence_camera::Install>, .remove = &battle::sequence_camera::Remove,
           .needs = {Need<&battle::command_menu::Install>()}},

    // The game's state as readings, and the world around the views.
    // readings only; fine without
    Module{.name = L"game state", .install = Call<&players::game_state::Install>},
    // without it no level state trigger is sent from the console (the repair of a spot's state left on)
    Module{.name = L"state signal", .install = Call<&world::state_signal::Install>,
           .needs = {Need<&world::data_rows::Install>(), Need<&players::game_state::Install>()}},
    // without it the split stays through the whole-screen moments in one window (the mirror look)
    Module{.name = L"split switch", .install = Call<&players::split_switch::Install>},
    // without it no moment collapses the split in one window
    Module{.name = L"whole screen", .install = Call<&views::whole_screen::Install>,
           .needs = {Need<&players::game_state::Install>()}},
    // without it a member near a further player's camera vanishes in both halves
    Module{.name = L"party fade", .install = Call<&world::party_fade::Install>},
    // without it characters in a ladder or squeeze turn see-through near another (removed any time: its slots pass
    // through and it writes no game state)
    Module{.name = L"overlap fade", .install = Call<&world::overlap_fade::Install>, .remove = &world::overlap_fade::Remove},
    // without it further players run free in a story walk (removed any time: its slots call the game's first; the
    // members keep the caps last copied)
    Module{.name = L"forced walk", .install = Call<&world::forced_walk::Install>, .remove = &world::forced_walk::Remove},
    // without it a further player's hook area uses player 1's search arrow (the seats' arrows cleared on the game thread)
    Module{.name = L"search arrows", .install = Call<&world::search_arrows::Install>, .remove = &world::search_arrows::Remove},
    // without it a further player's member fires its own story boxes as it walks into them, ahead of the story (removed
    // any time: the restore swaps the enter back; a waiting enter is then the next tick's or dropped, logged)
    Module{.name = L"story boxes", .install = Call<&world::story_boxes::Install>, .remove = &world::story_boxes::Remove},
    // the game's own move of a character beside another; without it nobody is brought over (the run catch-up and the talk
    // gather check it at their own install; the party warp row also names it)
    Module{.name = L"catch-up move", .install = Call<&world::catch_up_move::Install>},
    // without it a companion a further player took past its own story box runs back from there when the level's run
    // through the box goes to its AI (its step then plays where it stands)
    Module{.name = L"run catch-up", .install = Call<&world::run_catch_up::Install>,
           .needs = {Need<&world::story_boxes::Install>(), Need<&players::lead_gate::Install>()}},
    // without it a further player left behind in the field has no way back to player 1's character
    Module{.name = L"party warp", .install = Call<&world::party_warp::Install>,
           .needs = {Need<&world::catch_up_move::Install>(), Need<&battle::arena_warp::Install>()}},
    // without it characters near a further player animate by player 1's camera
    Module{.name = L"anim per view", .install = Call<&world::anim_views::Install>},
    // without it NPCs near a further player pause by player 1's camera
    Module{.name = L"npc detail", .install = Call<&world::npc_detail::Install>},
    // without it the crowd is spawned around player 1 only
    Module{.name = L"npc crowd", .install = Call<&world::npc_crowd::Install>},
    // without it NPCs see nobody beyond player 1's grid
    Module{.name = L"npc senses", .install = Call<&world::npc_senses::Install>},
    // without it only player 1's ground hides the area's actors
    Module{.name = L"env sets", .install = Call<&world::env_sets::Install>, .needs = {Need<&world::named_actor::Install>()}},
    // without it the world follows player 1's area alone (a further player's area unloads when player 1 leaves it)
    Module{.name = L"area levels", .install = Call<&world::area_levels::Install>, .remove = &world::area_levels::Remove,
           .needs = {Need<&world::env_sets::Install>()}},
    // logging only; without it the world's levels are not watched
    Module{.name = L"level watch", .install = Call<&world::level_watch::Install>, .remove = &world::level_watch::Remove},
    // The field hooks the seats' members use, removed only with every further player out (the field-action tick back
    // on any thread; the seats' triggers stop reaching the dispatcher; the presses and chest opens back to the
    // game's; the consumers of the presses first).
    // without it further players' members take ladders by themselves
    Module{.name = L"traversal action", .install = Call<&world::traversal_action::Install>, .remove = &world::traversal_action::Remove,
           .removal = Removal::WithPlayersOut},
    // without it a seat's trigger sweep (the walk-up below, removed before it) can crash the game on a member whose
    // referenced actor is not an End actor
    Module{.name = L"referenced actor", .install = Call<&world::referenced_actor::Install>, .remove = &world::referenced_actor::Remove,
           .removal = Removal::WithPlayersOut},
    // without it further players cannot use walk-up spots
    Module{.name = L"traversal walk-up", .install = Call<&world::traversal_walkup::Install>, .remove = &world::traversal_walkup::Remove,
           .removal = Removal::WithPlayersOut},
    // without it a spot's level state an earlier pass left on (a rail camera over every talk) stays until player 1's own
    // pass there or the console (without the traversal action module it never sends: nobody's action can be ruled out)
    Module{.name = L"stuck spots", .install = Call<&world::stuck_spots::Install>,
           .needs = {Need<&world::state_signal::Install>(), Need<&world::traversal_walkup::Install>()}},
    // without it further players cannot open chests or doors
    Module{.name = L"field targets", .install = Call<&world::field_targets::Install>, .remove = &world::field_targets::Remove,
           .removal = Removal::WithPlayersOut},
    // without it further players press no levers or switches of their own
    Module{.name = L"levers", .install = Call<&world::push_events::Install>, .remove = &world::push_events::Remove,
           .removal = Removal::WithPlayersOut, .needs = {Need<&world::field_targets::Install>()}},
    // without it a further player's shop is player 1's (camera, facing)
    Module{.name = L"shop act", .install = Call<&world::shop_act::Install>, .remove = &world::shop_act::Remove,
           .removal = Removal::WithPlayersOut, .needs = {Need<&world::field_targets::Install>()}},
    // without it player 1's character stays far from their talks
    Module{.name = L"talk gather", .install = Call<&world::talk_gather::Install>, .remove = &world::talk_gather::Remove,
           .removal = Removal::WithPlayersOut, .needs = {Need<&world::field_targets::Install>()}},
    // without it, every sound is player 1's (removed any time: it turns the seats off first; every hook then answers
    // as the game)
    Module{.name = L"sound listeners", .install = Call<&audio::listeners::Install>, .remove = &audio::listeners::Remove},

    // The HUD per view.
    // logging only; fine without
    Module{.name = L"hud probe", .install = Call<&hud::hud_probe::Install>, .remove = &hud::hud_probe::Remove},
    // without it each half shows the middle of the HUD (its removal takes the root placement's)
    Module{.name = L"overlay layout", .install = Call<&hud::overlay_layout::Install>, .remove = &RemoveOverlayLayout},
    // without it both halves sample one UI image
    Module{.name = L"ui targets", .install = Call<&hud::ui_targets::Install>, .remove = &hud::ui_targets::Remove},
    // without it the summon gauge's glow floats away from its bar in a split view (one particle, placed for one layout)
    Module{.name = L"ui effect pass", .install = Call<&hud::ui_effect_pass::Install>, .remove = &hud::ui_effect_pass::Remove},
    // without it a movie shows in the first view only
    Module{.name = L"movie frame", .install = Call<&render::movie_frame::Install>, .remove = &render::movie_frame::Remove},
    // the further players' HUD image: its entries, each seat's minimap, the image itself
    Module{.name = L"image entries", .install = Call<&hud::image_entries::Install>, .remove = &hud::image_entries::Remove},
    Module{.name = L"minimap", .install = Call<&hud::minimap::Install>, .remove = &hud::minimap::Remove,
           .needs = {Need<&hud::image_entries::Install>()}},
    Module{.name = L"second image", .install = Call<&hud::second_image::Install>, .remove = &hud::second_image::Remove,
           .needs = {Need<&hud::image_entries::Install>(), Need<&hud::minimap::Install>()}},
    // the markers, field icons and off-screen icons placed per view, in the image
    Module{.name = L"marker replay", .install = Call<&hud::marker_replay::Install>, .remove = &hud::marker_replay::Remove,
           .needs = {Need<&hud::second_image::Install>()}},
    Module{.name = L"field icons", .install = Call<&hud::field_icons::Install>, .remove = &hud::field_icons::Remove,
           .needs = {Need<&hud::second_image::Install>()}},
    Module{.name = L"outside icons", .install = Call<&hud::outside_icons::Install>, .remove = &hud::outside_icons::Remove,
           .needs = {Need<&hud::second_image::Install>()}},
    // without it a further player's lead the level needs finished goes to its AI (no marker to show the way)
    Module{.name = L"lead mark", .install = Call<&hud::lead_mark::Install>, .remove = &hud::lead_mark::Remove,
           .needs = {Need<&hud::field_icons::Install>(), Need<&hud::minimap::Install>()}},
};

/// The row of an installer, or the table's size if none has it.
constexpr std::size_t IndexOfInstaller(Installer install) noexcept
{
    for (std::size_t i = 0; i < kModules.size(); ++i)
    {
        if (kModules[i].install == install)
        {
            return i;
        }
    }
    return kModules.size();
}

/// The row of a module's Install, or the table's size if it has none.
template <auto Install>
constexpr std::size_t IndexOf() noexcept
{
    return IndexOfInstaller(&Call<Install>);
}

/// Both modules are in the table, Before's row ahead of After's.
template <auto Before, auto After>
constexpr bool Ordered() noexcept
{
    return IndexOf<Before>() < IndexOf<After>() && IndexOf<After>() < kModules.size();
}

constexpr bool EachOnce() noexcept
{
    for (std::size_t i = 0; i < kModules.size(); ++i)
    {
        for (std::size_t j = i + 1; j < kModules.size(); ++j)
        {
            if (kModules[i].install == kModules[j].install)
            {
                return false;
            }
        }
    }
    return true;
}

/// Every row's needs are rows of the table, ahead of it.
constexpr bool NeedsPrecede() noexcept
{
    for (std::size_t i = 0; i < kModules.size(); ++i)
    {
        for (const Installer need : kModules[i].needs)
        {
            if (need != nullptr && IndexOfInstaller(need) >= i)
            {
                return false;
            }
        }
    }
    return true;
}

// The order constraints the modules carry. A row moved past its dependency fails the build here. The weak pointers'
// and the field root's place before everything, and the session's before the table, are the entry point's.
static_assert(EachOnce(), "a module is in the table twice");
static_assert(NeedsPrecede(), "a row names a need that is not an earlier row of the table");
static_assert(Ordered<&render::shadows_per_view::Install, &render::area_blend::Install>(),
              "the shadows before the area blend: its site is the shadows' call");
static_assert(Ordered<&views::window_span::Install, &views::mirror_player::Install>(),
              "the window span before the mirror player: the mirrors fill the span's columns");
static_assert(Ordered<&world::field_targets::Install, &world::shop_act::Install>() &&
                  Ordered<&world::field_targets::Install, &world::talk_gather::Install>(),
              "the field targets before the consumers of the presses: removed in reverse, the consumers go first");

constexpr std::size_t kNoRow = kModules.size();

/// Each row's needs as rows of the table, worked out at compile time: the run-time walk compares no function
/// addresses (the Shipping link folds identical functions, so two installers could share one address there).
constexpr auto kNeedRows = [] {
    std::array<std::array<std::size_t, 2>, kModules.size()> rows{};
    for (std::size_t i = 0; i < kModules.size(); ++i)
    {
        for (std::size_t k = 0; k < rows[i].size(); ++k)
        {
            const Installer need = kModules[i].needs[k];
            rows[i][k]           = need != nullptr ? IndexOfInstaller(need) : kNoRow;
        }
    }
    return rows;
}();

/// The row the entry point asks about after the table.
constexpr std::size_t kMirrorPlayerRow = IndexOf<&views::mirror_player::Install>();
static_assert(kMirrorPlayerRow < kModules.size(), "the mirror player is a row of the table");

/// Each row's outcome at Install (false: it stood aside, or its need had).
std::array<bool, kModules.size()> g_installed{};

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    for (std::size_t i = 0; i < kModules.size(); ++i)
    {
        const Module& row = kModules[i];
        std::wstring  missing;
        for (const std::size_t need : kNeedRows[i])
        {
            if (need != kNoRow && !g_installed[need])
            {
                missing += missing.empty() ? L"" : L" and ";
                missing += kModules[need].name;
            }
        }
        if (!missing.empty())
        {
            platform::Log(L"{}: stands aside: {} not installed", row.name, missing);
            g_installed[i] = false;
            continue;
        }
        g_installed[i] = row.install(image);
    }
}

void Remove() noexcept
{
    for (auto row = kModules.rbegin(); row != kModules.rend(); ++row)
    {
        if (row->remove != nullptr && row->removal == Removal::Always)
        {
            row->remove();
        }
    }
}

void RemoveWithPlayersOut() noexcept
{
    for (auto row = kModules.rbegin(); row != kModules.rend(); ++row)
    {
        if (row->remove != nullptr && row->removal == Removal::WithPlayersOut)
        {
            row->remove();
        }
    }
}

bool MirrorPlayerInstalled() noexcept
{
    return g_installed[kMirrorPlayerRow];
}

void TickBeforeSession(const void* managerOne) noexcept
{
    players::game_state::Tick();  // the session reads this tick's values (the console's lines, before it, the last tick's)
    views::whole_screen::Tick(managerOne);  // the whole-screen read over them (the column source's moment, after the session)
    players::pad_presence::Tick();
}

Columns TickColumns(const ColumnsIn& in) noexcept
{
    views::letterbox_patch::Tick(in.playersIn > 1 || views::window_span::Held());
    views::window_span::Tick(in.playersIn > 1);
    // The columns this frame shows (views/column_rules.h): the players, the span's while held, or the console's.
    Columns columns;
    columns.held        = views::window_span::Held() ? views::window_span::Columns() : 0;
    columns.wanted      = views::column_rules::Wanted(in.playersIn, columns.held, views::column_layout::Override());
    columns.alone       = views::column_rules::Wanted(1, columns.held, views::column_layout::Override());
    columns.columnsHeld = columns.held > 0 || views::column_layout::Override() > 0;  // beyond the players
    // The layout asks whether the window spans monitors at all (held or entered at a join), not only while held.
    views::column_layout::Tick(columns.wanted, in.playersIn + views::mirror_player::Count(), views::window_span::Columns(),
                               views::window_span::ColumnWidths(), views::window_span::ColumnHeights(), in.listSeats);
    views::mirror_player::Tick(in.playersIn, in.seatIds, columns.wanted, columns.alone);
    // Whenever the letterbox patch is on the views are in window pixels, one view included: the span's hold keeps it on
    // with a further player awaiting a member, since otherwise player 1's view would streak past the resolution on a
    // wider monitor.
    views::split_scale::Tick(views::letterbox_patch::Applied());
    return columns;
}

namespace {

bool g_mirrorsOwnImage = false;  // the mirrors' columns had HUD passes of their own last tick (an Info line per change)

/// The world around the views (world/): party members kept visible near a camera, characters animated by the
/// camera that sees them best, the NPCs near every further player kept awake and spawned around them too, and
/// able to see them. Every world module per built seat: each told the seat's member, camera
/// or controller while the seat's player is in, then its once-a-tick work.
void TickWorld(const Frame& frame) noexcept
{
    bool anyIn = false;
    world::party_fade::Tick(frame.count);
    world::overlap_fade::Tick(frame.count);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const Frame::Seat& s           = frame.seats[static_cast<std::size_t>(seat)];
        const bool         seatIn      = s.In();
        void*              member      = seatIn ? s.member : nullptr;
        void*              camera      = seatIn ? s.manager : nullptr;
        void*              controller  = seatIn ? s.controller : nullptr;
        void*              localPlayer = seatIn ? s.localPlayer : nullptr;
        anyIn                          = anyIn || seatIn;
        world::anim_views::TickSeat(seat, camera, member);
        world::npc_crowd::TickSeat(seat, member);
        world::npc_senses::TickSeat(seat, member);
        players::ai_move_guard::TickSeat(seat, member);
        world::npc_detail::TickSeat(seat, camera);
        world::env_sets::TickSeat(seat, member, controller);
        world::traversal_walkup::TickSeat(seat, controller, member);
        world::traversal_action::TickSeat(seat, member);
        world::forced_walk::TickSeat(seat, member);
        world::story_boxes::TickSeat(seat, member);
        players::nav_area::TickSeat(seat, member);
        world::field_targets::TickSeat(seat, localPlayer, member);
        world::push_events::TickSeat(seat, localPlayer, member);
        world::search_arrows::TickSeat(seat, localPlayer);
        audio::listeners::TickSeat(seat, controller);
    }
    world::traversal_action::Tick(anyIn);
    world::stuck_spots::Tick();
    players::ai_move_guard::Tick();
    players::event_move_hand::Tick();
    world::story_boxes::Tick(frame.pawnOne);
    world::field_targets::Tick();
    world::push_events::Tick();
    world::search_arrows::Tick();
    world::anim_views::Tick();
    world::npc_crowd::Tick();
    world::npc_senses::Tick(anyIn ? frame.pawnOne : nullptr);
    world::npc_detail::Tick(anyIn ? frame.managerOne : nullptr);
    world::env_sets::Tick();
    world::area_levels::Tick();
    world::level_watch::Tick(anyIn);
    audio::listeners::Tick(frame.map.count);
}

/// A local player's view share (ULocalPlayer Origin and Size), read under a guard.
bool ReadView(const void* localPlayer, hud::overlay_layout::ViewShare& out) noexcept
{
    const auto* player = static_cast<const std::uint8_t*>(localPlayer);
    return player != nullptr && platform::CopyGuarded(&out.originX, player + facts::kLocalPlayerOrigin, 2 * sizeof(float)) &&
           platform::CopyGuarded(&out.sizeX, player + facts::kLocalPlayerSize, 2 * sizeof(float)) && out.sizeX > 0.0f &&
           out.sizeY > 0.0f && out.sizeX <= 1.0f && out.sizeY <= 1.0f;
}

/// Whether the mirrors' columns, `from` up to `read`, get HUD passes of their own: each column there is a mirror's (no
/// seat in the HUD's map, which lists a seat's column on player 1's picture as a mirror's: column_source::HudMap) and
/// one of them has another shape than player 1's view (overlay_pass::OtherShape: a monitor of another width in a span),
/// or `always` (player 1's window shows: a copy must not show it, so each copy draws its own HUD). Then every mirror's
/// column gets a pass (the columns with an image of their own stay the first ones, as the target and slot rules
/// expect), showing player 1's HUD laid out for its own monitor.
bool MirrorsOwnImage(const hud::overlay_layout::ViewShares& shares, int from, int read, bool always) noexcept
{
    namespace op = hud::overlay_pass;
    bool other = always;
    for (int column = std::max(from, 1); column < read; ++column)
    {
        if (op::SeatOf(column) != op::kNoSeat)
        {
            return false;  // a seat's own view among them: the seats' passes come first (`from` counts them)
        }
        other = other || op::OtherShape(shares[column].sizeX, shares[column].sizeY, shares[0].sizeX, shares[0].sizeY);
    }
    return other;
}

/// The HUD layout follows the columns (views/columns.h) and what each shows (views/column_source.h: column 0 is
/// player 1's, the seats' follow in the engine list's order, then the mirrors'). While no further column shows its
/// own view (the mirror players alone beside player 1, or every seat on player 1's picture), the HUD is laid out
/// for player 1's column and every view samples that one image: the further passes and the per-view targets stay
/// off, since only a further pass would draw into a second target, leaving those views empty. A mirror on a monitor of
/// another shape than player 1's (MirrorsOwnImage) gets a pass of its own instead, beside the seats' or alone with
/// player 1, so the game's cover fit does not enlarge or inset player 1's image there. A seat's column on player 1's
/// picture (a whole-screen moment on a monitor row) counts as a mirror's in the HUD's map (column_source::HudMap), so it
/// is handled the same: a menu or a subtitle shows whole on a wider monitor, and a pass that runs for it (the passes
/// cannot skip a middle column) shows player 1's HUD, never the seat's own over player 1's camera.
void TickHud(const Frame& frame) noexcept
{
    namespace op = hud::overlay_pass;
    hud::second_image::Columns columns;
    const int  players     = frame.shown.SplitShown() ? std::min(1 + frame.count, op::kMaxColumns) : 1;  // the players' columns
    const int  viewColumns = std::min(frame.map.count, op::kMaxColumns);
    hud::overlay_layout::ViewShares shares{};
    int                             read = 0;  // the columns whose views were read, from the first
    for (int column = 0; column < viewColumns && ReadView(frame.localPlayer[static_cast<std::size_t>(column)], shares[column]); ++column)
    {
        read = column + 1;
    }
    // A split, or the mirror beside player 1; not on the frame the single view's switch is written (the engine's sizes
    // change at the redraw, after this tick: the switch set means it shows, the fallback above decides otherwise).
    const bool views      = read >= 2 && !frame.shown.Single();
    const bool ownMirrors = views && read > players && MirrorsOwnImage(shares, players, read, lss::players::player_window::PlayerOneShown());
    columns.count         = ownMirrors ? read : players;  // the columns with a pass
    // The mirrors beside player 1 sample player 1's image (the single view: one view, nothing to sample).
    const bool mirror = !frame.shown.SplitShown() && frame.map.count > 1 && !frame.shown.Single() && !ownMirrors;
    const bool split  = views && columns.count > 1 && read >= columns.count;
    if (ownMirrors != g_mirrorsOwnImage)
    {
        g_mirrorsOwnImage = ownMirrors;
        platform::Log(L"HUD: {}", ownMirrors ? std::format(L"the mirrors' columns get HUD images of their own ({}; columns {}-{})",
                                                           lss::players::player_window::PlayerOneShown()
                                                               ? L"player 1's window shows: never on a copy"
                                                               : L"a monitor of another shape than player 1's view",
                                                           players + 1, read)
                                             : std::wstring(L"the mirrors' columns sample player 1's HUD image"));
    }
    hud::ui_targets::SetPlayerColumns(columns.count);  // a mirror's column beyond them samples player 1's HUD image
    hud::overlay_layout::Tick(views, shares, read);
    if (mirror && hud::ui_targets::Enabled())
    {
        hud::ui_targets::SetEnabled(false);
        platform::Log(L"HUD targets: off while every further column shows player 1's picture (each column samples player 1's HUD "
                      L"image; a further player's own view turns them on again)");
    }
    for (int column = 0; split && column < columns.count; ++column)
    {
        columns.localPlayer[column] = frame.localPlayer[static_cast<std::size_t>(column)];
        columns.controller[column]  = frame.controller[static_cast<std::size_t>(column)];
        // A further column's seat from the map: its member, read live and ungated as before (the frame's member is
        // null for a seat not in the game; a column's seat is in by the map).
        const int seat         = op::SeatOf(column);
        columns.member[column] = seat >= 1 ? players::scopes::Member(seat) : nullptr;
    }
    hud::second_image::Tick(split, columns);
    hud::minimap::Tick();
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const Frame::Seat& s = frame.seats[static_cast<std::size_t>(seat)];
        hud::minimap::TickSeat(seat, split && s.seated, split ? s.controller : nullptr);
        hud::lead_mark::TickSeat(seat, s.In() ? s.member : nullptr);
    }
    hud::marker_replay::Tick();
    hud::field_icons::Tick();
    hud::outside_icons::Tick();
    hud::root_placement::Tick(views && hud::overlay_layout::BandApplies());
}

/// Each further player's battle: its command menu first (ticked in its scopes), then the modules that read it.
void TickBattle(const Frame& frame) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        battle::command_menu::Tick(seat, frame.seats[static_cast<std::size_t>(seat)].seated);
    }
    battle::command_menu_tick::Tick();
    players::nav_area::Tick();
    battle::arena_warp::Tick();
    battle::menu_grey::Tick();
    battle::party_switch::Tick();
    battle::seat_toad::Tick();
    battle::encounter_watch::Tick();
    players::operator_battle_mode::Tick();
    battle::targeting::Tick();
    battle::target_cursor::Tick();
    battle::lock_camera::Tick();
    battle::member_rates::Tick();
    battle::enemy_leader::Tick();
    battle::enemy_coin::Tick();
    battle::battle_rates::Tick();
    battle::enemy_stall::Tick();
    battle::enemy_watch::Tick();
    battle::sequence_camera::Tick();
}

}  // namespace

void TickAfterSession(const Frame& frame) noexcept
{
    players::split_render::Tick(frame.map.count > 1);
    render::area_blend::Tick(frame.map.count > 1);  // before this frame's views
    render::low_health::Tick(frame.map);
    views::scene_camera::SeatManagers seatManagers{};
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        seatManagers[static_cast<std::size_t>(seat)] = frame.seats[static_cast<std::size_t>(seat)].manager;
    }
    views::scene_camera::Tick(frame.managerOne, seatManagers);
    views::narrow_camera::SeatPlayers viewPlayers{frame.localPlayer[0]};
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        viewPlayers[static_cast<std::size_t>(seat)] = frame.seats[static_cast<std::size_t>(seat)].localPlayer;
    }
    views::narrow_camera::Tick(viewPlayers);
    TickBattle(frame);
    TickWorld(frame);
    TickHud(frame);
    menu::coop_menu::Tick();
    hud::view_window::Tick();
}

}  // namespace lss::modules
