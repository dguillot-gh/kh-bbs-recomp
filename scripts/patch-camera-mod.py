"""Right-stick camera hooks for the generated code: python patch-camera-mod.py <generated dir> <disc id>

The camera mod (host/main.cpp: bbs_camera_mod_active, bbs_right_stick_axis; the idea of TheFloW's RemasteredControls
KHBBS, re-implemented) needs five small edits in two generated units: while the right stick is off centre, the game
takes its "L held, control type B" camera path and its two stick getters read the right stick. This script makes
those edits; it does nothing to a unit that already has them. Run it after scripts/3-GENERATE.sh.

Sites were found by matching the US code against Final Mix (same instructions, shifted): 2026-11-19.
"""
import pathlib
import sys

MARK = "[bbs-camera-mod]"
EXTERNS = ("// [bbs-camera-mod] host hooks (kh-bbs/host/main.cpp): right stick drives the camera, idea from\n"
           "// RemasteredControls KHBBS by TheFloW (GPL-3, re-implemented, not copied).\n"
           "extern \"C\" bool bbs_camera_mod_active() noexcept;\n"
           "extern \"C\" float bbs_right_stick_axis(int axis) noexcept;\n")

# unit, label of the "L held" test (andi a0,a0,4; beq a0,zero)
# unit, camera function label, axis, return addresses of its two stick-getter calls, getter address
SITES = {
    "ULUS-10505": {"l_held": (39, "0893FCE4"),
                   "axes": [(48, "08988F30", 0, ("08988F6C", "08988FAC"), "088162F0"),
                            (48, "089890F8", 1, ("08989130", "08989170"), "08816320")]},
    "ULJM-05775": {"l_held": (39, "08940FE8"),
                   "axes": [(49, "0898F668", 0, ("0898F6A4", "0898F6E4"), "08816330"),
                            (49, "0898F830", 1, ("0898F868", "0898F8A8"), "08816360")]},
}


def fail(msg):
    sys.exit("patch-camera-mod: " + msg)


def replace_after(text, label, old, new):
    """Replace the first `old` after the line `L_<label>:`; exactly one label must exist."""
    head = f"\nL_{label}:\n"
    if text.count(head) != 1:
        fail(f"label L_{label} found {text.count(head)} times")
    at = text.index(head)
    hit = text.find(old, at)
    if hit < 0 or hit - at > 2000:
        fail(f"no '{old.strip()}' shortly after L_{label}")
    return text[:hit] + new + text[hit + len(old):]


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in SITES:
        fail("usage: patch-camera-mod.py <generated dir> <" + "|".join(SITES) + ">")
    gen = pathlib.Path(sys.argv[1])
    site = SITES[sys.argv[2]]
    edits = {}  # unit -> list of functions(text) -> text

    unit, label = site["l_held"]
    edits.setdefault(unit, []).append(lambda t, label=label: replace_after(
        t, label, "    if (ctx.gpr[4] == 0u) {",
        f"    if (ctx.gpr[4] == 0u && !bbs_camera_mod_active()) {{  // {MARK} L held"))
    for unit, func, axis, returns, getter in site["axes"]:
        edits.setdefault(unit, []).append(lambda t, func=func: replace_after(
            t, func, "{ const bool branch_taken = static_cast<std::int32_t>(ctx.gpr[4]) > 0;",
            f"{{ const bool branch_taken = static_cast<std::int32_t>(ctx.gpr[4]) > 0 && !bbs_camera_mod_active();  // {MARK} type B"))
        for ret in returns:
            def hook(t, ret=ret, axis=axis, getter=getter):
                key = f", 0x{getter}u>(ctx, &aot_mem) && ctx.pc == 0x{ret}u) goto L_{ret};"
                if t.count(key) != 1:
                    fail(f"getter call 0x{getter} returning to 0x{ret} found {t.count(key)} times")
                line_start = t.rindex("\n", 0, t.index(key)) + 1
                return (t[:line_start] + f"    if (bbs_camera_mod_active()) {{ ctx.fpr[0] = bbs_right_stick_axis({axis}); "
                        f"goto L_{ret}; }}  // {MARK}\n" + t[line_start:])
            edits[unit].append(hook)

    for unit, fns in sorted(edits.items()):
        path = gen / f"generated_unit_{unit:04d}.cpp"
        raw = path.read_bytes().decode("utf-8")
        crlf = "\r\n" in raw
        text = raw.replace("\r\n", "\n")
        if MARK in text:
            print(f"{path.name}: already patched")
            continue
        for fn in fns:
            text = fn(text)
        ns = "\nnamespace psprecomp {\n"
        text = text.replace(ns, "\n" + EXTERNS + "namespace psprecomp {\n", 1)
        if crlf:
            text = text.replace("\n", "\r\n")
        path.write_bytes(text.encode("utf-8"))
        print(f"{path.name}: {len(fns)} edits")


if __name__ == "__main__":
    main()
