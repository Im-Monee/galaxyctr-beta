"""GalaxyCTR builder: everything the New 3DS port needs, made from the user's own game disc.

Run it with no arguments and answer its questions. It:
  1. asks for a disc image of Super Mario Galaxy (USA, RMGE01) and checks it is that game;
  2. extracts the disc's data partition (nodtool);
  3. checks main.dol is the original one (SHA-1);
  4. converts the game files for the 3DS (tools/3ds/convert_smg_files.py, the same pipeline the
     developers use);
  5. builds the CIA: the port's program plus the HOME Menu icon and banner taken from the disc;
  6. says the build is complete and asks whether to copy now, over FTP to the 3DS or to the SD card in
     this computer, without touching the saves; copied later, the steps are saved in install_guide.txt;
  7. deletes its temporary files (unless the user keeps them, settings.txt) and says whether everything
     went well.

Nothing in this program or next to it comes from the game: the disc provides all game data.
Everything the user sees is in English.
"""

import hashlib
import multiprocessing
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import wave
import zlib
from pathlib import Path


# ############################################################################
# # Where the builder's own files are                                       #
# #                                                                         #
# # This file always runs from a whole source tree: the repository, or the  #
# # release the launcher (tools/beta/launcher.py) keeps up to date in       #
# # GalaxyCTR-Builder-files/source. REPO is that tree's root; the converter #
# # and its modules are in their usual places in it. The launcher hands     #
# # over the helper programs and the port's program (GALAXYCTR_TOOLS,       #
# # GALAXYCTR_ELF); run straight from the repository (developer mode), they #
# # are looked for in build/.                                               #
# ############################################################################

# The launcher version this file needs. Raise it when the builder starts using something only a
# new executable has (a new helper program, a Python module the executable does not carry): the
# launcher then sends the user to download the new executable instead of running this file.
LAUNCHER_NEEDED = 1
LAUNCHER_LINES = []  # what the launcher printed before the builder started, for the log

FROZEN = getattr(sys, "frozen", False)
REPO = Path(__file__).resolve().parents[2]
TOOLS = os.environ.get("GALAXYCTR_TOOLS")
ELF = Path(os.environ.get("GALAXYCTR_ELF") or REPO / "build" / "3ds" / "smg.elf")
RELEASE = os.environ.get("GALAXYCTR_RELEASE", "dev")

for folder in (REPO / "tools" / "3ds", REPO / "tools" / "3ds" / "builder"):
    sys.path.insert(0, str(folder))

# The disc this port is built for. The main.dol hash is the one the decomp's Wii build matches
# (config/RMGE01/build.sha1), so a modified or damaged disc is refused here.
GAME_ID = "RMGE01"
GAME_REVISION = 0
MAIN_DOL_SHA1 = "9a71008ae1ee9010e267fa67d1f0b0d4f0e895dd"

# Rough space needs, in bytes: the extracted disc plus everything the converter writes next to
# it (models, cache), and what ends up on the SD card.
WORK_SPACE = 9 << 30
# The kept files (settings.txt, keep_files): the disc as extracted plus the converted game (about
# 3.2 + 1.7 GB, measured on 7 Nov 2026, above 6.5 GB while converting), and what a later build
# writes anew in it at most.
KEEP_SPACE = 9 << 30
KEEP_UPDATE_SPACE = 3 << 30
SD_SPACE = 4 << 30

# What the builder writes under sdmc:/smg. Everything else there (nand/ holds the saves, the log)
# is left alone.
SD_ITEMS = ("files", "native", "port", "cache", "files_version.txt")

CIA_NAME = "GalaxyCTR.cia"


# ############################################################################
# # Talking to the user                                                     #
# ############################################################################

LOG = None  # the builder's log file, opened in main()


def say(text=""):
    """A line for the user, also kept in the log."""
    print(text, flush=True)
    if LOG:
        LOG.write(text + "\n")
        LOG.flush()


def ask(question):
    answer = input(question).strip()
    if LOG:
        LOG.write(f"{question}{answer}\n")
    return answer.strip('"').strip("'")  # paths dragged into a terminal come quoted


class BuildError(Exception):
    """A problem the user can fix: printed as is, without a traceback."""


def step(number, total, title):
    say()
    say(f"[{number}/{total}] {title}")


def ask_yes(question):
    """A y/n question, asked again until the answer is one of the two."""
    while True:
        answer = ask(f"{question} (y/n) ").lower()
        if answer in ("y", "yes", "n", "no"):
            return answer.startswith("y")
        say("  Type y or n.")


# ############################################################################
# # The user's settings and the install guide                               #
# #                                                                         #
# # settings.txt and install_guide.txt sit in the folder that holds the     #
# # builder program (the home folder when that one cannot be written). The  #
# # settings are plain "name = value" lines the user may edit or delete:    #
# #   keep_files      yes or no, asked once: keep the game files between    #
# #                   builds, so an update converts only what changed       #
# #   work_folder     where the kept files are                              #
# #   ftp_address     the 3DS's FTP address, as ip:port                     #
# #   last_sd_letter  the SD card's drive letter last used (Windows)        #
# ############################################################################

