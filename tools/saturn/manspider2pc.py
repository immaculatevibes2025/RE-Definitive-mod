#!/usr/bin/env python3
"""Convert the RE 1.5 (Biohazard 2 prototype) Man Spider into a PC RE1 EMD that the
Battle Game loads in place of the Tyrant (enemy/em1018.emd, em1118.emd).

Source: PSX/EMD/CDEMD0.EMS, entry 14 (a run of PS1 EMDs, each sector aligned;
sections 0-8 = (empty), EDD, EMR, EDD, EMR, EDD, EMR, mesh, TIM).

  * Skeleton: the 1.5 EMR header/armature are the RE1 layout; copied.
  * Frames: 1.5 packs joint angles as 12-bit values, RE1 stores s16 - unpacked.
  * Animations: rebuilt in the Tyrant's two sets (3 + 10 clips) so the Tyrant's
    AI drives him; each clip is resampled to the Tyrant clip's frame count so
    frame-number checks in the AI still line up. TYRANT_MAP below picks the clips.
  * Mesh: 1.5 tri/quad lists (separate UV records) -> RE1 TMD mode-0x34 tris.
  * Texture: the TIM is the same shape as the Tyrant's (256x256 8bpp, 2 CLUT
    rows at y=480, pages 0x80/0x81) and is copied unchanged.

Usage: manspider2pc.py <CDEMD0.EMS> <Tyrant EM100C.EMD> <output dir>
"""
import struct, sys, os

STARTS_14 = 0x228800

# Tyrant clip -> Man Spider clip (EDD section 1 of the 1.5 file).
# (Set A is the player's grab reaction and stays the Tyrant's.)
# Set B: 0 walk, 1 walk/turn, 2 stand, 3 recover, 4 slash, 5 grab, 6 long grab,
#        7 stand, 8 death (forward), 9 lying.
# 1.5 clips the ported AI (Tyrant.cpp, manspider_*) plays:
#  0 crawl idle, 1 crawl walk, 2 rise/roar, 3 upright stalk, 4 hiss, 5 spit,
#  7 hit, 8/9 heavy hit, 10/11 death, 12 swipe, 13 swipe recover,
#  22 dodge dash, 23 crawl bite.
USED_CLIPS = {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 22, 23}

# The player's side of the upright grab (1.5 sections 5/6, clips 0-3, paired
# with the Man Spider's 14-17: lunge, hold, throw-off, decapitation). Leon's
# 1.5 skeleton is Chris's with the joints renumbered; RE1 joint j takes 1.5
# joint PLAYER_MAP[j]. Appended to set A after the Tyrant's three clips, so
# they are set-A clips 3-6.
PLAYER_CLIPS = [0, 1, 2, 3]
PLAYER_MAP = [0, 8, 1, 5, 6, 7, 2, 3, 4, 12, 13, 14, 9, 10, 11]


def sections(d, o):
    dirofs, cnt = struct.unpack_from('<II', d, o)
    offs = list(struct.unpack_from('<%dI' % cnt, d, o + dirofs)) + [dirofs]
    return [d[o + offs[i]:o + offs[i + 1]] for i in range(cnt)]

def edd_read(sec):
    na = struct.unpack_from('<2H', sec, 0)[1] // 4
    out = []
    for i in range(na):
        c, o = struct.unpack_from('<2H', sec, 4 * i)
        out.append([struct.unpack_from('<I', sec, o + 4 * k)[0] for k in range(c)])
    return out

def edd_write(clips, hi):
    n = len(clips); hdr = b''; body = b''; pos = 4 * n
    for c in clips:
        hdr += struct.pack('<2H', len(c), pos)
        for f in c: body += struct.pack('<I', (hi << 16) | f)
        pos += 4 * len(c)
    return hdr + body

