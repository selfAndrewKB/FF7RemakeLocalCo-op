cmake_minimum_required(VERSION 3.18)

# LocalSplitScreen: split-screen co-op for up to three players in FINAL FANTASY VII REMAKE, as a UE4SS C++ mod.
#
# Plan and research: Remake/Documentation/plan/README.md and research/RE-*.md.
# The platform layer (src/platform) is shared in design with Rebirth/LocalSplitScreen; every engine address
# here is Remake's own (src/engine_facts.h).
#
# This is a standalone CMake root. The one external dependency is RE-UE4SS, which provides the `UE4SS`
# target the mod links against. It is referenced where it already lives rather than copied.
project(LocalSplitScreen CXX)

set(UE4SS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../Rebirth/_tools/UE4SS_CppMods/RE-UE4SS"
    CACHE PATH "Path to the RE-UE4SS checkout that provides the UE4SS target")

if(NOT EXISTS "${UE4SS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "RE-UE4SS not found at UE4SS_ROOT='${UE4SS_ROOT}'. "
                        "Pass -DUE4SS_ROOT=<path to the RE-UE4SS checkout>.")
endif()

# The source lives outside this tree, so CMake needs an explicit build directory for it.
add_subdirectory("${UE4SS_ROOT}" "${CMAKE_BINARY_DIR}/RE-UE4SS")