SETTINGS_NAME = "settings.txt"
GUIDE_NAME = "install_guide.txt"


def builder_home():
    here = Path(sys.executable).resolve().parent if FROZEN else REPO / "build" / "beta" / "launcher-test"
    for base in (here, Path.home()):
        try:
            base.mkdir(parents=True, exist_ok=True)
            probe = base / ".galaxyctr-write-test"
            probe.write_bytes(b"")
            probe.unlink()
            return base
        except OSError:
            continue
    return Path.home()


class Settings:
    def __init__(self):
        self.path = builder_home() / SETTINGS_NAME
        self.values = {}
        try:
            for line in self.path.read_text(encoding="utf-8").splitlines():
                if "=" in line and not line.lstrip().startswith("#"):
                    name, value = line.split("=", 1)
                    self.values[name.strip()] = value.strip()
        except OSError:
            pass

    def get(self, name):
        return self.values.get(name) or None

    def set(self, name, value):
        self.values[name] = str(value)
        lines = ["# GalaxyCTR builder settings. The builder writes this file. You can edit it, or delete it to be",
                 "# asked again.", ""]
        lines += [f"{name} = {value}" for name, value in self.values.items()]
        try:
            self.path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        except OSError as error:
            say(f"  Could not save {self.path} ({error}).")


# ############################################################################
# # Helper programs                                                         #
# #                                                                         #
# # nodtool (github.com/encounter/nod-rs) reads every Wii disc format;     #
# # bannertool (icon and banner) and makerom make the CIA. The executable  #
# # carries the copies for its own system (GALAXYCTR_TOOLS); developer      #
# # mode looks in build/beta/tools and devkitPro's tools folder.            #
# ############################################################################

def system_key():
    """windows-x86_64, linux-x86_64, macos-arm64 and so on."""
    name = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")
    machine = platform.machine().lower()
    machine = {"amd64": "x86_64", "x64": "x86_64", "aarch64": "arm64"}.get(machine, machine)
    return f"{name}-{machine}"


def find_tool(name):
    exe = name + (".exe" if os.name == "nt" else "")
    candidates = [Path(TOOLS) / exe] if TOOLS else []
    if not TOOLS:
        candidates += sorted((REPO / "build" / "beta" / "tools").glob(f"*/{name}*"))
        # devkitPro: DEVKITPRO is often an msys path (/opt/devkitpro) on Windows, so try the usual folder too.
        for root in (os.environ.get("DEVKITPRO_WIN"), os.environ.get("DEVKITPRO"), "C:/devkitPro", "/opt/devkitpro"):
            if root:
                candidates.append(Path(root) / "tools" / "bin" / exe)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise BuildError(f"The helper program {exe} is missing from the builder. Download the builder again.")


def run_tool(name, *args):
    """Runs a helper program and returns its output; a failure stops the build with its message."""
    result = subprocess.run([str(find_tool(name)), *map(str, args)], capture_output=True, text=True,
                            errors="replace", stdin=subprocess.DEVNULL)
    if LOG:
        LOG.write(f"$ {name} {' '.join(map(str, args))}\n{result.stdout}{result.stderr}\n")
    if result.returncode != 0:
        raise BuildError(f"{name} failed:\n{(result.stderr or result.stdout).strip()[-800:]}")
    return result.stdout + result.stderr


# ############################################################################
# # Step 1: the disc image                                                  #
# #                                                                         #
# # nodtool reads ISO, RVZ, WIA, WBFS, CISO, GCZ and NFS images alike, and  #
# # prints the disc's ID and revision. Only the USA release, revision 0,    #
# # is accepted: the port's code and the converter are made for it.        #
# ############################################################################

def read_disc_info(image):
    text = run_tool("nodtool", "info", image)
    game_id = revision = None
    for line in text.splitlines():
        line = line.strip()
        # The first "Game ID" and "Disc" lines describe the disc; later ones its partitions.
        if game_id is None and line.startswith("Game ID:"):
            game_id = line.split(":", 1)[1].strip().split()[0]
        elif revision is None and line.startswith("Disc ") and "Revision" in line:
            revision = int(line.rsplit("Revision", 1)[1].strip())
    title = next((l.split(":", 1)[1].strip() for l in text.splitlines() if l.strip().startswith("Title:")), "?")
    fmt = next((l.split(":", 1)[1].strip() for l in text.splitlines() if l.strip().startswith("Format:")), "?")
    return game_id, revision, title, fmt


