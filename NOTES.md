# Kingdom Hearts: Birth by Sleep (USA): bring-up notes

Game-specific state and what worked. Keep it current: update it at the end of every session.
Shared runtime/codegen changes belong in `psp-native-runtime`, not here.

Build and run from the workspace root: `.\psp.ps1 -Game bbs -Stage regen|build|run`.
Inputs (local only): `extracted\KingdomHeartsBirthBySleep\` (`decrypted\EBOOT.BIN.dec`,
`disc-files\`, corpus `generated-psprecomp-fixed32k\`, or `generated-v2\` after a regen).

## Current: work-PC session 2026-11-16, part 5: Vulkan frames reach guest VRAM (auto-save notice visible)

- **Fix:** shared `psphost::ge_display_present_gpu_frame(rt, fb, stride, format, vblank)` in
  `psp-native-runtime\host-shared\ge_display.cpp/.hpp`, called from sceDisplaySetFrameBuf in `kh-bbs\host\main.cpp`
  and `mgs-peace-walker\host\main.cpp`. No-op on the software path. With a GPU backend: set_display_framebuffer,
  finish_color_frame(psphost::vblank_count()), and when it doesn't present directly, write the RGBA readback into
  guest VRAM at the displayed buffer (565/5551/4444/8888, stride; point-sampled to 480x272) + capture_frame_if_requested.
  Only the displayed buffer is written; other targets stay with the software rasterizer.
- **BBS (generated-v3, Backend=Vulkan, NativeGE=true, cap 60M, 170 s):** 8 dumps, from frame_000028 on the same
  auto-save notice as software frame_000020. Not pixel-exact: best frame differs in 15,745 of 130,560 pixels (mean
  |diff| ~50 per pixel over RGB), all inside the dialog panel (x 57-422, y 49-224): the software path has a fine
  dithered red pattern (~300 colours), Vulkan a smooth gradient (92 colours); text, icon, OK button match.
- **PW smoke (Vulkan, v2 corpus, cap 20M):** builds and runs to the usual `Dispatch limit reached at 0x08B42A6C`;
  `draws=262352 frames=0`, one black dump, i.e. unchanged. PW's GPU frames never finish (not investigated).
- Files edited (part 5): psp-native-runtime\host-shared\ge_display.cpp, ge_display.hpp, kh-bbs\host\main.cpp,
  mgs-peace-walker\host\main.cpp, GLES-BACKEND-PLAN.md.

## Current: work-PC session 2026-11-16, part 4: FIRST PICTURE (software GE); black = Vulkan backend not written back

- **Software backend shows real frames.** generated-v3 build, VCSNative.ini temporarily `Backend=Software` (restored to
  Vulkan after), `PSPRECOMP_GE_FRAME_DUMP=1`, cap 60M, 170 s: 8 dumps; frame_000014..20 non-black (~73k non-zero
  bytes, ~300 colours). frame_000020 = the red "INFORMATION: This game uses an auto-save feature..." dialog with an
  OK button (PNG copy of the scratch dump looked at by eye).
- **GE side is fine:** GE_DIAG: ~73k lists in 120 s, each `sceGeListEnQueue(list=0x09A7A900, stall=list)` then
  UpdateStallAddr runs it and the finish callback 0x08A8D630 fires; SetFrameBuf alternates 0x04000000/0x04044000,
  stride 512, format 1 (565), which is what the dump reads.
- **Cause of the black dumps with Vulkan:** `ge_gpu_backend_vulkan.cpp` draws into its own GPU target and only
  reads back/presents in its frame-finish path (report `frames=0`, `frame readback 0 MiB`), which VCS's display code
  drives (vcs_profile.cpp ~7044) but the shared `host-shared\ge_display.cpp` / BBS SetFrameBuf path never calls. Guest
  VRAM stays zero, so the dumps (which read guest VRAM) are black. Probably PW's black frames too (same shared GE path,
  Vulkan ini) — check PW with Backend=Software.
- **Proposed fix:** in ge_display (shared), on sceDisplaySetFrameBuf / list finish with the Vulkan backend, call the
  backend's frame finish with the displayed FBP and copy its readback into guest VRAM (or dump from the readback).
  Until then use Backend=Software for bring-up runs/dumps. Not implemented this session.
- No code edited in part 4 (VCSNative.ini in %TEMP%\psp-clang-build-v3\bbs changed for one run and restored).

## Current: work-PC session 2026-11-16, part 3: regen into generated-v3; frame loop runs, frames black

- No codegen change needed: 0xD03C8080 (vi2uc) was already in `src\decoder.cpp` (10/29) and in the 11/03 clang
  `psp_recomp.exe`. Regen (approved): `psp_recomp.exe EBOOT.BIN.dec --auto ...\generated-v3 0x08804000 0x8000`
  (BELOWNORMAL). 113 units, 656,217 instructions (same as v2); "vfpu4 not lowered" 18 -> 4. Left: 0xD0238082 /
  0xD0238004 (group 1 op 3, vrndf2) at 0x088583F8, 0x08858E80, 0x0886BD60, and 0xD0428080 (group 2 op 2, vbfy1)
  at 0x08AF9950. Other unlowered counts unchanged (vfpu0 262, vfpu3 92, special 949, unknown 1290). generated-v2 kept.
- Build: `clang-build.sh bbs ...\generated-v3` with CLANG_OUT=`%TEMP%\psp-clang-build-v3` (v2 objects kept).
- Run (170 s, cap 400M): no stop. Frame loop: in 60 s ~28,450 FlipSema waits / FlipThread wakeups.
  Only one frame dump (dumps keep changed frames), all pixels 0. Next: why the picture stays black (GE lists, draw
  targets, framebuffer vs dump; compare PW's black frames).

## Current: work-PC session 2026-11-16, part 2: FlipSema stall fixed; then VFPU 0xD03C8080 (vi2uc)

- **Cause:** FlipThread (prio 16) is woken by the vblank handler 0x0880919C via 0x088C55B0 (CancelWakeup +
  WakeupThread(5)); afterwards the handler clears the flip-request byte [0x08BD4A9C] (0x08809214). Our `wake_thread`
  switched to FlipThread *inside the handler* (guest-call chain on RsrcBuilder). The handler's tail ran only when
  RsrcBuilder next resumed, a frame later, after user_main had set the byte for frame 2; it cleared it, so FlipThread
  was never woken again and user_main waited on FlipSema forever. ([0x08B5BD18] = FlipThread uid 5, set once at
  0x088094E8: fine.) Evidence: PSPRECOMP_NO_CHAIN+TRACE with WATCH_WRITE on 0x08BD4A9C.
- **Fix:** `psp-native-runtime\host-shared\guest_calls.cpp/.hpp`: new `psphost::in_guest_calls()` (additive, PW
  unchanged); `kh-bbs\host\thread_manager.cpp` `wake_thread` does not preempt while the caller runs a guest-call chain
  (PSP handlers run to completion). SignalSema etc. from a handler may need the same rule later.
- **Result on generated-v2:** past FlipSema, GE draws=108, then `Unsupported Allegrex instruction 0xD03C8080 at
  0x08AF9210: vfpu4 not lowered yet`. 0xD03C8080 = VFPU4 group 1 op 28 = **vi2uc**, already decoded/lowered in
  `src\decoder.cpp` since 10/29; generated-v2 (10/27) predates it. Regen into `generated-v3` (part 3 below).
- Files edited (part 2): psp-native-runtime\host-shared\guest_calls.cpp, guest_calls.hpp, kh-bbs\host\thread_manager.cpp.

## Current: work-PC session 2026-11-16 (clang): the 0x48 null-object crash is fixed; now stalls on FlipSema

- **Cause of the 0x00000048 crash:** user_main (0x08A8BBB0..BC6C) loads group "common" (`0x08842344("SYS","common",1000,0)`),
  waits for the busy byte 0x09F35630, then looks up "common" type 1 (`0x0883BA98` -> `0x0883DA0C`, per-type lists at
  0x09F35388 + type*16). The lookup only accepts resource objects in state 7 (byte +9; check in 0x088378A8). common.arc
  (obj 0x09A20CD0) went 2 -> 4 -> 7 -> **3**: RsrcBuilder (prio 21) runs the load job 0x08836E2C, which submits the read
  (0x0880FDB0, completion callback 0x08837820), wakes AsyncFileAccess (prio 20) and *then* writes state 3 ("requested").
  Our sceIoReadAsync + sceIoWaitAsyncCB completed inline, so Async did the whole read and set 7 before RsrcBuilder
  resumed and overwrote it with 3. On a PSP the disc wait blocks Async, so 3 comes first. Same for FontEn/SeInfo/common_ex.
  Found with PSPRECOMP_WATCH_WRITE on the object and PSPRECOMP_NO_CHAIN=1 PSPRECOMP_TRACE=1.
- **Fix:** `host\disc_io.cpp/.hpp`: sceIoWaitAsync (0xE23EEC33) and WaitAsyncCB (0x35DBD746) call a hook after a
  successful wait (PollAsync doesn't); `host\main.cpp` sets it to `ThreadManager::wait_like_delay`. The crash is gone.
- **Also changed (shared, PW-safe, additive):** `psp-native-runtime\host-shared\interrupts.cpp/.hpp`: `psphost::vblank_count()`
  counts every delivered vblank (virtual clock too); BBS sceDisplayGetVcount (main.cpp) now returns it (was bbs::vblank_count,
  which only moved on vblank waits nobody makes). Did not unblock the next stop on its own.
- **Next stop (no error, runs until timeout):** one frame dumped (0x04000000, all black), then user_main waits on sema
  0x602 "FlipSema" after its second flip request (guest-call 0x08A8D630) and FlipThread is never woken again. Vblank
  handler 0x0880919C wakes it only if `GetVcount - [0x08BD4A74] != 1`, `[0x08B5BD18] != 0` and byte `[0x08BD4A9C] != 0`;
  next: watch those two globals (who should set them: GE finish/signal callback 0x08A8D630? sceGe list completion?).
- Files edited: kh-bbs\host\disc_io.cpp, kh-bbs\host\disc_io.hpp, kh-bbs\host\main.cpp,
  psp-native-runtime\host-shared\interrupts.cpp, psp-native-runtime\host-shared\interrupts.hpp. PW not rebuilt.

## Current: work-laptop session 2026-11-03 (clang; read first): first sceDisplaySetFrameBuf, then a null object

Built with `psp-native-runtime\bringup\clang-build.sh bbs` (this laptop has no MSVC/CMake) on the 10/27
`generated-v2`, copied over `out\Release\BBSBringup.exe` (the 10/28 MSVC exe kept as `*.msvc-20261028.exe`).
`.\psp.ps1 test` not run (needs CMake). Runs: `.\psp.ps1 -Game bbs -Stage run -Trace -MaxDispatch 8000000`.
1. The FlipThread crash is gone (the 10/28 review's lambda fix works).
2. Stops met and fixed, in order (all in `host\main.cpp` unless noted):
   - `sceAudio` 0x01562BA3 (Output2Reserve) and the rest of sceAudio: no sound; return values as VCS; the blocking
     outputs wait like DelayThread (`ThreadManager::wait_like_delay`) and move virtual time by the buffer length.
   - `sceSasCore` (all 27 imports): silent; every voice reads as ended, `__sceSasCore` writes a grain of silence.
   - `sceAtrac3plus` (9 imports): IDs handed out, every decode returns 0 samples with the end flag set.
   - `sceKernelLockMutexCB` (0x5BF4DD27, thread_manager.cpp): same as LockMutex.
   - `sceKernelGetSystemTimeWide` / `sceKernelGetSystemTime`: the monotonic clock (Wide = v0:v1).
   - `sceKernelSetAlarm` / SetSysClockAlarm / CancelAlarm: `psp-native-runtime\host-shared\interrupts.cpp`.
   - `sceUtilityGetSystemParamString` (nickname "PSP"), `sceNetGetLocalEtherAddr`, `sceNetEtherNtostr`.
   - **Null heap object at 0x08807ABC: the old `sceKernelReferThreadStatus` stub returned UNKNOWN_THID**, so the
     game's heap setup (unit_0001 L_0880D698, info->stack at +48) never ran and the allocator global 0x08B5BD98 stayed
     0. Now a real SceKernelThreadInfo in thread_manager.cpp (the main.cpp stub is gone).
   - `sceKernelCancelWakeupThread` (0xFCCFAD26), `Kernel_Library::sceKernelGetThreadId` (thread_manager.cpp).
3. Then only AUDIO and Sd::UpdateMSec ran (alarms one-shot, then: vblank handlers ran only on vblank waits).
   **user_main waits on sema 0x602 (WaitSemaCB), which FlipThread signals; FlipThread is woken by the vblank
   handler 0x0880919C (int 30 sub 0), and no thread waits for vblank.** Fix (interrupts.cpp, shared): alarms are
   periodic with their first interval, and vblank follows the virtual clock (DelayThread and the audio waits move
   it; `deliver_alarms` runs the vblank handlers once per 16.683 ms crossed).
4. **Now: `sceDisplaySetFrameBuf` twice (0x04000000, then 0x04044000), then exit 1:** guest access at 0x00000048 in
   0x08A8BC6C (unit_0080, thread on stack ~0x09FC2FE0): `0x0883BA98(...)` returned 0 and 0x08836660 reads +0x48.
   0x0883BA98 is a dispatcher into 0x0883DA0C (args: a stack struct, 1, 0, 1, t0=0). All file reads in the
   IO trace succeed (`PSPRECOMP_IO_TRACE=1`), so it is not a missing file. Next: find what 0x0883DA0C looks up
   (a resource table filled by RsrcBuilder?) and which HLE result it depends on.
- Log: `logs\bbs\20261103-122609-run.log`; IO trace run right after it.

## Earlier review 2026-10-28 evening (work PC): the S3 crash is a dangling lambda capture (fixed, not built)

- `host\thread_manager.cpp`, the 4 vblank waits (~692) and SleepThread(CB) (~703) were
  registered with `[this, &delay_yield]`: a reference to a local lambda of `install_hles`. It dies
  when `install_hles` returns; the first vblank/sleep call read garbage as `this` and crashed at
  `current_uid_`. Not heap corruption, not a BBS handler's OOB write. The review changed both to
  `[this, delay_yield]`.
- Rule: a lambda given to `register_hle` never captures a local by reference (`main()` locals are
  fine: they live for the whole run).
- **Also written by the review:** PW's S2c scheduler change (states `Asleep`/`Delayed`,
  `delayed_`, `sleep_thread`/`wake_thread`; `delay_yield` is a member now, so no local lambda
  is left). Syntax-checked with llvm-mingw clang on the work PC: all host files clean. Not
  built with MSVC; scheduler_test.cpp passes with it (clang, work PC).
- Next: SESSION-PLAN.md S3b (build + run).

## Current: session 2026-10-28 (home PC), S3 — into the frame loop (GE shim + display/ctrl in; crashes in delay_yield)

- **Step 1 (done):** copied PW's GE shim + display/ctrl handlers from `mgs-peace-walker\host\`
  into `kh-bbs\host\` (same code, `BRINGUP SHIM` comments kept). One build, clean.
- **New BBS-only handlers (in, `host\main.cpp`):** boot between the loader and the frame loop
  needed three more HLEs PW didn't hit:
  - `ThreadManForUser::0x369ED59D` sceKernelGetSystemTimeLow — low 32 bits of a steady µs clock.
  - `scePower::0x34F9C463` scePowerGetPllClockFrequencyInt — BBS caller wants 222 MHz; "already
    set" fast path, returns 222.
  - `ThreadManForUser::0x17C1684E` sceKernelReferThreadStatus — returns UNKNOWN_THID (0x80020198),
    leaves the caller's buffer untouched.
- **Where it runs (log `20261028-160546-run.log`, lines 364-374):** PGD load, GE init and
  thread/Power setup all pass. Threads up: user_main (uid 1, prio 19), DiscInfoBind (uid 2, 20),
  AsyncFileAccess (uid 3, 20), RsrcBuilder (uid 4, 21). Then **FlipThread (uid 5, prio 16 —
  HIGHEST)** is created and started; it signals FlipSema 0x602 (`sceKernelSignalSema` 0x3F53E640),
  then calls `sceKernelSleepThread` (0x9ACE131E).
- **The stop (NOT Done-when):** raw host access violation, **0xC0000005 / exit -1073741819**, the
  moment FlipThread yields to its ready background peers. A pinpoint diagnostic at the top of
  `delay_yield` printed `[pw-diag] delay_yield current=` and died — so the fault is reading the
  ThreadManager's **own** state (`current_uid_`/`ready_`/`threads_`), not `activate_next`.
  That's a raw hardware AV (a C++ throw would be 0xE06D7363; an AOT guest OOB would throw too), so
  it's a **heap-corruption of the scheduler's containers**, BBS-specific: the same `delay_yield`
  runs PW fine (S2/S2b), and the BBS trigger is FlipThread (highest prio, first to yield to ready
  uid 3/4) — a host-side OOB write from a BBS handler corrupting the ThreadManager. **No
  handler-copy fix; needs AddressSanitizer.** Carried to **S3b**.
- **Budget:** 5 of 6 run cycles used (cycles 1-3: the three handlers above; cycle 4: first crash
  report; cycle 5: the diagnostic that pinpointed `delay_yield`). Closing on bookkeeping rather than
  spending the 6th on an unpinnable heap bug.
- **Next (S3b):** ASAN / CRT-debug-heap build of `kh-bbs`, catch the OOB write that corrupts
  `ready_`/`threads_`; fix it; then BBS should pass the FlipThread yield and reach the vblank loop.

## Earlier (2026-10-28): S1 done — PGD archives decrypted, loader passes

- **Correction to the review below:** the PGD archives use **three different keys**, not one.
  All three sit contiguously at EBOOT file offset `0x32E3D8` (VA `0x08B313D8`), 16 bytes each:
  - BBS1.DAT: `77b9de64cd1fc39279d190564728a378`
  - BBS2.DAT: `8f67981eacf0717434b1a5f98a0cd18e`
  - BBS3.DAT: `7f0067c280626625276e8c3eb8307345`
  Decrypted sizes: BBS1 206176256 B, BBS2 206276608 B, BBS3 231120896 B (BBS3 plaintext
  starts `bbs3.dat`). Command: `pgddec.exe <in.DAT> <out.DAT> <key>` (flags 2, then 1; no
  libkirk edits were needed).
- **Run loop (5 cycles, `20261028-100243-run.log` is the last):**
  1. `sceKernelWaitThreadEndCB` (0x840E8133) — mapped to the existing `wait_thread_end`
     (cooperative HLE: CB variants have the same semantics). Both hosts, both NID tables.
  2. `sceKernelDeleteThread` (0x9FA03CD3) — new `ThreadManager::delete_thread`: only
     Created/Completed may be deleted (0x800201A4 otherwise, 0x80020198 unknown); erases the
     record and its end-waiters; stacks stay reserved (bump allocator has no free list).
     Mirrored from kh-bbs into PW.
  3. `sceGeEdramGetAddr` (0xE47E40E4) — returns `kVramPhysicalBase` (PW main.cpp).
  4. `sceGeSetCallback` (0xA4FC06A4) — validates the 16-byte struct, hands out a uid,
     never fires (PW main.cpp).
  5. `sceGeListEnQueue` (0xAB49E76A) — **stop.** Needs the GE display-list engine; that's
     PW's S2 (GE shim) and S7-S8 (real GE), which BBS picks up in S3/S9.
- **Done-when fired (log 20261028-100243, lines 242/244):** `[watch-write] uid=2
  name=DiscInfoBind ... 0x09A7A668 old=0x0 new=0x1`, then `[pw-thread] switch
  reason=thread-return uid=1 name=user_main pc=0x0880F6F8`. After that the guest creates
  the `SceGuSignal` event flag (uid 2305) and calls into GE init — the loader is through.

## Review 2026-10-28 (work PC): BBS1-3.DAT are PGD-encrypted; fix written (not built)

- `head -c 16` of the archives: **BBS1/2/3.DAT start with `00 50 47 44` = `\0PGD`** (amctrl DRM).
  BBS0.DAT (`bbsa...`) and BBS4.DAT (`bbs4.dat`) are plain, which is why BBS0 went through.
- `sceIoIoctl(fd, 0x04100001, key, 16)` is "define the PGD key for this file". After it, reads
  return **decrypted** data starting at plaintext offset 0. Our lenient handler did nothing, so the
  18-byte header read got ciphertext, the parser's virtual call failed, and the error flag at
  `0x09A7A66C` aborted the loader.
- **Key** (the game's own, `indata` = EBOOT `0x08B313D8`): `77b9de64cd1fc39279d190564728a378`.
  The same key is used for every PGD archive. The 16 bytes after it may be another key for
  something else; ignore it unless a run asks for it.
- **Fix:**
  - **Offline tool:** psp-native-runtime `tools\pspdec\pgddec.cpp` (`0409746`) decrypts a PGD file
    with libkirk, the same local-only GPL arrangement as pspdec.
  - **Host** (`host\disc_io.cpp` `DiscIo::ioctl`): on 0x04100001 for a file starting with `\0PGD`,
    the fd switches to `extracted\KingdomHeartsBirthBySleep\decrypted\<same name>`, at position 0.
    If the copy is missing, the run **stops with the exact `pgddec` command to run**. Other ioctl
    commands still return 0. The lenient lambda in `main.cpp` is gone.
- **Hints for the run after this:**
  - Expect `[io-ioctl] fd=N pgd-key: now reading ...BBS1.DAT`, then reads on that fd, then the
    same for BBS2/BBS3, then raw BBS4 (plain, like BBS0), then the flag `0x09A7A668` set to 1.
  - If the 18-byte header still fails: `Format-Hex` the first 32 bytes of the decrypted BBS1.DAT.
    BBS0's plaintext starts `bbsa`, so a sane header likely has a similar ASCII tag. Random bytes
    mean the decryption is wrong (wrong flag, or a block offset off by the 0x90 header).
  - The game may getstat the PGD file and size reads from `st_size`. That's the **encrypted** size
    (header + data + MAC table). If a read comes up short or a size check fails, report the
    decrypted size there (pgddec prints `data_size`). A one-line change in `getstat` for files
    with a decrypted copy.
  - After the flag is set, user_main leaves its wait loop at `0x0880F6F8`. Likely next stops:
    display (`sceDisplaySetMode`/`SetFrameBuf`), GE (`sceGeEdramGetAddr`, `sceGeListEnQueue`),
    ctrl and audio init. PW hit exactly these on 10/27 and has handlers for the first ones:
    `.\psp.ps1 hle <NID>` shows where.

## Review 2026-10-27 (work PC, evening): why DiscInfoBind quits, and the fix (not built)

DiscInfoBind (`L_0880F750`, `generated_unit_0001.cpp`) loads `BBS%d.DAT` for d = 0..4 and sets the
flag at `0x09A7A668` only if all five succeed. The loader `L_0880E170` → `L_0880EBD4`:
1. `sceIoGetstat("disc0:/PSP_GAME/USRDIR/BBS0.DAT", sp)`: size from `st_size`, and
   **`st_private[0]` (stat+0x40) = the file's starting sector on the UMD** → handle+4.
2. With index 0 it takes the **raw UMD** path: `sceUmdGetDriveStat`, lock, `sceIoOpen("umd1:")`
   (the whole disc as a block device), unlock.
3. `L_088111D4(fd, lbn, buf, sectors)`: seek and read whole 2048-byte sectors from umd1:.

What went wrong in our host:
- **`sceIoGetstat` cleared 280 bytes** of an 88-byte `SceIoStat` on the thread's stack. That
  wiped `L_0880EBD4`/`L_0880E170`'s saved registers and return addresses, so after the umd1:
  open the thread "returned" to 0 (`psp_thread_return` in the trace) and never got to the read.
  The stat layout was also wrong: date stamps from 0x0C overwrote the high half of `st_size`.
- umd1: was a dummy fd with nothing behind it, and `st_private[0]` was 0.
- `sceIoLseek`/`LseekAsync` read the offset from a1 and whence from a2. They're 64-bit:
  offset a2:a3, whence t0, result v0:v1.

**Fixed in `host\disc_io.*` + `host\main.cpp`:**
- Real 0x58-byte `SceIoStat`, with `st_private[0]` = the LBN from the ISO's directory.
- `umd0:`/`umd1:`/`umd:` open a block device on the ISO (seek/read in sectors).
  `disc0:/sce_lbn0x.._size0x..` opens a byte range of the ISO.
- ISO = `PSPRECOMP_ISO`, else the first `*.iso` next to `disc-files\`. The banner prints `ISO: ...`.
  Checked with Python against the real ISO: BBS0.DAT is LBN 58032, sizes and first 4 KB match
  the extracted files.
- 64-bit seeks. Message-pipe `result` pointer from t0 and `sceIoDevctl` outdata from t0 (same
  convention bug as PW; see PW NOTES).
- **Expect:** `[io-getstat] ... lbn=58032`, `[io-open] raw ISO fd=N block`, then `[io-seek]` /
  `[io-read] raw` lines for BBS0..4, then a `[watch-write]` setting `0x09A7A668` to 1, and the
  main thread leaving its wait loop.

## Session 2026-10-27 (home PC, evening)

- step 1: done — build OK, no host compile fixes needed (20261027-192621-build.log).
- step 2: done — banner `ISO: ... (863024 sectors)` ✓; getstat BBS0 `lbn=58032` ✓; `[io-open] raw ISO fd=3` ✓;
  seek 58032 + `[io-read] raw units=112` (first block) ✓. **First deviation:** BBS1.DAT was opened as a plain
  file (`disc0:/PSP_GAME/USRDIR/BBS1.DAT`, fd=4) and then `IoFileMgrForUser::0x63632449`
  (sceIoIoctl/sceIoDevctl; args fd=4, cmd=0x04100001, indata=0x08B313D8, inlen=0x10) → **exit 3, not
  registered in any host**. 10 lines before: async read of 112 sectors on fd=3 + waitasync OK, then
  getstat/open of BBS1. No watch-write of 0x09A7A668 yet (log 20261027-192743-run.log).
- step 3: **cycle 1 done, BBS parked per BUDGET-USED rule.**
  - Fixed the step-2 stop: `IoFileMgrForUser::0x63632449` = **sceIoIoctl** (fd in a0, unlike
    sceIoDevctl's device name; outdata = arg 5 in t0). Added NID to both shared tables
    (psp-native-runtime `nids.csv` + `nid_registry.cpp`) and a lenient success handler in
    `host\main.cpp` (cmd 0x04100001 semantics unknown; leaves outdata untouched, returns 0).
  - Re-run (20261027-195442-run.log): **exit 0, `Dispatch limit reached at 0x0880F6F8` — BUDGET
    USED.** Wait target: user_main (uid=1) polling byte `0x09A7A668` in its 33 ms DelayThreadCB
    loop; never written by anything.
  - What DiscInfoBind now does (log lines 153-180): BBS0 full raw-ISO path ✓ (getstat lbn=58032,
    open fd=3, `[io-read] raw units=112`). BBS1: getstat (lbn=394144), open fd=4, devctl OK,
    `sceUmdUser::0x6B4A146C` (GetDriveStat), lwmutex lock `0x09A7A6C4`,
    `IoFileMgrForUser::0xA0B5A7C2` = sceIoReadAsync(fd=4, buf=0x09FB8600, **18 bytes**)
    `[io-async] fd=4 result=0x12`, WaitAsyncCB ret=0, unlock — then **the guest itself exits the
    thread** (`psp_thread_return a0=0x62`). BBS2–4 never reached; flag never set.
  - Lifted code (generated_unit_0001.cpp:10652-10698): after the wait, a virtual call
    (vtable-style dispatch table: `{int16 id, func}` at *(obj)+24) returns; if negative, the
    failure path checks the byte at **`0x09A7A66C`** (flag+4: `0x09A80000-22908`) — if nonzero it
    proceeds toward thread exit; if zero it loops back (`L_08810F9C`) to the next DAT. Same byte is
    checked at the next function's entry (10782). So the guest recorded an error at 0x09A7A66C
    (its own C++ error handler) and aborted.
  - Leads for a fresh session (not chased this session): (a) our sceIoIoctl 0x04100001 handler
    writes nothing to outdata (t0) — the guest may expect a word there; (b) the 18-byte header
    read into 0x09FB8600 is parsed by the virtual call — verify the first 18 bytes of
    `disc-files\PSP_GAME\USRDIR\BBS1.DAT` against what the guest expects (magic/version);
    (c) `psp_thread_return a0=0x62` — the thread's own exit code, check L_0880F750's epilogue for
    what 0x62 means.
  - Per session rule (BUDGET USED → record, stop): **BBS loop ended here.**

## Earlier (2026-10-27, home PC): BBS0+BBS1 complete; guest exits the thread — parked on BUDGET USED

- **BBS0 + BBS1 now complete (2026-10-27, home PC).** BBS0: getstat `lbn=58032` → raw-ISO
  `sceIoOpen("umd1:")` fd=3 → 64-bit seek 58032 → `[io-read] raw units=112` ✓. BBS1: getstat
  (`lbn=394144`) → open fd=4 → `sceIoIoctl` (devctl) → `sceUmdGetDriveStat` → lwmutex →
  18-byte async read of the header into `0x09FB8600` → **then the guest itself calls
  `psp_thread_return a0=0x62`** (its own C++ error handler; it recorded an error at
  `0x09A7A66C` = flag+4). BBS2–4 never reached; flag `0x09A7A668` never set.
- Fixes landed this session: (1) `delay_yield` in `thread_manager.cpp` — the delaying thread
  now loses the scheduling round to any ready peer, so DiscInfoBind actually runs; (2)
  `DiscIo::open` — bare-drive / pseudo-path probes (e.g. "umd1:") get a valid synthetic fd
  (VCS does the same); (3) lenient `sceIoIoctl` (0x63632449) success handler (cmd 0x04100001
  semantics unknown; outdata untouched, returns 0).
- **Stop (exit 0, BUDGET USED, parked):** `Dispatch limit reached at 0x0880F6F8` (cap 300000).
  DiscInfoBind (uid=2) now runs its full visible job — `sceIoGetstat`, `sceUmdGetDriveStat(1)`,
  lock, `sceIoOpen("umd1:")` (succeeds), unlock — then **returns** without ever writing the
  done-flag at `0x09A7A668`. user_main (uid=1) polls that flag in a 33 ms `DelayThreadCB` loop at
  `0x0880F6F8` and spins to the cap. Trace: `logs\bbs\20261027-182557-run.log` (lines 116-132).
- **Next lead (fresh session):** DiscInfoBind exits via its own C++ error handler right after
  the 18-byte BBS1 header read (it wrote an error at `0x09A7A66C`). Leads (from the session
  record above): (a) our lenient `sceIoIoctl` writes nothing to outdata — the guest may expect
  a word there; (b) the 18-byte header into `0x09FB8600` is parsed by the virtual call — verify
  the first 18 bytes of `disc-files\PSP_GAME\USRDIR\BBS1.DAT` (magic/version) against what the
  guest expects; (c) `psp_thread_return a0=0x62` — check `L_0880F750`'s epilogue for what 0x62
  means. Re-run with `PSPRECOMP_WATCH_WRITE=0x09A7A66C` (+`SIZE=1`) to watch the error write.
- (Earlier, work-PC review) the run exited 0 at the dispatch cap:
  `Dispatch limit reached at 0x0880F6F8` (cap 300000). No missing HLE, no unlifted code.
- **It is not the main loop.** `generated_unit_0001.cpp` ~8020-8090, function around
  `L_0880F654`:
  1. `sceKernelCreateThread` (stub `0x08B30658`, via `0x088C54C4`) and `sceKernelStartThread`
     (stub `0x08B305F8`, via `0x088C54E0`) start a thread named **"DiscInfoBind"** (string at
     `0x08B3142C`; the next string is `disc0:/PSP_GAME/USRDIR/BBS%d.DAT`).
  2. Loop at `L_0880F6F0`: `sceKernelDelayThreadCB(33334)` → call `0x08816918` (reads the bytes
     at `0x08B5BE41/42`, probably an exit-request check) → if it returns 0, test the byte at
     `0x09A7A668` (`s0 = 0x09A80000`, offset -22936) and loop while it's 0.
  3. So the main thread waits for DiscInfoBind to set that byte, presumably after it opens and
     indexes BBS0-4.DAT. Our `DelayThreadCB` yields only to a *Ready* thread. If DiscInfoBind
     is asleep in a wait nobody completes, or exited early, the main thread spins until the
     budget runs out.
- **Next:** `.\psp.ps1 -Game bbs -Stage run -Trace -Env @{ PSPRECOMP_IO_TRACE = '1'; PSPRECOMP_WATCH_WRITE = '0x09A7A668'; PSPRECOMP_WATCH_WRITE_SIZE = '1' }`.
  Look for: the DiscInfoBind create/start lines, whether it ever runs, its `[io-open]` of
  `BBS%d.DAT`, its last HLE before it sleeps, and any `[watch-write]` on the flag.
- **Step 1 traced run (20261027-174943-run.log, 47.8 MB):**
  1. DiscInfoBind **created** (uid=2, entry=0x0880F750, priority=20, log line 109) and
     **started** (`sceKernelStartThread` 0xF475845D a0=2, returned OK, lines 113-114).
  2. **Never ran:** 0 `[pw-thread] switch` to uid=2 in the whole log; 0 `[io-open]`/`[io-`
     lines (no `BBS%d.DAT` open at all).
  3. Last HLE before the stop: `ThreadManForUser::0x68DA9E36` (DelayThreadCB 33334us) by
     user_main at 0x0880F6F8, repeating to the dispatch cap.
  4. Flag 0x09A7A668: only two writes — init zero and user_main store8 `new=0x0` (line 107).
     Nothing set it to 1.
  5. Root cause: `delay_yield` (thread_manager.cpp:619) re-enqueues the delaying thread as
     Ready and competes on priority: user_main (19) beats DiscInfoBind (20) every round, so
     the started thread is never scheduled. PW's host has no delay HLE, so this is BBS-only.
  6. Fix (step 2): in `delay_yield`, the delaying thread loses the scheduling round to any
     ready peer (priority bumped to 0xFFFFFFFF around `activate_next`, restored after).
- The `0x0880A5DC` unlifted-code stop below is fixed (the `5adcf15` seed fix; `.\psp.ps1 lifted
  0x0880A5DC -Game bbs` now says `lifted: yes`). The rest of this block is the earlier diagnosis.
- (Earlier, work-PC review) run stops 0.1 s in: `invalid internal function entry` at `0x0880A5DC`. **Not a deadlock and not
  a reason to park**: it's unlifted code (`.\psp.ps1 lifted 0x0880A5DC -Game bbs`: no label, no mention).
- `0x0880A5DC` is a real function (`addiu sp,sp,-16` right after a `jr ra`). Its only reference is
  the first word of **`.cplinit`** (`0x08B30970`, 0x320 bytes = 100 `{func, 0}` pairs): SNC's C++
  static-initializer table. All 99 functions in it were unlifted.
- Cause: BBS's EBOOT is a fixed ELF (type 2, **no relocations**), and SNC links `.cplinit`, `.rodata`
  (vtables) and `.data` into the *executable* segment. Codegen took data-pointer seeds only from
  non-executable segments and from R_MIPS_32 relocations, so it saw none of them. Words in those
  sections that point into code but have no label: `.rodata` ~10.7k, `.data` ~7.4k,
  `.linkonce.d` ~1.3k (some will be jump-table or mid-function targets, not functions).
- **Fix: psp-native-runtime `5adcf15`** (not compiled on the work PC). `collect_relocated_data_code_pointers`
  in `src\program_analysis.cpp` now also scans allocated, non-executable PROGBITS sections inside
  executable segments of a fixed ELF, and seeds only words that land in an executable section.
  PRX modules (KJFS, zlibdec) are unchanged.
- Expect after regen: `function seeds` well above 12,533 and `registered blocks` above 109,599; a
  larger corpus and a longer first build. Then `L_0880A5DC:` exists in `generated_unit_0000.cpp`.

## 2026-10-27: regen done, build still fails — shared-codegen root cause (parked)

Ran step 6. `.\psp.ps1 -Game bbs -Stage regen` **OK**: 113 units / 481,005 instructions →
`generated-v2`. `.\psp.ps1 -Game bbs -Stage build` **FAILED** (exit 1): 32 distinct errors, all
`recomp_unit_0113/0114/0115: undeclared identifier` (C2065) + `invoke_chained_direct: no matching
overload` (C2672). Failing units: 0042 0069 0074 0079 0105 0107.

This is **not** the 10/24 "stale corpus" error — the corpus is now fresh. It's a shared-codegen bug
in `psp-native-runtime\tools\codegen_main.cpp`:
- A fixed address "bucket" is only emitted if it has ≥1 global entry: `if (!unit.entries.empty())
  units.push_back(...)` (line 1536). A bucket with lifted code but no entry is **dropped**.
- But cross-unit edge resolution computes the destination bucket from the target address (lines
  897-899) and, when the target isn't a registered entry, falls back to a **bare** call to the bucket
  symbol (line 867). `direct_entry_ids` only holds entries of *emitted* buckets (lines 1542-1546), so a
  dropped-but-referenced bucket always takes the bare path → references a symbol that was never generated.
- BBS has fixed call edges into buckets 113/114/115 that have code but no entry → dropped → still
  referenced → C2065. PW's layout doesn't produce such buckets, so PW builds.

**Fix (parked, needs sign-off — it's shared infra + a regen):** relax the `!entries.empty()` filter at
line 1536 so every code-bearing bucket is emitted. Caveats: (1) confirm a no-entry unit still emits a
valid `recomp_unit_NNNN_entry` (declared at line 1557, referenced at 1615) — else this trades a compile
error for a link error; (2) mirror the same filter in
`profiles\vcs\tools\vcs_codegen_main.cpp`. Inputs are shared (load base `0x08804000`, unit span
`0x8000`, `bringup\stage.ps1:134`), so it's BBS's layout, not a per-game config slip.

**Review 2026-10-27 (work PC): the bug is in the `jal` path, and the fix is smaller than above.**
Don't relax the `!entries.empty()` filter. Emitting buckets with no entries would only trade the
compile error for calls into units with nothing to dispatch.
- Plain jumps already handle this: `emit_target` (`tools\codegen_main.cpp:897`) takes the direct
  `invoke_chained_direct` route only if `direct_entry_ids->contains(target)`. Otherwise it emits
  `ctx.pc = target; rt.invoke_chained_call(...)`, which looks the target up at run time.
- **The `jal` (call) path doesn't have that check.** `tools\codegen_main.cpp:1101`:
  `direct_unit = unit_span_bytes != 0u && target >= executable_base;`. Any call target at or above the
  base gets a direct call to `recomp_unit_<bucket>`, even when that bucket was never generated.
  Units 113–115 lie past the end of the generated range (`0x08804000 + 113*0x8000 = 0x08B8C000`).
  Example: `generated-v2\generated_unit_0042.cpp:3299`.
- **Applied 2026-10-27 in psp-native-runtime `68d027a`** (not compiled on the work PC). The change:
  make line 1101 match line 897:
  `const bool direct_unit = function.unit_span_bytes != 0u && target >= function.executable_base &&`
  `    function.direct_entry_ids != nullptr && function.direct_entry_ids->contains(target);`
  The existing `if (!direct_unit)` branches then emit `ctx.pc = target` + `invoke_chained_call`.
- Then rebuild the tool, regenerate and build:
  `cmake --build psp-native-runtime\out\framework --config Release --target psp_recomp`,
  `.\psp.ps1 -Game bbs -Stage regen`, `.\psp.ps1 -Game bbs -Stage build`.
  PW's main corpus doesn't need regenerating (it builds today), and **never regenerate PW's kjfs-generated**.

## What to expect after it links
- `host\` is a copy of the PW host from 10/23. Its PW-only parts (KJFS/zlibdec wiring) don't
  apply: BBS ships no `kjfs.prx`/`zlibdec.prx`. It packs `BBS0-4.DAT` instead.
- BBS loads 9 minigame `MODULE\*.ELF` at runtime. That needs a real `sceKernelLoadModule` /
  `StartModule` that runs each module's `module_start` and registers its exports, the way PW's
  host already does for kjfs/zlibdec.
- Later: RemasteredControls behavior (second stick moves the camera, plus its control-type and L-trigger
  handling), rebuilt as native input code.

- 10/27 (home PC): **step 1: done.** Rebuilt `psp_recomp` with the 68d027a jal fix
  (had to wipe stale CMakeCache from the work PC's D: tree; reconfigured with local
  VS 2026 Community). Build clean.
- 10/27 (home PC): **step 2: done.** Regen: OK, 113 units / 481,005 instructions.
- 10/27 (home PC): **step 3: done.** Build OK (exit 0) — BBSBringup.exe linked.
  The 32 `recomp_unit_0113/0114/0115` errors are gone; previously failing units
  0042/0069/0074/0079/0105/0107 now compile (cached objects from before, only
  those 6 recompiled). Confirmed regenerated code at 0042:3299 now emits
  `ctx.pc = 0x08B8D634u; rt.invoke_chained_call(...)`.
- 10/27 (home PC): **step 4: first run, HLE 1/5, then PARKED (exit 0).**
  - Run 1: exit 3, missing `SysMemUserForUser::0x35669D4C` =
    `sceKernelSetCompiledSdkVersion600_602`. Fixed: registered it next to the
    existing 5.00-5.50 variant (0x91DE343C) in `host\main.cpp` (same lambda,
    `compiled_sdk_version = ctx.gpr[4]`, return 0). Added NID to
    `psp-native-runtime\configs\nids.csv` + `src\nid_registry.cpp`.
  - Run 2: build OK; run **exit 0** — immediate stop, 0.1 s in:
    `Runtime stopped: Unsupported Allegrex instruction 0x00000000 at 0x0880A5DC: invalid internal function entry`
    Both version-set HLEs succeeded first (banner shows `Compiled SDK version:
    100794384` = 0x06020010, `Compiler version: 197382`), so this is the first
    dispatch back into guest code after `SetCompiledSdkVersion600_602`.
    `0x0880A5DC` = `0x08804000 + 0x65DC`, inside unit 0000, but not a
    registered entry of that unit (codegen's entries table doesn't contain it,
    so the runtime rejects the internal jump).
    13 lines before (all of them): harness banner only (ELF/Entry/GP/
    Relocations/Functions/Dispatch cap/versions) — no dispatch trace lines,
    log is 14 lines total. Per session rule, exit 0 → recorded and BBS parked.
  - **Coverage (same session):** `.\psp.ps1 coverage -Game bbs`:
    281 imports total — **handled 51, MISSING 230** (32 of them network/online).
    Boot-relevant gaps: InterruptManager (3 sub-intr), IoFileMgrForUser (10),
    Kernel_Library (4, incl. `sceKernelMemcpy/sceKernelMemset`), LoadExecForUser
    (`sceKernelExitGame`), ModuleMgrForUser (2), plus the audio/display/GE/MPEG
    stacks. Full table: `logs\bbs\hle-coverage.md`.
  - **Next lead (fresh session):** why is 0x0880A5DC not an entry of unit 0000
    (check codegen entry collection for that address; likely a `jalr`/`jr`
    target via a register that codegen treats as out-of-unit). Grep
    `generated-v2\generated_unit_0000.cpp` for `0x0880A5DC` to see how it is
    referenced.
- 10/27 (home PC, seed-fix session): **step 1: done.** `psp_recomp` rebuilt from
  `psp-native-runtime` HEAD `94e3158` (includes the `5adcf15` fixed-ELF data-section
  seed fix and `68d027a` jal fix). `program_analysis.cpp` compiled clean;
  `psp_recomp.exe` produced in `out\framework\Release`.
- 10/27 (home PC, seed-fix session): **step 2: done.** Regen OK: 113 units,
  656,217 instructions (was 481,005). `function seeds: 20702` (was 12,533),
  `registered blocks: 156781` (was 109,599) — seeds ~65% higher.
  `.\psp.ps1 lifted 0x0880A5DC` → **lifted: yes** at
  `generated_unit_0000.cpp:12801`, registered at :17239 (`register_function`).
- 10/27 (home PC, seed-fix session): **step 3: done.** Build OK (exit 0),
  BBSBringup.exe linked; all 113 units recompiled from the new corpus, no errors.
- 10/27 (home PC, HLE session): **step 4: in progress** — repeat exit 3, one HLE per cycle (≤6).
  - **Cycle 1:** exit 3, missing `ThreadManForUser::0xB7D098C6` = `sceKernelCreateMutex`.
    Fixed: 5 mutex HLEs (Create/Lock/TryLock/Unlock/Delete) in `host\thread_manager.cpp`
    (mutex = max-1 semaphore, lenient create) + NID names in both tables. Re-run cleared it.
  - **Cycle 2:** exit 3, missing `sceImpose::0x24FD7BCF` = `sceImposeGetLanguageMode`.
    Fixed: stub in `host\main.cpp` echoing the earlier `SetLanguageMode` value (1) + NID in
    `nids.csv`/registry. Re-run cleared it.
  - **Cycle 3:** exit 3, missing `ThreadManForUser::0x55C20A00` = `sceKernelCreateEventFlag`.
    Fix (this cycle): full event-flag set (Create/Delete/Set/Clear/Poll/Wait/WaitCB) in
    `thread_manager.cpp` adapted from the VCS profile to BBS's re-enter-on-wake suspend idiom,
    real-PSP Clear semantics (`pattern &= ~mask`), uids from 0x900. NIDs added to `nids.csv`
    (registry already had all 7). Re-run cleared it (next stop was a different import).
  - **Cycle 4:** exit 3, missing `scePower::0x04B7766E` = `scePowerRegisterCallback`.
    Fixed: success stub in `host\main.cpp` (accepted, never fired — VCS does the same).
    Re-run cleared it.
  - **Cycle 5:** exit 3, missing `scePower::0x87440F5E` = `scePowerIsPowerOnline`.
    Fixed: returns 1 (AC online) in `host\main.cpp` so the game never takes battery paths.
    Re-run cleared it — the game reached its main loop (33 ms frame delay).
  - **Cycle 6 (last):** exit 3, missing `ThreadManForUser::0x68DA9E36` =
    `sceKernelDelayThreadCB args=[0x8236 µs (≈33 ms)]`. Fix: `DelayThread` (0xCEADEB47) +
    `DelayThreadCB` in `thread_manager.cpp` — cooperative yield: if a peer is ready,
    re-enqueue self past the import (return 0) and let it run; otherwise return success
    and continue at host speed (no host-side timer in this scheduler). NIDs were already
    in both tables.
  - **Cycle 6 (last) re-run: exit 0.** `STOP: Runtime stopped: Dispatch limit reached at
    0x0880F6F8` (cap 300000) — the game ran its main loop at host speed until the dispatch
    budget was exhausted; no more missing HLEs, no deadlock, no unlifted code. Log is 14 lines
    (banner only, no dispatch trace): Entry `0x08A8BD58`, Functions `156782`, Dispatch cap
    `300000`, SDK `100794384` (0x06020010), Compiler `197382`; full log
    `logs\bbs\20261027-154740-run.log`. **6-cycle cap reached — BBS loop stopped here.**

## What worked
- 10/19: corpus generated, 113 units / 481,005 instructions / 281 import wrappers
  (needs regenerating, see above).
- 10/23: per-title CMake project wired to `psp-native-runtime`. Configures fine.
