# Resident Evil (Saturn) model tools

Python 3 tools for the Saturn release's `ENEMY/EM####.EMD` files.
Needs: `numpy`, `Pillow` (only for `render.py`).

| File | What it does |
|---|---|
| `satlz.py` | LZ decompressor, reimplemented from the SH-2 routine at `0x0602B03C` in `GAME1.PRG` (overlays load at `0x0602B000`). |
| `satmodel.py` | Parses a Saturn EMD: splits the 4 sections, decompresses the SGL model and texture data, reads parts, polygons, attributes, joint tree, texture table and palettes. |
| `render.py` | Rest-pose preview: `python3 render.py EM1016.EMD tick.png` |
| `sh2.py` | Small SH-2 disassembler used for the reverse engineering. |

## Saturn EMD layout (big-endian)

| Section | Contents |
|---|---|
| A | The PS1/PC EMD file, byte-identical to the PC one (skeleton, animations, PS1 mesh). |
| B | LZ-compressed SGL model: header, 16 XPDATA parts in joint order, joint tree (0x30-byte nodes), texture table. |
| C | LZ-compressed 4bpp VDP1 texture pixels, one texture per polygon. |
| D | Raw colour lookup tables, 16 x RGB555 each; a polygon's table is `colno >> 2`. |

`EM1016.EMD` / `EM1116.EMD` = Tick (Chris / Jill scenario). Same 16-joint skeleton as the Hunter (`EM1006`).

## Converting the Tick to a PC EMD

    python3 tick2pc.py <path to Saturn ENEMY folder> <output folder>

Writes `em1016.emd` and `em1116.emd`. Put them in `USA/ENEMY` and set `[Mods] Ticks=1`, or rename them to `em1006.emd` / `em1106.emd` to replace the Hunter outright.
Everything before the mesh (skeleton, animations) is the Hunter's, taken from section A of
the Saturn file. The mesh (TMD) and texture (TIM) are rebuilt from the Saturn model:
quads split into mode-0x34 triangles, vertices x4, normals recomputed, and the 374
per-polygon textures packed (at ~47% scale) into one 256x256 8bpp page with a 256-colour
CLUT, the same shape as the Hunter's.

Check an output: `python3 verify_pc.py out/em1006.emd <PC em1006.emd> EM1016.EMD check.png`

## The Tick's AI ([Mods] Ticks=1)

`src/game/entities/Hunter.cpp` `tick_variant_2_ai` ports the Saturn Tick's decision routine
(`GAME2.PRG` 0x0606d484). Compared with the Hunter it decapitates at close range: the
player must be under 1/3 of max health, within 4000 units, faced within 0x20, with no wall
ahead; it always rolls the 8-in-16 chance table; the close slash triggers under 2000.
The leap reuses the Hunter's pounce, aimed at and tracking the player. Not yet ported: the
Tick's own six-state leap routine (`GAME2.PRG` 0x0606df24) and its variant-0 (pack) AI.

## Tick sounds

    python3 ticksnd.py <Saturn SND/SE309A.CDP> <output folder>

Writes `TK_walkA/walkB/jump/att/land/smash/dam/Nout.wav` (16-bit mono, 22050 Hz) for
`USA/SOUND`. With `[Mods] Ticks=1` the game loads them in place of the Hunter's `HU_*`
(mansion) and `He_*` (underground) slots. `satsnd.py` reads the Saturn `.CDP` room banks
(CD_PACK container, room slot table, tone bank, sequences). Slot 7 (`Nout`) is a
zero-source voice on the Saturn, so `TK_Nout.wav` is silence.

## Where Ticks appear

As on the Saturn, only in `STAGE3` (courtyard / underground: ROOM3080-30B0, the Hunter
rooms after the courtyard tunnels). The Saturn has no code that swaps a Hunter for a Tick;
each room's data places one or the other, so the mansion-revisit Hunters (stages 6/7) stay
Hunters. In the decomp this is `mod_ticks_active()` in `Globals.h`, used by the model loader,
the AI and the sound loader.

## Zombie Wesker

`EM1017.EMD` / `EM1117.EMD` are Zombie Wesker, who only appears in the Battle Game. Section A of
the Saturn file is the white-coat zombie's EMD (em1000), so `tick2pc.py` builds him exactly like
the Tick (the part count is read from the Saturn model header: 15 here, 16 for the Tick) and writes
`em1017.emd` / `em1117.emd` for `USA/ENEMY`.

## The Battle Game

The Saturn release's Battle Game, on the title menu once the game has been finished (or with
`[BattleGame] AlwaysUnlocked=1`). Code: `src/game/BattleGame.cpp`, which lists every Saturn
address it was taken from.

    python3 battle2pc.py <Saturn STAGE8 folder> <PC USA folder> <USA/BATTLE>
    python3 battle_title.py <USA/DATA/t_start.tim> <USA/DATA/t_battle.tim>

* `battle2pc.py` - the fifteen rooms. Each Saturn `STAGE8/ROOM80x0.RDT` (LZ-compressed, Saturn
  models) is a copy of a main-game room with new scripts; the script sections (init, per-frame,
  events - the same bytecode on both machines) are grafted onto the PC room, whose cameras it
  checks against the Saturn file first. The room list is the Saturn's own table
  (`GAME5.PRG` 0x06099334).
* `battle_title.py` - the three-option title menu, NEW GAME / LOAD GAME / BATTLE GAME, built from
  the PC sheet's own lettering.

| Room | Copy of | Enemies |
|---|---|---|
| 1 | ROOM1000 | safe room (item box with the loadout) |
| 2 | ROOM1030 | zombies |
| 3 | ROOM3020 | Cerberus |
| 4 | ROOM1170 | crows |
| 5 | ROOM2100 | Yawn |
| 6 | ROOM1000 | safe room |
| 7 | ROOM4040 | Web Spinners |
| 8 | ROOM40C0 | Plant 42 |
| 9 | ROOM3090 | Hunters (Ticks with `[Mods] Ticks=1`, as it is a STAGE3 room) |
| 10 | ROOM30C0 | Black Tiger |
| 11 | ROOM30E0 | safe room |
| 12 | ROOM5090 | zombies, the first one Zombie Wesker |
| 13 | ROOM50F0 | Chimeras |
| 14 | ROOM5100 | Chimeras |
| 15 | ROOM5130 | the (Golden) Tyrant |

Score (`RANKING.PRG` 0x0602c3ac): remaining rounds x weight (handgun 2, shotgun 6, magnum 10,
bazooka 15 - Jill only) + 150 per first aid spray + life % + the seconds left under the par time
(Chris 12:00, Jill 7:00) + 50 per whole minute of them. Rank = score / 100: J (0-99) up to A
(900-999), S from 1000.

## Battle Game music

    python3 satbgm.py <Saturn SND/BATTLE.CDP> battle.wav

`BATTLE.CDP` is a CD_PACK holding an SCSP tone bank (entry 0) and a sound-driver sequence bank
(entry 1, two songs; `satseq.py` parses the format). Song 0 is the Battle Game's looping track
(76 BPM, 35.6 s); song 1 is a 20.8 s jingle that does not loop. `satbgm.py` renders a song through
an approximation of the SCSP (tone-bank PCM, loops, envelope, levels, pan) to 44.1 kHz stereo,
one seamless loop: put the output in `USA/SOUND/battle.wav`. During the Battle Game,
`update_room_bgm` (`SoundSystem.cpp`) plays it in place of the rooms' own music and keeps it running
across rooms; it stops at the results screen. Without the file the rooms keep their normal music.

Render the jingle with `python3 satbgm.py BATTLE.CDP jingle.wav 1`.
