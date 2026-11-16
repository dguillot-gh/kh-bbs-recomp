#!/usr/bin/env bash
# 6. Install on your phone: bash scripts/6-INSTALL-PHONE.sh
# USB debugging on, phone connected (pick one with ANDROID_SERIAL if several are). Installs the APK, then copies the
# game files the app reads (game/EBOOT.BIN.dec + game/disc-files, ~1.6 GB) into its own folder on the phone:
# Android/data/com.psprecomp.bbs/files. They stay on your phone and PC only.
source "$(dirname "$0")/bbs-common.sh"
ADB=${ADB:-$(pick adb || true)}
[ -n "$ADB" ] || { [ -n "${ANDROID_SDK_ROOT:-}" ] && ADB=$ANDROID_SDK_ROOT/platform-tools/adb$EXE; }
[ -x "$ADB" ] || fail "adb not found (put platform-tools on PATH or set ADB)"
export MSYS_NO_PATHCONV=1
APK=$REPO/build/android/bbs-recomp-debug.apk
[ -f "$APK" ] || fail "run scripts/5-BUILD-ANDROID.sh first"
[ -f "$GAME_DIR/EBOOT.BIN.dec" ] || fail "run scripts/1-2 first"

say "Installing the app"
"$ADB" install -r "$(native "$APK")"
DEST=/sdcard/Android/data/com.psprecomp.bbs/files
say "Copying the game files to the phone"
"$ADB" shell mkdir -p "$DEST" < /dev/null
"$ADB" push "$(native "$GAME_DIR/EBOOT.BIN.dec")" "$DEST/EBOOT.BIN.dec" < /dev/null
"$ADB" push "$(native "$GAME_DIR/disc-files")" "$DEST/" < /dev/null
echo "OK. Open \"Birth by Sleep\" on the phone (or: $ADB shell am start --user 0 -n com.psprecomp.bbs/.BbsActivity)"
