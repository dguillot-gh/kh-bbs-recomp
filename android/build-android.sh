#!/bin/bash
# Build libbbs.so (arm64-v8a, NDK r29) and the APK. Low priority, capped jobs.
#   bash android/build-android.sh [lib|apk|all]     (default all; scripts/5-BUILD-ANDROID.sh calls this)
# Settings (environment):
#   ANDROID_SDK_ROOT (or ANDROID_HOME)  Android SDK with build-tools 36.0.0, platforms;android-35, cmake 3.31.6
#   ANDROID_NDK_HOME                    NDK r29 (default: the newest $ANDROID_SDK_ROOT/ndk/29.*)
#   JAVA_HOME                           JDK 17+
#   PYTHON                              Python 3 (default: python3 / python on PATH)
#   CORPUS                              generated code (default: <repo>/game/generated, made by scripts/3-GENERATE.sh)
#   APP_ID APP_LABEL                    package (default com.psprecomp.bbs) and app name (KH BBS Recomp); a second
#                                       build (e.g. Final Mix) with its own APP_ID installs beside the first
#   ICON_IMAGE                          optional PNG for the launcher icon (default: the logo cut from PIC1.PNG)
#   LAUNCHER_BG                         optional JPG/PNG for the launcher background (default: the disc's PIC1.PNG)
#   GAME_ID                             disc the generated code is for (ULUS-10505; Final Mix: ULJM-05775)
#   BUILD_DIR JOBS OPT                  build folder (default <repo>/build/android), parallel jobs (4), -O for the
#                                       generated units (-O1)
# Without Gradle: the SDK's aapt2 + javac + d8 + zipalign + apksigner. Signed with ~/.android/debug.keystore
# (created if missing), so the APK is for your own phone only.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:?set ANDROID_SDK_ROOT to your Android SDK}}
NDK=${ANDROID_NDK_HOME:-$(ls -d "$SDK"/ndk/29.* 2>/dev/null | sort | tail -1)}
[ -d "$NDK" ] || { echo "set ANDROID_NDK_HOME to an NDK r29"; exit 1; }
JDK=${JAVA_HOME:?set JAVA_HOME to a JDK 17+}
PY=${PYTHON:-$(command -v python3 || command -v python || true)}
[ -n "$PY" ] || { echo "set PYTHON to Python 3"; exit 1; }
CORPUS=${CORPUS:-$REPO/game/generated}
OVERLAYS=${OVERLAYS:-$(dirname "$CORPUS")/overlays}
[ -f "$CORPUS/generated_registry.cpp" ] || { echo "no generated code in $CORPUS: run scripts/1-3 first"; exit 1; }
BUILD=${BUILD_DIR:-$REPO/build/android}
JOBS=${JOBS:-4}
WHAT=${1:-all}

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) HOSTTAG=windows-x86_64; X=.exe; w() { cygpath -m "$1"; } ;;
  Darwin*) HOSTTAG=darwin-x86_64; X=; w() { echo "$1"; } ;;
  *) HOSTTAG=linux-x86_64; X=; w() { echo "$1"; } ;;
esac
CMAKE_DIR=$(ls -d "$SDK"/cmake/3.31.* 2>/dev/null | sort | tail -1)
CMAKE=${CMAKE:-$CMAKE_DIR/bin/cmake$X}; NINJA=${NINJA:-$CMAKE_DIR/bin/ninja$X}
[ -x "$CMAKE" ] || CMAKE=$(command -v cmake); [ -x "$NINJA" ] || NINJA=$(command -v ninja)
BT=$SDK/build-tools/36.0.0
JAR=$SDK/platforms/android-35/android.jar
LIB_OUT=$BUILD/jniLibs/arm64-v8a

lowprio() {  # run a build command at low CPU priority
  if [ "$X" = .exe ]; then cmd //c "start /b /belownormal /wait $*"; else nice -n 10 "$@"; fi
}

if [ "$WHAT" != apk ]; then
  [ -f "$BUILD/build.ninja" ] || "$CMAKE" -G Ninja -S "$(w "$HERE")" -B "$(w "$BUILD")" \
    -DCMAKE_MAKE_PROGRAM="$(w "$NINJA")" \
    -DCMAKE_TOOLCHAIN_FILE="$(w "$NDK")/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DBBS_GENERATED_OPT="${OPT:--O1}" -DBBS_GENERATED_DIR="$(w "$CORPUS")" -DBBS_OVERLAY_DIR="$(w "$OVERLAYS")" \
    -DPSP_PYTHON="$(w "$PY")" -DPSP_GLSLC="$(w "$NDK")/shader-tools/$HOSTTAG/glslc$X"
  if [ "$X" = .exe ]; then lowprio "$(cygpath -w "$NINJA")" -C "$(cygpath -w "$BUILD")" -j "$JOBS" bbs
  else lowprio "$NINJA" -C "$BUILD" -j "$JOBS" bbs; fi
  mkdir -p "$LIB_OUT"
  "$NDK/toolchains/llvm/prebuilt/$HOSTTAG/bin/llvm-strip$X" --strip-unneeded -o "$LIB_OUT/libbbs.so" "$BUILD/libbbs.so"
  # First-run EBOOT decrypter (GPL-3.0 helper, android/decrypt/README.md); optional.
  if [ "$X" = .exe ]; then lowprio "$(cygpath -w "$NINJA")" -C "$(cygpath -w "$BUILD")" -j "$JOBS" bbsdecrypt
  else lowprio "$NINJA" -C "$BUILD" -j "$JOBS" bbsdecrypt; fi || echo "bbsdecrypt not built"
  [ -f "$BUILD/libbbsdecrypt.so" ] && "$NDK/toolchains/llvm/prebuilt/$HOSTTAG/bin/llvm-strip$X" --strip-unneeded     -o "$LIB_OUT/libbbsdecrypt.so" "$BUILD/libbbsdecrypt.so"
  ls -la "$LIB_OUT/libbbs.so"
