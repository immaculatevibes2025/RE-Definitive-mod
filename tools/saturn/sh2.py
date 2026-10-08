"""Minimal SH-2 disassembler (big-endian), enough for reverse engineering.
Usage: from sh2 import disasm; lines = disasm(data, base)
"""
import struct


def s8(x):
    return x - 256 if x & 0x80 else x


def s12(x):
    return x - 4096 if x & 0x800 else x


def decode(op, pc, data, base):
    """Return (mnemonic string, note) for one 16-bit opcode at address pc."""
    n = (op >> 8) & 15
    m = (op >> 4) & 15
    d4 = op & 15
    d8 = op & 255
    top = op >> 12
    note = ""

    def lit_l(addr):
        off = addr - base
        if 0 <= off + 4 <= len(data):
            return struct.unpack(">I", data[off:off + 4])[0]
        return None

    def lit_w(addr):
        off = addr - base
        if 0 <= off + 2 <= len(data):
            return struct.unpack(">h", data[off:off + 2])[0]
        return None

    if top == 0:
        low = op & 15
        if op == 0x0009: return "nop", note
        if op == 0x000B: return "rts", note
        if op == 0x002B: return "rte", note
        if op == 0x0008: return "clrt", note
        if op == 0x0018: return "sett", note
        if op == 0x0019: return "div0u", note
        if op == 0x0028: return "clrmac", note
        if op == 0x001B: return "sleep", note
        lo8 = op & 255
        if lo8 == 0x02: return f"stc SR,R{n}", note
        if lo8 == 0x12: return f"stc GBR,R{n}", note
        if lo8 == 0x22: return f"stc VBR,R{n}", note
        if lo8 == 0x0A: return f"sts MACH,R{n}", note
        if lo8 == 0x1A: return f"sts MACL,R{n}", note
        if lo8 == 0x2A: return f"sts PR,R{n}", note
        if lo8 == 0x23: return f"braf R{n}", note
        if lo8 == 0x03: return f"bsrf R{n}", note
        if lo8 == 0x29: return f"movt R{n}", note
        if low == 4: return f"mov.b R{m},@(R0,R{n})", note
        if low == 5: return f"mov.w R{m},@(R0,R{n})", note
        if low == 6: return f"mov.l R{m},@(R0,R{n})", note
        if low == 7: return f"mul.l R{m},R{n}", note
        if low == 12: return f"mov.b @(R0,R{m}),R{n}", note
        if low == 13: return f"mov.w @(R0,R{m}),R{n}", note
        if low == 14: return f"mov.l @(R0,R{m}),R{n}", note
        if low == 15: return f"mac.l @R{m}+,@R{n}+", note
        return f".word 0x{op:04x}", note
    if top == 1:
        return f"mov.l R{m},@({d4*4},R{n})", note
    if top == 2:
        t = {0: "mov.b R%d,@R%d", 1: "mov.w R%d,@R%d", 2: "mov.l R%d,@R%d",
             4: "mov.b R%d,@-R%d", 5: "mov.w R%d,@-R%d", 6: "mov.l R%d,@-R%d",
             7: "div0s R%d,R%d", 8: "tst R%d,R%d", 9: "and R%d,R%d", 10: "xor R%d,R%d",
             11: "or R%d,R%d", 12: "cmp/str R%d,R%d", 13: "xtrct R%d,R%d",
             14: "mulu.w R%d,R%d", 15: "muls.w R%d,R%d"}
        if d4 in t: return t[d4] % (m, n), note
        return f".word 0x{op:04x}", note
    if top == 3:
        t = {0: "cmp/eq", 2: "cmp/hs", 3: "cmp/ge", 4: "div1", 5: "dmulu.l", 6: "cmp/hi",
             7: "cmp/gt", 8: "sub", 10: "subc", 11: "subv", 12: "add", 13: "dmuls.l",
             14: "addc", 15: "addv"}
        if d4 in t: return f"{t[d4]} R{m},R{n}", note
        return f".word 0x{op:04x}", note
    if top == 4:
        lo8 = op & 255
        t = {0x00: "shll R%d", 0x01: "shlr R%d", 0x02: "sts.l MACH,@-R%d", 0x03: "stc.l SR,@-R%d",
             0x04: "rotl R%d", 0x05: "rotr R%d", 0x06: "lds.l @R%d+,MACH", 0x07: "ldc.l @R%d+,SR",
             0x08: "shll2 R%d", 0x09: "shlr2 R%d", 0x0A: "lds R%d,MACH", 0x0B: "jsr @R%d",
             0x0E: "ldc R%d,SR", 0x10: "dt R%d", 0x11: "cmp/pz R%d", 0x12: "sts.l MACL,@-R%d",
             0x13: "stc.l GBR,@-R%d", 0x15: "cmp/pl R%d", 0x16: "lds.l @R%d+,MACL",
             0x17: "ldc.l @R%d+,GBR", 0x18: "shll8 R%d", 0x19: "shlr8 R%d", 0x1A: "lds R%d,MACL",
             0x1B: "tas.b @R%d", 0x1E: "ldc R%d,GBR", 0x20: "shal R%d", 0x21: "shar R%d",
             0x22: "sts.l PR,@-R%d", 0x23: "stc.l VBR,@-R%d", 0x24: "rotcl R%d", 0x25: "rotcr R%d",
             0x26: "lds.l @R%d+,PR", 0x27: "ldc.l @R%d+,VBR", 0x28: "shll16 R%d", 0x29: "shlr16 R%d",
             0x2A: "lds R%d,PR", 0x2B: "jmp @R%d", 0x2E: "ldc R%d,VBR"}
        if lo8 in t: return t[lo8] % n, note
        if d4 == 15: return f"mac.w @R{m}+,@R{n}+", note
        return f".word 0x{op:04x}", note
    if top == 5:
        return f"mov.l @({d4*4},R{m}),R{n}", note
    if top == 6:
        t = {0: "mov.b @R%d,R%d", 1: "mov.w @R%d,R%d", 2: "mov.l @R%d,R%d", 3: "mov R%d,R%d",
             4: "mov.b @R%d+,R%d", 5: "mov.w @R%d+,R%d", 6: "mov.l @R%d+,R%d", 7: "not R%d,R%d",
             8: "swap.b R%d,R%d", 9: "swap.w R%d,R%d", 10: "negc R%d,R%d", 11: "neg R%d,R%d",
             12: "extu.b R%d,R%d", 13: "extu.w R%d,R%d", 14: "exts.b R%d,R%d", 15: "exts.w R%d,R%d"}
        return t[d4] % (m, n), note
    if top == 7:
        return f"add #{s8(d8)},R{n}", note
    if top == 8:
        sub = (op >> 8) & 15
        if sub == 0: return f"mov.b R0,@({d4},R{m})", note
        if sub == 1: return f"mov.w R0,@({d4*2},R{m})", note
        if sub == 4: return f"mov.b @({d4},R{m}),R0", note
        if sub == 5: return f"mov.w @({d4*2},R{m}),R0", note
        if sub == 8: return f"cmp/eq #{s8(d8)},R0", note
        tgt = pc + 4 + s8(d8) * 2
        if sub == 9: return f"bt 0x{tgt:08x}", note
        if sub == 11: return f"bf 0x{tgt:08x}", note
        if sub == 13: return f"bt/s 0x{tgt:08x}", note
        if sub == 15: return f"bf/s 0x{tgt:08x}", note
        return f".word 0x{op:04x}", note
    if top == 9:
        addr = pc + 4 + d8 * 2
        v = lit_w(addr)
        return f"mov.w @(0x{addr:08x}),R{n}", (f"= {v} (0x{v & 0xffff:04x})" if v is not None else "")
    if top == 10:
        return f"bra 0x{pc + 4 + s12(op & 0xfff) * 2:08x}", note
    if top == 11:
        return f"bsr 0x{pc + 4 + s12(op & 0xfff) * 2:08x}", note
    if top == 12:
        sub = (op >> 8) & 15
        if sub == 0: return f"mov.b R0,@({d8},GBR)", note
        if sub == 1: return f"mov.w R0,@({d8*2},GBR)", note
        if sub == 2: return f"mov.l R0,@({d8*4},GBR)", note
        if sub == 3: return f"trapa #{d8}", note
        if sub == 4: return f"mov.b @({d8},GBR),R0", note
        if sub == 5: return f"mov.w @({d8*2},GBR),R0", note
        if sub == 6: return f"mov.l @({d8*4},GBR),R0", note
        if sub == 7:
            addr = ((pc + 4) & ~3) + d8 * 4
            return f"mova @(0x{addr:08x}),R0", note
        if sub == 8: return f"tst #0x{d8:02x},R0", note
        if sub == 9: return f"and #0x{d8:02x},R0", note
        if sub == 10: return f"xor #0x{d8:02x},R0", note
        if sub == 11: return f"or #0x{d8:02x},R0", note
        if sub == 12: return f"tst.b #0x{d8:02x},@(R0,GBR)", note
        if sub == 13: return f"and.b #0x{d8:02x},@(R0,GBR)", note
        if sub == 14: return f"xor.b #0x{d8:02x},@(R0,GBR)", note
        if sub == 15: return f"or.b #0x{d8:02x},@(R0,GBR)", note
    if top == 13:
        addr = ((pc + 4) & ~3) + d8 * 4
        v = lit_l(addr)
        return f"mov.l @(0x{addr:08x}),R{n}", (f"= 0x{v:08x}" if v is not None else "")
    if top == 14:
        return f"mov #{s8(d8)},R{n}", note
    return f".word 0x{op:04x}", note


def disasm(data, base, start=0, end=None):
    """Linear sweep. Returns list of (addr, op, text, note)."""
    end = len(data) if end is None else end
    out = []
    off = start & ~1
    while off + 2 <= end:
        op = struct.unpack(">H", data[off:off + 2])[0]
        pc = base + off
        text, note = decode(op, pc, data, base)
        out.append((pc, op, text, note))
        off += 2
    return out


def fmt(entries):
    lines = []
    for pc, op, text, note in entries:
        lines.append(f"{pc:08x}: {op:04x}  {text:<32s} {('; ' + note) if note else ''}")
    return "\n".join(lines)


if __name__ == "__main__":
    import sys
    path = sys.argv[1]
    base = int(sys.argv[2], 0)
    start = int(sys.argv[3], 0) - base if len(sys.argv) > 3 else 0
    end = int(sys.argv[4], 0) - base if len(sys.argv) > 4 else None
    data = open(path, "rb").read()
    print(fmt(disasm(data, base, start, end)))