def choose_disc(given=None):
    while True:
        path = Path(given or ask("Disc image of Super Mario Galaxy (USA), or drag it here: ")).expanduser()
        if given:
            say(f"  {path}")
            given = None  # a refused --disc falls back to asking
        if not path.is_file():
            say("  That file does not exist. Try again.")
            continue
        try:
            game_id, revision, title, fmt = read_disc_info(path)
        except BuildError:
            say("  This file is not a Wii disc image that can be read. Try another one.")
            continue
        say(f"  {fmt} image: {title} ({game_id}, revision {revision})")
        if game_id != GAME_ID:
            region = {"RMGJ01": "the Japanese release", "RMGP01": "the European release",
                      "RMGK01": "the Korean release"}.get(game_id)
            say(f"  This is {region} of Super Mario Galaxy. Only the USA release (RMGE01) is supported."
                if region else "  This is not Super Mario Galaxy. Only the USA release (RMGE01) is supported.")
            continue
        if revision != GAME_REVISION:
            say(f"  Revision {revision} of the disc is not supported, only revision {GAME_REVISION}.")
            continue
        return path


# ############################################################################
# # Steps 2 and 3: extraction and the main.dol check                       #
# #                                                                         #
# # The data partition comes out as files/ (the game's files) and sys/     #
# # (main.dol and the disc's system files), the same layout Dolphin makes. #
# # The converter later finds sys/main.dol next to files/ by itself.        #
# ############################################################################

def free_space(path):
    return shutil.disk_usage(path).free


def extraction_ok(target):
    """An extraction with the game's files and the original main.dol."""
    dol = target / "sys" / "main.dol"
    return (target / "files" / "ObjectData").is_dir() and dol.is_file() \
        and hashlib.sha1(dol.read_bytes()).hexdigest() == MAIN_DOL_SHA1


def extract_disc(image, work, name="disc", check_space=True):
    if check_space and free_space(work) < WORK_SPACE:
        raise BuildError(f"Not enough free space in {work}: about {WORK_SPACE >> 30} GB are needed, "
                         f"{free_space(work) >> 30} GB are free. Free some space and run the builder again.")
    target = work / name
    shutil.rmtree(target, ignore_errors=True)
    run_tool("nodtool", "extract", "-q", image, target)
    dol = target / "sys" / "main.dol"
    if not (target / "files" / "ObjectData").is_dir() or not dol.is_file():
        raise BuildError("The disc was extracted, but its files are not where they should be.")
    if not extraction_ok(target):
        raise BuildError("This disc's main.dol is not the original one: the disc image is modified or damaged. "
                         "Make a new copy of your disc.")
    say("  Extracted, and main.dol is the original.")
    return target


# ############################################################################
# # Step 4: conversion                                                      #
# #                                                                         #
# # The converter (tools/3ds/convert_smg_files.py) runs in this process,    #
# # on the extracted files in place, exactly as with its own --source       #
# # option: update step (with the data inside main.dol), native models,    #
# # check, conversion, movies, SD cache. Its many progress lines are        #
# # folded into one line that updates itself; all of them go to the log.   #
# ############################################################################

class ConverterOutput:
    """Stands in for stdout while the converter runs."""

    def __init__(self, stream):
        self.stream, self.pending, self.shown = stream, "", ""

    def write(self, text):
        self.pending += text
        lines = self.pending.split("\n")
        self.pending = lines.pop()
        for line in lines:
            if LOG:
                LOG.write(line + "\n")
            if line.startswith("@@PROGRESS "):
                # "@@PROGRESS step steps percent name|detail" (convert_smg_files._progress)
                parts = line.split(" ", 4)
                name = parts[4].split("|", 1)[0] if len(parts) > 4 else ""
                percent = float(parts[3]) if len(parts) > 3 else -1
                shown = f"  {name}: {percent:5.1f}%" if percent >= 0 else f"  {name}..."
                if shown != self.shown:
                    self.stream.write("\r" + shown.ljust(60))
                    self.stream.flush()
                    self.shown = shown
            elif line.startswith(("Warning", "Error", "Native conversion found", "Preflight found", "Conversion stopped")):
                self.stream.write("\n  " + line + "\n")
        return len(text)

    def flush(self):
        self.stream.flush()

    def __getattr__(self, name):
        return getattr(self.stream, name)


def convert_files(disc, originals=None):
    """Converts disc/files in place, or, with the kept original extraction, fills and updates the
    kept converted tree from it: only the files a new release converts differently are redone."""
    import convert_smg_files as converter

    saved_argv, saved_out, saved_err = sys.argv, sys.stdout, sys.stderr
    sys.argv = ["convert_smg_files", "--source", str(disc / "files"), "--native", str(disc / "native"), "--validated"]
    if originals is not None:
        sys.argv += ["--originals", str(originals / "files")]
    sys.stdout = sys.stderr = ConverterOutput(saved_out)
    try:
        code = converter.main()
    except SystemExit as stop:
        code = stop.code
    finally:
        sys.argv, sys.stdout, sys.stderr = saved_argv, saved_out, saved_err
    print()
    if code not in (0, None):
        raise BuildError(f"The conversion stopped with an error ({code}). The details are in the log.")
    missing = [name for name in SD_ITEMS if not (disc / name).exists()]
    if missing:
        raise BuildError(f"The conversion finished without writing {', '.join(missing)}.")
    say("  Converted.")


