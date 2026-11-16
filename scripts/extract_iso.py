"""Extract every file from a PSP UMD disc image (plain ISO 9660, 2048-byte sectors).

    python extract_iso.py <game.iso> <output-dir>

Standard library only. Directory names and file names are taken from the ISO 9660 primary volume
descriptor (PSP discs use uppercase 8.3-style names; ";1" version suffixes are dropped).
"""
import os
import struct
import sys

SECTOR = 2048


def read_sector(f, lba, count=1):
    f.seek(lba * SECTOR)
    return f.read(count * SECTOR)


def records(data):
    """Yield (name, lba, size, is_dir) for each record in a directory extent."""
    pos = 0
    while pos < len(data):
        length = data[pos]
        if length == 0:  # records never span sectors: skip to the next sector
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + length]
        lba = struct.unpack_from('<I', rec, 2)[0]
        size = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        name_len = rec[32]
        raw = rec[33:33 + name_len]
        pos += length
        if raw in (b'\x00', b'\x01'):  # "." and ".."
            continue
        name = raw.decode('ascii', 'replace').split(';')[0]
        if name.endswith('.') and not flags & 2:
            name = name[:-1]
        yield name, lba, size, bool(flags & 2)


def extract_dir(f, lba, size, out, counts):
    os.makedirs(out, exist_ok=True)
    data = read_sector(f, lba, (size + SECTOR - 1) // SECTOR)[:size]
    for name, child_lba, child_size, is_dir in records(data):
        path = os.path.join(out, name)
        if is_dir:
            extract_dir(f, child_lba, child_size, path, counts)
            continue
        f.seek(child_lba * SECTOR)
        remaining = child_size
        with open(path, 'wb') as dst:
            while remaining:
                chunk = f.read(min(remaining, 8 * 1024 * 1024))
                if not chunk:
                    raise SystemExit(f'truncated image while reading {path}')
                dst.write(chunk)
                remaining -= len(chunk)
        counts[0] += 1
        counts[1] += child_size


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    iso, out = sys.argv[1], sys.argv[2]
    with open(iso, 'rb') as f:
        pvd = read_sector(f, 16)
        if pvd[0] != 1 or pvd[1:6] != b'CD001':
            raise SystemExit(f'{iso}: no ISO 9660 primary volume descriptor at sector 16')
        root = pvd[156:156 + 34]
        root_lba = struct.unpack_from('<I', root, 2)[0]
        root_size = struct.unpack_from('<I', root, 10)[0]
        counts = [0, 0]
        extract_dir(f, root_lba, root_size, out, counts)
    print(f'extracted {counts[0]} files, {counts[1] / 1048576:.0f} MB -> {out}')


if __name__ == '__main__':
    main()
