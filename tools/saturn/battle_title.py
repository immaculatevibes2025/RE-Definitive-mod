#!/usr/bin/env python3
"""Build data/t_battle.tim, the title menu with a BATTLE GAME option.

The PC title menu is two pre-drawn cells in data/t_start.tim (NEW GAME and
LOAD GAME, one of them lit, plus the copyright lines). This makes the
three-option version from the same sheet, so the new line is in the game's own
lettering: BATTLE is assembled from the letters of PRESS START BUTTON, NEW GAME
and LOAD GAME (B, T from BUTTON; A, E from GAME; L from LOAD).

Output: a 256x256 4bpp TIM with the sheet's own CLUT, three 80-row cells:

    rows   0- 79  NEW GAME lit
    rows  80-159  LOAD GAME lit
    rows 160-239  BATTLE GAME lit

Each cell: NEW at +0, LOAD at +18, BATTLE at +36, copyright at +58 and +70.
A lit line uses colours 1 (face) and 2 (edge); an unlit one 2 and 3, exactly as
the original cells do.

Usage:  python3 battle_title.py <USA/DATA/t_start.tim> <out t_battle.tim>
Needs numpy.
"""
import struct
import sys

import numpy as np

CELL_H = 80
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


def main():
    src, dst = sys.argv[1], sys.argv[2]
    clut, px = read_tim(src)

    press = px[5:5 + LINE_H]       # PRESS START BUTTON
    new_on = px[84:84 + LINE_H]    # NEW GAME, lit
    load_on = px[194:194 + LINE_H]  # LOAD GAME, lit
    copy1 = px[131:140]
    copy2 = px[143:152]

    def glyph(band, x0, x1):
        return band[:, x0:x1 + 1]

    letters = [
        glyph(press, 159, 168),     # B  (BUTTON)
        glyph(new_on, 144, 154),    # A  (GAME)
        glyph(press, 183, 191),     # T  (BUTTON)
        glyph(press, 194, 202),     # T  (BUTTON)
        glyph(load_on, 72, 80),     # L  (LOAD)
        glyph(new_on, 173, 181),    # E  (GAME)
    ]
    game = glyph(new_on, 131, 181)  # GAME, spacing as drawn
    gaps = [2, 2, 1, 3, 3]          # B-A-T-T-L-E, from the source words
    parts = []
    for i, g in enumerate(letters):
        parts.append(g)
        if i < len(gaps):
            parts.append(np.zeros((LINE_H, gaps[i]), np.uint8))
    parts.append(np.zeros((LINE_H, 15), np.uint8))   # word gap of NEW GAME
    parts.append(game)
    word = np.concatenate(parts, 1)
    battle_on = np.zeros_like(new_on)
    x0 = 128 - word.shape[1] // 2
    battle_on[:, x0:x0 + word.shape[1]] = word

    lines_on = [new_on, load_on, battle_on]
    out = np.zeros((256, 256), np.uint8)
    for cell in range(3):
        base = cell * CELL_H
        for k, line in enumerate(lines_on):
            y = base + k * 18
            out[y:y + LINE_H] = line if k == cell else dim(line)
        out[base + 58:base + 67] = copy1
        out[base + 70:base + 79] = copy2
    write_tim(dst, clut, out)
    print("wrote", dst)


if __name__ == "__main__":
    main()
