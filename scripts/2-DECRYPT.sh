#!/usr/bin/env bash
# 2. Decrypt the game's executable: bash scripts/2-DECRYPT.sh
# Builds the small decrypter (the engine's tools/pspdec + pspdecrypt's libkirk, compiled locally, never shipped)
# and writes game/EBOOT.BIN.dec from game/disc-files/PSP_GAME/SYSDIR/EBOOT.BIN.
source "$(dirname "$0")/bbs-common.sh"
need_runtime; need_cxx; need_python
[ -f "$PSPDECRYPT/libkirk/kirk_engine.c" ] || fail "pspdecrypt not found: run 'git submodule update --init' (third_party/pspdecrypt)"
SRC=$GAME_DIR/disc-files/PSP_GAME/SYSDIR/EBOOT.BIN
[ -f "$SRC" ] || fail "run scripts/1-EXTRACT.sh first"
# Final Mix: the program is already a plain ELF (EBOOT.OLD on the UNDUB release, behind its loader): just copy it.
is_elf() { [ "$(head -c 4 "$1" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ]; }
for plain in "$GAME_DIR/disc-files/PSP_GAME/SYSDIR/EBOOT.OLD" "$SRC"; do
  if [ -f "$plain" ] && is_elf "$plain"; then
    cp "$plain" "$GAME_DIR/EBOOT.BIN.dec"
    sum=$(sha1_of "$GAME_DIR/EBOOT.BIN.dec")
    [ "$sum" = "$FM_EBOOT_SHA1" ] || echo "WARNING: executable SHA-1 $sum differs from the supported Final Mix one's"
    echo "OK: $(disc_id), plain executable copied (nothing to decrypt). Next: bash scripts/3-GENERATE.sh"
    exit 0
  fi
done

PSPDEC=$TOOLS_DIR/pspdec$EXE
if [ ! -x "$PSPDEC" ]; then
  say "Building the decrypter"
  mkdir -p "$TOOLS_DIR/pspdec-obj"
  for c in "$PSPDECRYPT"/libkirk/*.c; do
    "$HOST_CC" -O2 -w -c -I "$PSPDECRYPT" "$c" -o "$TOOLS_DIR/pspdec-obj/$(basename "$c" .c).o"
  done
  STATIC=; [ -n "$EXE" ] && STATIC=-static
  "$HOST_CXX" -std=c++17 -O2 -w $STATIC -I "$PSPDECRYPT" "$RUNTIME/tools/pspdec/main.cpp" \
    "$PSPDECRYPT/PrxDecrypter.cpp" "$TOOLS_DIR"/pspdec-obj/*.o -o "$PSPDEC"
fi

say "Decrypting EBOOT.BIN"
"$PSPDEC" "$(native "$SRC")" "$(native "$GAME_DIR/EBOOT.BIN.dec")"
sum=$(sha1_of "$GAME_DIR/EBOOT.BIN.dec")
[ "$sum" = "$EBOOT_DEC_SHA1" ] || echo "WARNING: decrypted executable SHA-1 $sum differs from the supported dump's"
echo "OK. Next: bash scripts/3-GENERATE.sh"
