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
set(BBS_OVERLAY_DIR "${BBS_REPO}/game/overlays" CACHE PATH "BBS recompiled MODULE/*.ELF overlays (scripts/gen-overlays.sh)")
set(BBS_GENERATED_OPT "-O2" CACHE STRING "Optimization flag for the generated units")
# CPU baseline for everything in libbbs (2026-11-20). Empty = the NDK default (ARMv8.0: atomics go through
# __aarch64_* helper calls). ARMv8.2-A (every phone CPU since ~2018: Cortex-A55/A75 and later) inlines LSE atomics
# and allows fp16/dot-product code.
set(BBS_ARCH_FLAGS "" CACHE STRING "Extra -march/-mcpu flags for libbbs, e.g. -march=armv8.2-a+fp16+dotprod")
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
    ${VCS}/framebuffer_capture.cpp ${GE_RENDERER_PATCHED} ${VCS}/texture_replacer.cpp ${VCS}/ge_gpu_backend_vulkan.cpp ${VCS}/vcs_config.cpp
    ${VCS}/vcs_fps_overlay.cpp ${VCS}/vcs_project2dfx.cpp ${VCS}/vcs_project2dfx_lights.cpp
    ${VCS}/vcs_draw_distance_patch.cpp ${VCS}/vcs_runtime_log.cpp ${VCS}/audio_output_android.cpp
    ${BBS_REPO}/host/main.cpp ${BBS_REPO}/host/disc_io.cpp ${BBS_REPO}/host/savedata.cpp ${BBS_REPO}/host/overlays.cpp ${BBS_REPO}/host/movie_skip.cpp ${BBS_REPO}/host/movie_player.cpp ${BBS_REPO}/host/movie_video_decoder.cpp ${BBS_REPO}/host/sas_hle.cpp ${BBS_REPO}/host/atrac_hle.cpp
    ${BBS_REPO}/host/thread_manager.cpp ${BBS_REPO}/host/partition_memory.cpp
    jni/bbs_jni.cpp)

file(GLOB BBS_OVERLAY_UNITS CONFIGURE_DEPENDS "${BBS_OVERLAY_DIR}/*/generated_*.cpp")
if(NOT EXISTS "${BBS_OVERLAY_DIR}/overlays_registry.cpp")
    message(FATAL_ERROR "No overlays in ${BBS_OVERLAY_DIR}: run scripts/3-GENERATE.sh")
endif()
add_library(bbs SHARED ${HOST_SOURCES} "${BBS_GENERATED_DIR}/generated_registry.cpp" ${BBS_GENERATED_UNITS}
    "${BBS_OVERLAY_DIR}/overlays_registry.cpp" ${BBS_OVERLAY_UNITS})
add_dependencies(bbs bbs_spirv)
target_compile_definitions(bbs PRIVATE VCS_VULKAN_GE_BACKEND)
target_include_directories(bbs PRIVATE "${RT}/include" "${VCS}" "${HS}" "${BBS_REPO}/host"
    "${BBS_GENERATED_DIR}" "${SPV_DIR}")
target_compile_options(bbs PRIVATE -w -fexceptions -frtti)
if(BBS_ARCH_FLAGS)
    separate_arguments(BBS_ARCH_LIST UNIX_COMMAND "${BBS_ARCH_FLAGS}")
    target_compile_options(bbs PRIVATE ${BBS_ARCH_LIST})
endif()
# BBS's main() becomes bbs_main(), called from the JNI game thread.
set_source_files_properties(${BBS_REPO}/host/main.cpp PROPERTIES COMPILE_DEFINITIONS "main=bbs_main")
set_source_files_properties(${HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
set_source_files_properties(${BBS_GENERATED_UNITS} ${BBS_OVERLAY_UNITS} "${BBS_GENERATED_DIR}/generated_registry.cpp"
    PROPERTIES COMPILE_OPTIONS "${BBS_GENERATED_OPT};-g0")
# ATRAC3/3+ decoders (atrac_hle.cpp): PPSSPP's at3_standalone, LGPL 2.1+ (third_party/at3_standalone).
set(BBS_AT3_DIR "${BBS_REPO}/third_party/at3_standalone")
add_library(bbs_at3 STATIC
    "${BBS_AT3_DIR}/atrac.cpp" "${BBS_AT3_DIR}/atrac3.cpp" "${BBS_AT3_DIR}/atrac3plus.cpp"
    "${BBS_AT3_DIR}/atrac3plusdec.cpp" "${BBS_AT3_DIR}/atrac3plusdsp.cpp" "${BBS_AT3_DIR}/get_bits.cpp"
    "${BBS_AT3_DIR}/compat.cpp" "${BBS_AT3_DIR}/fft.cpp" "${BBS_AT3_DIR}/mem.cpp")
target_include_directories(bbs_at3 PRIVATE "${BBS_AT3_DIR}")
target_compile_options(bbs_at3 PRIVATE -O2 -w)
target_link_libraries(bbs_at3 PRIVATE log)
set_target_properties(bbs_at3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(bbs PRIVATE "${BBS_AT3_DIR}")
target_link_libraries(bbs PRIVATE vulkan android log aaudio mediandk bbs_at3)
target_link_options(bbs PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
# First-run setup (C2): EBOOT decrypter, its own executable (GPL-3.0: links third_party/pspdecrypt's PrxDecrypter +
# libkirk). Packaged as lib/arm64-v8a/libbbsdecrypt.so so Android extracts it next to libbbs.so; the app runs it as a
# separate process (LauncherActivity / IsoInstaller), it is never linked into libbbs. Source: android/decrypt/.
set(BBS_PSPDECRYPT_DIR "${BBS_REPO}/third_party/pspdecrypt")
if(EXISTS "${BBS_PSPDECRYPT_DIR}/PrxDecrypter.cpp")
    enable_language(C)   # libkirk and kl4e are C
    file(GLOB BBS_KIRK_SOURCES "${BBS_PSPDECRYPT_DIR}/libkirk/*.c")
    add_executable(bbsdecrypt "${CMAKE_CURRENT_LIST_DIR}/decrypt/bbs_decrypt.cpp" "${BBS_PSPDECRYPT_DIR}/PrxDecrypter.cpp"
        "${BBS_PSPDECRYPT_DIR}/kl4e.c" ${BBS_KIRK_SOURCES})
    target_include_directories(bbsdecrypt PRIVATE "${BBS_PSPDECRYPT_DIR}")
    target_compile_options(bbsdecrypt PRIVATE -O2 -w)
    target_link_libraries(bbsdecrypt PRIVATE z)
    set_target_properties(bbsdecrypt PROPERTIES OUTPUT_NAME "libbbsdecrypt.so" SUFFIX "")
else()
    message(WARNING "third_party/pspdecrypt missing (git submodule update --init): no first-run EBOOT decrypter")
endif()