# ############################################################################
# # Step 5: the CIA                                                         #
# #                                                                         #
# # The HOME Menu icon and banner are the game's own save icon and banner,  #
# # from ObjectData/SaveIconBanner.arc on the disc (the Wii shows them in  #
# # its data management screen; SaveDataBannerCreator.cpp writes them to   #
# # the Wii save). Both are RGB5A3 textures (.bti): the icon is 48x48, the  #
# # 3DS size; the 192x64 banner is scaled into the 3DS's 256x128.           #
# ############################################################################

def rarc_members(data):
    """(name, bytes) of each file in a RARC archive (big-endian, as on the disc)."""
    be = lambda offset: int.from_bytes(data[offset:offset + 4], "big")
    info = be(8)
    data_start = info + be(12)
    count, entries, strings = be(info + 8), info + be(info + 12), info + be(info + 20)
    for index in range(count):
        entry = entries + index * 0x14
        flags_name = be(entry + 4)
        if not (flags_name >> 24) & 1:  # directories
            continue
        name_start = strings + (flags_name & 0xFFFFFF)
        name = data[name_start:data.index(b"\0", name_start)].decode("ascii")
        yield name, data[data_start + be(entry + 8):data_start + be(entry + 8) + be(entry + 12)]


def decode_bti(bti):
    """A .bti texture to (width, height, RGBA rows)."""
    import texreduce

    fmt, width, height = bti[0], int.from_bytes(bti[2:4], "big"), int.from_bytes(bti[4:6], "big")
    image = bti[int.from_bytes(bti[0x1C:0x20], "big"):]
    r, g, b, a = texreduce.decode(fmt, image, width, height)
    pixels = list(zip(r, g, b, a))
    return width, height, [pixels[y * width:(y + 1) * width] for y in range(height)]


def scale_into(rows, width, height):
    """Bilinear scale to width x height (the source keeps its proportions), centred; the rows above
    and below repeat the image's edge rows, which suits the banner's space background."""
    src_h, src_w = len(rows), len(rows[0])
    inner_h = round(src_h * width / src_w)
    top = (height - inner_h) // 2
    out = []
    for y in range(height):
        sy = min(max((y - top + 0.5) * src_h / inner_h - 0.5, 0), src_h - 1)
        y0 = int(sy); y1 = min(y0 + 1, src_h - 1); fy = sy - y0
        row = []
        for x in range(width):
            sx = min(max((x + 0.5) * src_w / width - 0.5, 0), src_w - 1)
            x0 = int(sx); x1 = min(x0 + 1, src_w - 1); fx = sx - x0
            row.append(tuple(round(rows[y0][x0][c] * (1 - fx) * (1 - fy) + rows[y0][x1][c] * fx * (1 - fy)
                                   + rows[y1][x0][c] * (1 - fx) * fy + rows[y1][x1][c] * fx * fy) for c in range(4)))
        out.append(row)
    return out


def write_png(path, rows):
    """An RGBA PNG, with the standard library only."""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    raw = b"".join(b"\0" + bytes(c for pixel in row for c in pixel) for row in rows)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", len(rows[0]), len(rows), 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def make_icon_and_banner(disc, work):
    """Right after extraction: the converter rewrites SaveIconBanner.arc for the 3DS later."""
    import yaz0

    archive = (disc / "files" / "ObjectData" / "SaveIconBanner.arc").read_bytes()
    if yaz0.is_compressed(archive):
        archive = yaz0.decompress(archive)
    textures = {name.lower(): data for name, data in rarc_members(archive)}
    if "saveicon00.bti" not in textures or "savebanner.bti" not in textures:
        raise BuildError("The disc's save icon and banner were not found.")

    _, _, icon = decode_bti(textures["saveicon00.bti"])
    width, height, banner = decode_bti(textures["savebanner.bti"])
    meta = work / "cia"
    meta.mkdir(exist_ok=True)
    write_png(meta / "icon.png", icon)
    write_png(meta / "banner.png", scale_into(banner, 256, 128))
    with wave.open(str(meta / "banner.wav"), "wb") as sound:  # the banner's sound: one silent second
        sound.setnchannels(1); sound.setsampwidth(2); sound.setframerate(22050); sound.writeframes(bytes(44100))
    say("  HOME Menu icon and banner taken from the disc.")


def make_cia(work):
    meta = work / "cia"
    elf, rsf = ELF, REPO / "tools" / "3ds" / "cia_showcase.rsf"
    if not elf.is_file():
        raise BuildError("The port's program is missing from the builder. Download the builder again.")
    major, minor, micro = port_version()
    run_tool("bannertool", "makesmdh", "-s", "Super Mario Galaxy", "-l", "GalaxyCTR - New 3DS port (beta)",
             "-p", "GalaxyCTR", "-i", meta / "icon.png", "-o", meta / "icon.smdh")
    run_tool("bannertool", "makebanner", "-i", meta / "banner.png", "-a", meta / "banner.wav", "-o", meta / "banner.bnr")
    cia = work / CIA_NAME
    run_tool("makerom", "-f", "cia", "-o", cia, "-rsf", rsf, "-target", "t", "-elf", elf,
             "-icon", meta / "icon.smdh", "-banner", meta / "banner.bnr", "-major", major, "-minor", minor, "-micro", micro)
    say(f"  {CIA_NAME} built (version {major}.{minor}.{micro}).")
    return cia


