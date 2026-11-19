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

# Whole-disc compile, after Recomp Compiler's PS1 flow: survey -> profile -> static code for every image -> report.
# The main executable and the MODULE/*.ELF overlays call each other, so each side's calls become the other's seeds
# and the two are regenerated until no new main-side seed appears (normally 2 rounds).
DISC=$GAME_DIR/disc-files
say "Surveying the disc"
"$PYTHON" -I "$(native "$REPO/scripts/wholedisc.py")" profile "$(native "$DISC")" "$(native "$GAME_DIR/EBOOT.BIN.dec")" \
  "$(native "$GAME_DIR/wholedisc.json")"
"$PYTHON" -I "$(native "$REPO/scripts/wholedisc.py")" main-pointers "$(native "$GAME_DIR/EBOOT.BIN.dec")" \
  "$(native "$GAME_DIR/wholedisc.json")" "$(native "$GAME_DIR/overlay_seeds.txt")"
: > "$GAME_DIR/main_seeds.txt"
for round in 1 2 3; do
  say "Round $round: overlays (seconds)"
  PSPRECOMP_SEED_PROLOGUES=1 PSPRECOMP_SEED_CODE_POINTERS=1 PSPRECOMP_EXTRA_SEEDS="$(native "$GAME_DIR/overlay_seeds.txt")" \
    bash "$REPO/scripts/gen-overlays.sh" "$RECOMP" "$DISC/PSP_GAME/USRDIR/MODULE" "$GAME_DIR/overlays"
  if [ -f "$GAME_DIR/generated/generated_registry.cpp" ] && cmp -s "$GAME_DIR/overlays/main_seeds.txt" "$GAME_DIR/main_seeds.txt"; then
    break
  fi
  cp "$GAME_DIR/overlays/main_seeds.txt" "$GAME_DIR/main_seeds.txt"
  say "Round $round: main executable (several minutes)"
  rm -rf "$GAME_DIR/generated.new"
  PSPRECOMP_EXTRA_SEEDS="$(native "$GAME_DIR/main_seeds.txt")" \
    "$RECOMP" "$(native "$GAME_DIR/EBOOT.BIN.dec")" --auto "$(native "$GAME_DIR/generated.new")" $LOAD_BASE $UNIT_SIZE
  [ -f "$GAME_DIR/generated.new/generated_registry.cpp" ] || fail "the recompiler did not finish"
  rm -rf "$GAME_DIR/generated"; mv "$GAME_DIR/generated.new" "$GAME_DIR/generated"
done
# Right-stick camera hooks (host/main.cpp) for the disc this is (US or Final Mix).
DISC_ID=$(head -c 10 "$DISC/UMD_DATA.BIN")
"$PYTHON" -I "$(native "$REPO/scripts/patch-camera-mod.py")" "$(native "$GAME_DIR/generated")" "$DISC_ID" \
  || echo "warning: no right-stick camera hooks for $DISC_ID"
mkdir -p "$REPO/build"
"$PYTHON" -I "$(native "$REPO/scripts/wholedisc.py")" report "$(native "$GAME_DIR/wholedisc.json")" \
  "$(native "$GAME_DIR/generated")" "$(native "$GAME_DIR/overlays")" "$(native "$REPO/build/wholedisc-report.txt")"
echo "OK: $(ls "$GAME_DIR"/generated/generated_unit_*.cpp | wc -l) units in game/generated, overlays in game/overlays; report in build/wholedisc-report.txt. Next: bash scripts/4-BUILD-PC.sh or 5-BUILD-ANDROID.sh"
