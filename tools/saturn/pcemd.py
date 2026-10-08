"""Reader for PC/PS1 Resident Evil EMD files: footer, TMD mesh, TIM texture."""
import struct

class PcEmd:
    def __init__(self, data):
        self.d = d = data
        sz = len(d) & ~3
        self.footer = list(struct.unpack("<5I", d[sz - 0x14:sz]))
        self.tmd_off = self.footer[3]
        self.tim_off = self.footer[4]
        self._tmd()
        self._tim()

    def _tmd(self):
        d = self.d; T = self.tmd_off
        self.tmd_size, self.tmd_flags, nobj = struct.unpack("<3I", d[T:T + 12])
        base = T + 12
        self.objs = []
        for i in range(nobj):
            vt, nv, nt, nn, pt, npr, sc = struct.unpack("<7I", d[base + i * 28:base + i * 28 + 28])
            verts = [struct.unpack("<4h", d[base + vt + k * 8:base + vt + k * 8 + 8])[:3] for k in range(nv)]
            norms = [struct.unpack("<4h", d[base + nt + k * 8:base + nt + k * 8 + 8])[:3] for k in range(nn)]
            prims = []
            p = base + pt
            for k in range(npr):
                h = struct.unpack("<I", d[p:p + 4])[0]
                ilen = (h >> 8) & 0xff; mode = h >> 24
                w = struct.unpack("<%dI" % ilen, d[p + 4:p + 4 + 4 * ilen])
                if mode == 0x34 and ilen == 6:
                    u0, v0, cba = w[0] & 0xff, (w[0] >> 8) & 0xff, w[0] >> 16
                    u1, v1, tsb = w[1] & 0xff, (w[1] >> 8) & 0xff, w[1] >> 16
                    u2, v2 = w[2] & 0xff, (w[2] >> 8) & 0xff
                    n0, i0, n1, i1, n2, i2 = struct.unpack("<6H", d[p + 16:p + 28])
                    prims.append(dict(uv=[(u0, v0), (u1, v1), (u2, v2)], cba=cba, tsb=tsb,
                                      v=[i0, i1, i2], n=[n0, n1, n2], hdr=h))
                p += 4 * (ilen + 1)
            self.objs.append(dict(verts=verts, norms=norms, prims=prims, scale=sc))

    def _tim(self):
        d = self.d; X = self.tim_off
        tid, fl = struct.unpack("<II", d[X:X + 8])
        o = X + 8
        self.tim_flags = fl
        blen, cx, cy, cw, ch = struct.unpack("<I4H", d[o:o + 12])
        self.clut_xywh = (cx, cy, cw, ch)
        self.clut = list(struct.unpack("<%dH" % (cw * ch), d[o + 12:o + 12 + cw * ch * 2]))
        o += blen
        blen, ix, iy, iw, ih = struct.unpack("<I4H", d[o:o + 12])
        self.img_xywh = (ix, iy, iw, ih)
        self.pixels = d[o + 12:o + 12 + iw * 2 * ih]
        self.width, self.height = iw * 2, ih   # 8bpp
