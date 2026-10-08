#!/usr/bin/env python3
"""Rebuild the Option Mode tab buttons in Jopt06.tim / Opt11.tim.

The PC release paints its tab buttons into each 320x240 16-bit background:
a wide "SideWinder game pad" bar over JOY PAD | KEYBOARD | EXIT. This splits
the bar into two buttons, VIDEO | SOUND, and relabels JOY PAD as GAME PAD,
in the art's own 3x7 lettering. KEYBOARD and EXIT are left as they are.

Usage:  python3 option_tabs.py <in.tim> <out.tim>
Needs numpy.
"""
import sys
import numpy as np

G = {
    'X': ["#.#", "#.#", "#.#", ".#.", "#.#", "#.#", "#.#"],
    'T': ["###", ".#.", ".#.", ".#.", ".#.", ".#.", ".#."],
    'K': ["#.#", "#.#", "##.", "#..", "##.", "#.#", "#.#"],
    'B': ["##.", "#.#", "#.#", "##.", "#.#", "#.#", "##."],
    'R': ["##.", "#.#", "#.#", "##.", "#.#", "#.#", "#.#"],
    'Q': ["###", "#.#", "#.#", "#.#", "#.#", "##.", ".##"],
    'L': ["#..", "#..", "#..", "#..", "#..", "#..", "###"],
    'Y': ["#.#", "#.#", "#.#", ".#.", ".#.", ".#.", ".#."],
    'A': ["###", "#.#", "#.#", "###", "#.#", "#.#", "#.#"],
    'D': ["##.", "#.#", "#.#", "#.#", "#.#", "#.#", "##."],
    'E': ["###", "#..", "#..", "###", "#..", "#..", "###"],
    'G': ["###", "#..", "#..", "#.#", "#.#", "#.#", "###"],
    'I': ["#", "#", "#", "#", "#", "#", "#"],
    'M': ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    'N': ["#..#", "##.#", "##.#", "#.##", "#.##", "#..#", "#..#"],
    'O': ["###", "#.#", "#.#", "#.#", "#.#", "#.#", "###"],
    'P': ["###", "#.#", "#.#", "###", "#..", "#..", "#.."],
    'S': ["###", "#..", "#..", "###", "..#", "..#", "###"],
    'U': ["#.#", "#.#", "#.#", "#.#", "#.#", "#.#", "###"],
    'V': ["#.#", "#.#", "#.#", "#.#", "#.#", "#.#", ".#."],
}
INK = (7, 11, 12)          # the art's lettering colour (5-bit RGB)


