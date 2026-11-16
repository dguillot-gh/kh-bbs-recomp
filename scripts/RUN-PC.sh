#!/usr/bin/env bash
# Run the PC build: bash scripts/RUN-PC.sh [max-dispatches]
# Frames: FRAMES=1 bash scripts/RUN-PC.sh  ->  build/pc/frames/frame_*.ppm (only frames that change are kept).
source "$(dirname "$0")/bbs-common.sh"
OUT=$REPO/build/pc
[ -x "$OUT/bbs$EXE" ] || fail "run scripts/4-BUILD-PC.sh first"
if [ "${FRAMES:-0}" = 1 ]; then
  mkdir -p "$OUT/frames"; export PSPRECOMP_GE_FRAME_DUMP=1 PSPRECOMP_FRAME_DUMP_DIR="$(native "$OUT/frames")"
fi
cd "$OUT"
"./bbs$EXE" "$(native "$GAME_DIR/EBOOT.BIN.dec")" "$(native "$GAME_DIR/disc-files")" "${1:-400000000}"
