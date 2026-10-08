#!/usr/bin/env python3
"""Convert the Sega Saturn exclusive outfits (ENEMY/EM1032.EMD Chris,
EM1033.EMD Jill) into PC player EMDs.

On the PC those two file names are the PC's own second alternate outfits
(the "Brave Bomber" jacket set), so the Saturn ones are written under new
names: em1034.emd (Chris) and em1035.emd (Jill).

The skeleton, animations and everything but the mesh and texture come from
section A of the Saturn file, which is the plain CHAR10 / CHAR11 player EMD.
The mesh is rebuilt from the Saturn SGL model as tick2pc.py does for enemies,
with two player-specific differences:

  * part order: a Saturn player has more parts than joints (spare hands and
    forearms); the joint tree's node ids name the PC joint each part is
    drawn on, so PC object i = the part of node id i. The PC's sixteenth
    object (the ejected clip, joint 15) has no Saturn counterpart and is left
    empty.
  * texture: laid out like the PC player's - a 256x256 8bpp image split into
    two 128-texel pages, page 0 (tsb 0x80) with CLUT row 480 and page 1
    (tsb 0x81) with CLUT row 481. Both rows hold the same palette.

Usage:  python3 satcostume.py <saturn ENEMY dir> <output dir>
Needs numpy and Pillow; tick2pc.py and satmodel.py alongside.
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tick2pc
from satmodel import SatModel

PRIM_HDR = 0x34000609
PAGE_W, PAGE_H = 128, 256
CBA = (0x7800, 0x7840)      # CLUT rows 480 / 481
TSB = (0x0080, 0x0081)      # 8bpp, pages 0 / 1


def joint_order(m):
    idx = {p["offset"]: i for i, p in enumerate(m.parts)}
    order = {}

    def walk(n):
        while n:
            if n["pdata"] in idx:
                order[n["id"]] = idx[n["pdata"]]
            walk(n["child"])
            n = n["sibling"]
    walk(m.root)
    n = max(order) + 1
    return [order[i] for i in range(n)]


def build_tmd(objs):
    nobj = len(objs)
    prim_blobs, vert_blobs, norm_blobs = [], [], []
    for verts, norms, tris in objs:
        pb = bytearray()
        for (i0, i1, i2), (t0, t1, t2) in tris:
            page = 1 if min(t0[0], t1[0], t2[0]) >= PAGE_W else 0
            sub = PAGE_W * page
            pb += struct.pack("<I", PRIM_HDR)
            pb += struct.pack("<I", (t0[0] - sub) | (t0[1] << 8) | (CBA[page] << 16))
            pb += struct.pack("<I", (t1[0] - sub) | (t1[1] << 8) | (TSB[page] << 16))
            pb += struct.pack("<I", (t2[0] - sub) | (t2[1] << 8))
            pb += struct.pack("<6H", i0, i0, i1, i1, i2, i2)
        prim_blobs.append((bytes(pb), len(tris)))
        vert_blobs.append(b"".join(struct.pack("<4h", *v, 0) for v in verts))
        norm_blobs.append(b"".join(struct.pack("<4h", *n, 0) for n in norms))
    off = nobj * 28
    po, vo, no = [], [], []
    for pb, _ in prim_blobs:
        po.append(off); off += len(pb)
    for vb in vert_blobs:
        vo.append(off); off += len(vb)
    for nb in norm_blobs:
        no.append(off); off += len(nb)
    table = b"".join(struct.pack("<7I", vo[i], len(o[0]), no[i], len(o[1]), po[i],
                                 prim_blobs[i][1], 0) for i, o in enumerate(objs))
    body = table + b"".join(p for p, _ in prim_blobs) + b"".join(vert_blobs) + b"".join(norm_blobs)
    return struct.pack("<3I", 12 + len(body), 0, nobj) + body


def build_tim(pixels, clut):
    rows = struct.pack("<256H", *clut) * 2
    clut_block = struct.pack("<I4H", 12 + len(rows), 0, 480, 256, 2) + rows
    img = pixels.astype(np.uint8).tobytes()
    img_block = struct.pack("<I4H", 12 + len(img), 0, 0, PAGE_W, PAGE_H) + img
    return struct.pack("<II", 0x10, 0x09) + clut_block + img_block


def convert(path):
    m = SatModel(path)
    order = joint_order(m)
    # Only the parts actually drawn get atlas space.
    used = set(order)
    full = m.parts
    m.parts = [p for i, p in enumerate(full) if i in used]
    pixels, clut, rects, scale = tick2pc.build_atlas(m)
    m.parts = full
    # A seam must not cross the two 128-texel pages: rects from build_atlas
    # already sit wholly in one half (it packs two 128-wide bins).
    objs = tick2pc.build_objects(m, rects, order)
    while len(objs) < 16:                      # joint 15 (the ejected clip)
        objs.append(([(0, 0, 0)], [(0, -4096, 0)], []))
    base = m.pc_emd
    sz = len(base) & ~3
    footer = list(struct.unpack("<5I", base[sz - 0x14:sz]))
    tmd = build_tmd(objs)
    tim = build_tim(pixels, clut)
    footer[4] = footer[3] + len(tmd)
    out = base[:footer[3]] + tmd + tim + struct.pack("<5I", *footer)
    return out, scale, order


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    for sat, pc in (("EM1032.EMD", "em1034.emd"), ("EM1033.EMD", "em1035.emd")):
        out, scale, order = convert(os.path.join(src, sat))
        open(os.path.join(dst, pc), "wb").write(out)
        print(f"{sat} -> {pc}: parts {order}, textures at {scale:.0%}, {len(out)} bytes")


if __name__ == "__main__":
    main()
