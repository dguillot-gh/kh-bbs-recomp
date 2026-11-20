#!/usr/bin/env bash
# 6. Install on your phone: bash scripts/6-INSTALL-PHONE.sh
# USB debugging on, phone connected (pick one with ANDROID_SERIAL if several are). Installs the APK, then copies the
# game files the app reads (game/EBOOT.BIN.dec + game/disc-files, ~1.6 GB) into its own folder on the phone:
# Android/data/<app id>/files (com.psprecomp.bbs, or com.psprecomp.bbsfm for Final Mix). They stay on your phone
# and PC only. (Without this script, the app's first-run setup does the same from your ISO on the phone.)
source "$(dirname "$0")/bbs-common.sh"
ADB=${ADB:-$(pick adb || true)}
[ -n "$ADB" ] || { [ -n "${ANDROID_SDK_ROOT:-}" ] && ADB=$ANDROID_SDK_ROOT/platform-tools/adb$EXE; }
[ -x "$ADB" ] || fail "adb not found (put platform-tools on PATH or set ADB)"
export MSYS_NO_PATHCONV=1
APK=${BUILD_DIR:-$REPO/build/android}/bbs-recomp-debug.apk
[ -f "$APK" ] || fail "run scripts/5-BUILD-ANDROID.sh first"
[ -f "$GAME_DIR/EBOOT.BIN.dec" ] || fail "run scripts/1-2 first"

say "Installing the app"
"$ADB" install -r "$(native "$APK")"
PKG=com.psprecomp.bbs; [ "$(disc_id)" = ULJM-05775 ] && PKG=com.psprecomp.bbsfm
PKG=${APP_ID:-$PKG}
DEST=/sdcard/Android/data/$PKG/files
say "Copying the game files to the phone"
"$ADB" shell mkdir -p "$DEST" < /dev/null
"$ADB" push "$(native "$GAME_DIR/EBOOT.BIN.dec")" "$DEST/EBOOT.BIN.dec" < /dev/null
"$ADB" push "$(native "$GAME_DIR/disc-files")" "$DEST/" < /dev/null
echo "OK. Open the app on the phone (or: $ADB shell am start --user 0 -n $PKG/com.psprecomp.bbs.LauncherActivity)"
