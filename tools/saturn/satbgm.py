#!/usr/bin/env python3
"""Render Saturn Resident Evil sequenced music (.CDP: CD_PACK with an SCSP tone
bank and a Sega sound-driver sequence bank) to a 16-bit stereo WAV.

    python3 satbgm.py BATTLE.CDP battle.wav [song] [loops]

This is an approximation of the Saturn's SCSP, not an emulation: each note
plays its tone-bank layer's PCM (linear interpolation, the layer's loop),
pitched from the layer's base note, with the SCSP envelope (AR / D1R / DL /
D2R / RR, worked in dB), total level, direct-send level and pan, the
velocity and the channel's volume / pan controllers.

Tone bank (big-endian): u16 offsets {mixer, velocity, PEG, PLFO, voice...};
the voice table runs up to the first section (the mixer). A voice is a
4-byte header (byte 2 = layer count - 1) and 0x20-byte layers:
  +0 lowest key, +1 highest key, +2.. SCSP slot registers 0x00-0x15:
     +2 reg00 (LPCTL bits 6-5, PCM8B bit 4, SA bits 19-16), +4 SA, +6 LSA,
     +8 LEA, +A reg08 (D2R, D1R, EGHOLD, AR), +C reg0A (KRS, DL, RR),
     +E reg0C (TL in the low byte)
  +0x18 DISDL (bits 7-5) / DIPAN (bits 4-0), +0x19 base key,
  +0x1A fine tune (signed, 1/128 semitone)
"""
import struct
import sys
import wave

import numpy as np

import satseq

RATE = 44100


def cdpack(d):
    ents = {}
    o = 0x10
    while struct.unpack(">I", d[o:o + 4])[0] != 0xFFFFFFFF:
        i, off, size, addr = struct.unpack(">4I", d[o:o + 16])
        ents[i] = (d[off:off + size], addr)
        o += 16
    return ents


def tone_bank(ton):
    sections = struct.unpack(">4H", ton[:8])
    first = min(sections)
    nvoices = (first - 8) // 2
    voffs = struct.unpack(">%dH" % nvoices, ton[8:8 + 2 * nvoices])
    voices = []
    for vo in voffs:
        nl = ton[vo + 2] + 1
        layers = []
        for k in range(nl):
            L = ton[vo + 4 + k * 0x20: vo + 4 + (k + 1) * 0x20]
            r00, sa, lsa, lea, r08, r0a, r0c = struct.unpack(">7H", L[2:16])
            sa |= (r00 & 0xF) << 16
            pcm8 = bool(r00 & 0x10)
            if pcm8:
                raw = np.frombuffer(ton[sa:sa + lea + 1], dtype=np.int8).astype(np.float32) / 128.0
            else:
                raw = np.frombuffer(ton[sa:sa + (lea + 1) * 2], dtype=">i2").astype(np.float32) / 32768.0
            fine = L[0x1A] - 256 if L[0x1A] >= 128 else L[0x1A]
            layers.append(dict(
                lo=L[0], hi=L[1], pcm=raw, lpctl=(r00 >> 5) & 3,
                lsa=lsa, lea=lea,
                ar=r08 & 0x1F, d1r=(r08 >> 6) & 0x1F, d2r=(r08 >> 11) & 0x1F,
                dl=(r0a >> 5) & 0x1F, rr=r0a & 0x1F, tl=r0c & 0xFF,
                disdl=L[0x18] >> 5, dipan=L[0x18] & 0x1F,
                base=L[0x19], fine=fine))
        voices.append(layers)
    return voices


def rate_time(r, full):
    """Seconds an SCSP EG rate takes over its whole range (approximation:
    rate 31 is instant, each 2 steps down doubles the time)."""
    if r <= 0:
        return None
    if r >= 31:
        return 0.0
    return full / (2.0 ** ((r - 1) / 2.0))


def pan_gains(dipan, cc_pan):
    # SCSP DIPAN: 0x00-0x0F attenuate the right side, 0x10-0x1F the left,
    # 3 dB per step. Then the channel's MIDI pan on top.
    att = (dipan & 0xF) * 3.0
    l = r = 1.0
    if dipan & 0x10:
        l = 10 ** (-att / 20)
    else:
        r = 10 ** (-att / 20)
    p = cc_pan / 127.0
    return l * np.cos(p * np.pi / 2) * 1.4142, r * np.sin(p * np.pi / 2) * 1.4142