fi
if [ "$WHAT" != lib ]; then
  [ -f "$LIB_OUT/libbbs.so" ] || { echo "build the library first (bash android/build-android.sh lib)"; exit 1; }
  KS=$HOME/.android/debug.keystore
  if [ ! -f "$KS" ]; then
    mkdir -p "$HOME/.android"
    "$JDK/bin/keytool$X" -genkeypair -keystore "$(w "$KS")" -storepass android -keypass android -alias androiddebugkey \
      -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US"
  fi
  O=$BUILD/apk; rm -rf "$O"; mkdir -p "$O/classes" "$O/dex" "$O/lib/arm64-v8a"
  # Launcher icon: the logo from the disc's PSP_GAME/PIC1.PNG, made here so nothing from the disc is committed.
  # The logo's box in PIC1: top-centre on the US disc (MakeIcon's default), top-right on Final Mix.
  ICON_BOX=; [ "${GAME_ID:-}" = ULJM-05775 ] && ICON_BOX=312,14,478,134
  # ICON_IMAGE=<png>: use a picture of your own instead (kept outside the repo; never commit game art).
  [ -n "${ICON_IMAGE:-}" ] && [ -f "$ICON_IMAGE" ] && ICON_BOX=$(w "$ICON_IMAGE")
  "$JDK/bin/java$X" "$(w "$HERE/tools/MakeIcon.java")" "$(w "$(dirname "$CORPUS")/disc-files/PSP_GAME/PIC1.PNG")" "$(w "$O/res")" "$ICON_BOX"
  "$BT/aapt2$X" compile --dir "$(w "$O/res")" -o "$(w "$O/res.zip")"
  sed -e "s|android:label=\"KH BBS Recomp\"|android:label=\"${APP_LABEL:-KH BBS Recomp}\"|" \
      -e "s|</application>|    <meta-data android:name=\"bbs.game_id\" android:value=\"${GAME_ID:-ULUS-10505}\" />\n    </application>|" \
    "$HERE/app/src/main/AndroidManifest.xml" > "$O/AndroidManifest.xml"
  # LAUNCHER_BG=<jpg|png>: launcher background of your own, packed as assets/launcher_bg (kept outside the repo).
  ASSETS=()
  if [ -n "${LAUNCHER_BG:-}" ] && [ -f "$LAUNCHER_BG" ]; then
    mkdir -p "$O/assets" && cp "$LAUNCHER_BG" "$O/assets/launcher_bg" && ASSETS=(-A "$(w "$O/assets")")
  fi
  "$BT/aapt2$X" link -o "$(w "$O/base.apk")" -I "$(w "$JAR")" -R "$(w "$O/res.zip")" "${ASSETS[@]}" \
    --manifest "$(w "$O/AndroidManifest.xml")" --min-sdk-version 26 --target-sdk-version 35 \
    --version-code 1 --version-name 0.1.0 --rename-manifest-package "${APP_ID:-com.psprecomp.bbs}"
  "$JDK/bin/javac$X" -source 17 -target 17 -nowarn -encoding UTF-8 -cp "$(w "$JAR")" \
    -d "$(w "$O/classes")" $(find "$HERE/app/src/main/java" -name '*.java' | while read f; do w "$f"; done)
  "$JDK/bin/java$X" -cp "$(w "$BT/lib/d8.jar")" com.android.tools.r8.D8 --release --min-api 26 \
    --lib "$(w "$JAR")" --output "$(w "$O/dex")" \
    $(find "$O/classes" -name '*.class' | while read f; do w "$f"; done)
  cp "$LIB_OUT/libbbs.so" "$O/lib/arm64-v8a/"
  LIBS=lib/arm64-v8a/libbbs.so
  if [ -f "$LIB_OUT/libbbsdecrypt.so" ]; then cp "$LIB_OUT/libbbsdecrypt.so" "$O/lib/arm64-v8a/"; LIBS="$LIBS lib/arm64-v8a/libbbsdecrypt.so"
  else echo "warning: no libbbsdecrypt.so: the app can't set itself up from an ISO"; fi
  (cd "$O/dex" && "$JDK/bin/jar$X" uf ../base.apk classes.dex) && (cd "$O" && "$JDK/bin/jar$X" uf0 base.apk $LIBS)
  "$BT/zipalign$X" -f -p 16 "$(w "$O/base.apk")" "$(w "$O/aligned.apk")"
  "$JDK/bin/java$X" -jar "$(w "$BT/lib/apksigner.jar")" sign --ks "$(w "$KS")" \
    --ks-pass pass:android --key-pass pass:android --ks-key-alias androiddebugkey \
    --out "$(w "$BUILD/bbs-recomp-debug.apk")" "$(w "$O/aligned.apk")"
  ls -la "$BUILD/bbs-recomp-debug.apk"
fi