def emr_15(sec):
    armofs, frmofs, cnt, esz = struct.unpack_from('<4H', sec, 0)
    head = sec[:frmofs]
    frames = []
    for f in range((len(sec) - frmofs) // esz):
        p = frmofs + f * esz
        offspd = sec[p:p + 12]; raw = sec[p + 12:p + esz]; ang = []
        for k in range(cnt * 3):
            bi = (k * 12) // 8
            v = raw[bi] | ((raw[bi + 1] & 15) << 8) if k % 2 == 0 else (raw[bi] >> 4) | (raw[bi + 1] << 4)
            ang.append(v)
        frames.append((offspd, ang))
    return head, cnt, frames

def emr_write(head, cnt, frames):
    esz = (12 + cnt * 6 + 3) & ~3
    armofs, frmofs = struct.unpack_from('<2H', head, 0)
    h = bytearray(head); struct.pack_into('<4H', h, 0, armofs, frmofs, cnt, esz)
    out = bytes(h)
    for offspd, ang in frames:
        fr = offspd + struct.pack('<%dh' % len(ang), *[a if a < 2048 else a - 4096 for a in ang])
        out += fr + b'\0' * (esz - len(fr))
    return out

def resample(seq, n):
    if n <= 1: return [seq[0]]
    return [seq[round(i * (len(seq) - 1) / (n - 1))] for i in range(n)]

def build_set_native(clips15, frames15, keep):
    """The Man Spider's own clip list, in 1.5 numbering (ManSpider AI plays
    these indices directly). Clips the port does not use are cut to one frame
    to keep the file inside the room's model budget."""
    pool = []; clips = []; where = {}
    for ci, c in enumerate(clips15):
        seq = [x & 0xfff for x in c] if ci in keep else [c[0] & 0xfff]
        idx = []
        for fi in seq:
            if fi not in where:
                where[fi] = len(pool); pool.append(frames15[fi])
            idx.append(where[fi])
        clips.append(idx)
    return pool, clips

def mesh_tmd(sec):
    n = struct.unpack_from('<I', sec, 8)[0] // 2
    base = 12
    objs = []
    for i in range(n):
        t = struct.unpack_from('<7I', sec, 12 + i * 56); q = struct.unpack_from('<7I', sec, 12 + i * 56 + 28)
        V = [sec[base + t[0] + 8 * k: base + t[0] + 8 * k + 8] for k in range(t[1])]
        N = [sec[base + t[2] + 8 * k: base + t[2] + 8 * k + 8] for k in range(t[3])]
        prims = []
        for k in range(t[5]):
            n0, v0, n1, v1, n2, v2 = struct.unpack_from('<6H', sec, base + t[4] + 12 * k)
            u0, w0, cba, u1, w1, tsb, u2, w2, _ = struct.unpack_from('<BBHBBHBBH', sec, base + t[6] + 12 * k)
            prims.append(((u0, w0), (u1, w1), (u2, w2), cba, tsb, (n0, v0), (n1, v1), (n2, v2)))
        if q[0] != t[0]:
            raise SystemExit('quad list has its own vertices - not handled')
        for k in range(q[5]):
            n0, v0, n1, v1, n2, v2, n3, v3 = struct.unpack_from('<8H', sec, base + q[4] + 16 * k)
            u0, w0, cba, u1, w1, tsb, u2, w2, _, u3, w3, _ = struct.unpack_from('<BBHBBHBBHBBH', sec, base + q[6] + 16 * k)
            A = ((u0, w0), (n0, v0)); B = ((u1, w1), (n1, v1)); C = ((u2, w2), (n2, v2)); D = ((u3, w3), (n3, v3))
            for a, b, c in ((A, B, C), (B, D, C)):
                prims.append((a[0], b[0], c[0], cba, tsb, a[1], b[1], c[1]))
        objs.append((V, N, prims))
    # One object past the joints, as the Tyrant's TMD has (16 objects for 15
    # joints - its heart). Code for entity id 0x10 builds an object for index
    # jointCount; without it that read runs off the table (the room-15 crash).
    objs.append(min(objs, key=lambda o: len(o[2])))
    # write TMD
    hdr_sz = 28 * len(objs)
    blobs = b''; table = b''
    for V, N, prims in objs:
        vo = hdr_sz + len(blobs); blobs += b''.join(V)
        no = hdr_sz + len(blobs); blobs += b''.join(N)
        po = hdr_sz + len(blobs)
        for (uv0, uv1, uv2, cba, tsb, a, b, c) in prims:
            blobs += struct.pack('<I', 0x34000609)
            blobs += struct.pack('<BBH', uv0[0], uv0[1], cba)
            blobs += struct.pack('<BBH', uv1[0], uv1[1], tsb)
            blobs += struct.pack('<BBH', uv2[0], uv2[1], 0)
            blobs += struct.pack('<6H', a[0], a[1], b[0], b[1], c[0], c[1])
        table += struct.pack('<7I', vo, len(V), no, len(N), po, len(prims), 0)
    body = table + blobs
    return struct.pack('<3I', 12 + len(body), 0, len(objs)) + body, len(objs)

def unflip_root(offspd, ang):
    """1.5 player clips carry a half-turn about Y on the root (euler about
    (2048, 0, -2048)), so on screen the body faces opposite to the heading the
    engine holds. Take it out (R' = Ry(pi) R, root offset x/z negated) so the
    player's heading and their on-screen facing agree. Root euler is
    R = Rx(a) Ry(b) Rz(c), 4096 units per turn."""
    import math
    k = 2 * math.pi / 4096
    a, b, c = [(v if v < 2048 else v - 4096) * k for v in ang[0:3]]
    ca, sa, cb, sb, cc, sc = math.cos(a), math.sin(a), math.cos(b), math.sin(b), math.cos(c), math.sin(c)
    R = [[cb * cc, -cb * sc, sb],
         [sa * sb * cc + ca * sc, -sa * sb * sc + ca * cc, -sa * cb],
         [-ca * sb * cc + sa * sc, ca * sb * sc + sa * cc, ca * cb]]
    R = [[-v for v in R[0]], R[1], [-v for v in R[2]]]
    b2 = math.asin(max(-1.0, min(1.0, R[0][2])))
    a2 = math.atan2(-R[1][2], R[2][2])
    c2 = math.atan2(-R[0][1], R[0][0])
    na = [int(round(x / k)) & 4095 for x in (a2, b2, c2)]
    ox, oy, oz, sx, sy, sz = struct.unpack('<6h', offspd)
    offspd = struct.pack('<6h', -ox, oy, -oz, sx, sy, sz)
    return offspd, na + list(ang[3:])

def add_player_grab(emr_a, edd_a, s):
    armofs, frmofs, cnt, esz = struct.unpack_from('<4H', emr_a, 0)
    assert cnt == 15
    clips = edd_read(edd_a)
    pclips = edd_read(s[5]); _, pcnt, pframes = emr_15(s[6])
    assert pcnt == 15
    base = max(f & 0xffff for c in clips for f in c) + 1
    assert frmofs + base * esz <= len(emr_a)
    out = bytearray(emr_a[:frmofs + base * esz])
    n = 0
    for ci in PLAYER_CLIPS:
        idx = []
        for e in pclips[ci]:
            offspd, ang = pframes[e & 0xfff]
            offspd, ang = unflip_root(offspd, ang)
            vals = []
            for j in range(15):
                k = PLAYER_MAP[j]
                vals += [a if a < 2048 else a - 4096 for a in ang[3 * k:3 * k + 3]]
            fr = offspd + struct.pack('<45h', *vals)
            out += fr + b'\0' * (esz - len(fr))
            idx.append(base + n); n += 1
        clips.append([(1 << 16) | i for i in idx])
    # edd_write takes frame lists (low word) and a timing word
    hdr = b''; body = b''; pos = 4 * len(clips)
    for c in clips:
        hdr += struct.pack('<2H', len(c), pos)
        for f in c: body += struct.pack('<I', f)
        pos += 4 * len(c)
    return bytes(out), hdr + body

def main(ems, tyrant, outdir):
    d = open(ems, 'rb').read()
    s = sections(d, STARTS_14)
    clips15 = edd_read(s[1]); head, cnt, frames15 = emr_15(s[2])
    ty = open(tyrant, 'rb').read(); sz = len(ty) & ~3
    ft = struct.unpack('<5I', ty[sz - 20:sz])
    ty_b = edd_read(ty[ft[2]:ft[3]])
    # Set A is the PLAYER's side of the Tyrant's grab (15 joints, played on
    # Chris / Jill), not the Tyrant's own - keep the Tyrant's unchanged.
    emr_a = ty[0:ft[0]]; edd_a = ty[ft[0]:ft[1]]
    emr_a, edd_a = add_player_grab(emr_a, edd_a, s)
    pool_b, cb = build_set_native(clips15, frames15, USED_CLIPS)
    emr_b = emr_write(head, cnt, pool_b); edd_b = edd_write(cb, 1)
    tmd, nobj = mesh_tmd(s[7])
    tim = s[8]
    out = bytearray(); offs = []
    def put(b):
        while len(out) % 4: out.append(0)
        offs.append(len(out)); out.extend(b)
    put(emr_a); put(edd_a); put(emr_b); put(edd_b); put(tmd); put(tim)
    while len(out) % 4: out.append(0)
    out += struct.pack('<5I', *offs[1:])
    os.makedirs(outdir, exist_ok=True)
    for name in ('em1018.emd', 'em1118.emd'):
        open(os.path.join(outdir, name), 'wb').write(out)
    print('joints', cnt, 'objects', nobj, 'size', len(out))

if __name__ == '__main__':
    main(*sys.argv[1:4])
