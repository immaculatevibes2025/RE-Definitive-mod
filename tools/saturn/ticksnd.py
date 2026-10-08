#!/usr/bin/env python3
"""Extract the Sega Saturn Tick's sound effects as PC-ready WAVs.

The Saturn plays the Tick in the underground rooms, whose sound bank is
/SND/SE309A.CDP (stage 3 room 09). Its room table assigns PC sound slots 0-7 -
the slots the PC fills with He_walkA..He_Nout there and HU_walkA..HU_Nout in
the mansion - to tone-bank sequences; every sequence plays its own program at
note 60, so a voice's playback rate is 44100 * 2^((60 - base note) / 12).

Writes TK_walkA.wav ... TK_Nout.wav (16-bit mono, 22050 Hz). Slot 7 (Nout) is
a zero-source voice on the Saturn (SSCTL = 2), i.e. silent, so TK_Nout.wav is
a short silence.

Usage: python3 ticksnd.py <SE309A.CDP> <output dir> [SE309.CDP]

With SE309.CDP (the cave rooms' bank) it also writes TK_roar.wav: those
rooms put the Tick's roar in slot 7, which SE309A leaves silent.
Needs: numpy, scipy.
"""
import os
import struct
import sys
from fractions import Fraction

import numpy as np
try:
    from scipy.signal import resample_poly
except ImportError:            # numpy-only fallback (FFT resample)
    def resample_poly(x, up, down):
        x = np.asarray(x, dtype=np.float64)
        n = int(round(len(x) * up / down))
        return np.fft.irfft(np.fft.rfft(x), n) * (n / len(x)) if len(x) else x

from satsnd import cdpack, room_table, tone_bank, layer_pcm

SLOT_NAMES = ["walkA", "walkB", "jump", "att", "land", "smash", "dam", "Nout"]
OUT_RATE = 22050
EXTRA_VOICES = [(0, "cry"), (1, "x1"), (10, "x10"), (11, "x11")]


def write_wav(path, samples, rate):
    data = np.clip(np.round(samples), -32768, 32767).astype("<i2").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def extract_roar(cdp_path, out_dir):
    """TK_roar.wav: slot 7 of SE309.CDP (silent in SE309A)."""
    os.makedirs(out_dir, exist_ok=True)
    ents = cdpack(cdp_path)
    table = {e["slot"]: e for e in room_table(ents[0][0]) if e["kind"] == 8}
    bank = ents[1][0]
    layer = tone_bank(bank)[table[7]["voice"]]["layers"][0]
    src = np.array(layer_pcm(bank, layer), dtype=np.float64)
    rate = 44100 * 2 ** ((60 - layer["base"]) / 12)
    fr = Fraction(OUT_RATE / rate).limit_denominator(1000)
    out = resample_poly(src, fr.numerator, fr.denominator)
    write_wav(os.path.join(out_dir, "TK_roar.wav"), out, OUT_RATE)
    return len(out) / OUT_RATE


def extract(cdp_path, out_dir):
    ents = cdpack(cdp_path)
    table = {e["slot"]: e for e in room_table(ents[0][0]) if e["kind"] == 8}
    bank = ents[1][0]
    voices = tone_bank(bank)
    os.makedirs(out_dir, exist_ok=True)
    report = []
    # Voices the room table gives no PC slot, played by the Saturn's Tick code
    # directly. Voice 0 is its scream as it kills (matched against Saturn
    # footage); the others are kept for reference.
    for vi, name in EXTRA_VOICES:
        layer = voices[vi]["layers"][0]
        src = np.array(layer_pcm(bank, layer), dtype=np.float64)
        rate = 44100 * 2 ** ((60 - layer["base"]) / 12)
        fr = Fraction(OUT_RATE / rate).limit_denominator(1000)
        write_wav(os.path.join(out_dir, f"TK_{name}.wav"),
                  resample_poly(src, fr.numerator, fr.denominator), OUT_RATE)
    for slot, name in enumerate(SLOT_NAMES):
        e = table[slot]
        layer = voices[e["voice"]]["layers"][0]
        ssctl = (layer["flags"] >> 7) & 3
        path = os.path.join(out_dir, f"TK_{name}.wav")
        if ssctl != 0:
            write_wav(path, np.zeros(OUT_RATE // 10), OUT_RATE)
            report.append((name, e["voice"], None, 0.1, "silent on the Saturn"))
            continue
        src = np.array(layer_pcm(bank, layer), dtype=np.float64)
        rate = 44100 * 2 ** ((60 - layer["base"]) / 12)
        fr = Fraction(OUT_RATE / rate).limit_denominator(1000)
        out = resample_poly(src, fr.numerator, fr.denominator)
        write_wav(path, out, OUT_RATE)
        report.append((name, e["voice"], round(rate), len(out) / OUT_RATE,
                       "8-bit" if layer["pcm8"] else "16-bit"))
    return report


if __name__ == "__main__":
    if len(sys.argv) > 3:
        print(f"TK_roar  {extract_roar(sys.argv[3], sys.argv[2]):5.2f}s  (SE309 slot 7)")
    for name, voice, rate, secs, note in extract(sys.argv[1], sys.argv[2]):
        print(f"TK_{name:6s} voice {voice:2d}  Saturn rate {rate or '-':>5}  {secs:5.2f}s  {note}")
