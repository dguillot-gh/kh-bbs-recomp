#!/usr/bin/env bash
# 3. Recompile the game's code to C++: bash scripts/3-GENERATE.sh
# Builds the engine's recompiler (psp_recomp) and translates game/EBOOT.BIN.dec into game/generated
# (about 113 units / 656k instructions; several minutes). The output is derived from the game: never commit it.
source "$(dirname "$0")/bbs-common.sh"
need_runtime; need_cxx
[ -f "$GAME_DIR/EBOOT.BIN.dec" ] || fail "run scripts/2-DECRYPT.sh first"

RECOMP=$TOOLS_DIR/psp_recomp$EXE
if [ ! -x "$RECOMP" ] || [ -n "$(find "$RUNTIME/src" "$RUNTIME/tools/codegen_main.cpp" -newer "$RECOMP" 2>/dev/null | head -1)" ]; then
  say "Building the recompiler"
  mkdir -p "$TOOLS_DIR"
  STATIC=; [ -n "$EXE" ] && STATIC=-static
  "$HOST_CXX" -std=c++20 -O2 -w $STATIC -I "$RUNTIME/include" "$RUNTIME/tools/codegen_main.cpp" \
    "$RUNTIME"/src/decoder.cpp "$RUNTIME"/src/deflate.cpp "$RUNTIME"/src/elf32.cpp "$RUNTIME"/src/guest_memory.cpp \
    "$RUNTIME"/src/nid_registry.cpp "$RUNTIME"/src/program_analysis.cpp "$RUNTIME"/src/runtime.cpp \
    "$RUNTIME"/src/sha256.cpp -o "$RECOMP"
fi

say "Generating code (several minutes)"
rm -rf "$GAME_DIR/generated.new"
"$RECOMP" "$(native "$GAME_DIR/EBOOT.BIN.dec")" --auto "$(native "$GAME_DIR/generated.new")" $LOAD_BASE $UNIT_SIZE
[ -f "$GAME_DIR/generated.new/generated_registry.cpp" ] || fail "the recompiler did not finish"
rm -rf "$GAME_DIR/generated"; mv "$GAME_DIR/generated.new" "$GAME_DIR/generated"
echo "OK: $(ls "$GAME_DIR"/generated/generated_unit_*.cpp | wc -l) units in game/generated. Next: bash scripts/4-BUILD-ANDROID.sh"
