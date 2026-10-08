#!/usr/bin/env python3
"""Build the PC Battle Game rooms from the Sega Saturn release's STAGE8.

The Saturn Battle Game is fifteen rooms, STAGE8/ROOM8010.RDT .. ROOM80F0.RDT.
Each one is an ordinary room file of a main-game room (same cameras, collision,
models, masks) with new scripts: the init script places the battle's enemies
and the safe rooms keep only their item box and the exit door. The Saturn's own
table of which room each one is a copy of (GAME5.PRG 0x06099334, indexed by the
battle room number) is BASE_ROOMS below; the camera data of every file matches
its PC counterpart byte for byte, which is how it was confirmed.

The Saturn room files are LZ-compressed (satlz.py) and carry Saturn-format
models and textures, so they are not loaded as they are. Instead each PC room
is taken whole and only its three script sections are replaced with the
Saturn's:

    RDT +0x60  initialization SCD   (enemy_set, omodel_set, door_set, ...)
    RDT +0x64  per-frame SCD
    RDT +0x68  event scripts        (table of offsets relative to the table)

The bytecode is the same on both machines. The new sections are appended to
the end of the file and the three header offsets re-pointed at them; nothing
else in the PC file moves.

Usage
-----
    python3 battle2pc.py <Saturn STAGE8 folder> <PC USA folder> <output folder>

Writes ROOM8010.RDT .. ROOM80F0.RDT. Put them in USA/BATTLE.
"""
import os
import struct
import sys

from satlz import decompress

# battle room number -> (PC stage index 0-6, room id). GAME5.PRG 0x06099334.
BASE_ROOMS = {
    0x1: (0, 0x00), 0x2: (0, 0x03), 0x3: (2, 0x02), 0x4: (0, 0x17),
    0x5: (1, 0x10), 0x6: (0, 0x00), 0x7: (3, 0x04), 0x8: (3, 0x0C),
    0x9: (2, 0x09), 0xA: (2, 0x0C), 0xB: (2, 0x0E), 0xC: (4, 0x09),
    0xD: (4, 0x0F), 0xE: (4, 0x10), 0xF: (4, 0x13),
}

CAMERA_SIZE = 0x2C
HDR_INIT, HDR_FRAME, HDR_EVENTS, HDR_NEXT = 0x60, 0x64, 0x68, 0x6C


def find_file(folder, name):
    p = os.path.join(folder, name)
    if os.path.exists(p):
        return p
    low = name.lower()
    for f in os.listdir(folder):
        if f.lower() == low:
            return os.path.join(folder, f)
    raise FileNotFoundError(os.path.join(folder, name))


def cameras(rdt):
    n = rdt[1]
    return [rdt[0x94 + i * CAMERA_SIZE + 8:0x94 + (i + 1) * CAMERA_SIZE] for i in range(n)]


def build(sat, pc):
    """Return the PC room with the Saturn room's scripts."""
    so = struct.unpack_from("<4I", sat, HDR_INIT)
    if not (so[0] <= so[1] <= so[2] <= so[3]):
        raise ValueError("Saturn script sections are not contiguous")
    init = sat[so[0]:so[1]]
    frame = sat[so[1]:so[2]]
    events = sat[so[2]:so[3]]

    out = bytearray(pc)
    while len(out) % 4:
        out.append(0)
    base = len(out)
    out += init + frame + events
    while len(out) % 4:
        out.append(0)
    struct.pack_into("<3I", out, HDR_INIT, base, base + len(init), base + len(init) + len(frame))
    return bytes(out)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    sat_dir, usa_dir, dst = sys.argv[1:4]
    os.makedirs(dst, exist_ok=True)
    for idx, (stage, room) in sorted(BASE_ROOMS.items()):
        name = "ROOM80%X0.RDT" % idx
        sat, _ = decompress(open(find_file(sat_dir, name), "rb").read())
        pc_path = find_file(find_file(usa_dir, "STAGE%d" % (stage + 1)),
                            "ROOM%d%02X0.RDT" % (stage + 1, room))
        pc = open(pc_path, "rb").read()
        if cameras(sat) != cameras(pc):
            raise ValueError("%s does not match %s" % (name, os.path.basename(pc_path)))
        out = build(sat, pc)
        open(os.path.join(dst, name), "wb").write(out)
        print("%s <- %s + Saturn scripts (%d bytes)" % (name, os.path.basename(pc_path), len(out)))


if __name__ == "__main__":
    main()