def port_version():
    """The port's version, from platform/3ds/host/build_version.h."""
    header = REPO / "platform" / "3ds" / "host" / "build_version.h"
    text = header.read_text(encoding="ascii")
    return tuple(int(text.split(f"SMG_CIA_VERSION_{part} ", 1)[1].split()[0]) for part in ("MAJOR", "MINOR", "MICRO"))


# ############################################################################
# # Step 6: the SD card                                                     #
# #                                                                         #
# # On Windows the user types the card's drive letter; Linux and macOS have #
# # no letters, so the mounted drives are listed to choose from. The game   #
# # files replace the previous ones; sdmc:/smg/nand (the saves) and         #
# # anything else on the card stay as they are.                             #
# ############################################################################

def removable_drive(letter):
    """True for a removable drive on Windows (GetDriveTypeW: 2 = removable)."""
    import ctypes
    return ctypes.windll.kernel32.GetDriveTypeW(f"{letter}:\\") == 2


def mounted_drives():
    """Candidate SD card mount points on Linux and macOS."""
    user = os.environ.get("USER", "")
    roots = [Path("/Volumes")] if sys.platform == "darwin" else [Path("/media") / user, Path("/run/media") / user, Path("/media"), Path("/mnt")]
    found = []
    for root in roots:
        if root.is_dir():
            found += [p for p in sorted(root.iterdir()) if p.is_dir() and os.path.ismount(p) and p not in found]
    return found


def choose_sd_card(settings):
    say("Insert your 3DS's SD card into this computer.")
    last = settings.get("last_sd_letter")
    while True:
        if os.name == "nt":
            prompt = f"Drive letter of the SD card (Enter for {last}): " if last else "Drive letter of the SD card (for example E): "
            letter = (ask(prompt) or last or "").rstrip(":\\/").upper()
            if len(letter) != 1 or not letter.isalpha():
                say("  Type just the letter.")
                continue
            card = Path(f"{letter}:\\")
            if not card.is_dir():
                say(f"  There is no drive {letter}:. Check the letter in File Explorer.")
                continue
            if letter == Path(os.environ.get("SystemDrive", "C:")).drive.rstrip(":").upper():
                say("  That is this computer's system drive, not an SD card.")
                continue
            if not removable_drive(letter) and ask(f"  {letter}: is not a removable drive. Use it anyway? (y/n) ").lower() != "y":
                continue
        else:
            drives = mounted_drives()
            for number, drive in enumerate(drives, 1):
                say(f"  {number}. {drive}")
            answer = ask("Number of the SD card above, or its full path: ")
            card = drives[int(answer) - 1] if answer.isdigit() and 0 < int(answer) <= len(drives) else Path(answer).expanduser()
            if not card.is_dir():
                say("  That folder does not exist.")
                continue
        try:
            probe = card / ".galaxyctr-write-test"
            probe.write_bytes(b"")
            probe.unlink()
        except OSError:
            say("  The card cannot be written. Check its lock switch and try again.")
            continue
        if os.name == "nt":
            settings.set("last_sd_letter", letter)
        return card


def copy_to_sd(disc, cia, card):
    needed = sum(f.stat().st_size for item in SD_ITEMS for f in ([disc / item] if (disc / item).is_file() else (disc / item).rglob("*")) if f.is_file())
    old = sum(f.stat().st_size for item in SD_ITEMS[:-1] if (card / "smg" / item).is_dir() for f in (card / "smg" / item).rglob("*") if f.is_file())
    if free_space(card) + old < needed + cia.stat().st_size:
        raise BuildError(f"The SD card needs about {(needed + cia.stat().st_size) >> 20} MB free, "
                         f"it has {free_space(card) >> 20} MB.")
    smg = card / "smg"
    smg.mkdir(exist_ok=True)
    copied, started = 0, time.monotonic()
    for item in SD_ITEMS:
        source, target = disc / item, smg / item
        if target.is_dir():
            shutil.rmtree(target)  # the old converted files; saves are in smg/nand and stay
        if source.is_file():
            shutil.copyfile(source, target)
            continue
        for path in sorted(source.rglob("*")):
            destination = target / path.relative_to(source)
            if path.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, destination)
            copied += path.stat().st_size
            print(f"\r  Copying: {100 * copied / needed:5.1f}%", end="", flush=True)
    print()
    (card / "cias").mkdir(exist_ok=True)
    shutil.copyfile(cia, card / "cias" / CIA_NAME)
    say(f"  Copied in {int(time.monotonic() - started)} s: the game files to {smg}, the CIA to {card / 'cias' / CIA_NAME}.")