add_library(LocalSplitScreen SHARED
    src/mod.cpp
    src/modules.cpp
    src/control/registry.cpp
    src/control/remote.cpp
    src/control/runner.cpp
    src/control/script.cpp
    src/control/server.cpp
    src/battle/command_camera.cpp
    src/battle/arena_warp.cpp
    src/battle/battle_rates.cpp
    src/battle/battle_warps.cpp
    src/battle/command_menu_tick.cpp
    src/battle/command_menu.cpp
    src/battle/menu_grey.cpp
    src/battle/enemy_coin.cpp
    src/battle/enemy_leader.cpp
    src/battle/enemy_stall.cpp
    src/battle/enemy_watch.cpp
    src/battle/lock_camera.cpp
    src/battle/member_rates.cpp
    src/battle/party_switch.cpp
    src/battle/encounter_watch.cpp
    src/battle/seat_toad.cpp
    src/battle/sequence_camera.cpp
    src/battle/target_cursor.cpp
    src/battle/targeting.cpp
    src/hud/hud_probe.cpp
    src/hud/field_icons.cpp
    src/hud/image_entries.cpp
    src/hud/lead_mark.cpp
    src/hud/marker_replay.cpp
    src/hud/minimap.cpp
    src/hud/outside_icons.cpp
    src/render/shadows_per_view.cpp
    src/hud/overlay_pass.cpp
    src/hud/pass_hide.cpp
    src/hud/save_notice.cpp
    src/hud/second_image.cpp
    src/hud/overlay_band.cpp
    src/hud/overlay_layout.cpp
    src/hud/root_placement.cpp
    src/hud/ui_targets.cpp
    src/hud/ui_effect_pass.cpp
    src/hud/user_widgets.cpp
    src/platform/byte_scope.cpp
    src/platform/call_redirect.cpp
    src/platform/guarded.cpp
    src/platform/image.cpp
    src/platform/instruction_patch.cpp
    src/platform/log.cpp
    src/platform/module_pin.cpp
    src/platform/name_text.cpp
    src/platform/object_class.cpp
    src/platform/settings.cpp
    src/platform/code_memory.cpp
    src/platform/code_redirect.cpp
    src/platform/entry_redirect.cpp
    src/platform/game_thread.cpp
    src/platform/verified_code.cpp
    src/platform/vtable_entry.cpp
    src/players/dpad_keys.cpp
    src/players/field_exit.cpp
    src/players/field_root.cpp
    src/players/game_globals.cpp
    src/players/game_state.cpp
    src/players/lead_gate.cpp
    src/players/local_player_list.cpp
    src/players/own_controller.cpp
    src/players/action_key.cpp
    src/players/ai_move_guard.cpp
    src/players/event_move_hand.cpp
    src/players/pad_buttons.cpp
    src/players/pad_presence.cpp
    src/players/pad_table.cpp
    src/players/player_window.cpp
    src/hud/view_prompt.cpp
    src/hud/view_window.cpp
    src/menu/coop_menu.cpp
    src/menu/coop_page.cpp
    src/menu/coop_page_open.cpp
    src/menu/coop_slider.cpp
    src/menu/game_ui.cpp
    src/menu/title_row.cpp
    src/players/party_events.cpp
    src/players/party_member.cpp
    src/players/party_seat.cpp
    src/players/player_trigger.cpp
    src/players/camera_override.cpp
    src/players/operator_battle_mode.cpp
    src/players/nav_area.cpp
    src/players/seat_camera.cpp
    src/players/scopes.cpp
    src/players/seat_objects.cpp
    src/players/session.cpp
    src/players/weak_object.cpp
    src/players/single_view.cpp
    src/players/split_render.cpp
    src/players/split_switch.cpp
    src/players/unreal_call.cpp
    src/render/ao_origin.cpp
    src/render/bloom_origin.cpp
    src/render/distortion_apply.cpp
    src/render/lighting_volume.cpp
    src/render/fog_volume_watch.cpp
    src/render/history_cut.cpp
    src/render/low_health.cpp
    src/render/movie_frame.cpp
    src/render/occlusion_exempt.cpp
    src/render/family_boundary.cpp
    src/render/frame_watch.cpp
    src/render/family_now.cpp
    src/render/motion_history.cpp
    src/render/view_marks.cpp
    src/render/one_family.cpp
    src/render/pass_marks.cpp
    src/render/view_places.cpp
    src/render/ssr_origin.cpp
    src/render/sun_cascades.cpp
    src/views/letterbox_patch.cpp
    src/views/column_layout.cpp
    src/views/column_source.cpp
    src/views/split_scale.cpp
    src/views/narrow_camera.cpp
    src/views/split_fov.cpp
    src/views/display_align.cpp
    src/views/display_config.cpp
    src/views/display_move.cpp
    src/views/game_window.cpp
    src/views/mirror_player.cpp
    src/views/scene_camera.cpp
    src/views/whole_screen.cpp
    src/views/monitor_list.cpp
    src/views/monitor_picks.cpp
    src/views/monitor_row.cpp
    src/views/record_file.cpp
    src/views/span_desktop.cpp
    src/views/window_span.cpp
    src/world/actor_place.cpp
    src/world/anim_views.cpp
    src/world/area_levels.cpp
    src/world/catch_up_move.cpp
    src/world/env_sets.cpp
    src/world/level_watch.cpp
    src/world/data_rows.cpp
    src/world/npc_crowd.cpp
    src/world/npc_detail.cpp
    src/world/named_actor.cpp
    src/world/npc_senses.cpp
    src/world/party_fade.cpp
    src/world/party_warp.cpp
    src/world/overlap_fade.cpp
    src/world/forced_walk.cpp
    src/world/search_arrows.cpp
    src/world/shop_act.cpp
    src/world/run_catch_up.cpp
    src/world/state_signal.cpp
    src/world/state_slots.cpp
    src/world/story_boxes.cpp
    src/world/stuck_spots.cpp
    src/world/talk_gather.cpp
    src/render/area_blend.cpp
    src/world/field_targets.cpp
    src/world/push_events.cpp
    src/world/target_icons.cpp
    src/world/traversal_action.cpp
    src/world/traversal_walkup.cpp
    src/world/referenced_actor.cpp
    src/audio/listeners.cpp
)

