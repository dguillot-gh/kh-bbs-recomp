# Kingdom Hearts: Birth by Sleep (USA): bring-up notes

Game-specific state and what worked. Keep it current: update it at the end of every session.
Shared runtime/codegen changes belong in `psp-native-runtime`, not here.

Build and run from the workspace root: `.\psp.ps1 -Game bbs -Stage regen|build|run`.
Inputs (local only): `extracted\KingdomHeartsBirthBySleep\` (`decrypted\EBOOT.BIN.dec`,
`disc-files\`, corpus `generated-psprecomp-fixed32k\`, or `generated-v2\` after a regen).

## Current state (2026-10-26): doesn't build yet

The 10/24 build (`logs\bbs_build_20261024.log`) fails with **42 errors** in units
0042 0069 0074 0079 0101 0103 0105 0106 0107 0110:
`'recomp_unit_NNNN': undeclared identifier` inside `invoke_chained_direct<...>`.

**Cause:** the corpus is from 10/19 12:22, before the codegen fix (PW was regenerated at 13:10)
that sends jumps into units outside the corpus through `invoke_chained_call`.
`psp_recomp.exe` (10/19 14:35) already has the fix.

**Next:** `.\psp.ps1 -Game bbs -Stage regen` (about 30 s), then `-Stage build`. The build picks up
`generated-v2` automatically.

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

## What worked
- 10/19: corpus generated, 113 units / 481,005 instructions / 281 import wrappers
  (needs regenerating, see above).
- 10/23: per-title CMake project wired to `psp-native-runtime`. Configures fine.