def check_dsp_firmware(card):
    """The 3DS's sound needs the console's own DSP firmware, which nobody may distribute."""
    if (card / "3ds" / "dspfirm.cdc").is_file():
        return True
    say("  Note: sdmc:/3ds/dspfirm.cdc is missing, so the game would have no sound.")
    say("  Run the homebrew app 'DSP1' once on your 3DS: it saves that file from your own console.")
    return False


# ############################################################################
# # Step 5 over Wi-Fi: FTP                                                  #
# #                                                                         #
# # The same files sent to an FTP server app running on the 3DS (ftpd and   #
# # the like), at the address it shows (ip:port). As on a card in this      #
# # computer, the old smg/files, native, port and cache are deleted first   #
# # and smg/nand (the saves) is left alone.                                 #
# ############################################################################

FTP_DEFAULT_PORT = 5000  # ftpd's


def parse_ftp_address(text):
    """'192.168.1.10:5000' -> ('192.168.1.10', 5000); None when it is not an address."""
    host, _, port = text.strip().partition(":")
    parts = host.split(".")
    if len(parts) != 4 or not all(p.isdigit() and int(p) < 256 for p in parts):
        return None
    if port and not (port.isdigit() and 0 < int(port) < 65536):
        return None
    return host, int(port) if port else FTP_DEFAULT_PORT


def choose_ftp_address(settings):
    say("Start an FTP app on your 3DS (for example ftpd) and connect it to the same Wi-Fi as this computer.")
    last = settings.get("ftp_address")
    while True:
        prompt = f"Address it shows, as ip:port (Enter for {last}): " if last else "Address it shows, as ip:port (for example 192.168.1.10:5000): "
        text = ask(prompt) or last or ""
        address = parse_ftp_address(text)
        if address is None:
            say("  Type the address as four numbers and the port, for example 192.168.1.10:5000.")
            continue
        settings.set("ftp_address", f"{address[0]}:{address[1]}")
        return address


def ftp_remove_tree(ftp, ftplib, path):
    """Deletes a folder on the 3DS with everything in it; nothing when it does not exist. The
    folder is entered and then listed: some servers list their root for a listing given a path."""
    try:
        ftp.cwd(path)
    except ftplib.error_perm:
        return
    try:
        entries = [(name, facts.get("type") == "dir") for name, facts in ftp.mlsd(facts=["type"])
                   if facts.get("type") not in ("cdir", "pdir")]
    except ftplib.error_perm:  # no MLSD: a name is a folder when it can be entered
        entries = []
        for name in ftp.nlst():
            name = name.rsplit("/", 1)[-1]
            try:
                ftp.cwd(f"{path}/{name}")
                entries.append((name, True))
            except ftplib.error_perm:
                entries.append((name, False))
    ftp.cwd("/")
    for name, is_folder in entries:
        if name in (".", ".."):
            continue
        child = f"{path}/{name}"
        if is_folder:
            ftp_remove_tree(ftp, ftplib, child)
        else:
            ftp.delete(child)
    ftp.rmd(path)


def ftp_make_folder(ftp, ftplib, path):
    try:
        ftp.mkd(path)
    except ftplib.error_perm:
        pass  # already there


def copy_over_ftp(disc, cia, address):
    try:
        import ftplib
    except ImportError:
        raise BuildError("This builder program cannot copy over FTP. Download the latest one from "
                         "https://github.com/Im-Monee/galaxyctr-beta/releases/latest, or copy to the SD card in this computer.")
    files = [(f, f"/smg/{item}" + ("" if f == disc / item else "/" + f.relative_to(disc / item).as_posix()))
             for item in SD_ITEMS
             for f in ([disc / item] if (disc / item).is_file() else sorted((disc / item).rglob("*")))
             if f.is_file()]
    files.append((cia, f"/cias/{CIA_NAME}"))
    needed = sum(f.stat().st_size for f, _ in files)
    host, port = address
    say(f"  Connecting to {host}:{port}...")
    started = time.monotonic()
    try:
        ftp = ftplib.FTP()
        ftp.connect(host, port, timeout=30)
        ftp.login()
        ftp.set_pasv(True)
        say("  Connected. Over Wi-Fi this can take a long time: keep the 3DS on and near the router.")
        for item in SD_ITEMS[:-1]:
            ftp_remove_tree(ftp, ftplib, f"/smg/{item}")  # the old converted files; saves are in smg/nand
        made = set()
        copied = 0
        for path, remote in files:
            folder = remote.rsplit("/", 1)[0]
            parts = folder.strip("/").split("/")
            for depth in range(1, len(parts) + 1):
                sub = "/" + "/".join(parts[:depth])
                if sub not in made:
                    ftp_make_folder(ftp, ftplib, sub)
                    made.add(sub)

            def sent(block, size=[0]):
                size[0] += len(block)
                print(f"\r  Copying: {100 * (copied + size[0]) / needed:5.1f}%", end="", flush=True)

            with open(path, "rb") as source:
                ftp.storbinary(f"STOR {remote}", source, blocksize=1 << 16, callback=sent)
            copied += path.stat().st_size
        print()
        try:
            ftp.size("/3ds/dspfirm.cdc")
            sound = True
        except ftplib.all_errors:
            sound = False
        ftp.quit()
    except (OSError, EOFError, ftplib.Error) as error:
        print()
        raise BuildError(f"The FTP copy stopped ({error}). Check the address and that the FTP app is still running.")
    say(f"  Copied in {int(time.monotonic() - started)} s: the game files to smg, the CIA to cias/{CIA_NAME}.")
    if not sound:
        say("  Note: 3ds/dspfirm.cdc was not found on the SD card, so the game would have no sound.")
        say("  Run the homebrew app 'DSP1' once on your 3DS: it saves that file from your own console.")
    return sound


