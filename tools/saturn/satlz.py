"""Decompressor for the LZ scheme used by Resident Evil (Sega Saturn).

Reimplemented from the SH-2 routine at 0x0602B03C in GAME1.PRG..GAME5.PRG
(load address 0x0602B000). Signature in the original: R4 = src, R5 = dst,
returns the number of bytes written.

Bitstream: control bits are read LSB-first, 8 per control byte, interleaved
with the data bytes as they are needed.
    1        -> literal: copy one byte
    0 0 a b  -> short match: length = ((a<<1)|b) + 2 (2..5),
                offset byte o: source = dst - 256 + o
    0 1      -> long match: little-endian word w = b0 | b1<<8
                w == 0         -> end of stream
                offset = (w >> 3) - 8192   (back 1..8192)
                n = b0 & 7
                n != 0 -> length = n + 2  (3..9)
                n == 0 -> length = next byte + 1 (1..256)
"""


def decompress(src, start=0, limit=None):
    """Return (output bytes, number of input bytes consumed)."""
    pos = start
    out = bytearray()
    ctrl = 0
    nbits = 0

    def getbit():
        nonlocal ctrl, nbits, pos
        if nbits == 0:
            ctrl = src[pos]
            pos += 1
            nbits = 8
        bit = ctrl & 1
        ctrl >>= 1
        nbits -= 1
        return bit

    while True:
        if limit is not None and len(out) > limit:
            raise ValueError("output exceeds limit")
        if getbit():
            out.append(src[pos])
            pos += 1
            continue
        if not getbit():
            n = getbit() << 1
            n |= getbit()
            off = src[pos] - 256
            pos += 1
            length = n + 2
        else:
            b0 = src[pos]
            b1 = src[pos + 1]
            pos += 2
            w = b0 | (b1 << 8)
            if w == 0:
                break
            off = (w >> 3) - 8192
            n = b0 & 7
            if n:
                length = n + 2
            else:
                length = src[pos] + 1
                pos += 1
        s = len(out) + off
        if s < 0:
            raise ValueError(f"match before start of output (offset {off} at {len(out)})")
        for _ in range(length):
            out.append(out[s])
            s += 1
    return bytes(out), pos - start


if __name__ == "__main__":
    import sys
    data = open(sys.argv[1], "rb").read()
    off = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    out, used = decompress(data, off)
    sys.stdout.write(f"in={used} out={len(out)}\n")
    if len(sys.argv) > 3:
        open(sys.argv[3], "wb").write(out)
