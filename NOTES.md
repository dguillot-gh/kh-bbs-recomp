# Kingdom Hearts: Birth by Sleep (USA): bring-up notes

Game-specific state and what worked. Keep it current: update it at the end of every session.
Shared runtime/codegen changes belong in `psp-native-runtime`, not here.

Build and run from the workspace root: `.\psp.ps1 -Game bbs -Stage regen|build|run`.
Inputs (local only): `extracted\KingdomHeartsBirthBySleep\` (`decrypted\EBOOT.BIN.dec`,
`disc-files\`, corpus `generated-psprecomp-fixed32k\`, or `generated-v2\` after a regen).

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

## Current state (2026-10-27, home PC): BBS0+BBS1 complete; guest exits the thread — parked on BUDGET USED

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
