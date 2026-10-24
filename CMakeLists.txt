cmake_minimum_required(VERSION 3.18)

# LocalSplitScreen — two-player split-screen for FINAL FANTASY VII REMAKE, as a UE4SS C++ mod.
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
    src/control/registry.cpp
    src/control/remote.cpp
    src/control/runner.cpp
    src/control/script.cpp
    src/control/server.cpp
    src/battle/command_camera_two.cpp
    src/battle/command_menu_tick.cpp
    src/battle/command_menu_two.cpp
    src/battle/enemy_coin.cpp
    src/battle/enemy_leader.cpp
    src/battle/lock_camera_two.cpp
    src/battle/member_rates.cpp
    src/battle/party_switch.cpp
    src/battle/sequence_camera_two.cpp
    src/battle/target_cursor_two.cpp
    src/battle/targeting_two.cpp
    src/hud/hud_probe.cpp
    src/hud/field_icons.cpp
    src/hud/image_entries.cpp
    src/hud/marker_replay.cpp
    src/hud/minimap_two.cpp
    src/hud/outside_icons.cpp
    src/render/shadows_per_view.cpp
    src/hud/overlay_pass.cpp
    src/hud/pass_hide.cpp
    src/hud/second_image.cpp
    src/hud/overlay_band.cpp
    src/hud/overlay_layout.cpp
    src/hud/root_placement.cpp
    src/hud/ui_targets.cpp
    src/hud/user_widgets.cpp
    src/platform/byte_scope.cpp
    src/platform/call_redirect.cpp
    src/platform/guarded.cpp
    src/platform/image.cpp
    src/platform/instruction_patch.cpp
    src/platform/log.cpp
    src/platform/module_pin.cpp
    src/platform/settings.cpp
    src/platform/code_memory.cpp
    src/platform/code_redirect.cpp
    src/platform/entry_redirect.cpp
    src/platform/verified_code.cpp
    src/platform/vtable_entry.cpp
    src/players/field_exit.cpp
    src/players/game_state.cpp
    src/players/lead_gate.cpp
    src/players/local_player_list.cpp
    src/players/own_controller.cpp
    src/players/pad_buttons.cpp
    src/players/pad_presence.cpp
    src/players/party_events.cpp
    src/players/party_member.cpp
    src/players/party_seat.cpp
    src/players/player_trigger.cpp
    src/players/camera_override_two.cpp
    src/players/operator_battle_mode.cpp
    src/players/nav_area_two.cpp
    src/players/player_two_camera.cpp
    src/players/scopes.cpp
    src/players/seat_objects.cpp
    src/players/session.cpp
    src/players/weak_object.cpp
    src/players/split_render.cpp
    src/players/unreal_call.cpp
    src/render/ao_origin.cpp
    src/render/bloom_origin.cpp
    src/render/distortion_apply.cpp
    src/render/lighting_volume.cpp
    src/render/fog_volume_watch.cpp
    src/render/history_cut.cpp
    src/render/movie_frame.cpp
    src/render/occlusion_exempt.cpp
    src/render/ssr_origin.cpp
    src/render/sun_cascades.cpp
    src/views/letterbox_patch.cpp
    src/views/column_layout.cpp
    src/views/display_align.cpp
    src/views/mirror_player.cpp
    src/views/scene_camera_two.cpp
    src/views/monitor_row.cpp
    src/views/window_span.cpp
    src/world/anim_two_views.cpp
    src/world/env_sets_two.cpp
    src/world/npc_crowd.cpp
    src/world/npc_detail.cpp
    src/world/npc_senses.cpp
    src/world/party_fade.cpp
)

# Includes are rooted at src/ so every #include names its module ("platform/log.h").
target_include_directories(LocalSplitScreen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(LocalSplitScreen PRIVATE cxx_std_20)
target_link_libraries(LocalSplitScreen PUBLIC UE4SS)

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

add_executable(SunOwnerTests
    tests/sun_owner_test.cpp
)
target_include_directories(SunOwnerTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SunOwnerTests PRIVATE cxx_std_20)
add_test(NAME sun_owner COMMAND SunOwnerTests)

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

add_executable(TypeAnswerCodeTests
    tests/type_answer_code_test.cpp
)
target_include_directories(TypeAnswerCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(TypeAnswerCodeTests PRIVATE cxx_std_20)
add_test(NAME type_answer_code COMMAND TypeAnswerCodeTests)

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
