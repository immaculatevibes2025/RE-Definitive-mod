#!/usr/bin/env python3
"""Build data/t_menu.tim, the scrolling title menu (NEW GAME / LOAD GAME /
BATTLE GAME / OPTIONS / QUIT GAME) in the PC title's own lettering.

Every word is assembled from the letters of data/t_start.tim's three lines
(PRESS START BUTTON, NEW GAME, LOAD GAME), as battle_title.py does for BATTLE
GAME. OPTIONS needs an I, which none of those lines has: it is the stem of the
T (from BUTTON), carried up to the full letter height.

Output: a 256x256 4bpp TIM with the sheet's own CLUT. One 11-row line per
option, lit and unlit, the two copyright lines, and two arrows:

    rows   0- 10  NEW GAME     lit        rows  48- 58  NEW GAME     unlit
    rows  12- 22  LOAD GAME    lit        rows  60- 70  LOAD GAME    unlit
    rows  24- 34  BATTLE GAME  lit        rows  72- 82  BATTLE GAME  unlit
    rows  36- 46  OPTIONS      lit        rows  84- 94  OPTIONS      unlit
    rows  96-104  copyright line 1        rows 108-116  copyright line 2
    rows 120-124  up arrow (x 0-8)        rows 128-132  down arrow (x 0-8)

Lit = colours 1 (face) / 2 (edge), unlit = 2 / 3, as in the original cells.
Every word is centred on x 128.

Usage:  python3 title_menu.py <USA/DATA/t_start.tim> <out t_menu.tim>
Needs numpy.
"""
import struct
import sys

import numpy as np

LINE_H = 11


def read_tim(path):
    d = open(path, "rb").read()
    o = 8
    clen = struct.unpack_from("<I", d, o)[0]
    clut_block = d[o:o + clen]
    o += clen
    ilen, x, y, w, h = struct.unpack_from("<I4H", d, o)
    raw = np.frombuffer(d[o + 12:o + ilen], np.uint8)
    px = np.stack([raw & 15, raw >> 4], 1).reshape(h, w * 4)
    return clut_block, px


def write_tim(path, clut_block, px):
    h, w = px.shape
    pk = (px[:, 0::2] | (px[:, 1::2] << 4)).astype(np.uint8).tobytes()
    img = struct.pack("<I4H", 12 + len(pk), 0, 0, w // 4, h) + pk
    open(path, "wb").write(struct.pack("<II", 0x10, 8) + clut_block + img)


def dim(line):
    out = line.copy()
    out[line == 2] = 3
    out[line == 1] = 2
    return out


def assemble(letters, gaps, total_w=256):
    parts = []
    for i, g in enumerate(letters):
        parts.append(g)
        if i < len(gaps):
            parts.append(np.zeros((LINE_H, gaps[i]), np.uint8))
    word = np.concatenate(parts, 1)
    line = np.zeros((LINE_H, total_w), np.uint8)
    x0 = 128 - word.shape[1] // 2
    line[:, x0:x0 + word.shape[1]] = word
    return line


def recentre(line):
    cols = np.nonzero((line != 0).any(0))[0]
    word = line[:, cols[0]:cols[-1] + 1]
    out = np.zeros_like(line)
    x0 = 128 - word.shape[1] // 2
    out[:, x0:x0 + word.shape[1]] = word
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    clut, px = read_tim(src)

    press = px[5:5 + LINE_H]        # PRESS START BUTTON
    new_on = px[84:84 + LINE_H]     # NEW GAME, lit
    load_on = px[194:194 + LINE_H]  # LOAD GAME, lit
    copy1 = px[131:140]
    copy2 = px[143:152]

    def g(band, x0, x1):
        return band[:, x0:x1 + 1]

    # BATTLE GAME, exactly as battle_title.py builds it.
    battle = assemble([g(press, 159, 168), g(new_on, 144, 154), g(press, 183, 191),
                       g(press, 194, 202), g(load_on, 72, 80), g(new_on, 173, 181),
                       np.zeros((LINE_H, 15), np.uint8), g(new_on, 131, 181)],
                      [2, 2, 1, 3, 3, 0, 0])

    # OPTIONS: O (BUTTON), P (PRESS), T (BUTTON), I (the T's stem), O, N
    # (BUTTON), S (PRESS).
    t = g(press, 183, 191)
    stem_cols = np.nonzero((t[LINE_H - 1] != 0))[0]
    stem = t[:, stem_cols[0]:stem_cols[-1] + 1].copy()
    stem[:] = stem[LINE_H - 1]                     # full height
    o = g(press, 204, 214)
    options = assemble([o, g(press, 30, 39), t, stem, o, g(press, 217, 226),
                        g(press, 64, 73)],
                       [2, 2, 2, 2, 2, 2])

    # QUIT GAME: Q is the O with a tail drawn through its lower right,
    # U (BUTTON), I (the T's stem), T, then GAME from NEW GAME.
    q = o.copy()
    qh, qw = q.shape
    for k in range(4):                      # 4-pixel diagonal tail
        r, c = qh - 4 + k, qw - 4 + k
        if c < qw:
            q[r, c] = 1
            if c - 1 >= 0 and q[r, c - 1] == 0:
                q[r, c - 1] = 2
    quit_ = assemble([q, g(press, 171, 180), stem, t,
                      np.zeros((LINE_H, 15), np.uint8), g(new_on, 131, 181)],
                     [2, 2, 2, 0, 0])

    lines = [recentre(new_on), recentre(load_on), battle, options, recentre(quit_)]

    # Layout (5 lines): lit at k*12, unlit at 60 + k*12, copyright 120 / 132,
    # arrows 144 / 152.
    out = np.zeros((256, 256), np.uint8)
    for k, line in enumerate(lines):
        out[k * 12:k * 12 + LINE_H] = line
        out[60 + k * 12:60 + k * 12 + LINE_H] = dim(line)
    out[120:129] = copy1
    out[132:141] = copy2

    # Arrows: 9 wide, 5 tall, face colour 2 (the unlit text's face).
    up = np.zeros((5, 9), np.uint8)
    for r in range(5):
        up[r, 4 - r:4 + r + 1] = 2
    out[144:149, 0:9] = up
    out[152:157, 0:9] = up[::-1]

    write_tim(dst, clut, out)
    print("wrote", dst)


if __name__ == "__main__":
    main()
