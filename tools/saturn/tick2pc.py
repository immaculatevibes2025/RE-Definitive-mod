#!/usr/bin/env python3
"""Convert the Sega Saturn Tick (ENEMY/EM1016.EMD, EM1116.EMD) into a PC-format
Resident Evil EMD that drops in for the Hunter (enemy/em1006.emd, em1106.emd).

What is kept and what is replaced
---------------------------------
A Saturn enemy file embeds the PS1/PC EMD byte-for-byte (its section A). For the
Tick that embedded file is the Hunter's, so the skeleton, animations and every
other section the game reads are taken unchanged from there. Only two sections
are rebuilt:

  * TMD (footer[3]) - 16 objects, one per joint, built from the Saturn SGL parts.
    Saturn POINTs are 16.16 fixed in units of 1/4 of the PC's, same axes.
    Quads are split into two mode-0x34 triangles (textured, gouraud), with the
    vertex order reversed to match the PC winding. Normals are recomputed from
    the geometry (4.12 fixed, pointing against (v1-v0)x(v2-v0) like the PC's).
  * TIM (footer[4]) - one 256x256 8bpp page with a 256-colour CLUT at (0,480),
    the exact shape of the Hunter's, so the loader takes the same path. The
    Saturn's one-texture-per-polygon set (374 textures, 3.4x the page area) is
    scaled down uniformly and packed into the two 128-pixel halves of the page;
    no triangle crosses the halves, matching the original data.

Usage
-----
    python3 tick2pc.py <saturn ENEMY dir> <output dir>

Writes em1016.emd and em1116.emd (first and second player block, as on the
Saturn). Put them in USA/ENEMY and set [Mods] Ticks=1 in config.ini, or rename
them to em1006.emd / em1106.emd to replace the Hunter outright.

Also writes em1017.emd / em1117.emd, Zombie Wesker, which the Battle Game uses
in its twelfth room. It is built the same way on the white-coat zombie's
skeleton (section A of the Saturn file is em1000's), so the part count comes
from the Saturn model header rather than being fixed at the Hunter's 16.
Needs: Python 3.8+, numpy, Pillow.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

from satmodel import SatModel

PAGE_W, PAGE_H = 128, 256          # one half of the 256x256 texture
CBA = 0x7800                       # CLUT at x=0, y=480 (same as the Hunter)
TSB = 0x0080                       # page 0, 8bpp (same as the Hunter)
PRIM_HDR = 0x34000609              # mode 0x34, flag 0, ilen 6, olen 9
GUTTER = 0                         # models are drawn with point sampling (MarniDX), so no bleed


# ---------------------------------------------------------------- textures --

def saturn_texture_rgb(model, poly):
    """RGB array of a polygon's texture with its flip flags applied."""
    w, h, px = model.texture_pixels(poly["texno"])
    bank = model.palette[(poly["colno"] >> 2) % len(model.palette)]
    arr = np.array([bank[i][:3] for i in px], dtype=np.uint8).reshape(h, w, 3)
    if poly["dir"] & 0x10:
        arr = arr[:, ::-1]
    if poly["dir"] & 0x20:
        arr = arr[::-1]
    return arr


def pack(sizes, bins=2, bw=PAGE_W, bh=PAGE_H):
    """Shelf packer. sizes: {key: (w, h)} incl. gutter.
    Returns {key: (bin, x, y)} or None if they do not fit."""
    order = sorted(sizes, key=lambda k: (-sizes[k][1], -sizes[k][0]))
    placed = {}
    shelves = [[] for _ in range(bins)]   # per bin: list of [y, height, x_cursor]
    tops = [0] * bins
    for k in order:
        w, h = sizes[k]
        if w > bw or h > bh:
            return None
        done = False
        for b in range(bins):
            for s in shelves[b]:
                if h <= s[1] and s[2] + w <= bw:
                    placed[k] = (b, s[2], s[0])
                    s[2] += w
                    done = True
                    break
            if done:
                break
            if tops[b] + h <= bh:
                shelves[b].append([tops[b], h, w])
                placed[k] = (b, 0, tops[b])
                tops[b] += h
                done = True
                break
        if not done:
            return None
    return placed


