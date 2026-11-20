#!/usr/bin/env bash
# 5. Build the Android app: bash scripts/5-BUILD-ANDROID.sh
# Needs ANDROID_SDK_ROOT (build-tools 36.0.0, platforms;android-35, cmake 3.31), an NDK r29 (ANDROID_NDK_HOME or
# $ANDROID_SDK_ROOT/ndk/29.*) and JAVA_HOME (JDK 17+). Output: build/android/bbs-recomp-debug.apk.
# The first build compiles ~93 MB of generated C++ for arm64 at -O2 (about an hour at one job; OPT=-O1 is faster
# to build but runs a little slower).
source "$(dirname "$0")/bbs-common.sh"
[ -f "$GAME_DIR/generated/generated_registry.cpp" ] || fail "run scripts/3-GENERATE.sh first"
# Final Mix gets its own package and name, so it installs beside the USA app.
GAME_ID=${GAME_ID:-$(disc_id)}
if [ "$GAME_ID" = ULJM-05775 ]; then
  APP_ID=${APP_ID:-com.psprecomp.bbsfm}; APP_LABEL=${APP_LABEL:-KH BBS Final Mix}
fi
export GAME_ID APP_ID APP_LABEL
CORPUS=$GAME_DIR/generated PYTHON=$PYTHON bash "$REPO/android/build-android.sh" "${1:-all}"
echo "OK. Next: bash scripts/6-INSTALL-PHONE.sh"
