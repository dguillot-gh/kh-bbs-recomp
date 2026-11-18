#!/usr/bin/env bash
# 4. Build the PC (Windows) version: bash scripts/4-BUILD-PC.sh
# Compiles the engine, the BBS host and game/generated with clang (llvm-mingw on Windows works well; set HOST_CXX)
# into build/pc/bbs.exe, with the Vulkan GE backend (GE_BACKEND=dx12 for DirectX 12 instead). Run it with
# scripts/RUN-PC.sh. Today it runs without a window and can dump frames (see README).
source "$(dirname "$0")/bbs-common.sh"
need_runtime; need_cxx; need_python
GEN=$GAME_DIR/generated
[ -f "$GEN/generated_registry.cpp" ] || fail "run scripts/3-GENERATE.sh first"
OUT=$REPO/build/pc; mkdir -p "$OUT/gen" "$OUT/host" "$OUT/lib"
JOBS=${JOBS:-4}
V=$RUNTIME/profiles/vcs/host; H=$RUNTIME/host-shared
INC="-I $RUNTIME/include -I $V -I $H"
GE_FILE=ge_gpu_backend_dx12.cpp; GE_LIBS="-ld3d12 -ldxgi -ld3dcompiler"
if [ "${GE_BACKEND:-vulkan}" = vulkan ]; then
  NDK=${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME: the Vulkan build uses the NDK r29 glslc and Vulkan headers}
  case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) TAG=windows-x86_64 ;; Darwin*) TAG=darwin-x86_64 ;; *) TAG=linux-x86_64 ;; esac
  mkdir -p "$OUT/vk/gen/vulkan" "$OUT/vk/inc"
  [ -d "$OUT/vk/inc/vulkan" ] || cp -r "$NDK/toolchains/llvm/prebuilt/$TAG/sysroot/usr/include/vulkan" \
    "$NDK/toolchains/llvm/prebuilt/$TAG/sysroot/usr/include/vk_video" "$OUT/vk/inc/"
  "$PYTHON" -I "$(native "$V/vulkan/embed_spirv.py")" "$(native "$NDK/shader-tools/$TAG/glslc$EXE")" \
    "$(native "$V/vulkan")" "$(native "$OUT/vk/gen/vulkan/ge_spv.hpp")"
  INC="$INC -DVCS_VULKAN_GE_BACKEND -I $OUT/vk/gen -isystem $OUT/vk/inc"
  GE_FILE=ge_gpu_backend_vulkan.cpp
  if [ -n "$EXE" ]; then  # Windows: import library made from the system's own vulkan-1.dll (no Vulkan SDK needed)
    BIN=$(dirname "$(command -v "$HOST_CXX")")
    if [ ! -f "$OUT/vk/libvulkan-1.a" ]; then
      (cd "$OUT/vk" && "$BIN/gendef$EXE" - "$(native "${SYSTEMROOT:-C:/Windows}/System32/vulkan-1.dll")" > vulkan-1.def \
        && "$BIN/llvm-dlltool$EXE" -m i386:x86-64 -d vulkan-1.def -l libvulkan-1.a) || fail "could not make the Vulkan import library"
    fi
    GE_LIBS="-L$OUT/vk -lvulkan-1"
  else
    GE_LIBS="-lvulkan"
  fi
fi

say "Compiling the generated game code ($JOBS jobs; cached between runs)"
compile_one() {
  local f=$1 o=$OUT/gen/$(basename "$(dirname "$1")")_$(basename "$1" .cpp).o
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
    "$HOST_CXX" -std=c++20 -O1 -w -c $INC -I "$GEN" "$f" -o "$o" 2> "$o.err" || { echo "FAIL $f"; head -3 "$o.err"; }
  fi
}
export -f compile_one; export OUT HOST_CXX INC GEN
OVL=$GAME_DIR/overlays
rm -f "$OUT"/gen/generated_unit_*.o "$OUT"/gen/generated_registry.o  # object names before overlays (2026-11-16)
[ -f "$OVL/overlays_registry.cpp" ] || fail "no overlays: run scripts/3-GENERATE.sh again"
ls "$GEN"/generated_unit_*.cpp "$GEN/generated_registry.cpp" "$OVL"/*/generated_*.cpp | xargs -P "$JOBS" -I{} bash -c 'compile_one "$@"' _ {}

"$HOST_CXX" -std=c++20 -O1 -w -c $INC -I "$REPO/host" "$OVL/overlays_registry.cpp" -o "$OUT/gen/overlays_registry.o"
say "Compiling the engine and host"
for f in "$REPO"/host/*.cpp; do
  "$HOST_CXX" -std=c++20 -O2 -w -c $INC -I "$REPO/host" -I "$GEN" "$f" -o "$OUT/host/$(basename "$f" .cpp).o"
done
rm -f "$OUT"/lib/ge_gpu_backend_*.o
for f in "$H"/*.cpp "$V"/framebuffer_capture.cpp "$V"/vcs_config.cpp "$V"/vcs_fps_overlay.cpp "$V"/vcs_project2dfx.cpp \
         "$V"/vcs_project2dfx_lights.cpp "$V"/vcs_draw_distance_patch.cpp "$V"/vcs_runtime_log.cpp "$V"/texture_replacer.cpp "$V/$GE_FILE" \
         "$RUNTIME"/src/*.cpp; do
  "$HOST_CXX" -std=c++20 -O2 -w -c $INC "$f" -o "$OUT/lib/$(basename "$f" .cpp).o"
done
# One line of ge_renderer.cpp uses an array bound clang rejects (MSVC accepts it): compile a patched copy.
sed 's/std::array<std::uint32_t, regs.size()> values{};/std::array<std::uint32_t, 23> values{};/' \
  "$V/ge_renderer.cpp" > "$OUT/lib/ge_renderer_clang.cpp"
"$HOST_CXX" -std=c++20 -O2 -w -c $INC "$OUT/lib/ge_renderer_clang.cpp" -o "$OUT/lib/ge_renderer.o"

say "Linking"
STACK=; STATIC=; [ -n "$EXE" ] && { STACK=-Wl,--stack,268435456; STATIC=-static; }
"$HOST_CXX" -std=c++20 $STATIC "$OUT"/gen/*.o "$OUT"/host/*.o "$OUT"/lib/*.o $GE_LIBS $STACK -o "$OUT/bbs$EXE"
[ -f "$OUT/VCSNative.ini" ] || printf '[Rendering]\nBackend=%s\nNativeGE=true\n' \
  "$([ "${GE_BACKEND:-vulkan}" = vulkan ] && echo Vulkan || echo DirectX12)" > "$OUT/VCSNative.ini"
echo "OK: $OUT/bbs$EXE. Run: bash scripts/RUN-PC.sh"
