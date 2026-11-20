# Shared settings for the bbs-recomp scripts. Sourced, not run.
#
# Everything made from your disc goes into game/ (never committed). Tools are found on PATH or through these
# environment variables; nothing here is specific to one PC:
#   PYTHON        Python 3 (default: python3, then python)
#   HOST_CXX      C++ compiler for the PC-side tools (default: clang++, then g++)
#   HOST_CC       C compiler for the same (default: clang, then gcc)
#   ANDROID_SDK_ROOT / ANDROID_NDK_HOME / JAVA_HOME   for the Android build (see README)
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
GAME_DIR=${BBS_GAME_DIR:-$REPO/game}
TOOLS_DIR=$REPO/build/tools
# Engine and decrypter: git submodules inside the repo (git submodule update --init),
# or sibling checkouts in a workspace that already has them.
RUNTIME=${PSPRECOMP_RUNTIME_DIR:-$REPO/runtime}
[ -f "$RUNTIME/CMakeLists.txt" ] || RUNTIME=$REPO/../psp-native-runtime
PSPDECRYPT=${PSPDECRYPT_DIR:-$REPO/third_party/pspdecrypt}
[ -f "$PSPDECRYPT/PrxDecrypter.cpp" ] || PSPDECRYPT=$REPO/../references/pspdecrypt

# The supported dumps (2026-11-20: both build from the same scripts; the disc is told apart by UMD_DATA.BIN).
# USA, ULUS-10505, retail:
ISO_SHA1=0a61928fc0eaa78f169b4a172ba6165ecf8564a0
EBOOT_DEC_SHA1=fd68f8dcd544fc2ae20028dedf59a67a6f012d38
# Final Mix, Japan, ULJM-05775, with Truthkey's English patch v1.0.12 (the .chd converted to .iso with chdman).
# Its EBOOT.BIN is a plain ELF (nothing to decrypt); the UNDUB release carries the same program as EBOOT.OLD.
FM_ISO_SHA1=01240b99b5b8104299ad6f210cb49abaa385bf02
FM_EBOOT_SHA1=857b241afa2b6099a8a8d2558863b5e5ce8a4f8e
# Product code of the extracted disc (ULUS-10505 or ULJM-05775), empty before 1-EXTRACT.
disc_id() { head -c 10 "$GAME_DIR/disc-files/UMD_DATA.BIN" 2>/dev/null || true; }
LOAD_BASE=0x08804000
UNIT_SIZE=0x8000

say()  { printf '\n== %s\n' "$*"; }
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

pick() {  # first command that exists
  local c
  for c in "$@"; do command -v "$c" > /dev/null 2>&1 && { echo "$c"; return 0; }; done
  return 1
}
# The first Python that actually runs: on Windows `python3` can be the Microsoft Store placeholder.
PYTHON=${PYTHON:-$(for c in python3 python py; do "$c" -c 'import sys' > /dev/null 2>&1 && { echo "$c"; break; }; done)}
HOST_CXX=${HOST_CXX:-$(pick clang++ g++ || true)}
HOST_CC=${HOST_CC:-$(pick clang gcc || true)}

sha1_of() { "$PYTHON" -c 'import hashlib,sys; h=hashlib.sha1(); f=open(sys.argv[1],"rb")
for b in iter(lambda: f.read(1<<20), b""): h.update(b)
print(h.hexdigest())' "$1"; }

# Windows (Git Bash) wants native paths for the tools; elsewhere this is a no-op.
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi; }
EXE=; case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=.exe ;; esac

need_runtime() {
  [ -f "$RUNTIME/CMakeLists.txt" ] || fail "engine not found: run 'git submodule update --init' (runtime/)"
}
need_python() { [ -n "$PYTHON" ] || fail "Python 3 not found (set PYTHON)"; }
need_cxx() { [ -n "$HOST_CXX" ] && [ -n "$HOST_CC" ] || fail "no C/C++ compiler found (set HOST_CXX / HOST_CC)"; }
