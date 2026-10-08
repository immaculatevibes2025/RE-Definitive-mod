#!/usr/bin/env python3
"""Saturn Extractor - builds every Sega Saturn extra this fork uses.

Pick your Resident Evil (Sega Saturn) disc image (.cue, .bin or .iso, or a
folder with the disc's files copied out) and your Resident Evil PC game folder (the
one holding USA\\), press Extract. Everything is converted from your own disc and
written into the game's USA folder:

    ENEMY\\em1016/em1116.emd (+ tk1016/tk1116)  Tick         (tick2pc.py)
    ENEMY\\em1017/em1117.emd                     Zombie Wesker (tick2pc.py)
    ENEMY\\em1034/em1035.emd (+ st1034/st1035)  Saturn outfits (satcostume.py)
    BATTLE\\ROOM8010-80F0.RDT                    Battle Game rooms (battle2pc.py)
    SOUND\\battle.wav, battle_rank.wav           Battle Game music (satbgm.py)
    SOUND\\TK_*.wav                              Tick sounds (ticksnd.py)
    DATA\\t_battle.tim, t_menu.tim               title menus (from the PC t_start.tim)
    DATA\\Jopt06m.tim, Opt11m.tim                Option Mode tabs (from the PC art)

Nothing from the disc is redistributed: the files are made on your machine.

Run with no arguments for the window, or from a command line:
    python saturn_extractor.py <disc .cue/.bin/.iso or folder> <PC game folder>

Needs Python 3 with numpy and Pillow (scipy is optional).
"""
import os
import shutil
import struct
import sys
import tempfile
import threading
import io
import contextlib
import runpy

# Frozen (.exe): the converter scripts are unpacked next to the program.
HERE = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

# Saturn files the conversions read, by disc path.
NEEDED = (["ENEMY/EM%s.EMD" % n for n in ("1016", "1116", "1017", "1117", "1032", "1033")]
          + ["STAGE8/ROOM80%X0.RDT" % i for i in range(1, 16)]
          + ["SND/BATTLE.CDP", "SND/SE309.CDP", "SND/SE309A.CDP"])


