# ============================================================================
# Kingdom Hearts: Birth by Sleep — native bring-up (game repo)
#
# Per-title repo. Depends on the shared psp-native-runtime framework (sibling
# dir by default) and a LOCAL, git-ignored generated corpus.
#
#   cmake -S . -B out -DBBS_GENERATED_DIR=<...>/generated-psprecomp-fixed32k
#   cmake --build out --config Release --target BBSBringup --parallel 2
#
# BBS does not ship kjfs.prx / zlibdec.prx as disc PRXs (it packs BBS0-4.DAT
# and loads 9 minigame MODULE/*.ELF at runtime), so this bring-up links the
# EBOOT corpus only. KJFS/DAT reads and minigame PRX loading are added as the
# boot path reveals them.
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(kh-bbs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- Pull in the shared framework by local path (sibling dir by default) ----
set(PSPRECOMP_RUNTIME_DIR "" CACHE PATH
    "Path to the psp-native-runtime shared framework repo")
if(NOT PSPRECOMP_RUNTIME_DIR)
    # The runtime/ submodule (git submodule update --init), else a sibling psp-native-runtime checkout.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/CMakeLists.txt")
        set(PSPRECOMP_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
    else()
        get_filename_component(_parent "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
        set(PSPRECOMP_RUNTIME_DIR "${_parent}/psp-native-runtime")
    endif()
endif()
if(NOT EXISTS "${PSPRECOMP_RUNTIME_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "psp-native-runtime framework not found at ${PSPRECOMP_RUNTIME_DIR} "
        "(set -DPSPRECOMP_RUNTIME_DIR=...)")
endif()
set(PSPRECOMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${PSPRECOMP_RUNTIME_DIR}" "${CMAKE_BINARY_DIR}/psp-native-runtime")

# --- Local generated corpus (game-derived; never committed) ------------------
set(BBS_GENERATED_DIR "" CACHE PATH
    "Local directory containing the generated BBS C++ corpus")
if(NOT IS_DIRECTORY "${BBS_GENERATED_DIR}")
    message(FATAL_ERROR
        "Set BBS_GENERATED_DIR to your local generated-psprecomp-fixed32k directory")
endif()
file(GLOB BBS_GENERATED_UNITS CONFIGURE_DEPENDS
    "${BBS_GENERATED_DIR}/generated_unit_*.cpp")
if(NOT BBS_GENERATED_UNITS)
    message(FATAL_ERROR "No generated_unit_*.cpp files found in ${BBS_GENERATED_DIR}")
endif()
if(NOT EXISTS "${BBS_GENERATED_DIR}/generated_registry.cpp")
    message(FATAL_ERROR "generated_registry.cpp is missing from ${BBS_GENERATED_DIR}")
endif()

# VCS software renderer + its dependencies, compiled unchanged from
# psp-native-runtime/profiles/vcs/host (roadmap phase 2 step 1, the way
# sw-renegade-squadron reused them). Link-checked on the work PC 10/28: these 9
# files + d3d12/dxgi/d3dcompiler resolve every symbol. Nothing calls them yet;
# vcs_draw_distance_patch only runs from install_project2dfx, which we never call.
set(PSP_VCS_HOST_DIR "${PSPRECOMP_RUNTIME_DIR}/profiles/vcs/host")
set(PSP_VCS_RENDERER_SOURCES)
foreach(name framebuffer_capture ge_renderer ge_gpu_backend_dx12 vcs_config
        vcs_fps_overlay vcs_project2dfx vcs_project2dfx_lights
        vcs_draw_distance_patch vcs_runtime_log audio_output)
    list(APPEND PSP_VCS_RENDERER_SOURCES "${PSP_VCS_HOST_DIR}/${name}.cpp")
endforeach()
# GE HLEs shared by the game hosts (display lists -> the renderer above).
list(APPEND PSP_VCS_RENDERER_SOURCES
    "${PSPRECOMP_RUNTIME_DIR}/host-shared/ge_display.cpp"
    "${PSPRECOMP_RUNTIME_DIR}/host-shared/guest_calls.cpp"
    "${PSPRECOMP_RUNTIME_DIR}/host-shared/interrupts.cpp")

add_executable(BBSBringup
    host/main.cpp
    host/disc_io.cpp
    host/savedata.cpp
    host/overlays.cpp
    host/movie_skip.cpp
    host/sas_hle.cpp
    host/thread_manager.cpp
    host/partition_memory.cpp
    ${PSP_VCS_RENDERER_SOURCES}
    "${BBS_GENERATED_DIR}/generated_registry.cpp"
    ${BBS_GENERATED_UNITS})
target_link_libraries(BBSBringup PRIVATE psprecomp_core)
if(WIN32)
    target_link_libraries(BBSBringup PRIVATE d3d12 dxgi d3dcompiler dxguid winmm)
endif()
target_include_directories(BBSBringup PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/host" "${BBS_GENERATED_DIR}"
    "${PSPRECOMP_RUNTIME_DIR}/profiles/vcs/host" "${PSPRECOMP_RUNTIME_DIR}/host-shared")

# Host-side scheduler/frame-dump/GE test: the real host/thread_manager.cpp with
# fake guest threads, no game code. Not part of the normal build.
# Run: .\psp.ps1 test -Game <game>   (psp-native-runtime\bringup\tests\scheduler_test.cpp)
add_executable(SchedulerTest EXCLUDE_FROM_ALL
    "${PSPRECOMP_RUNTIME_DIR}/bringup/tests/scheduler_test.cpp"
    host/thread_manager.cpp
    ${PSP_VCS_RENDERER_SOURCES})
target_link_libraries(SchedulerTest PRIVATE psprecomp_core)
if(WIN32)
    target_link_libraries(SchedulerTest PRIVATE d3d12 dxgi d3dcompiler dxguid)
endif()
target_include_directories(SchedulerTest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/host" "${PSPRECOMP_RUNTIME_DIR}/profiles/vcs/host"
    "${PSPRECOMP_RUNTIME_DIR}/host-shared")
target_compile_definitions(SchedulerTest PRIVATE GAME_NS=bbs)

# Bring-up builds need fast feedback and manageable compiler memory. Raise the
# optimization level only after the boot path is stable.
if(MSVC)
    set_source_files_properties(${BBS_GENERATED_UNITS} "${BBS_GENERATED_DIR}/generated_registry.cpp"
        PROPERTIES COMPILE_OPTIONS "/Od;/Ob0;/bigobj;/GL-")
else()
    set_source_files_properties(${BBS_GENERATED_UNITS} "${BBS_GENERATED_DIR}/generated_registry.cpp"
        PROPERTIES COMPILE_OPTIONS "-O0;-g0")
endif()
