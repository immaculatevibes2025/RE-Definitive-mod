"""Parser for Resident Evil (Sega Saturn) enemy model files (ENEMY/EM####.EMD).

File layout (all big-endian):
  header: 4 x u32 section offsets
  section A: the PS1/PC EMD file, byte-identical (skeleton, animations, PS1 mesh)
  section B: LZ-compressed SGL model (see satlz.py)
  section C: LZ-compressed 4bpp texture pixels (VDP1 character data)
  section D: raw colour banks, 16 x RGB555 per bank

Decompressed section B:
  u32 0x10, u32 palette bank count, u32 root node, u32 texture table,
  u32 part[16] -> XPDATA (joint order)
  XPDATA (0x18): pntbl, nbPoint, pltbl, nbPolygon, attbl, vntbl
    POINT   = 3 x FIXED (16.16)
    POLYGON = 3 x FIXED normal + 4 x u16 vertex index   (20 bytes)
    ATTR    = u8 flag, u8 sort, u16 texno, u16 atrb, u16 colno, u16 gstb, u16 dir
    vntbl   = per-vertex normals, 3 x FIXED
  node (0x30): pdata, u16 id, u16 pad, pos[3] FIXED, 2 x u32, scale[3] FIXED,
               child, sibling
  texture table: {u16 w, u16 h, u16 cgadr (x8 bytes), u16 hvsize}, terminated
                 by 0xFFFFFFFF
"""
import struct
from satlz import decompress


def fx(v):
    return v / 65536.0


class SatModel:
    def __init__(self, path):
        raw = open(path, "rb").read()
        self.raw = raw
        offs = list(struct.unpack(">4I", raw[:16]))
        bounds = offs + [len(raw)]
        self.sec = [raw[bounds[i]:bounds[i + 1]] for i in range(4)]
        self.pc_emd = self.sec[0]
        self.mesh, _ = decompress(self.sec[1])
        self.cg, _ = decompress(self.sec[2])
        self.palette = self._palette(self.sec[3])
        self._parse()

    @staticmethod
    def _palette(data):
        banks = []
        for b in range(len(data) // 32):
            bank = []
            for i in range(16):
                w = struct.unpack(">H", data[b * 32 + i * 2:b * 32 + i * 2 + 2])[0]
                bank.append(((w & 31) << 3, ((w >> 5) & 31) << 3, ((w >> 10) & 31) << 3, w))
            banks.append(bank)
        return banks

    def u32(self, o):
        return struct.unpack(">I", self.mesh[o:o + 4])[0]

    def s32(self, o):
        return struct.unpack(">i", self.mesh[o:o + 4])[0]

    def u16(self, o):
        return struct.unpack(">H", self.mesh[o:o + 2])[0]

    def _parse(self):
        m = self.mesh
        self.nbanks = self.u32(4)
        self.root_off = self.u32(8)
        self.tex_off = self.u32(12)
        self.nparts = self.u32(0)
        self.part_offs = [self.u32(16 + 4 * i) for i in range(self.nparts)]
        self.parts = [self._pdata(o) for o in self.part_offs]
        # textures
        self.textures = []
        o = self.tex_off
        while self.u32(o) != 0xFFFFFFFF:
            w, h, cg, hv = struct.unpack(">4H", m[o:o + 8])
            self.textures.append((w, h, cg * 8, hv))
            o += 8
        # nodes
        self.nodes = {}
        self.root = self._node(self.root_off)

    def _pdata(self, o):
        pnt, npt, pol, npol, att, vn = struct.unpack(">6I", self.mesh[o:o + 24])
        pts = [tuple(fx(self.s32(pnt + i * 12 + k * 4)) for k in range(3)) for i in range(npt)]
        polys = []
        for i in range(npol):
            b = pol + i * 20
            nrm = tuple(fx(self.s32(b + k * 4)) for k in range(3))
            v = struct.unpack(">4H", self.mesh[b + 12:b + 20])
            a = att + i * 12
            flag, sort = self.mesh[a], self.mesh[a + 1]
            texno, atrb, colno, gstb, dirw = struct.unpack(">5H", self.mesh[a + 2:a + 12])
            polys.append(dict(normal=nrm, verts=v, flag=flag, sort=sort, texno=texno,
                              atrb=atrb, colno=colno, gstb=gstb, dir=dirw))
        vns = [tuple(fx(self.s32(vn + i * 12 + k * 4)) for k in range(3)) for i in range(npt)] if vn else []
        return dict(offset=o, points=pts, polys=polys, vnormals=vns)

    def _node(self, o):
        if o == 0:
            return None
        if o in self.nodes:
            return self.nodes[o]
        f = struct.unpack(">I2H3i2I3i2I", self.mesh[o:o + 0x30])
        node = dict(offset=o, pdata=f[0], id=f[1], pad=f[2],
                    pos=(fx(f[3]), fx(f[4]), fx(f[5])), extra=(f[6], f[7]),
                    scale=(fx(f[8]), fx(f[9]), fx(f[10])), child_off=f[11], sibling_off=f[12])
        self.nodes[o] = node
        node["child"] = self._node(f[11])
        node["sibling"] = self._node(f[12])
        return node

    def texture_pixels(self, texno):
        """Return (w, h, list of 4-bit indices)."""
        w, h, cg, hv = self.textures[texno]
        n = w * h // 2
        data = self.cg[cg:cg + n]
        px = []
        for b in data:
            px.append(b >> 4)
            px.append(b & 15)
        return w, h, px


if __name__ == "__main__":
    import sys
    m = SatModel(sys.argv[1])
    print("palette banks", len(m.palette), "textures", len(m.textures), "cg bytes", len(m.cg))
    tp = tv = 0
    for i, p in enumerate(m.parts):
        tp += len(p["points"]); tv += len(p["polys"])
        print(f"part {i:2d} @0x{p['offset']:04x}: {len(p['points']):3d} pts {len(p['polys']):3d} polys")
    print("total", tp, "points", tv, "polys")

    def walk(n, depth=0):
        while n:
            print("  " * depth + f"node id={n['id']:2d} pdata=0x{n['pdata']:04x} pos=({n['pos'][0]:.1f},{n['pos'][1]:.1f},{n['pos'][2]:.1f}) extra={n['extra']}")
            walk(n["child"], depth + 1)
            n = n["sibling"]
    walk(m.root)
