# bbsdecrypt (GPL-3.0 helper)

`bbs_decrypt.cpp` + pspdecrypt's `PrxDecrypter.cpp`, `kl4e.c` and `libkirk/` (git submodule `third_party/pspdecrypt`,
https://github.com/John-K/pspdecrypt, GPL-3.0) build one small executable that decrypts the player's own
`PSP_GAME/SYSDIR/EBOOT.BIN` on the phone during first-run setup.

- The executable is **GPL-3.0** (it links GPL code). Its complete source is this folder plus the submodule; the
  license text is `third_party/pspdecrypt/LICENSE.TXT`.
- It is packaged in the APK as `lib/arm64-v8a/libbbsdecrypt.so` (an executable, named like a library so Android
  extracts it to the app's native library folder) and run as a **separate process**:
  `libbbsdecrypt.so <EBOOT.BIN> <EBOOT.BIN.dec>`. The MIT app never links it; it only runs it and reads the output.
- Contains no game data and no keys beyond what pspdecrypt itself ships.

Build: `android/CMakeLists.txt` target `bbsdecrypt` (`build-android.sh lib` builds it next to libbbs).
