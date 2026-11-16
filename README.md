# kh-bbs-recomp

A native port of **Kingdom Hearts: Birth by Sleep** (PSP) to Android phones and Windows PCs, made by *static
recompilation*: the game's own MIPS (Allegrex) code is translated to C++ ahead of time and compiled for your device,
instead of being emulated. The PSP's operating system and hardware (threads, file system, display, GE graphics) are
re-implemented by the [psp-native-runtime](https://github.com/dguillot-gh/psp-native-runtime) engine, which is
included as the `runtime/` submodule.

> **This repository contains no game data.** You build the game from your own copy of the disc: the scripts read your
> ISO, decrypt and recompile its executable on your PC, and nothing made from it is ever committed or shared. Please
> don't share your builds (the APK and the game folder contain the game).

**Status: early, but playing.** On Android (Vulkan) it boots through the notices and save-data prompt, the title
screen and main menu, loads the first world and starts its in-engine cutscene. The whole disc is recompiled: the
main executable and the nine `MODULE/*.ELF` code overlays, which are swapped in as the game loads them. Movies are
skipped and there is no sound yet (see To do). See `NOTES.md` for the detailed log.

**To do:**
- **Install script:** `6-INSTALL-PHONE.sh` copies `EBOOT.BIN.dec` and `disc-files/` only. The game also needs the
  ISO (it reads `BBS0-4.DAT` as raw disc sectors) and offline-decrypted `decrypted/BBS1-3.DAT` (PGD) beside them,
  and no script makes those yet. Files pushed with adb also need their permissions opened for the app.
- **Movies (FFmpeg):** intro and cut-scene movies are skipped (`host/movie_skip.cpp` reports the stream as finished).
  Real playback needs a minimal FFmpeg build (H.264 video + ATRAC3plus audio, as the runtime's VCS profile uses)
  for Windows and Android, and an `sceMpeg` player; portable-ops-native's `hle_mpeg.cpp` is the model.
- **Audio:** no sound output yet. Needs an Android output (AAudio) and a Windows one behind `sceAudio`, plus ATRAC3plus
  decoding for the game's sound (same FFmpeg build).
- **HD ReMix:** support [Birth-by-Sleep-HD-ReMix](https://github.com/AkiraJkr/Birth-by-Sleep-HD-ReMix) (the
  community HD texture/asset mod) once the game plays; to be looked at, not yet evaluated.

## Supported dump

| Game | Region | Product code | ISO SHA-1 |
|---|---|---|---|
| Kingdom Hearts: Birth by Sleep (En,Fr,Es) | USA | ULUS-10505 (retail) | `0a61928fc0eaa78f169b4a172ba6165ecf8564a0` |

Other releases are untested; `1-EXTRACT.sh` refuses them unless you set `BBS_ANY_DUMP=1`.

## Building

You need **Git** (Git Bash on Windows), **Python 3**, a **clang** (on Windows: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)),
and for Android: the **Android SDK** (build-tools 36.0.0, platforms;android-35, cmake 3.31), **NDK r29** and **JDK 17+**.

```bash
git clone --recurse-submodules https://github.com/dguillot-gh/kh-bbs-recomp.git
cd kh-bbs-recomp
bash scripts/1-EXTRACT.sh "/path/to/Kingdom Hearts - Birth by Sleep (USA).iso"   # checks the dump, extracts it
bash scripts/2-DECRYPT.sh        # builds the decrypter, decrypts EBOOT.BIN
bash scripts/3-GENERATE.sh       # builds the recompiler, generates the C++ (several minutes)
bash scripts/4-BUILD-PC.sh       # Windows/Linux build -> build/pc   (then scripts/RUN-PC.sh)
bash scripts/5-BUILD-ANDROID.sh  # Android build -> build/android/bbs-recomp-debug.apk
bash scripts/6-INSTALL-PHONE.sh  # installs the APK and copies the game files to the phone
```

Settings come from the environment: `HOST_CXX`/`HOST_CC` (compilers), `PYTHON`, `ANDROID_SDK_ROOT`,
`ANDROID_NDK_HOME`, `JAVA_HOME`, `JOBS` (parallel compiles, default 4), `ANDROID_SERIAL` (which phone).
Everything made from your disc goes into `game/`; build output into `build/`. Both are ignored by git.

The PC build uses the Vulkan renderer by default (`GE_BACKEND=dx12` for DirectX 12 on Windows). It currently runs
without a window; `FRAMES=1 bash scripts/RUN-PC.sh` saves the frames it shows to `build/pc/frames/`.

## Credits

- **psp-native-runtime** (MIT): the engine. It grew from
  [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) (MIT); its `CREDITS.md` lists everything it uses.
- **Vulkan renderer:** ported from [lcs-recomp](https://github.com/elmasas/lcs-recomp) (MIT, "PSPRecomp
  contributors"); license text in `runtime/licenses/lcs-recomp-LICENSE`.
- **[pspdecrypt](https://github.com/John-K/pspdecrypt)** (GPL-3.0): its libkirk/PrxDecrypter are fetched as the
  `third_party/pspdecrypt` submodule and only compiled into the local decrypting tool on your PC; nothing of it ships
  in the game.
- **References** (read, not copied): [PPSSPP](https://github.com/hrydgard/ppsspp) for PSP hardware behaviour,
  [OpenKH](https://openkh.dev/bbs/) for Birth by Sleep's file formats, and
  [Portable Ops Native](https://github.com/zexk/portable-ops-native) / PortableKit for another PSP static
  recompilation.

Kingdom Hearts is © Disney / Square Enix. This project is not affiliated with them.
