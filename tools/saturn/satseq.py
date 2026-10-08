"""Sega Saturn sound driver sequence (.SEQ inside a CD_PACK) parser.

Bank: u16 song count, u32 song offsets. Song: u16 resolution, u16 tempo
event count, u16 data offset, u16 tempo loop offset, tempo events
{u32 step, u32 us per quarter}, then one merged track:
  0x00-0x7F  note: ctl(ch | ext bits), key, velocity, gate, step
             ctl bit 5 (0x20) adds 256 to gate, bit 6 (0x40) adds 256 to step
  0x81       reference: u16 offset (from song data start), u8 event count
  0x82       loop start marker (1 data byte)
  0x83       end of track
  0x88-0x8B  extend gate by 0x200 / 0x800 / 0x1000 / 0x2000
  0x8C-0x8F  extend step by the same
  0xB0-0xBF  control change: ctrl, value, step
  0xC0-0xCF  program change: program, step
  0xD0-0xDF  channel pressure: value, step
  0xE0-0xEF  pitch bend: value (7 bit, 0x40 centre), step
"""
import struct

EXT = [0x200, 0x800, 0x1000, 0x2000]


def songs(seq):
    n = struct.unpack(">H", seq[:2])[0]
    return list(struct.unpack(">%dI" % n, seq[2:2 + 4 * n]))


def parse_song(seq, off):
    res, ntempo, dofs, tloop = struct.unpack(">4H", seq[off:off + 8])
    tempos = [struct.unpack(">2I", seq[off + 8 + 8 * i: off + 16 + 8 * i]) for i in range(ntempo)]
    base = off + dofs
    events = []          # (tick, kind, ch, a, b, gate)
    loop_tick = None
    state = {"tick": 0}

    def run(pos, limit):
        count = 0
        gate_ext = 0
        step_ext = 0
        while True:
            if limit is not None and count >= limit:
                return pos, False
            c = seq[pos]
            if c < 0x80:
                key, vel, gate, step = seq[pos + 1:pos + 5]
                if c & 0x20: gate += 0x100
                if c & 0x40: step += 0x100
                gate += gate_ext; step += step_ext
                gate_ext = step_ext = 0
                events.append((state["tick"], "note", c & 0x0F, key, vel, gate))
                state["tick"] += step
                pos += 5
            elif c == 0x81:
                ref, cnt = struct.unpack(">HB", seq[pos + 1:pos + 4])
                run(base + ref, cnt)
                pos += 4
            elif c == 0x82:
                if nonlocal_loop[0] is None:
                    nonlocal_loop[0] = state["tick"]
                pos += 2
            elif c == 0x83:
                return pos, True
            elif 0x88 <= c <= 0x8B:
                gate_ext += EXT[c - 0x88]; pos += 1
                continue
            elif 0x8C <= c <= 0x8F:
                step_ext += EXT[c - 0x8C]; pos += 1
                continue
            elif 0xB0 <= c <= 0xBF:
                ctrl, val, step = seq[pos + 1:pos + 4]
                step += step_ext; step_ext = 0
                events.append((state["tick"], "cc", c & 0xF, ctrl, val, 0))
                state["tick"] += step
                pos += 4
            elif 0xC0 <= c <= 0xCF:
                prog, step = seq[pos + 1:pos + 3]
                step += step_ext; step_ext = 0
                events.append((state["tick"], "prog", c & 0xF, prog, 0, 0))
                state["tick"] += step
                pos += 3
            elif 0xD0 <= c <= 0xDF:
                val, step = seq[pos + 1:pos + 3]
                step += step_ext; step_ext = 0
                state["tick"] += step
                pos += 3
            elif 0xE0 <= c <= 0xEF:
                val, step = seq[pos + 1:pos + 3]
                step += step_ext; step_ext = 0
                events.append((state["tick"], "bend", c & 0xF, val, 0, 0))
                state["tick"] += step
                pos += 3
            else:
                raise ValueError("unknown event 0x%02X at 0x%X" % (c, pos))
            count += 1

    nonlocal_loop = [None]
    run(base, None)
    return dict(res=res, tempos=tempos, events=events, end=state["tick"],
                loop=nonlocal_loop[0])
