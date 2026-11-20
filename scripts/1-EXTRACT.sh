#!/usr/bin/env bash
# 1. Extract your disc image: bash scripts/1-EXTRACT.sh "<path to your Birth by Sleep .iso>"
# Checks the image is a supported dump (USA, or Final Mix English), then copies every file on it to game/disc-files.
# One disc per game folder: for the other disc, set BBS_GAME_DIR to a second folder.
source "$(dirname "$0")/bbs-common.sh"
need_python
ISO=${1:-}
[ -f "$ISO" ] || fail "usage: bash scripts/1-EXTRACT.sh <game.iso>"

say "Checking the disc image"
sum=$(sha1_of "$ISO")
if [ "$sum" != "$ISO_SHA1" ] && [ "$sum" != "$FM_ISO_SHA1" ]; then
  echo "SHA-1 $sum is not a supported dump ($ISO_SHA1 USA ULUS-10505, $FM_ISO_SHA1 Final Mix English)."
  [ "${BBS_ANY_DUMP:-0}" = 1 ] || fail "set BBS_ANY_DUMP=1 to try it anyway"
fi

say "Extracting to $GAME_DIR/disc-files"
rm -rf "$GAME_DIR/disc-files"
mkdir -p "$GAME_DIR"
"$PYTHON" "$(native "$REPO/scripts/extract_iso.py")" "$(native "$ISO")" "$(native "$GAME_DIR/disc-files")"
[ -f "$GAME_DIR/disc-files/PSP_GAME/SYSDIR/EBOOT.BIN" ] || fail "no PSP_GAME/SYSDIR/EBOOT.BIN on that disc"
echo "OK: $(disc_id). Next: bash scripts/2-DECRYPT.sh"