def build_atlas(model, head_part=None):
    """Return (8bpp pixels 256x256, 256 CLUT words, {texkey: (x0, y0, x1, y1)}, scale).

    head_part: index of the part carrying the face. Its textures are packed at
    twice the common scale (capped at 1:1) and resampled without filtering, and
    their brightest texels - the eyes - are made the small bright yellow the
    Saturn shows. At the ~50% the rest of the model fits in, a Lanczos
    downscale averaged the 1-2 pixel eye highlights into the brown skin."""
    polys = [p for part in model.parts for p in part["polys"]]
    head_keys = set()
    if head_part is not None:
        head_keys = {(p["texno"], p["colno"], p["dir"] & 0x30)
                     for p in model.parts[head_part]["polys"]}
    tex = {}
    for p in polys:
        key = (p["texno"], p["colno"], p["dir"] & 0x30)
        if key not in tex:
            tex[key] = saturn_texture_rgb(model, p)
    for k in head_keys:
        a = tex[k].copy()
        lum = a.astype(int).sum(axis=2)
        a[lum >= 480] = (255, 226, 96)      # eye highlights -> bright yellow
        tex[k] = a

    def scale_of(k, s):
        return min(1.0, s * 2) if k in head_keys else s

    def sizes_at(s):
        return {k: (max(2, round(a.shape[1] * scale_of(k, s))) + 2 * GUTTER,
                    max(2, round(a.shape[0] * scale_of(k, s))) + 2 * GUTTER)
                for k, a in tex.items()}

    lo, hi = 0.2, 1.0
    best = None
    for _ in range(30):
        mid = (lo + hi) / 2
        pl = pack(sizes_at(mid))
        if pl is not None:
            lo, best = mid, (mid, pl)
        else:
            hi = mid
    scale, placed = best
    sizes = sizes_at(scale)

    atlas = np.zeros((PAGE_H, PAGE_W * 2, 3), np.uint8)
    rects = {}
    for k, (b, x, y) in placed.items():
        w, h = sizes[k][0] - 2 * GUTTER, sizes[k][1] - 2 * GUTTER
        img = Image.fromarray(tex[k]).resize(
            (w, h), Image.NEAREST if k in head_keys else Image.LANCZOS)
        a = np.asarray(img)
        a = np.pad(a, ((GUTTER, GUTTER), (GUTTER, GUTTER), (0, 0)), mode="edge")
        ox = b * PAGE_W + x
        atlas[y:y + a.shape[0], ox:ox + a.shape[1]] = a
        x0, y0 = ox + GUTTER, y + GUTTER
        rects[k] = (x0, y0, x0 + w - 1, y0 + h - 1)

    # The eye yellow is a handful of texels: a median cut over the whole page
    # would fold it into the brown, so it gets a reserved CLUT entry (255).
    eye = np.all(atlas == (255, 226, 96), axis=2)
    q = Image.fromarray(atlas).quantize(colors=255 if eye.any() else 256,
                                        method=Image.Quantize.MEDIANCUT,
                                        dither=Image.Dither.NONE)
    pal = q.getpalette()[:256 * 3]
    pal += [0] * (256 * 3 - len(pal))
    qa = np.asarray(q, dtype=np.uint8).copy()
    if eye.any():
        pal[255 * 3:256 * 3] = [255, 226, 96]
        qa[eye] = 255
        q = Image.fromarray(qa)
    clut = []
    for i in range(256):
        r, g, b = pal[i * 3:i * 3 + 3]
        c = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
        clut.append(c if c else 0x8000)   # 0x0000 is transparent on PS1/PC
    return np.asarray(q, dtype=np.uint8), clut, rects, scale


# -------------------------------------------------------------------- mesh --