def word(text, gap):
    cols = []
    for ch in text:
        if ch == ' ':
            cols.append(np.zeros((7, 2), bool))
        else:
            cols.append(np.array([[c == '#' for c in r] for r in G[ch]]))
        cols.append(np.zeros((7, gap), bool))
    return np.concatenate(cols[:-1], 1)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = bytearray(open(src, 'rb').read())
    px = np.frombuffer(bytes(d[20:20 + 320 * 240 * 2]), '<u2').reshape(240, 320).copy()
    orig = px.copy()
    r = (px & 31).astype(int); g = ((px >> 5) & 31).astype(int); b = ((px >> 10) & 31).astype(int)

    def clean(x0, x1, y0, y1):
        # Fill each face column with its own lightest shade: the faces are a
        # horizontal gradient, flat top to bottom.
        for x in range(x0, x1):
            lum = r[y0:y1, x] + g[y0:y1, x] + b[y0:y1, x]
            k = y0 + int(np.argmax(lum))
            px[y0:y1, x] = px[k, x]

    def ink(mask, x0, y0):
        h, w = mask.shape
        v = INK[0] | (INK[1] << 5) | (INK[2] << 10)
        px[y0:y0 + h, x0:x0 + w][mask] = v

    def centred(text, gap, cx, y0):
        m = word(text, gap)
        ink(m, cx - m.shape[1] // 2, y0)

    # Top bar face rows 32-41, bottom faces 46-55 (14 rows lower).
    clean(12, 127, 32, 42)
    # Split the bar: the gap between JOY PAD and KEYBOARD, carried up.
    for y in range(30, 44):
        px[y, 66:72] = px[y + 14, 46:52]
    clean(12, 46, 46, 56)

    centred("VIDEO", 2, 39, 33)
    centred("SOUND", 2, 99, 33)
    centred("GAME PAD", 1, 29, 48)

    # --- GAMEPLAY: a third row, the original full-width bar relabelled ---
    bar = orig[30:44, :].copy()
    tmp = px
    px = bar          # the helpers below draw into whatever px names
    rb = (bar & 31).astype(int); gb = ((bar >> 5) & 31).astype(int); bb = ((bar >> 10) & 31).astype(int)
    for x in range(12, 127):
        lum = rb[2:12, x] + gb[2:12, x] + bb[2:12, x]
        k = 2 + int(np.argmax(lum))
        bar[2:12, x] = bar[k, x]
    centred("GAMEPLAY", 1, 69, 3)
    px = tmp

    # --- Title: OPTION MODE -> OPTION ---
    # Erase " MODE" (fill from the rows above and below), then an S after
    # the N: the E's top half over a mirrored E's bottom half.
    e = px[11:25, 115:125].copy()
    s = np.concatenate([e[:8], e[8:, ::-1]], 0)
    for x in range(75, 128):
        a, z = int(px[10, x]), int(px[25, x])
        for y in range(11, 25):
            t = (y - 10) / 15.0
            v = 0
            for c in range(3):
                ca = (a >> (5 * c)) & 31; cz = (z >> (5 * c)) & 31
                v |= int(round(ca + (cz - ca) * t)) << (5 * c)
            px[y, x] = v
    # (No S: the title reads OPTION.)

    # --- Re-stack the left column (x < 135): the title, then five tab rows
    # (EXIT / VIDEO | SOUND / GAME PAD | KEYBOARD / GAMEPLAY / QUIT GAME),
    # each 12 rows (two face rows fewer than the original buttons), then the
    # portrait frame, 6 black rows shorter so everything fits in 240.
    W = 135
    old = px[:, :W].copy()          # title already renamed above

    def tab_row(left, right=None):
        global_px = None
        b = orig[30:44, :W].copy()  # the original full-width bar
        rb = (b & 31).astype(int) + ((b >> 5) & 31).astype(int) + ((b >> 10) & 31).astype(int)
        for x in range(12, 127):
            k = 2 + int(np.argmax(rb[2:12, x]))
            b[2:12, x] = b[k, x]
        if right is not None:       # split like the original bottom row's gap
            b[:, 66:72] = orig[44:58, 46:52]
        b = b[[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13]]
        v = INK[0] | (INK[1] << 5) | (INK[2] << 10)
        def put(text, cx):
            m = word(text, 1 if ' ' in text or len(text) > 6 else 2)
            x0 = cx - m.shape[1] // 2
            b[2:9, x0:x0 + m.shape[1]][m] = v
        if right is None:
            put(left, 69)
        else:
            put(left, 39)
            put(right, 99)
        return b

    rows = [tab_row("EXIT"), tab_row("VIDEO", "SOUND"), tab_row("GAME PAD", "KEYBOARD"),
            tab_row("GAMEPLAY"), tab_row("QUIT GAME")]
    new = np.concatenate([old[2:3], old[11:26], orig[29:30, :W]] + rows +
                         [orig[58:59, :W], old[59:62], old[62:120], old[126:227]], 0)
    assert new.shape[0] == 240, new.shape
    px[:, :W] = new

    # --- QUICK TURN: one more button square on the right-hand column ---
    R = 134
    if 'jopt' in src.lower():
        # GAME PAD: five squares -> eight (QUICK TURN, QUICK KNIFE, RELOAD),
        # 26 px apart from y 16.
        sq = px[27:51, 137:161].copy()
        for top in (27, 68, 112, 154, 197):
            px[top - 1:top + 26, 136:163] = px[top - 1:top + 26, 166:193]
        for k in range(8):
            top = 16 + 26 * k
            px[top:top + 24, 137:161] = sq
    else:
        # KEYBOARD: rebuild the button rail (x 134-165) with twelve squares
        # 18 px tall (two face rows dropped) in groups of 3 / 4 / 5, and take
        # the old group lines out of the panel beside it.
        C0, C1 = R, 166
        old = px.copy()
        sq = np.concatenate([old[22:30, C0:C1], old[32:42, C0:C1]], 0)   # 18 rows
        parts = [old[19:22, C0:C1]]
        parts += [sq] * 3 + [old[82:88, C0:C1]]
        parts += [sq] * 4 + [old[168:175, C0:C1]]
        parts += [sq] * 5 + [old[221:225, C0:C1]]
        rail = np.concatenate(parts, 0)
        y0 = 2
        px[:, C0:C1] = old[:, C0:C1]
        # background behind/under the rail: the column just right of it
        px[:, C0:C1] = old[:, C1:C1 + (C1 - C0)]
        px[y0:y0 + rail.shape[0], C0:C1] = rail
        for a0, a1 in ((80, 90), (166, 177), (193, 202)):
            n = a1 - a0
            px[a0:a1, C1:] = old[a0 - n:a0, C1:]

    d[20:20 + 320 * 240 * 2] = px.astype('<u2').tobytes()
    open(dst, 'wb').write(bytes(d))
    print("wrote", dst)


if __name__ == "__main__":
    main()
