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
    get_filename_component(_parent "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    set(PSPRECOMP_RUNTIME_DIR "${_parent}/psp-native-runtime")
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

add_executable(BBSBringup
    host/main.cpp
    host/disc_io.cpp
    host/thread_manager.cpp
    host/partition_memory.cpp
    "${BBS_GENERATED_DIR}/generated_registry.cpp"
    ${BBS_GENERATED_UNITS})
target_link_libraries(BBSBringup PRIVATE psprecomp_core)
target_include_directories(BBSBringup PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/host" "${BBS_GENERATED_DIR}")

# Bring-up builds need fast feedback and manageable compiler memory. Raise the
# optimization level only after the boot path is stable.
if(MSVC)
    set_source_files_properties(${BBS_GENERATED_UNITS} "${BBS_GENERATED_DIR}/generated_registry.cpp"
        PROPERTIES COMPILE_OPTIONS "/Od;/Ob0;/bigobj;/GL-")
else()
    set_source_files_properties(${BBS_GENERATED_UNITS} "${BBS_GENERATED_DIR}/generated_registry.cpp"
        PROPERTIES COMPILE_OPTIONS "-O0;-g0")
endif()
