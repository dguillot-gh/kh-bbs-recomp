#!/usr/bin/env python3
"""Whole-disc profile for BBS (2026-11-16), after Recomp Compiler's PS1 tools/wholedisc.py:
survey the disc's code files, write game/wholedisc.json, list cross-image seeds, and report coverage.

  wholedisc.py profile <disc-files> <EBOOT.BIN.dec> <out.json>
  wholedisc.py main-pointers <EBOOT.BIN.dec> <out.json> <seeds.txt>   words in the main image that point into the window
  wholedisc.py report <out.json> <main generated> <overlays dir> <report.txt>
"""
import json, os, re, struct, sys

SKIP = {  # code on the disc that a port never runs
    "PSP_GAME/SYSDIR/BOOT.BIN": "unencrypted copy of the main executable",
    "PSP_GAME/SYSDIR/OPNSSMP.BIN": "PSP system module (opening/splash), not game code",
    "PSP_GAME/SYSDIR/UPDATE": "PSP firmware updater",
}

def elf_segments(data):
    if data[:4] != b"\x7fELF":
        return None
    etype, = struct.unpack_from("<H", data, 16)
    phoff, = struct.unpack_from("<I", data, 28)
    phnum, = struct.unpack_from("<H", data, 44)
    segs = []
    for i in range(phnum):
        t, off, vaddr, _, filesz, memsz, flags, _ = struct.unpack_from("<8I", data, phoff + 32 * i)
        if t == 1:
            segs.append({"vaddr": vaddr, "filesz": filesz, "memsz": memsz, "exec": bool(flags & 1)})
    return {"type": hex(etype), "segments": segs}

def profile(disc, eboot, out):
    images, skipped = [], []
    for root, _, files in os.walk(disc):
        for f in sorted(files):
            path = os.path.join(root, f)
            rel = os.path.relpath(path, disc).replace(os.sep, "/")
            reason = next((r for k, r in SKIP.items() if rel == k or rel.startswith(k + "/")), None)
            with open(path, "rb") as fh:
                head = fh.read(4)
            if head not in (b"\x7fELF", b"~PSP"):
                continue
            if reason:
                skipped.append({"file": rel, "reason": reason}); continue
            if head == b"~PSP":
                skipped.append({"file": rel, "reason": "encrypted PRX (EBOOT.BIN is used decrypted)"}); continue
            with open(path, "rb") as fh:
                info = elf_segments(fh.read())
            images.append({"file": rel, "kind": "overlay", **info})
    with open(eboot, "rb") as fh:
        main = elf_segments(fh.read())
    window = [min(s["vaddr"] for i in images for s in i["segments"]),
              max(s["vaddr"] + s["memsz"] for i in images for s in i["segments"])]
    doc = {"game": "ULUS-10505", "load_base": "0x08804000",
           "main": {"file": "PSP_GAME/SYSDIR/EBOOT.BIN (decrypted)", **main},
           "overlay_window": [hex(window[0]), hex(window[1])],
           "overlays": images, "skipped": skipped}
    with open(out, "w") as fh:
        json.dump(doc, fh, indent=2)
    print(f"profile: {len(images)} overlays in {doc['overlay_window']}, {len(skipped)} skipped")

def main_pointers(eboot, prof, out):
    doc = json.load(open(prof))
    lo, hi = (int(x, 16) for x in doc["overlay_window"])
    data = open(eboot, "rb").read()
    seeds = set()
    words = [struct.unpack_from("<I", data, o)[0] for o in range(0, len(data) - 3, 4)]
    for w in words:  # plain pointers in data
        if lo <= w < hi and w % 4 == 0:
            seeds.add(w)
    for i, w in enumerate(words[:-4]):  # lui rX,hi ; addiu/ori rX,rX,lo within the next few instructions
        if w >> 26 != 0x0F:
            continue
        rt, upper = (w >> 16) & 31, w & 0xFFFF
        for n in words[i + 1:i + 5]:
            op = n >> 26
            if op in (0x09, 0x0D) and (n >> 21) & 31 == rt:
                imm = n & 0xFFFF
                v = (upper << 16) + (imm - 0x10000 if op == 0x09 and imm & 0x8000 else imm)
                if lo <= v < hi and v % 4 == 0:
                    seeds.add(v & 0xFFFFFFFF)
                break
    with open(out, "w") as fh:
        fh.writelines(f"{s:08X}\n" for s in sorted(seeds))
    print(f"main-pointers: {len(seeds)} candidate overlay entries")

def registered(directory):
    regs = set()
    for root, _, files in os.walk(directory):
        for f in files:
            if f.startswith("generated_unit_") and f.endswith(".cpp"):
                for m in re.finditer(r"register_function\(0x([0-9A-F]{8})u", open(os.path.join(root, f)).read()):
                    regs.add(m.group(1))
    return regs

def report(prof, gen, ovl, out):
    doc = json.load(open(prof))
    main_regs = registered(gen)
    base = int(doc["load_base"], 16)  # targets below the main image are data the sweep decoded, not calls
    need = {a for a in open(os.path.join(ovl, "main_seeds.txt")).read().split() if int(a, 16) >= base}
    lines = [f"BBS whole-disc report", f"main: {len(main_regs)} registered functions",
             f"overlay -> main calls: {len(need)}, unresolved: {len(need - main_regs)}"]
    lines += [f"  unresolved {a}" for a in sorted(need - main_regs)[:50]]
    for img in doc["overlays"]:
        name = os.path.splitext(os.path.basename(img["file"]))[0]
        lines.append(f"overlay {name}: {len(registered(os.path.join(ovl, name)))} registered functions")
    lines += [f"skipped {s['file']}: {s['reason']}" for s in doc["skipped"]]
    open(out, "w").write("\n".join(lines) + "\n")
    print("\n".join(lines[:3]))

if __name__ == "__main__":
    cmd, *a = sys.argv[1:]
    {"profile": profile, "main-pointers": main_pointers, "report": report}[cmd](*a)