# Includes are rooted at src/ so every #include names its module ("platform/log.h").
target_include_directories(LocalSplitScreen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(LocalSplitScreen PRIVATE cxx_std_20)
target_link_libraries(LocalSplitScreen PUBLIC UE4SS)

# ------------------------------------------------------------------------------------------------
# The setup app: picks the monitors and writes LocalSplitScreen.ini; shares the mod's row rules and settings table
# ------------------------------------------------------------------------------------------------
enable_language(RC)

# The app's version is the mod's: read from the one version string, and read again whenever that file changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/mod.cpp)
file(STRINGS src/mod.cpp LSS_VERSION_LINE REGEX "kVersion = L\"" LIMIT_COUNT 1)
# Plain numbers, none starting with a zero: the resource compiler reads a leading zero as octal.
if(NOT LSS_VERSION_LINE MATCHES "L\"((0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)[^\"]*)\"")
    message(FATAL_ERROR "src/mod.cpp: kVersion not found as L\"major.minor.patch...\"")
endif()
set(LSS_VERSION "${CMAKE_MATCH_1}")
set(LSS_VERSION_MAJOR "${CMAKE_MATCH_2}")
set(LSS_VERSION_MINOR "${CMAKE_MATCH_3}")
set(LSS_VERSION_PATCH "${CMAKE_MATCH_4}")

# The app's file name is the one the mod starts its guard by: read from that line, and read again whenever its file changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/views/display_move.h)
file(STRINGS src/views/display_move.h LSS_SETUP_LINE REGEX "kSetupExe = L\"" LIMIT_COUNT 1)
if(NOT LSS_SETUP_LINE MATCHES "L\"([A-Za-z0-9_]+)\\.exe\"")
    message(FATAL_ERROR "src/views/display_move.h: kSetupExe not found as L\"Name.exe\"")
endif()
set(LSS_SETUP_NAME "${CMAKE_MATCH_1}")
configure_file(setup/setup_version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/setup_version.h" @ONLY)

add_executable(SetupApp WIN32
    setup/main.cpp
    setup/app.cpp
    setup/guard.cpp
    setup/tips.cpp
    setup/view.cpp
    setup/app.rc
    setup/app.manifest
    src/views/display_config.cpp
    src/views/display_move.cpp
    src/views/monitor_list.cpp
    src/views/monitor_picks.cpp
    src/views/monitor_row.cpp
    src/views/record_file.cpp
)
set_property(TARGET SetupApp PROPERTY OUTPUT_NAME "${LSS_SETUP_NAME}")
target_include_directories(SetupApp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}"
                                            "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(SetupApp PRIVATE cxx_std_20)
target_compile_definitions(SetupApp PRIVATE NOMINMAX)
target_link_libraries(SetupApp PRIVATE user32 gdi32 msimg32 comctl32 shell32 dwmapi uxtheme)
set_source_files_properties(setup/app.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/setup/app.ico")  # a new icon rebuilds it
set_property(TARGET SetupApp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")  # one file: no runtime to install

# ------------------------------------------------------------------------------------------------
# Host-side tests: the logic that needs neither the game nor UE4SS (run with --target <name>, then the exe)
# ------------------------------------------------------------------------------------------------
enable_testing()

add_executable(ControlScriptTests
    tests/control_script_test.cpp
    src/control/registry.cpp
    src/control/script.cpp
)
target_include_directories(ControlScriptTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ControlScriptTests PRIVATE cxx_std_20)
add_test(NAME control_script COMMAND ControlScriptTests)

add_executable(ControlRunnerTests
    tests/control_runner_test.cpp
    src/control/registry.cpp
    src/control/runner.cpp
    src/control/script.cpp
)
target_include_directories(ControlRunnerTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ControlRunnerTests PRIVATE cxx_std_20)
add_test(NAME control_runner COMMAND ControlRunnerTests)

add_executable(OverlayBandTests
    tests/overlay_band_test.cpp
    src/hud/overlay_band.cpp
)
target_include_directories(OverlayBandTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(OverlayBandTests PRIVATE cxx_std_20)
add_test(NAME overlay_band COMMAND OverlayBandTests)

add_executable(LayoutCodeTests
    tests/layout_code_test.cpp
    src/platform/code_memory.cpp
    src/platform/code_redirect.cpp
)
target_include_directories(LayoutCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(LayoutCodeTests PRIVATE cxx_std_20)
add_test(NAME layout_code COMMAND LayoutCodeTests)

add_executable(CodeMemoryTests
    tests/code_memory_test.cpp
    src/platform/code_memory.cpp
)
target_include_directories(CodeMemoryTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CodeMemoryTests PRIVATE cxx_std_20)
add_test(NAME code_memory COMMAND CodeMemoryTests)

add_executable(ImageCodeTests
    tests/image_code_test.cpp
    src/platform/code_memory.cpp
    src/platform/code_redirect.cpp
)
target_include_directories(ImageCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ImageCodeTests PRIVATE cxx_std_20)
add_test(NAME image_code COMMAND ImageCodeTests)

add_executable(EntryRedirectTests
    tests/entry_redirect_test.cpp
    src/platform/entry_redirect.cpp
    src/platform/module_pin.cpp
    src/platform/code_memory.cpp
)
target_include_directories(EntryRedirectTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(EntryRedirectTests PRIVATE cxx_std_20)
add_test(NAME entry_redirect COMMAND EntryRedirectTests)

add_executable(ByteScopeTests
    tests/byte_scope_test.cpp
    src/platform/byte_scope.cpp
    src/platform/guarded.cpp
)
target_include_directories(ByteScopeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ByteScopeTests PRIVATE cxx_std_20)
add_test(NAME byte_scope COMMAND ByteScopeTests)

add_executable(ActionTablesTests
    tests/action_tables_test.cpp
)
target_include_directories(ActionTablesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ActionTablesTests PRIVATE cxx_std_20)
add_test(NAME action_tables COMMAND ActionTablesTests)

add_executable(MenuPadsTests
    tests/menu_pads_test.cpp
)
target_include_directories(MenuPadsTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(MenuPadsTests PRIVATE cxx_std_20)
add_test(NAME menu_pads COMMAND MenuPadsTests)

add_executable(PadRulesTests
    tests/pad_rules_test.cpp
)
target_include_directories(PadRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(PadRulesTests PRIVATE cxx_std_20)
add_test(NAME pad_rules COMMAND PadRulesTests)

add_executable(PlayerWindowTests
    tests/player_window_test.cpp
)
target_include_directories(PlayerWindowTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(PlayerWindowTests PRIVATE cxx_std_20)
add_test(NAME player_window COMMAND PlayerWindowTests)

add_executable(PartyRankRulesTests
    tests/party_rank_rules_test.cpp
)
target_include_directories(PartyRankRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(PartyRankRulesTests PRIVATE cxx_std_20)
add_test(NAME party_rank_rules COMMAND PartyRankRulesTests)

add_executable(LowHealthRulesTests
    tests/low_health_rules_test.cpp
)
target_include_directories(LowHealthRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(LowHealthRulesTests PRIVATE cxx_std_20)
add_test(NAME low_health_rules COMMAND LowHealthRulesTests)

add_executable(PartySwitchRulesTests
    tests/party_switch_rules_test.cpp
)
target_include_directories(PartySwitchRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(PartySwitchRulesTests PRIVATE cxx_std_20)
add_test(NAME party_switch_rules COMMAND PartySwitchRulesTests)

add_executable(PushEventRulesTests
    tests/push_event_rules_test.cpp
)
target_include_directories(PushEventRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(PushEventRulesTests PRIVATE cxx_std_20)
add_test(NAME push_event_rules COMMAND PushEventRulesTests)

add_executable(SunOwnerTests
    tests/sun_owner_test.cpp
)
target_include_directories(SunOwnerTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SunOwnerTests PRIVATE cxx_std_20)
add_test(NAME sun_owner COMMAND SunOwnerTests)

add_executable(FamilySplitTests
    tests/family_split_test.cpp
)
target_include_directories(FamilySplitTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(FamilySplitTests PRIVATE cxx_std_20)
add_test(NAME family_split COMMAND FamilySplitTests)

add_executable(FamilyBoundaryListTests
    tests/family_boundary_list_test.cpp
)
target_include_directories(FamilyBoundaryListTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(FamilyBoundaryListTests PRIVATE cxx_std_20)
add_test(NAME family_boundary_list COMMAND FamilyBoundaryListTests)

add_executable(IndexRangesTests
    tests/index_ranges_test.cpp
)
target_include_directories(IndexRangesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(IndexRangesTests PRIVATE cxx_std_20)
add_test(NAME index_ranges COMMAND IndexRangesTests)

add_executable(InstructionPatchTests
    tests/instruction_patch_test.cpp
    src/platform/instruction_patch.cpp
    src/platform/code_memory.cpp
)
target_include_directories(InstructionPatchTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(InstructionPatchTests PRIVATE cxx_std_20)
add_test(NAME instruction_patch COMMAND InstructionPatchTests)

add_executable(FieldHoldTests
    tests/field_hold_test.cpp
)
target_include_directories(FieldHoldTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(FieldHoldTests PRIVATE cxx_std_20)
add_test(NAME field_hold COMMAND FieldHoldTests)

add_executable(StatePathTests
    tests/state_path_test.cpp
)
target_include_directories(StatePathTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(StatePathTests PRIVATE cxx_std_20)
add_test(NAME state_path COMMAND StatePathTests)

add_executable(AnimChoiceTests
    tests/anim_choice_test.cpp
)
target_include_directories(AnimChoiceTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(AnimChoiceTests PRIVATE cxx_std_20)
add_test(NAME anim_choice COMMAND AnimChoiceTests)

add_executable(CharaPopRuleTests
    tests/charapop_rule_test.cpp
)
target_include_directories(CharaPopRuleTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CharaPopRuleTests PRIVATE cxx_std_20)
add_test(NAME charapop_rule COMMAND CharaPopRuleTests)

add_executable(NpcSensesCodeTests
    tests/npc_senses_code_test.cpp
    src/platform/code_memory.cpp
    src/platform/code_redirect.cpp
)
target_include_directories(NpcSensesCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(NpcSensesCodeTests PRIVATE cxx_std_20)
add_test(NAME npc_senses_code COMMAND NpcSensesCodeTests)

add_executable(EnvSetViewTests
    tests/env_set_view_test.cpp
)
target_include_directories(EnvSetViewTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(EnvSetViewTests PRIVATE cxx_std_20)
add_test(NAME env_set_view COMMAND EnvSetViewTests)

add_executable(AreaLevelsRulesTests
    tests/area_levels_rules_test.cpp
)
target_include_directories(AreaLevelsRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(AreaLevelsRulesTests PRIVATE cxx_std_20)
add_test(NAME area_levels_rules COMMAND AreaLevelsRulesTests)

add_executable(SparseArrayTests
    tests/sparse_array_test.cpp
    src/platform/guarded.cpp
)
target_include_directories(SparseArrayTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SparseArrayTests PRIVATE cxx_std_20)
add_test(NAME sparse_array COMMAND SparseArrayTests)

add_executable(LeashRulesTests
    tests/leash_rules_test.cpp
)
target_include_directories(LeashRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(LeashRulesTests PRIVATE cxx_std_20)
add_test(NAME leash_rules COMMAND LeashRulesTests)

add_executable(LeadKeepRulesTests
    tests/lead_keep_rules_test.cpp
)
target_include_directories(LeadKeepRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(LeadKeepRulesTests PRIVATE cxx_std_20)
add_test(NAME lead_keep_rules COMMAND LeadKeepRulesTests)

add_executable(StallRulesTests
    tests/stall_rules_test.cpp
)
target_include_directories(StallRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(StallRulesTests PRIVATE cxx_std_20)
add_test(NAME stall_rules COMMAND StallRulesTests)

add_executable(CoinTableTests
    tests/coin_table_test.cpp
)
target_include_directories(CoinTableTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CoinTableTests PRIVATE cxx_std_20)
add_test(NAME coin_table COMMAND CoinTableTests)

add_executable(CoinChoiceTests
    tests/coin_choice_test.cpp
)
target_include_directories(CoinChoiceTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CoinChoiceTests PRIVATE cxx_std_20)
add_test(NAME coin_choice COMMAND CoinChoiceTests)

add_executable(MonitorRowTests
    tests/monitor_row_test.cpp
    src/views/monitor_row.cpp
)
target_include_directories(MonitorRowTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(MonitorRowTests PRIVATE cxx_std_20)
add_test(NAME monitor_row COMMAND MonitorRowTests)

add_executable(MonitorPicksTests
    tests/monitor_picks_test.cpp
    src/views/monitor_picks.cpp
    src/views/monitor_row.cpp
)
target_include_directories(MonitorPicksTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(MonitorPicksTests PRIVATE cxx_std_20)
add_test(NAME monitor_picks COMMAND MonitorPicksTests)

add_executable(RowJournalTests
    tests/row_journal_test.cpp
)
target_include_directories(RowJournalTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(RowJournalTests PRIVATE cxx_std_20)
add_test(NAME row_journal COMMAND RowJournalTests)

add_executable(TitleRowRulesTests
    tests/title_row_rules_test.cpp
)
target_include_directories(TitleRowRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(TitleRowRulesTests PRIVATE cxx_std_20)
add_test(NAME title_row_rules COMMAND TitleRowRulesTests)

add_executable(SetupRulesTests
    tests/setup_rules_test.cpp
    src/views/monitor_picks.cpp
    src/views/monitor_row.cpp
)
target_include_directories(SetupRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(SetupRulesTests PRIVATE cxx_std_20)
add_test(NAME setup_rules COMMAND SetupRulesTests)

add_executable(TypeAnswerCodeTests
    tests/type_answer_code_test.cpp
)
target_include_directories(TypeAnswerCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(TypeAnswerCodeTests PRIVATE cxx_std_20)
add_test(NAME type_answer_code COMMAND TypeAnswerCodeTests)

add_executable(TalkPressesTests
    tests/talk_presses_test.cpp
)
target_include_directories(TalkPressesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(TalkPressesTests PRIVATE cxx_std_20)
add_test(NAME talk_presses COMMAND TalkPressesTests)

add_executable(TalkGatherRulesTests
    tests/talk_gather_rules_test.cpp
)
target_include_directories(TalkGatherRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(TalkGatherRulesTests PRIVATE cxx_std_20)
add_test(NAME talk_gather_rules COMMAND TalkGatherRulesTests)

add_executable(PartyWarpRulesTests
    tests/party_warp_rules_test.cpp
)
target_include_directories(PartyWarpRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(PartyWarpRulesTests PRIVATE cxx_std_20)
add_test(NAME party_warp_rules COMMAND PartyWarpRulesTests)

add_executable(MenuGreyRulesTests
    tests/menu_grey_rules_test.cpp
)
target_include_directories(MenuGreyRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(MenuGreyRulesTests PRIVATE cxx_std_20)
add_test(NAME menu_grey_rules COMMAND MenuGreyRulesTests)

add_executable(BattleRulesTests
    tests/battle_rules_test.cpp
)
target_include_directories(BattleRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(BattleRulesTests PRIVATE cxx_std_20)
add_test(NAME battle_rules COMMAND BattleRulesTests)

add_executable(ColumnsTests
    tests/columns_test.cpp
)
target_include_directories(ColumnsTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ColumnsTests PRIVATE cxx_std_20)
add_test(NAME columns COMMAND ColumnsTests)

add_executable(ColumnSourceTests
    tests/column_source_test.cpp
    src/views/column_source.cpp
    src/control/registry.cpp
)
target_include_directories(ColumnSourceTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ColumnSourceTests PRIVATE cxx_std_20)
add_test(NAME column_source COMMAND ColumnSourceTests)

add_executable(WholeScreenRulesTests
    tests/whole_screen_rules_test.cpp
)
target_include_directories(WholeScreenRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(WholeScreenRulesTests PRIVATE cxx_std_20)
add_test(NAME whole_screen_rules COMMAND WholeScreenRulesTests)

add_executable(SingleViewTests
    tests/single_view_test.cpp
    src/players/single_view.cpp
)
target_include_directories(SingleViewTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SingleViewTests PRIVATE cxx_std_20)
add_test(NAME single_view COMMAND SingleViewTests)

add_executable(SettingsTests
    tests/settings_test.cpp
)
target_include_directories(SettingsTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SettingsTests PRIVATE cxx_std_20)
add_test(NAME settings COMMAND SettingsTests)

add_executable(BattleRatesRulesTests
    tests/battle_rates_rules_test.cpp
)
target_include_directories(BattleRatesRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(BattleRatesRulesTests PRIVATE cxx_std_20)
add_test(NAME battle_rates_rules COMMAND BattleRatesRulesTests)

add_executable(CoopMenuRulesTests
    tests/coop_menu_rules_test.cpp
)
target_include_directories(CoopMenuRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CoopMenuRulesTests PRIVATE cxx_std_20)
add_test(NAME coop_menu_rules COMMAND CoopMenuRulesTests)

add_executable(SeatsTests
    tests/seats_test.cpp
)
target_include_directories(SeatsTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SeatsTests PRIVATE cxx_std_20)
add_test(NAME seats COMMAND SeatsTests)

add_executable(OverlayPassTests
    tests/overlay_pass_test.cpp
)
target_include_directories(OverlayPassTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(OverlayPassTests PRIVATE cxx_std_20)
add_test(NAME overlay_pass COMMAND OverlayPassTests)

add_executable(BarClassTests
    tests/bar_class_test.cpp
)
target_include_directories(BarClassTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(BarClassTests PRIVATE cxx_std_20)
add_test(NAME bar_class COMMAND BarClassTests)

add_executable(CommandMenuRulesTests
    tests/command_menu_rules_test.cpp
)
target_include_directories(CommandMenuRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CommandMenuRulesTests PRIVATE cxx_std_20)
add_test(NAME command_menu_rules COMMAND CommandMenuRulesTests)

add_executable(LockCameraRulesTests
    tests/lock_camera_rules_test.cpp
)
target_include_directories(LockCameraRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(LockCameraRulesTests PRIVATE cxx_std_20)
add_test(NAME lock_camera_rules COMMAND LockCameraRulesTests)

add_executable(MinimapRulesTests
    tests/minimap_rules_test.cpp
)
target_include_directories(MinimapRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(MinimapRulesTests PRIVATE cxx_std_20)
add_test(NAME minimap_rules COMMAND MinimapRulesTests)

add_executable(TargetRotationTests
    tests/target_rotation_test.cpp
)
target_include_directories(TargetRotationTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(TargetRotationTests PRIVATE cxx_std_20)
add_test(NAME target_rotation COMMAND TargetRotationTests)

add_executable(SplitScaleRulesTests
    tests/split_scale_rules_test.cpp
)
target_include_directories(SplitScaleRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SplitScaleRulesTests PRIVATE cxx_std_20)
add_test(NAME split_scale_rules COMMAND SplitScaleRulesTests)

add_executable(SplitFovRulesTests
    tests/split_fov_rules_test.cpp
)
target_include_directories(SplitFovRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SplitFovRulesTests PRIVATE cxx_std_20)
add_test(NAME split_fov_rules COMMAND SplitFovRulesTests)

add_executable(ColumnRulesTests
    tests/column_rules_test.cpp
)
target_include_directories(ColumnRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ColumnRulesTests PRIVATE cxx_std_20)
add_test(NAME column_rules COMMAND ColumnRulesTests)

add_executable(SeatStateTests
    tests/seat_state_test.cpp
)
target_include_directories(SeatStateTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SeatStateTests PRIVATE cxx_std_20)
add_test(NAME seat_state COMMAND SeatStateTests)

add_executable(ArrowTimersTests
    tests/arrow_timers_test.cpp
)
target_include_directories(ArrowTimersTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ArrowTimersTests PRIVATE cxx_std_20)
add_test(NAME arrow_timers COMMAND ArrowTimersTests)

add_executable(ListenerRulesTests
    tests/listener_rules_test.cpp
)
target_include_directories(ListenerRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ListenerRulesTests PRIVATE cxx_std_20)
add_test(NAME listener_rules COMMAND ListenerRulesTests)

add_executable(ShadowCodeTests
    tests/shadow_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/code_memory.cpp
)
target_include_directories(ShadowCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ShadowCodeTests PRIVATE cxx_std_20)
add_test(NAME shadow_code COMMAND ShadowCodeTests)

add_executable(AreaChainsTests
    tests/area_chains_test.cpp
)
target_include_directories(AreaChainsTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(AreaChainsTests PRIVATE cxx_std_20)
add_test(NAME area_chains COMMAND AreaChainsTests)

add_executable(DxbcFixTests
    tests/dxbc_fix_test.cpp
)
target_include_directories(DxbcFixTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(DxbcFixTests PRIVATE cxx_std_20)
target_compile_definitions(DxbcFixTests PRIVATE LSS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
add_test(NAME dxbc_fix COMMAND DxbcFixTests)

add_executable(StoryBoxRulesTests
    tests/story_box_rules_test.cpp
)
target_include_directories(StoryBoxRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(StoryBoxRulesTests PRIVATE cxx_std_20)
add_test(NAME story_box_rules COMMAND StoryBoxRulesTests)

add_executable(HitchStepTests
    tests/hitch_step_test.cpp
)
target_include_directories(HitchStepTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(HitchStepTests PRIVATE cxx_std_20)
add_test(NAME hitch_step COMMAND HitchStepTests)

add_executable(StuckSpotRulesTests
    tests/stuck_spot_rules_test.cpp
)
target_include_directories(StuckSpotRulesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(StuckSpotRulesTests PRIVATE cxx_std_20)
add_test(NAME stuck_spot_rules COMMAND StuckSpotRulesTests)

add_executable(StateSlotsWalkTests
    tests/state_slots_walk_test.cpp
    src/platform/guarded.cpp
)
target_include_directories(StateSlotsWalkTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(StateSlotsWalkTests PRIVATE cxx_std_20)
add_test(NAME state_slots_walk COMMAND StateSlotsWalkTests)
