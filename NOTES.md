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