# ############################################################################
# # When the files are not copied now                                       #
# #                                                                         #
# # They stay where they were made, and the steps to copy them by hand are  #
# # printed and saved as install_guide.txt next to the builder.             #
# ############################################################################

def write_install_guide(disc, cia, work, keep):
    items = ", ".join(SD_ITEMS)
    lines = [
        "GalaxyCTR: how to put the game on your 3DS",
        "",
        f"The game files are in {disc}",
        f"The CIA is {cia}",
        "",
        "- Put your 3DS's SD card into this computer, or start an FTP app on the 3DS and connect to it.",
        "- On the SD card, open the folder smg. Create it if it is missing.",
        "- In smg, delete the folders files, native, port and cache if they are there. Leave the folder nand: it holds your saves.",
        f"- Copy {items} from the game files folder above into smg.",
        f"- Copy {CIA_NAME} into the folder cias on the SD card. Create it if it is missing.",
        "- Check that the SD card has the file 3ds/dspfirm.cdc. If not, run the homebrew app DSP1 once on your 3DS, or the game has no sound.",
        f"- Put the SD card back into your 3DS and install cias/{CIA_NAME} with FBI (or another CIA installer).",
    ]
    if keep:
        lines.append(f"- Keep the folder {work}: the next build updates it and is much faster.")
    else:
        lines.append(f"- When everything is copied, delete the folder {work} to free the space.")
    say()
    for line in lines:
        say(line)
    guide = builder_home() / GUIDE_NAME
    try:
        guide.write_text("\n".join(lines) + "\n", encoding="utf-8")
        say()
        say(f"These steps are saved in {guide}")
    except OSError as error:
        say(f"Could not save {guide} ({error}).")


# ############################################################################
# # The whole build                                                         #
# ############################################################################

def ask_keep_files(settings):
    """Asked once, right after the update check: the answer is kept in settings.txt."""
    value = settings.get("keep_files")
    if value in ("yes", "no"):
        return value == "yes"
    say()
    say(f"The builder can keep the game files it makes (about {KEEP_SPACE >> 30} GB) after each build. The next updates then")
    say("convert only what changed and do not need the disc image, so they are much faster.")
    keep = ask_yes("Keep the game files after each build?")
    settings.set("keep_files", "yes" if keep else "no")
    say(f"  Saved in {settings.path}. Edit or delete that file to change it.")
    return keep


def folder_size(folder):
    return sum(f.stat().st_size for f in folder.rglob("*") if f.is_file()) if folder.is_dir() else 0


def keep_space_needed(folder):
    """What the kept folder still needs on its drive: the whole folder less what is already there,
    and at least room for what each build writes anew (the updated files and the SD cache)."""
    return max(KEEP_SPACE - folder_size(folder), KEEP_UPDATE_SPACE)


def check_keep_space(folder):
    needed, free = keep_space_needed(folder), free_space(folder)
    if free < needed:
        raise BuildError(f"Not enough free space for {folder}: about {-(-needed >> 30)} GB more are needed, "
                         f"{free >> 30} GB are free. Free some space, or move the folder and change work_folder "
                         f"in {SETTINGS_NAME}.")


def choose_keep_folder(settings, default):
    """Asked when the files are kept for the first time (or the kept folder is gone): where."""
    say(f"  The kept game files need about {KEEP_SPACE >> 30} GB. Choose a drive with that much free space.")
    while True:
        answer = ask(f"Folder for the kept game files (Enter for {default}): ")
        folder = Path(answer).expanduser() if answer else default
        try:
            folder.mkdir(parents=True, exist_ok=True)
            probe = folder / ".galaxyctr-write-test"
            probe.write_bytes(b"")
            probe.unlink()
        except OSError:
            say("  That folder cannot be made or written. Choose another one.")
            continue
        needed, free = keep_space_needed(folder), free_space(folder)
        if free < needed:
            say(f"  That drive has {free >> 30} GB free, about {-(-needed >> 30)} GB are needed. Choose another one.")
            continue
        settings.set("work_folder", folder)
        return folder


def kept_folder(settings):
    """The folder of the kept files, when it holds a good original extraction."""
    folder = settings.get("work_folder")
    if folder and extraction_ok(Path(folder) / "original"):
        return Path(folder)
    return None


