"""Software preview renderer for a parsed Saturn model (rest pose, orthographic)."""
import math
import sys
import numpy as np
from PIL import Image
from satmodel import SatModel


def joint_world(model):
    """Map node id -> (world offset, pdata offset) by summing parent-relative positions."""
    out = {}

    def walk(n, base):
        while n:
            p = (base[0] + n["pos"][0], base[1] + n["pos"][1], base[2] + n["pos"][2])
            out[n["id"]] = (p, n["pdata"])
            walk(n["child"], p)
            n = n["sibling"]
    walk(model.root, (0.0, 0.0, 0.0))
    return out


def tex_rgba(model, poly):
    w, h, px = model.texture_pixels(poly["texno"])
    bank = model.palette[(poly["colno"] >> 2) % len(model.palette)]
    arr = np.array([bank[i][:3] for i in px], dtype=np.uint8).reshape(h, w, 3)
    flip = poly["dir"] & 0x30
    if flip & 0x10:
        arr = arr[:, ::-1]
    if flip & 0x20:
        arr = arr[::-1]
    return arr


def render(model, yaw_deg, size=520, light=True):
    joints = joint_world(model)
    by_pdata = {}
    for jid, (pos, pd) in joints.items():
        by_pdata[pd] = pos
    tris = []
    yaw = math.radians(yaw_deg)
    cy, sy = math.cos(yaw), math.sin(yaw)

    def xf(v, base):
        x, y, z = v[0] + base[0], v[1] + base[1], v[2] + base[2]
        return (x * cy - z * sy, y, x * sy + z * cy)

    for part in model.parts:
        base = by_pdata.get(part["offset"], (0, 0, 0))
        pts = [xf(p, base) for p in part["points"]]
        for poly in part["polys"]:
            tex = tex_rgba(model, poly)
            h, w = tex.shape[:2]
            uv = [(0, 0), (w - 1, 0), (w - 1, h - 1), (0, h - 1)]
            v = poly["verts"]
            P = [pts[i] for i in v]
            tris.append((P[0], P[1], P[2], uv[0], uv[1], uv[2], tex))
            if v[2] != v[3]:
                tris.append((P[0], P[2], P[3], uv[0], uv[2], uv[3], tex))
    allp = np.array([p for t in tris for p in t[:3]])
    mn = allp.min(0); mx = allp.max(0)
    scale = (size - 40) / max(mx[0] - mn[0], mx[1] - mn[1])
    cx = (mn[0] + mx[0]) / 2; cyy = (mn[1] + mx[1]) / 2
    img = np.zeros((size, size, 3), np.uint8); img[:] = (40, 40, 48)
    zb = np.full((size, size), np.inf)
    for a, b, c, ua, ub, uc, tex in tris:
        S = [((p[0] - cx) * scale + size / 2, (p[1] - cyy) * scale + size / 2, p[2]) for p in (a, b, c)]
        xs = [s[0] for s in S]; ys = [s[1] for s in S]
        x0, x1 = max(int(min(xs)), 0), min(int(max(xs)) + 1, size - 1)
        y0, y1 = max(int(min(ys)), 0), min(int(max(ys)) + 1, size - 1)
        if x1 < x0 or y1 < y0:
            continue
        (ax, ay, az), (bx, by, bz), (qx, qy, qz) = S
        den = (by - qy) * (ax - qx) + (qx - bx) * (ay - qy)
        if abs(den) < 1e-9:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        l0 = ((by - qy) * (gx - qx) + (qx - bx) * (gy - qy)) / den
        l1 = ((qy - ay) * (gx - qx) + (ax - qx) * (gy - qy)) / den
        l2 = 1 - l0 - l1
        inside = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
        if not inside.any():
            continue
        z = l0 * az + l1 * bz + l2 * qz
        u = l0 * ua[0] + l1 * ub[0] + l2 * uc[0]
        v = l0 * ua[1] + l1 * ub[1] + l2 * uc[1]
        th, tw = tex.shape[:2]
        ui = np.clip(np.round(u).astype(int), 0, tw - 1)
        vi = np.clip(np.round(v).astype(int), 0, th - 1)
        sub = zb[y0:y1 + 1, x0:x1 + 1]
        m = inside & (z < sub)
        col = tex[vi, ui]
        if light:
            n = np.cross(np.subtract(b, a), np.subtract(c, a))
            nn = np.linalg.norm(n)
            shade = 0.55 + 0.45 * abs(n[2] / nn) if nn else 1
            col = (col * shade).astype(np.uint8)
        region = img[y0:y1 + 1, x0:x1 + 1]
        region[m] = col[m]
        sub[m] = z[m]
    return Image.fromarray(img)


if __name__ == "__main__":
    path, out = sys.argv[1], sys.argv[2]
    m = SatModel(path)
    views = [render(m, a) for a in (90, 0, -45)]
    W = sum(v.width for v in views)
    sheet = Image.new("RGB", (W, views[0].height))
    x = 0
    for v in views:
        sheet.paste(v, (x, 0)); x += v.width
    sheet.save(out)
    print("saved", out)
