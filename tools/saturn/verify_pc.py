"""Check a PC-format EMD against the Hunter's and render it from PC data only."""
import struct
import sys
import numpy as np
from PIL import Image
from pcemd import PcEmd


def check(out_path, ref_path):
    out = open(out_path, "rb").read()
    ref = open(ref_path, "rb").read()
    a, r = PcEmd(out), PcEmd(ref)
    errs = []
    if out[:a.tmd_off] != ref[:r.tmd_off]:
        errs.append("sections before the TMD differ from the Hunter")
    if a.footer[:4] != r.footer[:4]:
        errs.append(f"footer[0..3] differ: {a.footer[:4]} vs {r.footer[:4]}")
    if a.footer[4] != a.tmd_off + a.tmd_size:
        errs.append("footer[4] does not follow the TMD")
    if len(out) != a.footer[4] + 8 + 524 + 12 + 65536 + 0x14:
        errs.append("file length does not match TIM + footer")
    if len(a.objs) != len(r.objs):
        errs.append("object count differs")
    if (a.tim_flags, a.clut_xywh, a.img_xywh) != (r.tim_flags, r.clut_xywh, r.img_xywh):
        errs.append("TIM header shape differs")
    if 0 in a.clut:
        errs.append("CLUT contains 0x0000 (transparent)")
    total = 0
    for i, o in enumerate(a.objs):
        nv, nn = len(o["verts"]), len(o["norms"])
        for p in o["prims"]:
            total += 1
            if p["hdr"] != 0x34000609: errs.append(f"obj {i}: bad prim header")
            if p["cba"] != 0x7800 or p["tsb"] != 0x80: errs.append(f"obj {i}: bad cba/tsb")
            if any(v >= nv for v in p["v"]) or any(n >= nn for n in p["n"]):
                errs.append(f"obj {i}: index out of range")
            us = [u for u, v in p["uv"]]
            if min(us) < 128 <= max(us): errs.append(f"obj {i}: triangle crosses page halves")
        for n in o["norms"]:
            L = np.linalg.norm(n)
            if not 4080 < L < 4112: errs.append(f"obj {i}: normal length {L:.0f}")
        if any(abs(c) > 32767 for v in o["verts"] for c in v): errs.append(f"obj {i}: vertex overflow")
    return a, total, errs


def skeleton_offsets(emd):
    """Joint offsets from the Saturn file aren't in the PC file; for the preview we
    reuse the Saturn tree, scaled x4, passed in by the caller."""
    raise NotImplementedError


def render_pc(emd, joint_pos, yaw_deg, size=520):
    tex = np.frombuffer(emd.pixels, np.uint8).reshape(emd.height, emd.width)
    pal = np.array([((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3) for c in emd.clut], np.uint8)
    rgb = pal[tex]
    yaw = np.radians(yaw_deg); cy, sy = np.cos(yaw), np.sin(yaw)
    tris = []
    for i, o in enumerate(emd.objs):
        base = joint_pos[i]
        V = []
        for v in o["verts"]:
            x, y, z = v[0] + base[0], v[1] + base[1], v[2] + base[2]
            V.append((x * cy - z * sy, y, x * sy + z * cy))
        for p in o["prims"]:
            tris.append(([V[k] for k in p["v"]], p["uv"]))
    allp = np.array([q for t in tris for q in t[0]])
    mn, mx = allp.min(0), allp.max(0)
    sc = (size - 40) / max(mx[0] - mn[0], mx[1] - mn[1])
    cx, cyy = (mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2
    img = np.zeros((size, size, 3), np.uint8); img[:] = (40, 40, 48)
    zb = np.full((size, size), np.inf)
    for P, UV in tris:
        S = [((q[0] - cx) * sc + size / 2, (q[1] - cyy) * sc + size / 2, q[2]) for q in P]
        (ax, ay, az), (bx, by, bz), (qx, qy, qz) = S
        den = (by - qy) * (ax - qx) + (qx - bx) * (ay - qy)
        if abs(den) < 1e-9: continue
        x0, x1 = max(int(min(ax, bx, qx)), 0), min(int(max(ax, bx, qx)) + 1, size - 1)
        y0, y1 = max(int(min(ay, by, qy)), 0), min(int(max(ay, by, qy)) + 1, size - 1)
        if x1 < x0 or y1 < y0: continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + .5, np.arange(y0, y1 + 1) + .5)
        l0 = ((by - qy) * (gx - qx) + (qx - bx) * (gy - qy)) / den
        l1 = ((qy - ay) * (gx - qx) + (ax - qx) * (gy - qy)) / den
        l2 = 1 - l0 - l1
        ins = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
        if not ins.any(): continue
        z = l0 * az + l1 * bz + l2 * qz
        u = l0 * UV[0][0] + l1 * UV[1][0] + l2 * UV[2][0]
        v = l0 * UV[0][1] + l1 * UV[1][1] + l2 * UV[2][1]
        col = rgb[np.clip(np.round(v).astype(int), 0, 255), np.clip(np.round(u).astype(int), 0, 255)]
        n = np.cross(np.subtract(P[1], P[0]), np.subtract(P[2], P[0])); L = np.linalg.norm(n)
        col = (col * (0.55 + 0.45 * abs(n[2] / L) if L else col)).astype(np.uint8)
        sub = zb[y0:y1 + 1, x0:x1 + 1]; m = ins & (z < sub)
        img[y0:y1 + 1, x0:x1 + 1][m] = col[m]; sub[m] = z[m]
    return Image.fromarray(img)


if __name__ == "__main__":
    from satmodel import SatModel
    from render import joint_world
    out_path, ref_path, sat_path, png = sys.argv[1:5]
    emd, total, errs = check(out_path, ref_path)
    print(f"{out_path}: {len(emd.objs)} objects, {total} triangles, TMD {emd.tmd_size} bytes")
    print("checks:", "all passed" if not errs else f"{len(errs)} problems")
    for e in errs[:20]: print("  -", e)
    sat = SatModel(sat_path)
    jw = joint_world(sat)
    by_pd = {pd: pos for pos, pd in jw.values()}
    jpos = [tuple(c * 4 for c in by_pd[part["offset"]]) for part in sat.parts]
    views = [render_pc(emd, jpos, a) for a in (90, 0, -45)]
    sheet = Image.new("RGB", (sum(v.width for v in views), views[0].height))
    x = 0
    for v in views: sheet.paste(v, (x, 0)); x += v.width
    sheet.save(png)
    tex = np.frombuffer(emd.pixels, np.uint8).reshape(256, 256)
    pal = np.array([((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3) for c in emd.clut], np.uint8)
    Image.fromarray(pal[tex]).resize((512, 512), Image.NEAREST).save(png.replace(".png", "_texture.png"))
    print("saved", png)
