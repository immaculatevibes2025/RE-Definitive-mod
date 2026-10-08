"""Resident Evil (Sega Saturn) room sound banks: /SND/SE<stage><room>[A|B].CDP

CD_PACK container ("CD_PACK Ver.1.0"): from 0x10, 16-byte entries
{u32 id, u32 file offset, u32 size, u32 load address}, terminated by 0xFFFFFFFF.
  id 0 -> main RAM 0x002E8430: room sound table, 8-byte records
          {u8 kind, u8 slot, u8 bank, u8 voice, u16 1, u16 0}, ends with 0xFFFF.
          `slot` is the PC per-room slot (g_RoomSndData column), `voice` the
          tone-bank voice that plays it.
  id 1 -> SCSP RAM 0x25A2F840: tone bank (Sega sound driver .TON layout)
  id 2 -> SCSP RAM 0x25A54000: per-sound sequence data

Tone bank: u16 offsets {mixer, velocity, PEG, PLFO, voice[0..n-1]}. A voice is
a 4-byte header (byte 2 = layer count - 1) followed by 0x20-byte layers:
  +0 start note, +1 end note, +2 u16 flags (bit 4 = 8-bit PCM, bits 0-3 = SA
  bits 16-19), +4 u16 SA (byte offset from the bank start), +6 u16 LSA,
  +8 u16 LEA (sample count), ... +0x19 base note, +0x18 signed fine tune.
PCM is big-endian signed 16-bit (or signed 8-bit).
"""
import struct


def cdpack(path):
    d = open(path, "rb").read()
    if not d.startswith(b"CD_PACK"):
        raise ValueError("not a CD_PACK")
    ents = {}
    o = 0x10
    while struct.unpack(">I", d[o:o + 4])[0] != 0xFFFFFFFF:
        i, off, size, addr = struct.unpack(">4I", d[o:o + 16])
        ents[i] = (d[off:off + size], addr)
        o += 16
    return ents


def room_table(blk):
    out = []
    for i in range(0, len(blk) - 7, 8):
        if blk[i:i + 2] == b"\xff\xff":
            break
        kind, slot, bank, voice = blk[i:i + 4]
        out.append(dict(kind=kind, slot=slot, bank=bank, voice=voice))
    return out


def tone_bank(blk):
    h = struct.unpack(">4H", blk[:8])
    first_voice = struct.unpack(">H", blk[8:10])[0]
    nvoices = (first_voice - 8) // 2
    voffs = struct.unpack(">%dH" % nvoices, blk[8:8 + 2 * nvoices])
    voices = []
    for vo in voffs:
        nl = (blk[vo + 2] if blk[vo + 2] < 0x80 else 0) + 1
        layers = []
        for k in range(nl):
            L = blk[vo + 4 + k * 0x20: vo + 4 + (k + 1) * 0x20]
            flags = struct.unpack(">H", L[2:4])[0]
            sa = ((flags & 0xF) << 16) | struct.unpack(">H", L[4:6])[0]
            lsa, lea = struct.unpack(">2H", L[6:10])
            pcm8 = bool(flags & 0x10)
            fine = L[0x18] - 256 if L[0x18] >= 128 else L[0x18]
            layers.append(dict(lo=L[0], hi=L[1], flags=flags, sa=sa, lsa=lsa, lea=lea,
                               pcm8=pcm8, base=L[0x19], fine=fine, raw=L))
        voices.append(dict(offset=vo, layers=layers))
    return voices


def layer_pcm(blk, layer):
    """Signed 16-bit samples (list) for a layer."""
    if layer["pcm8"]:
        raw = blk[layer["sa"]:layer["sa"] + layer["lea"]]
        return [(b - 256 if b >= 128 else b) * 256 for b in raw]
    raw = blk[layer["sa"]:layer["sa"] + layer["lea"] * 2]
    return list(struct.unpack(">%dh" % (len(raw) // 2), raw))
