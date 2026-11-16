# ============================================================================
# Kingdom Hearts: Birth by Sleep - Android native library (libbbs.so), 2026-11-16.
#
# Same sources as ../CMakeLists.txt (BBSBringup) with the Vulkan GE backend in place of DX12,
# plus jni/bbs_jni.cpp (runs BBS's main() on a game thread, SurfaceView -> Vulkan surface).
# Built directly with the NDK toolchain + Ninja (see build-android.sh), then packaged by Gradle (app/).
# ============================================================================
cmake_minimum_required(VERSION 3.22)
project(bbs_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(BBS_REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
# The engine: the runtime/ submodule (git submodule update --init), or a sibling psp-native-runtime checkout.
set(PSPRECOMP_RUNTIME_DIR "" CACHE PATH "psp-native-runtime checkout")
if(NOT PSPRECOMP_RUNTIME_DIR)
    if(EXISTS "${BBS_REPO}/runtime/CMakeLists.txt")
        set(PSPRECOMP_RUNTIME_DIR "${BBS_REPO}/runtime")
    else()
        get_filename_component(PSPRECOMP_RUNTIME_DIR "${BBS_REPO}/../psp-native-runtime" ABSOLUTE)
    endif()
endif()
set(RT "${PSPRECOMP_RUNTIME_DIR}")
set(VCS "${RT}/profiles/vcs/host")
set(HS "${RT}/host-shared")
# Generated from your own disc by scripts/1-3 (never committed).
set(BBS_GENERATED_DIR "${BBS_REPO}/game/generated" CACHE PATH "BBS generated code (scripts/3-GENERATE.sh)")
set(BBS_GENERATED_OPT "-O1" CACHE STRING "Optimization flag for the generated units")
set(PSP_PYTHON "python" CACHE FILEPATH "Python 3 for embed_spirv.py")
set(PSP_GLSLC "" CACHE FILEPATH "glslc (NDK shader-tools)")

# SPIR-V header, generated at build time with the NDK glslc.
set(SPV_DIR "${CMAKE_BINARY_DIR}/spv")
set(SPV_HEADER "${SPV_DIR}/vulkan/ge_spv.hpp")
add_custom_command(OUTPUT "${SPV_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SPV_DIR}/vulkan"
    COMMAND "${PSP_PYTHON}" -I "${VCS}/vulkan/embed_spirv.py" "${PSP_GLSLC}" "${VCS}/vulkan" "${SPV_HEADER}"
    DEPENDS "${VCS}/vulkan/ge.vert" "${VCS}/vulkan/ge.frag" "${VCS}/vulkan/present.vert"
            "${VCS}/vulkan/present.frag" "${VCS}/vulkan/embed_spirv.py"
    COMMENT "Compiling GE/present shaders to SPIR-V")
add_custom_target(bbs_spirv DEPENDS "${SPV_HEADER}")

file(GLOB BBS_GENERATED_UNITS CONFIGURE_DEPENDS "${BBS_GENERATED_DIR}/generated_unit_*.cpp")
if(NOT BBS_GENERATED_UNITS)
    message(FATAL_ERROR "No generated_unit_*.cpp in ${BBS_GENERATED_DIR}")
endif()

# ge_renderer.cpp: clang rejects one array bound MSVC accepts (same patch as bringup/clang-build.sh).
set(GE_RENDERER_PATCHED "${CMAKE_BINARY_DIR}/ge_renderer_clang.cpp")
file(READ "${VCS}/ge_renderer.cpp" _ge_src)
string(REPLACE "std::array<std::uint32_t, regs.size()> values{};" "std::array<std::uint32_t, 23> values{};"
    _ge_src "${_ge_src}")
file(WRITE "${GE_RENDERER_PATCHED}.tmp" "${_ge_src}")
configure_file("${GE_RENDERER_PATCHED}.tmp" "${GE_RENDERER_PATCHED}" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VCS}/ge_renderer.cpp")

set(HOST_SOURCES
    ${RT}/src/decoder.cpp ${RT}/src/deflate.cpp ${RT}/src/elf32.cpp ${RT}/src/guest_memory.cpp
    ${RT}/src/nid_registry.cpp ${RT}/src/program_analysis.cpp ${RT}/src/runtime.cpp ${RT}/src/sha256.cpp
    ${HS}/ge_display.cpp ${HS}/guest_calls.cpp ${HS}/interrupts.cpp
    ${VCS}/framebuffer_capture.cpp ${GE_RENDERER_PATCHED} ${VCS}/ge_gpu_backend_vulkan.cpp ${VCS}/vcs_config.cpp
    ${VCS}/vcs_fps_overlay.cpp ${VCS}/vcs_project2dfx.cpp ${VCS}/vcs_project2dfx_lights.cpp
    ${VCS}/vcs_draw_distance_patch.cpp ${VCS}/vcs_runtime_log.cpp
    ${BBS_REPO}/host/main.cpp ${BBS_REPO}/host/disc_io.cpp
    ${BBS_REPO}/host/thread_manager.cpp ${BBS_REPO}/host/partition_memory.cpp
    jni/bbs_jni.cpp)

add_library(bbs SHARED ${HOST_SOURCES} "${BBS_GENERATED_DIR}/generated_registry.cpp" ${BBS_GENERATED_UNITS})
add_dependencies(bbs bbs_spirv)
target_compile_definitions(bbs PRIVATE VCS_VULKAN_GE_BACKEND)
target_include_directories(bbs PRIVATE "${RT}/include" "${VCS}" "${HS}" "${BBS_REPO}/host"
    "${BBS_GENERATED_DIR}" "${SPV_DIR}")
target_compile_options(bbs PRIVATE -w -fexceptions -frtti)
# BBS's main() becomes bbs_main(), called from the JNI game thread.
set_source_files_properties(${BBS_REPO}/host/main.cpp PROPERTIES COMPILE_DEFINITIONS "main=bbs_main")
set_source_files_properties(${HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
set_source_files_properties(${BBS_GENERATED_UNITS} "${BBS_GENERATED_DIR}/generated_registry.cpp"
    PROPERTIES COMPILE_OPTIONS "${BBS_GENERATED_OPT};-g0")
target_link_libraries(bbs PRIVATE vulkan android log)
target_link_options(bbs PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