# --------------------------------------------------------------------------
# Disc images: ISO 9660 on 2048-byte (.iso) or raw 2352-byte (.bin) sectors.
# --------------------------------------------------------------------------
class Disc:
    def __init__(self, path):
        if path.lower().endswith(".cue"):
            path = self._bin_from_cue(path)
        self.f = open(path, "rb")
        head = self.f.read(16)
        if head[:12] == b"\x00" + b"\xff" * 10 + b"\x00":
            self.size = 2352
            self.offset = 24 if head[15] == 2 else 16   # Mode 2 / Mode 1
        else:
            self.size, self.offset = 2048, 0
        pvd = self.sector(16)
        if pvd[1:6] != b"CD001":
            raise ValueError("Not a readable disc image (no ISO 9660 volume found).")
        self.root = self._record(pvd[156:190])

    @staticmethod
    def _bin_from_cue(cue):
        folder = os.path.dirname(cue)
        for line in open(cue, "r", errors="replace"):
            line = line.strip()
            if line.upper().startswith("FILE"):
                name = line[4:].strip()
                name = name[1:name.rindex('"')] if name.startswith('"') else name.split()[0]
                return os.path.join(folder, name)
        raise ValueError("The .cue file names no .bin file.")

    def sector(self, lba, count=1):
        out = bytearray()
        for i in range(count):
            self.f.seek((lba + i) * self.size + self.offset)
            out += self.f.read(2048)
        return bytes(out)

    @staticmethod
    def _record(r):
        lba, length = struct.unpack_from("<I", r, 2)[0], struct.unpack_from("<I", r, 10)[0]
        name = r[33:33 + r[32]].decode("ascii", "replace").split(";")[0].upper()
        return dict(lba=lba, size=length, dir=bool(r[25] & 2), name=name)

    def listdir(self, d):
        data = self.sector(d["lba"], (d["size"] + 2047) // 2048)
        out, pos = {}, 0
        while pos < len(data):
            n = data[pos]
            if n == 0:                       # records never cross a sector
                pos = (pos // 2048 + 1) * 2048
                continue
            rec = self._record(data[pos:pos + n])
            if rec["name"] not in ("\x00", "\x01", ""):
                out[rec["name"]] = rec
            pos += n
        return out

    def read(self, path):
        d = self.root
        for part in path.upper().split("/"):
            d = self.listdir(d).get(part)
            if d is None:
                raise FileNotFoundError(path)
        return self.sector(d["lba"], (d["size"] + 2047) // 2048)[:d["size"]]


def find_ci(folder, *parts):
    """Case-insensitive path lookup under folder; None if missing."""
    cur = folder
    for p in parts:
        if not os.path.isdir(cur):
            return None
        hit = [n for n in os.listdir(cur) if n.lower() == p.lower()]
        if not hit:
            return None
        cur = os.path.join(cur, hit[0])
    return cur


def out_dir(usa, name):
    path = find_ci(usa, name) or os.path.join(usa, name)
    os.makedirs(path, exist_ok=True)
    return path


# --------------------------------------------------------------------------
def run(disc_path, game, log):
    if not os.path.exists(disc_path):
        raise ValueError("Cannot find the disc image: %s" % disc_path)
    if not os.path.isdir(game):
        raise ValueError("Cannot find the game folder: %s" % game)
    usa = find_ci(game, "USA") or (game if find_ci(game, "STAGE1") else None)
    if usa is None:
        raise ValueError("No USA folder found in the PC game folder.")

    work = tempfile.mkdtemp(prefix="re1sat_")
    try:
        log("Reading the Saturn disc...")
        sat = os.path.join(work, "saturn")
        if os.path.isdir(disc_path):
            for p in NEEDED:
                src = find_ci(disc_path, *p.split("/"))
                if src is None:
                    raise FileNotFoundError(p)
                os.makedirs(os.path.join(sat, os.path.dirname(p)), exist_ok=True)
                shutil.copyfile(src, os.path.join(sat, p))
        else:
            disc = Disc(disc_path)
            for p in NEEDED:
                os.makedirs(os.path.join(sat, os.path.dirname(p)), exist_ok=True)
                open(os.path.join(sat, p), "wb").write(disc.read(p))
        log("  found all %d files." % len(NEEDED))

        def tool(label, script, *args):
            # Runs a converter in-process (works the same from the .exe).
            if label:
                log(label)
            out = io.StringIO()
            old = sys.argv
            sys.argv = [script] + list(args)
            try:
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                    runpy.run_path(os.path.join(HERE, script), run_name="__main__")
            except SystemExit as e:
                if e.code not in (None, 0):
                    raise RuntimeError("%s failed:\n%s" % (script, out.getvalue()[-1500:]))
            except Exception as e:
                raise RuntimeError("%s failed: %s\n%s" % (script, e, out.getvalue()[-1500:]))
            finally:
                sys.argv = old

        tmp = os.path.join(work, "out")
        enemy, sound, data = (out_dir(usa, n) for n in ("ENEMY", "SOUND", "DATA"))

        tool("Converting the Tick and Zombie Wesker models...", "tick2pc.py",
             os.path.join(sat, "ENEMY"), tmp)
        for n in ("em1016.emd", "em1116.emd", "em1017.emd", "em1117.emd"):
            shutil.copyfile(os.path.join(tmp, n), os.path.join(enemy, n))
        for n in ("1016", "1116"):              # Director's Cut mode reads these names
            shutil.copyfile(os.path.join(tmp, "em%s.emd" % n), os.path.join(enemy, "tk%s.emd" % n))

        tool("Converting the Saturn outfits...", "satcostume.py", os.path.join(sat, "ENEMY"), tmp)
        for n in ("1034", "1035"):
            shutil.copyfile(os.path.join(tmp, "em%s.emd" % n), os.path.join(enemy, "em%s.emd" % n))
            shutil.copyfile(os.path.join(tmp, "em%s.emd" % n), os.path.join(enemy, "st%s.emd" % n))

        tool("Building the Battle Game rooms...", "battle2pc.py",
             os.path.join(sat, "STAGE8"), usa, out_dir(usa, "BATTLE"))

        cdp = os.path.join(sat, "SND", "BATTLE.CDP")
        tool("Rendering the Battle Game music (this takes a minute)...", "satbgm.py",
             cdp, os.path.join(sound, "battle.wav"))
        tool("Rendering the results jingle...", "satbgm.py",
             cdp, os.path.join(sound, "battle_rank.wav"), "1")

        tool("Extracting the Tick sounds...", "ticksnd.py",
             os.path.join(sat, "SND", "SE309A.CDP"), sound, os.path.join(sat, "SND", "SE309.CDP"))

        t_start = find_ci(data, "t_start.tim")
        if t_start:
            tool("Building the title menus...", "battle_title.py", t_start,
                 os.path.join(data, "t_battle.tim"))
            tool("", "title_menu.py", t_start, os.path.join(data, "t_menu.tim"))
        else:
            log("  (no DATA\\t_start.tim - title menus skipped)")
        for src, dst in (("JOPT06.TIM", "Jopt06m.tim"), ("OPT11.TIM", "Opt11m.tim")):
            path = find_ci(data, src)
            if path:
                tool("Building the Option Mode tabs..." if src == "JOPT06.TIM" else "",
                     "option_tabs.py", path, os.path.join(data, dst))
        log("\nDone! Start the game and turn the extras on in Option Mode or config.ini.")
    finally:
        shutil.rmtree(work, ignore_errors=True)


# --------------------------------------------------------------------------
def gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox

    root = tk.Tk()
    root.title("RE1 Saturn Extractor")
    root.resizable(False, False)
    disc, game = tk.StringVar(), tk.StringVar()

    def pick_disc():
        p = filedialog.askopenfilename(title="Resident Evil (Saturn) disc image",
                                       filetypes=[("Disc image", "*.cue *.bin *.iso"), ("All files", "*.*")])
        if p:
            disc.set(p)

    def pick_game():
        p = filedialog.askdirectory(title="Resident Evil PC game folder (the one with USA inside)")
        if p:
            game.set(p)

    tk.Label(root, text="Saturn disc (.cue / .bin / .iso):").grid(row=0, column=0, sticky="w", padx=8, pady=(10, 0))
    tk.Entry(root, textvariable=disc, width=52).grid(row=1, column=0, padx=8)
    tk.Button(root, text="Browse...", command=pick_disc).grid(row=1, column=1, padx=(0, 8))
    tk.Label(root, text="Resident Evil PC game folder:").grid(row=2, column=0, sticky="w", padx=8, pady=(8, 0))
    tk.Entry(root, textvariable=game, width=52).grid(row=3, column=0, padx=8)
    tk.Button(root, text="Browse...", command=pick_game).grid(row=3, column=1, padx=(0, 8))

    go = tk.Button(root, text="Extract", width=16)
    go.grid(row=4, column=0, columnspan=2, pady=10)
    box = tk.Text(root, width=64, height=14, state="disabled")
    box.grid(row=5, column=0, columnspan=2, padx=8, pady=(0, 10))

    def log(msg):
        if not msg:
            return
        def add():
            box.configure(state="normal")
            box.insert("end", msg + "\n")
            box.see("end")
            box.configure(state="disabled")
        root.after(0, add)

    def work():
        try:
            run(disc.get(), game.get(), log)
            root.after(0, lambda: messagebox.showinfo("Saturn Extractor", "All done!"))
        except Exception as e:
            err = str(e) if not isinstance(e, FileNotFoundError) else \
                "This disc is missing %s - is it Resident Evil for the Saturn?" % e
            log("\nERROR: " + err)
            root.after(0, lambda: messagebox.showerror("Saturn Extractor", err))
        finally:
            root.after(0, lambda: go.configure(state="normal"))

    def start():
        if not disc.get() or not game.get():
            messagebox.showwarning("Saturn Extractor", "Pick the disc image and the game folder first.")
            return
        go.configure(state="disabled")
        box.configure(state="normal")
        box.delete("1.0", "end")
        box.configure(state="disabled")
        threading.Thread(target=work, daemon=True).start()

    go.configure(command=start)
    root.mainloop()


def _console_stdout():
    """The windowed .exe starts with no sys.stdout; when a parent program
    (the asset migrator's Saturn tab) passes a pipe, write to that."""
    if sys.stdout is not None or os.name != "nt":
        return
    try:
        import ctypes
        import msvcrt
        h = ctypes.windll.kernel32.GetStdHandle(-11)      # STD_OUTPUT_HANDLE
        if h not in (0, -1, None):
            fd = msvcrt.open_osfhandle(h, os.O_WRONLY | os.O_TEXT)
            sys.stdout = sys.stderr = open(fd, "w", buffering=1, encoding="utf-8")
    except Exception:
        pass


def cli(disc, game):
    _console_stdout()

    def log(msg):
        if msg and sys.stdout is not None:
            print(msg, flush=True)
    try:
        run(disc, game, log)
    except FileNotFoundError as e:
        log("ERROR: this disc is missing %s - is it Resident Evil for the Saturn?" % e)
        sys.exit(1)
    except Exception as e:
        log("ERROR: %s" % e)
        sys.exit(1)


if __name__ == "__main__":
    if len(sys.argv) == 3:
        cli(sys.argv[1], sys.argv[2])
    else:
        gui()
