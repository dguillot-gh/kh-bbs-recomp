"""Story-slot addresses for a BBS disc: python find-story-addresses.py <EBOOT.BIN.dec>

host/main.cpp reads which character is playing as halfword(*( *(ROOT) + 168 ) + 600). This finds ROOT from the
game's own Player.GetHandle script handler (lui rX, hi; lw rX, lo(rX); ... lw rY, 168(rX)) and checks the other two
facts the host relies on: GetPlayerID's helper reads halfword +600, and the PLAYER_ID_* constants are
Ventus 1, Aqua 2, Terra 3. Prints the per-disc line to paste into main.cpp. 2026-11-20.
"""
import struct
import sys


def main():
    d = open(sys.argv[1], "rb").read()
    phoff = struct.unpack_from("<I", d, 28)[0]
    _, off, va, _, fsz = struct.unpack_from("<5I", d, phoff)
    lo, hi = va, va + fsz
    word = lambda a: struct.unpack_from("<I", d, a - va + off)[0]
    words = struct.unpack("<%dI" % (len(d) // 4), d[:len(d) // 4 * 4])

    def cstr(a):
        o = a - va + off
        return d[o:d.find(b"\0", o)]

    table, consts = {}, {}
    for i in range(len(words) - 1):
        n, f = words[i], words[i + 1]
        if lo <= n < hi:
            s = cstr(n)
            if lo <= f < hi and f % 4 == 0 and s and s.isascii():
                table.setdefault(s.decode(), f)
            if s.startswith(b"PLAYER_ID_") and f < 16:
                consts[s.decode()] = f
    print("constants:", consts)

    # Player.GetHandle: lui + lw (signed lo) gives ROOT, then lw ..., 168(..)
    h = table["GetHandle"]
    hi16, root = None, None
    for a in range(h, h + 0x40, 4):
        w = word(a)
        op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 0x0F:                       # lui
            hi16 = imm
        elif op == 0x23 and hi16 is not None and root is None:   # first lw after the lui
            root = ((hi16 << 16) + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
        elif op == 0x23 and root is not None and imm == 168:
            break
    else:
        sys.exit("GetHandle: pattern not found")
    print("GetHandle handler 0x%08X -> root 0x%08X" % (h, root))

    # GetPlayerID helper: the jal after the handle lookup; it must contain lhu ..., 600(a0)
    g = table["GetPlayerID"]
    helper = None
    for a in range(g, g + 0x80, 4):
        w = word(a)
        if w >> 26 == 3:
            t = ((w & 0x03FFFFFF) << 2) | (a & 0xF0000000)
            if any((word(b) >> 26) == 0x25 and (word(b) & 0xFFFF) == 600 for b in range(t, t + 0x10, 4)):
                helper = t
    print("GetPlayerID helper:", "0x%08X (reads +600)" % helper if helper else "NOT FOUND")
    ok = helper and consts == {"PLAYER_ID_VENTUS": 1, "PLAYER_ID_AQUA": 2, "PLAYER_ID_TERRA": 3}
    print("OK" if ok else "CHECK FAILED", "- main.cpp: g_bbs_player_root = 0x%08Xu" % root)


if __name__ == "__main__":
    main()
