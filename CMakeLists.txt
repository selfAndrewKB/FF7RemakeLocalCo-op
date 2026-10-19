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
    src/platform/call_redirect.cpp
    src/platform/guarded.cpp
    src/platform/image.cpp
    src/platform/log.cpp
    src/platform/module_pin.cpp
    src/platform/code_memory.cpp
    src/platform/code_redirect.cpp
    src/platform/verified_code.cpp
    src/platform/vtable_entry.cpp
    src/players/field_exit.cpp
    src/players/game_state.cpp
    src/players/own_controller.cpp
    src/players/party_seat.cpp
    src/players/player_two_camera.cpp
    src/players/session.cpp
    src/players/split_render.cpp
    src/players/unreal_call.cpp
    src/render/ao_origin.cpp
    src/render/bloom_origin.cpp
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

add_executable(CodeMemoryTests
    tests/code_memory_test.cpp
    src/platform/code_memory.cpp
)
target_include_directories(CodeMemoryTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CodeMemoryTests PRIVATE cxx_std_20)
add_test(NAME code_memory COMMAND CodeMemoryTests)