def render_note(layer, key, vel, dur, bend=0.0):
    """Mono float32 samples for one note held for dur seconds (plus release)."""
    pcm = layer["pcm"]
    if len(pcm) < 2:
        return None
    semis = key - layer["base"] + layer["fine"] / 128.0 + bend
    step = 2.0 ** (semis / 12.0)

    # Envelope in dB of attenuation (0 = full), 96 dB range.
    t_att = rate_time(layer["ar"], 8.1)
    t_d1 = rate_time(layer["d1r"], 118.2)
    t_d2 = rate_time(layer["d2r"], 118.2)
    t_rr = rate_time(layer["rr"], 118.2)
    dl_db = layer["dl"] * 3.0
    rel = t_rr if t_rr is not None else 10.0
    rel = min(rel, 4.0)
    total = dur + rel
    n = int(total * RATE)
    if n <= 0:
        return None
    t = np.arange(n, dtype=np.float32) / RATE

    att = np.zeros(n, dtype=np.float32)
    # decay 1 to DL, then decay 2
    if t_d1 is not None:
        r1 = 96.0 / max(t_d1, 1e-4)
        att = np.minimum(t * r1, dl_db)
        t_reach = dl_db / r1
        if t_d2 is not None:
            r2 = 96.0 / max(t_d2, 1e-4)
            att = np.where(t > t_reach, dl_db + (t - t_reach) * r2, att)
    # release after the gate
    hold_n = min(int(dur * RATE), n)
    if hold_n < n:
        a0 = att[hold_n - 1] if hold_n > 0 else 0.0
        rr = 96.0 / max(rel, 1e-4)
        att[hold_n:] = a0 + (t[hold_n:] - dur) * rr
    amp = 10 ** (-np.minimum(att, 96.0) / 20.0)
    if t_att:
        amp *= np.minimum(t / t_att, 1.0)

    # sample positions, with the loop
    pos = np.arange(n, dtype=np.float64) * step
    end = layer["lea"]
    if layer["lpctl"] and layer["lea"] > layer["lsa"]:
        lsa, ln = layer["lsa"], layer["lea"] - layer["lsa"]
        over = pos >= lsa
        pos[over] = lsa + np.mod(pos[over] - lsa, ln)
    else:
        keep = pos < end - 1
        if not keep.any():
            return None
        last = int(np.nonzero(keep)[0][-1]) + 1
        pos, amp = pos[:last], amp[:last]
    i0 = np.floor(pos).astype(np.int64)
    i0 = np.clip(i0, 0, len(pcm) - 2)
    frac = (pos - i0).astype(np.float32)
    s = pcm[i0] * (1 - frac) + pcm[i0 + 1] * frac

    level = 10 ** (-(layer["tl"] * 0.375) / 20.0)
    level *= (10 ** (-(7 - layer["disdl"]) * 6.0 / 20.0)) if layer["disdl"] else 0.0
    level *= (vel / 127.0) ** 2
    return (s * amp * level).astype(np.float32)


def render(cdp_path, song_index=0, loops=1, tail=0.0):
    d = open(cdp_path, "rb").read()
    ents = cdpack(d)
    ton = ents[0][0]
    seq = ents[1][0]
    voices = tone_bank(ton)
    song = satseq.parse_song(seq, satseq.songs(seq)[song_index])
    us = song["tempos"][0][1]
    sec_per_tick = us / 1e6 / song["res"]
    length = song["end"] * sec_per_tick

    out = np.zeros((int((length * loops + 5) * RATE), 2), dtype=np.float32)
    for lp in range(loops):
        offset = lp * length
        prog = {}
        vol = {}
        pan = {}
        for tick, kind, ch, a, b, gate in song["events"]:
            if kind == "prog":
                prog[ch] = a
            elif kind == "cc":
                if a == 7:
                    vol[ch] = b
                elif a == 10:
                    pan[ch] = b
            elif kind == "note":
                v = prog.get(ch, 0)
                if v >= len(voices):
                    continue
                start = offset + tick * sec_per_tick
                dur = max(gate, 1) * sec_per_tick
                cv = vol.get(ch, 127) / 127.0
                cp = pan.get(ch, 64)
                for layer in voices[v]:
                    if not (layer["lo"] <= a <= layer["hi"]):
                        continue
                    s = render_note(layer, a, b, dur)
                    if s is None:
                        continue
                    gl, gr = pan_gains(layer["dipan"], cp)
                    i = int(start * RATE)
                    j = min(i + len(s), len(out))
                    out[i:j, 0] += s[:j - i] * gl * cv * cv
                    out[i:j, 1] += s[:j - i] * gr * cv * cv
    end = min(int((length * loops + tail) * RATE), len(out))
    return out[:end], length


def write_wav(path, data, peak=0.89):
    m = float(np.max(np.abs(data))) or 1.0
    pcm = np.clip(data * (peak / m), -1, 1)
    pcm = (pcm * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    song = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    d = open(sys.argv[1], "rb").read()
    seq = cdpack(d)[1][0]
    parsed = satseq.parse_song(seq, satseq.songs(seq)[song])
    if parsed["loop"] is not None:
        # Looping song: render two passes and keep the second, so the notes
        # still ringing at the end of a pass are heard at the start of the
        # next one - the file then loops seamlessly from sample 0.
        data, length = render(sys.argv[1], song, 2)
        n = int(round(length * RATE))
        write_wav(sys.argv[2], data[n:2 * n])
        print("%s: song %d, %.2f s seamless loop" % (sys.argv[2], song, length))
    else:
        # One-shot (a jingle): one pass with its release tail.
        data, length = render(sys.argv[1], song, 1, tail=2.5)
        write_wav(sys.argv[2], data)
        print("%s: song %d, %.2f s, plays once" % (sys.argv[2], song, len(data) / RATE))