def copy_now(disc, cia, settings):
    """Step 5: asks whether and how to copy. True when copied, with the sound check's result."""
    if not ask_yes("Copy the files to your 3DS now?"):
        return False, None
    if ask_yes("Copy over Wi-Fi with FTP? Answer n to copy to the SD card in this computer."):
        return True, copy_over_ftp(disc, cia, choose_ftp_address(settings))
    card = choose_sd_card(settings)
    copy_to_sd(disc, cia, card)
    return True, check_dsp_firmware(card)


def build(disc_arg=None, sd_arg=None, work_arg=None):
    say("GalaxyCTR builder: Super Mario Galaxy for New Nintendo 3DS (beta)")
    say("Everything is made from your own copy of the game. Only the USA release (RMGE01) is supported.")
    say("The game needs a New 3DS, New 3DS XL or New 2DS XL.")
    settings = Settings()
    keep = ask_keep_files(settings)

    total = 6
    step(1, total, "Disc image")
    kept = kept_folder(settings) if keep else None
    if kept:
        image = None
        say(f"  Using the game files kept from the last build, in {kept}. The disc image is not needed.")
        work = kept
    else:
        image = choose_disc(disc_arg)
        # Work next to the disc image by default: it is on a drive that already holds 4.7 GB of game.
        parent = work_arg or (image.parent if os.access(image.parent, os.W_OK) else None)
        if keep:
            work = choose_keep_folder(settings, Path(parent or builder_home()) / "GalaxyCTR-files")
        else:
            work = Path(tempfile.mkdtemp(prefix="galaxyctr-", dir=parent))
    say(f"  {'Folder of the kept files' if keep else 'Temporary folder'}: {work}")
    if kept:
        check_keep_space(work)

    copied, sound, failed = False, None, True
    try:
        if keep:
            # The disc as extracted stays in original/, the converted game in game/: each build
            # converts again only what the new release changed.
            if image is None:
                step(2, total, "Extracting the disc: already done")
                originals = work / "original"
            else:
                step(2, total, "Extracting the disc (about a minute)")
                originals = extract_disc(image, work, "original", check_space=False)  # checked for the whole folder
            disc = work / "game"
            make_icon_and_banner(originals, work)
            step(3, total, "Converting the game files for the 3DS (only what changed)")
            convert_files(disc, originals)
        else:
            step(2, total, "Extracting the disc (about a minute)")
            disc = extract_disc(image, work)
            make_icon_and_banner(disc, work)
            step(3, total, "Converting the game files for the 3DS (this takes a while)")
            convert_files(disc)
        step(4, total, "Building the CIA")
        cia = make_cia(work)
        failed = False

        step(5, total, "Copying to the 3DS")
        say("  The build is complete, without errors.")
        if sd_arg:
            copy_to_sd(disc, cia, Path(sd_arg))
            copied, sound = True, check_dsp_firmware(Path(sd_arg))
        else:
            try:
                copied, sound = copy_now(disc, cia, settings)
            except BuildError as error:
                say(f"  ERROR: {error}")
                say("  The files were not all copied.")
    finally:
        step(6, total, "Cleaning up")
        if keep:
            say(f"  The game files stay in {work} for the next build.")
        elif failed or copied:
            shutil.rmtree(work, ignore_errors=True)
            say("  Temporary files deleted." if not work.exists() else f"  Could not delete {work}: delete it by hand.")
        else:
            say(f"  The files stay in {work}.")

    if not copied:
        write_install_guide(disc, cia, work, keep)
        return
    say()
    say("Done. Everything went well." if sound else "Done, with the note above about the sound.")
    say(f"Put the SD card back into your 3DS if it is in this computer, and install cias/{CIA_NAME} with FBI "
        "(or another CIA installer).")


def main():
    global LOG
    log_path = Path.home() / "galaxyctr-builder-log.txt"
    LOG = log_path.open("w", encoding="utf-8")
    LOG.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {system_key()} {platform.platform()} release {RELEASE}\n")
    for line in LAUNCHER_LINES:
        LOG.write(line + "\n")
    code = 0
    # Developer options, for testing without questions: --disc <image> --sd <folder> --work <folder>.
    args = sys.argv[1:]
    option = lambda name: args[args.index(name) + 1] if name in args and args.index(name) + 1 < len(args) else None
    try:
        build(option("--disc"), option("--sd"), option("--work"))
    except BuildError as error:
        say()
        say(f"ERROR: {error}")
        code = 1
    except KeyboardInterrupt:
        say()
        say("Stopped.")
        code = 1
    except Exception:  # noqa: BLE001 - every unexpected failure is reported, with its details in the log
        LOG.write(traceback.format_exc())
        say()
        say("ERROR: something unexpected went wrong. Please report it with the log file below.")
        code = 1
    say(f"Log file: {log_path}")
    LOG.close()
    if FROZEN:
        input("Press Enter to close this window.")
    return code


if __name__ == "__main__":
    multiprocessing.freeze_support()  # the converter encodes music in several processes
    sys.exit(main())
