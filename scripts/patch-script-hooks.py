"""Host hooks on the game's script commands: python patch-script-hooks.py <generated dir> <EBOOT.BIN.dec>

BBS runs its events and battles through script commands registered as {name, handler} tables in the main program
("ReqLoadRsrcArcEnemyBoss", "SetBossCamera", ...). This script finds those tables in the EBOOT (so the same list
works for the US disc and Final Mix), and inserts `bbs_script_command("<name>")` at the start of each watched
handler in the generated code. host/main.cpp decides what to do with them (trace, boss-fight save state).
Units that already have a hook for a name are left alone. Run after scripts/3-GENERATE.sh. 2026-11-19.
"""
import pathlib
import re
import struct
import sys

MARK = "[bbs-script-hook]"
WATCH = [
    # boss fight start/end candidates
    "ReqLoadRsrcArcEnemyBoss", "ReqDestroyRsrcArcEnemyBoss", "SetBossCamera", "SetBattleFlagBossDead",
    # context: events, warps, battle state, arena
    "NotifyEventStart", "NotifyEventEnd", "LoadEvent", "Warp", "SetArenaRoundEnd", "ReqLoadRsrcArcEnemyZako",
]
EXTERN = 'extern "C" void bbs_script_command(const char *name) noexcept;  // ' + MARK + " host/main.cpp\n"


def command_table(eboot):
    d = pathlib.Path(eboot).read_bytes()
    phoff = struct.unpack_from("<I", d, 28)[0]
    _, seg_off, seg_va, _, seg_filesz = struct.unpack_from("<5I", d, phoff)
    va2off = lambda v: v - seg_va + seg_off
    words = struct.unpack("<%dI" % (len(d) // 4), d[:len(d) // 4 * 4])
    lo, hi = seg_va, seg_va + seg_filesz
    table = {}
    for i in range(len(words) - 1):
        n, f = words[i], words[i + 1]
        if not (lo <= n < hi and lo <= f < hi and f % 4 == 0):
            continue
        o = va2off(n)
        e = d.find(b"\0", o)
        name = d[o:e]
        if 3 <= len(name) < 64 and re.fullmatch(rb"[A-Za-z_][A-Za-z0-9_]*", name):
            table.setdefault(name.decode(), f)
    return table


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: patch-script-hooks.py <generated dir> <EBOOT.BIN.dec>")
    gen = pathlib.Path(sys.argv[1])
    table = command_table(sys.argv[2])
    units = sorted(gen.glob("generated_unit_*.cpp"))
    texts = {}
    for name in WATCH:
        addr = table.get(name)
        if addr is None:
            print(f"{name}: not in the command table")
            continue
        label = f"\nL_{addr:08X}:\n"
        for u in units:
            if u not in texts:
                texts[u] = u.read_bytes().decode("utf-8")
            t = texts[u]
            if label not in t.replace("\r\n", "\n"):
                continue
            crlf = "\r\n" in t
            t = t.replace("\r\n", "\n")
            hook = f'    bbs_script_command("{name}");  // {MARK}\n'
            at = t.index(label) + len(label)
            if not t[at:].startswith(hook):
                t = t[:at] + hook + t[at:]
                if EXTERN not in t:
                    t = t.replace("\nnamespace psprecomp {\n", "\n" + EXTERN + "namespace psprecomp {\n", 1)
                print(f"{name}: 0x{addr:08X} in {u.name}")
            else:
                print(f"{name}: already hooked")
            texts[u] = t.replace("\n", "\r\n") if crlf else t
            break
        else:
            print(f"{name}: handler 0x{addr:08X} not found in the generated code")
    for u, t in texts.items():
        if u.read_bytes().decode("utf-8") != t:
            u.write_bytes(t.encode("utf-8"))


if __name__ == "__main__":
    main()