def build_objects(model, rects, order=None):
    objs = []
    parts = [model.parts[i] for i in order] if order else model.parts
    for part in parts:
        V = np.array(part["points"], dtype=float) * 4.0
        verts = [tuple(int(round(c)) for c in v) for v in V]
        tris = []   # (vertex idx x3, uv x3)
        for p in part["polys"]:
            x0, y0, x1, y1 = rects[(p["texno"], p["colno"], p["dir"] & 0x30)]
            uvA, uvB, uvC, uvD = (x0, y0), (x1, y0), (x1, y1), (x0, y1)
            a, b, c, d = p["verts"]
            if c == d:
                # VDP1 sprite with C == D: the whole bottom row meets at C.
                mid = ((x0 + x1) // 2, y1)
                tris.append(((c, b, a), (mid, uvB, uvA)))
            else:
                tris.append(((c, b, a), (uvC, uvB, uvA)))
                tris.append(((d, c, a), (uvD, uvC, uvA)))
        # per-vertex normals, PC convention: opposite to (v1-v0)x(v2-v0)
        acc = np.zeros_like(V)
        for (i0, i1, i2), _ in tris:
            fn = np.cross(V[i1] - V[i0], V[i2] - V[i0])
            acc[i0] -= fn; acc[i1] -= fn; acc[i2] -= fn
        norms = []
        for n in acc:
            L = np.linalg.norm(n)
            n = n / L if L else np.array([0.0, -1.0, 0.0])
            norms.append(tuple(int(round(c * 4096)) for c in n))
        objs.append((verts, norms, tris))
    return objs


def build_tmd(objs):
    nobj = len(objs)
    table_len = nobj * 28
    prim_blobs, vert_blobs, norm_blobs = [], [], []
    for verts, norms, tris in objs:
        pb = bytearray()
        for (i0, i1, i2), (t0, t1, t2) in tris:
            pb += struct.pack("<I", PRIM_HDR)
            pb += struct.pack("<I", t0[0] | (t0[1] << 8) | (CBA << 16))
            pb += struct.pack("<I", t1[0] | (t1[1] << 8) | (TSB << 16))
            pb += struct.pack("<I", t2[0] | (t2[1] << 8))
            pb += struct.pack("<6H", i0, i0, i1, i1, i2, i2)   # normal idx == vertex idx
        prim_blobs.append((bytes(pb), len(tris)))
        vert_blobs.append(b"".join(struct.pack("<4h", *v, 0) for v in verts))
        norm_blobs.append(b"".join(struct.pack("<4h", *n, 0) for n in norms))
    # layout: table | all prims | all verts | all normals  (as in the Hunter)
    off = table_len
    prim_off = []
    for pb, _ in prim_blobs:
        prim_off.append(off); off += len(pb)
    vert_off = []
    for vb in vert_blobs:
        vert_off.append(off); off += len(vb)
    norm_off = []
    for nb in norm_blobs:
        norm_off.append(off); off += len(nb)
    body_len = off
    table = b""
    for i, (verts, norms, tris) in enumerate(objs):
        table += struct.pack("<7I", vert_off[i], len(verts), norm_off[i], len(norms),
                             prim_off[i], prim_blobs[i][1], 0)
    body = table + b"".join(pb for pb, _ in prim_blobs) + b"".join(vert_blobs) + b"".join(norm_blobs)
    assert len(body) == body_len
    total = 12 + len(body)
    return struct.pack("<3I", total, 0, nobj) + body


def build_tim(pixels, clut):
    clut_block = struct.pack("<I4H", 12 + 512, 0, 480, 256, 1) + struct.pack("<256H", *clut)
    img = pixels.astype(np.uint8).tobytes()
    img_block = struct.pack("<I4H", 12 + len(img), 0, 0, PAGE_W, PAGE_H) + img
    return struct.pack("<II", 0x10, 0x09) + clut_block + img_block


def node_order(m):
    """PC object i = the part SGL node i draws. The PC skeleton's joint ids are
    the Saturn node ids (same hierarchy, relpos = node pos * 4); the parts are
    listed in the file in a different order."""
    off2part = {p["offset"]: i for i, p in enumerate(m.parts)}
    res = {}

    def walk(n):
        while n:
            res[n["id"]] = off2part[n["pdata"]]
            walk(n["child"])
            n = n["sibling"]
    walk(m.root)
    return [res[i] for i in range(len(res))]


def convert(saturn_path, order=None):
    m = SatModel(saturn_path)
    is_tick = order is None          # Wesker passes its own order; no eye pass
    if order is None:
        order = node_order(m)
    base = m.pc_emd
    sz = len(base) & ~3
    footer = list(struct.unpack("<5I", base[sz - 0x14:sz]))
    tmd_off = footer[3]
    # The face is the part SGL node 2 draws (the Hunter skeleton's head joint).
    head = order[2] if is_tick else None
    pixels, clut, rects, scale = build_atlas(m, head)
    objs = build_objects(m, rects, order)
    tmd = build_tmd(objs)
    tim = build_tim(pixels, clut)
    footer[4] = tmd_off + len(tmd)
    out = base[:tmd_off] + tmd + tim + struct.pack("<5I", *footer)
    stats = dict(scale=scale, tris=sum(len(o[2]) for o in objs),
                 verts=sum(len(o[0]) for o in objs), size=len(out), base_size=len(base))
    return out, stats, pixels, clut


# Saturn-only enemy models converted by default: the Tick (Hunter skeleton)
# and Zombie Wesker (white-coat zombie skeleton, Battle Game only). Each is a
# Chris-block / Jill-block pair, as on the Saturn disc.
#
# The Saturn lists a model's parts in its own order, which is not always the
# PC skeleton's joint order. Zombie Wesker's first and third parts are the head
# and the hips, the other way round from em1000's joints 0 (hips) and 2 (head):
# taken as they were, the head was drawn on the hip joint inside the chest and
# the hips sat on the neck - in game, a headless Wesker. The order below maps
# PC object i to Saturn part order[i]. The Tick's do NOT line up either: its
# first four parts are listed head, neck, chest, hips - the reverse of the
# Hunter's joints 0-3 - which put the Tick's face on the hip joint. With no
# explicit order, convert() now derives it from the SGL node tree (node_order).
WESKER_ORDER = [2, 1, 0] + list(range(3, 15))
MODELS = (("EM1016.EMD", "em1016.emd", None), ("EM1116.EMD", "em1116.emd", None),
          ("EM1017.EMD", "em1017.emd", WESKER_ORDER), ("EM1117.EMD", "em1117.emd", WESKER_ORDER))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    for sat_name, pc_name, order in MODELS:
        path = os.path.join(src, sat_name)
        if not os.path.exists(path):
            path = os.path.join(src, sat_name.lower())
        out, st, pixels, clut = convert(path, order)
        open(os.path.join(dst, pc_name), "wb").write(out)
        print(f"{sat_name} -> {pc_name}: {st['tris']} triangles, {st['verts']} vertices, "
              f"textures at {st['scale']:.0%} of Saturn size, {st['size']} bytes "
              f"(PC base model: {st['base_size']})")


if __name__ == "__main__":
    main()
