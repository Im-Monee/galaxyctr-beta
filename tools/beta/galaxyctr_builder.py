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

It talks to the user through a window (tools/beta/builder_gui.py) when the builder program carries
Qt (PySide6) and the system can show it, and through the terminal otherwise, or with --console. The
steps, the questions and settings.txt are the same in both.
"""

import hashlib
import multiprocessing
import os
import platform
import queue
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.parse
import wave
import zlib
from collections import deque
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
# is left alone. The folders first, files_version.txt last (SD_ITEMS[:-1] are the folders).
# loading/ (the loading screen, builder_loading.py) is made only when this program can draw it.
SD_ITEMS = ("files", "native", "port", "cache", "loading", "files_version.txt")
OPTIONAL_SD_ITEMS = ("loading",)

CIA_NAME = "GalaxyCTR.cia"


# ############################################################################
# # Talking to the user                                                     #
# #                                                                         #
# # Every line and question goes through UI: TextUI below (the terminal),   #
# # or the window's GuiUI (builder_gui.py), which runs the same build in a  #
# # thread of its own and answers the questions from dialogs. ask() gets    #
# # a kind (text, disc, folder, sd) so the window can offer the matching    #
# # picker; the terminal just reads a line. Everything also goes to LOG.    #
# ############################################################################

LOG = None  # the builder's log file, opened in main()
LOG_LOCK = threading.Lock()  # the FTP copy writes to it from several threads


EARLY = []  # what was logged before the log file was opened (the launcher's first start)


STEP_LINE = re.compile(r"^\[(\d+)/(\d+)\] (.+)$")


DETAIL = "\t"  # starts a line that is a detail of the entry before it (log_format)


def log_format(text):
    """The log's look (owner, 25 Nov 2026): a title for each step ("Step 4 of 6: Building the
    CIA"), a dash before every entry, and an indented dash before an entry's details (lines that
    start with DETAIL: an answer, a program's output, the converter's report). Lines that already
    have their dash, and the title line, stay as they are."""
    out = []
    for line in str(text).split("\n"):
        stripped = line.strip()
        found = STEP_LINE.match(stripped)
        if not stripped:
            out.append("")
        elif found:
            out.append(f"Step {found[1]} of {found[2]}: {found[3]}")
        elif line.startswith(("- ", "  - ", "== ")):
            out.append(line)
        elif line.startswith(DETAIL):
            out.append("  - " + stripped)
        else:
            out.append("- " + stripped)
    return "\n".join(out)


def log(text):
    text = log_format(text)
    if LOG:
        with LOG_LOCK:
            LOG.write(text + "\n")
            LOG.flush()
    else:
        EARLY.append(text)


def terminal():
    """Where the terminal's words go: while the converter runs, sys.stdout is ConverterOutput,
    which only logs what it gets, so the progress line and the notes the builder shows then went
    to the log instead of the screen (terminal mode, found on Linux 25 Nov 2026)."""
    # The converter wraps sys.stdout once more (its _LogTee, with the same .stream), so the layers
    # are walked down to ConverterOutput's own stream.
    out = sys.stdout
    for _ in range(4):
        if isinstance(out, ConverterOutput):
            return out.stream or sys.__stdout__
        inner = out.__dict__.get("stream") if hasattr(out, "__dict__") else None
        if inner is None:
            break
        out = inner
    return sys.stdout


class TextUI:
    """The terminal, as the builder always was."""

    def __init__(self):
        self.shown = None  # the progress line on screen, None when there is none

    def end_progress(self):
        if self.shown is not None:
            print(file=terminal(), flush=True)
            self.shown = None

    def say(self, text=""):
        self.end_progress()
        print(text, file=terminal(), flush=True)

    def problem(self, text):
        self.say(text)

    def step(self, number, total, title):
        self.say()
        self.say(f"[{number}/{total}] {title}")

    def progress(self, label, percent=-1.0, detail=""):
        shown = f"  {label}: {percent:5.1f}%" if percent >= 0 else f"  {label}..."
        if detail:
            shown += f"  {detail}"
        if shown != self.shown:
            print("\r" + shown.ljust(60), end="", file=terminal(), flush=True)
            self.shown = shown

    def ask(self, question, kind="text", default=None):
        self.end_progress()
        return input(question)

    def ask_yes(self, question):
        """A y/n question, asked again until the answer is one of the two."""
        while True:
            answer = ask(f"{question} (y/n) ").lower()
            if answer in ("y", "yes", "n", "no"):
                return answer.startswith("y")
            say("  Type y or n.")

    def ask_ftp(self, question, last, connections):
        address = ask(question)
        count = ask(f"Parallel connections, 1 to {FTP_MAX_CONNECTIONS} (Enter for {connections}): ")
        return address, count


UI = TextUI()


def say(text=""):
    """A line for the user, also kept in the log."""
    UI.say(text)
    log(text)


def problem(text):
    """Why an answer was refused; the window shows it in the next question's dialog too."""
    UI.problem(text)
    log(text)


def ask(question, kind="text", default=None):
    answer = str(UI.ask(question, kind, default) or "").strip()
    log(f"Asked: {question.strip().rstrip(':').strip()}\n{DETAIL}Answer: {answer or '(none)'}")
    return clean_path_text(answer) if kind != "text" else answer.strip('"').strip("'")


class BuildError(Exception):
    """A problem the user can fix: printed as is, without a traceback."""


def duration(seconds):
    """517 -> '8 min 37 s'."""
    seconds = int(seconds)
    return f"{seconds // 60} min {seconds % 60} s" if seconds >= 60 else f"{seconds} s"


def step(number, total, title):
    UI.step(number, total, title)
    log(f"\n[{number}/{total}] {title}")


def progress(label, percent=-1.0, detail=""):
    UI.progress(label, percent, detail)


def end_progress():
    UI.end_progress()


def ask_yes(question):
    answer = bool(UI.ask_yes(question))
    log(f"Asked: {question.strip()}\n{DETAIL}Answer: {'yes' if answer else 'no'}")
    return answer


# Characters that ride along with a path copied from Windows' file properties or some chat apps,
# invisible but enough to make the file "not exist".
INVISIBLE = dict.fromkeys(map(ord, "​‌‍‎‏‪‫‬‭‮"
                                   "⁦⁧⁨⁩﻿"))


def clean_path_text(text):
    """A path as typed, pasted or dragged in: quotes, PowerShell's "& '...'", file:// addresses, the
    backslashes macOS and Linux terminals put before spaces, and invisible characters removed."""
    text = text.translate(INVISIBLE).strip()
    if text.startswith("& "):
        text = text[2:].strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        text = text[1:-1]
    if text.lower().startswith("file://"):
        text = urllib.parse.unquote(urllib.parse.urlparse(text).path)
        if os.name == "nt" and re.match(r"^/[A-Za-z]:", text):
            text = text[1:]
    if os.name != "nt" and "\\" in text and not Path(text).expanduser().exists():
        text = re.sub(r"\\(.)", r"\1", text)
    return text.strip()


# ############################################################################
# # The user's settings and the install guide                               #
# #                                                                         #
# # settings.txt sits in the user's settings folder (settings_path, on      #
# # Windows %APPDATA%\GalaxyCTR), so the folder of the builder program      #
# # holds only the program and install_guide.txt. The settings are plain    #
# # "name = value" lines the user may edit or delete:                       #
# #   keep_files      yes or no, asked once: keep the game files between    #
# #                   builds, so an update converts only what changed       #
# #   work_folder     where the kept files are                              #
# #   ftp_address     the 3DS's FTP address, as ip:port                     #
# #   ftp_connections how many files the FTP copy sends at once (1 to 10,   #
# #                   5 when not set)                                       #
# #   last_sd_letter  the SD card's drive letter last used (Windows)        #
# #                                                                         #
# # A builder downloaded again into another folder finds the same answers.  #
# # The settings.txt older builders kept next to the program moves there    #
# # the first time (its answers win, as they were the ones in use).         #
# ############################################################################

SETTINGS_NAME = "settings.txt"
GUIDE_NAME = "install_guide.txt"


def program_folder():
    """The folder the user keeps the builder program in. For the macOS app
    (GalaxyCTR Builder.app/Contents/MacOS/<program>, build_builder.pack_app) that is the folder
    the app is in: nothing is written inside the app, which would break its signature."""
    exe = Path(sys.executable).resolve()
    if sys.platform == "darwin" and len(exe.parents) > 3 and exe.parent.name == "MacOS" \
            and exe.parents[1].name == "Contents" and exe.parents[2].suffix == ".app":
        return exe.parents[3]
    return exe.parent


def builder_home():
    # A macOS app started from Downloads before it was moved runs from a read-only copy (App
    # Translocation): the write test below then falls back to the home folder.
    here = program_folder() if FROZEN else REPO / "build" / "beta" / "launcher-test"
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


def settings_folder():
    """The user's settings folder for GalaxyCTR (created when it is written)."""
    if os.environ.get("GALAXYCTR_HOME"):  # tests
        return Path(os.environ["GALAXYCTR_HOME"])
    if os.name == "nt":
        base = Path(os.environ.get("APPDATA") or Path.home() / "AppData" / "Roaming")
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    return base / "GalaxyCTR"


def settings_path():
    return settings_folder() / SETTINGS_NAME


def data_home():
    """Where the builder keeps its own files (the game look, GalaxyCTR-Builder-files): the user's
    settings folder, so the builder's folder holds only the program and install_guide.txt. Run
    from the repository, build/beta/launcher-test (or GALAXYCTR_HOME, for tests)."""
    if os.environ.get("GALAXYCTR_HOME"):
        return settings_folder()
    return settings_folder() if FROZEN else REPO / "build" / "beta" / "launcher-test"


# ############################################################################
# # Checking settings.txt                                                   #
# #                                                                         #
# # The user may edit the file, but every line must be a comment (#), empty #
# # or "name = value" with a name the builder knows, once, and a value of   #
# # the right kind (SETTING_RULES; an empty value means "not set"). A file  #
# # that breaks any of this is emptied at once, before anything reads it    #
# # (Settings, and check_settings, the launcher's very first step).         #
# ############################################################################

def balanced(text):
    """Every bracket of text closed, in order."""
    pairs, stack = {")": "(", "]": "[", "}": "{"}, []
    for char in text:
        if char in "([{":
            stack.append(char)
        elif char in pairs:
            if not stack or stack.pop() != pairs[char]:
                return False
    return not stack


def good_path(value):
    """A full path to a file or folder: no characters Windows refuses, brackets closed."""
    if len(value) > 1024 or any(ord(char) < 32 for char in value) or not balanced(value):
        return False
    path = Path(value)
    if not path.is_absolute():
        return False
    if os.name == "nt":
        rest = value[2:] if path.drive and len(path.drive) == 2 else value
        if any(char in '<>"|?*' for char in value) or ":" in rest:
            return False
    return True


def good_look_disc(value):
    """builder_exeicon.disc_identity: path|size|seconds."""
    parts = value.rsplit("|", 2)
    return len(parts) == 3 and good_path(parts[0]) and parts[1].isdigit() and parts[2].lstrip("-").isdigit()


def good_geometry(value):
    """builder_window.Window.GEOMETRY (x,y,width,height,state,screen), or the base64 an earlier
    builder wrote."""
    if re.fullmatch(r"-?\d+,-?\d+,\d+,\d+,(normal|maximized)(,[^\x00-\x1f]*)?", value):
        return True
    import base64
    import binascii
    if not re.fullmatch(r"[A-Za-z0-9+/]+={0,2}", value):
        return False
    try:
        base64.b64decode(value, validate=True)
    except (binascii.Error, ValueError):
        return False
    return True


SETTING_RULES = {
    "keep_files": lambda v: v in ("yes", "no"),
    "work_folder": good_path,
    "disc_image": good_path,
    "ftp_address": lambda v: ":" in v and parse_ftp_address(v) is not None,
    "ftp_connections": lambda v: v.isdigit() and 1 <= int(v) <= FTP_MAX_CONNECTIONS,
    "last_sd_letter": lambda v: len(v) == 1 and "A" <= v.upper() <= "Z",
    "look_disc": good_look_disc,
    "remember_window": lambda v: v in ("yes", "no"),
    "window_geometry": good_geometry,
    "graphics_fps": lambda v: v in ("screen", "30", "60", "120", "144", "165", "240", "0"),
    "graphics_vsync": lambda v: v in ("on", "off"),
    "graphics_gpu": lambda v: v in ("auto", "saving", "fast", "second"),
    "graphics_motion": lambda v: v in ("system", "always", "still"),
    "graphics_counter": lambda v: v in ("on", "off"),
}


def settings_problem(text):
    """What is wrong in settings.txt's text, or None."""
    seen = set()
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if "=" not in stripped:
            return f"line {number} is not 'name = value'"
        name, value = (part.strip() for part in stripped.split("=", 1))
        if name not in SETTING_RULES:
            return f"line {number}: unknown setting '{name[:40]}'"
        if name in seen:
            return f"line {number}: '{name}' is set twice"
        seen.add(name)
        if value and not SETTING_RULES[name](value):
            return f"line {number}: '{value[:80]}' is not a valid {name}"
    return None


def read_settings(path):
    """settings.txt's answers. OSError when the file is missing; ValueError, with the reason, when
    it breaks the rules above."""
    try:
        text = path.read_bytes().decode("utf-8-sig")
    except UnicodeDecodeError:
        raise ValueError("it is not UTF-8 text") from None
    problem_found = settings_problem(text)
    if problem_found:
        raise ValueError(problem_found)
    values = {}
    for line in text.splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            name, value = line.split("=", 1)
            values[name.strip()] = value.strip()
    return values


def check_settings():
    """The launcher's first step: settings.txt checked (and emptied when it is wrong)."""
    Settings()


class Settings:
    def __init__(self):
        self.path = settings_path()
        self.values = {}
        try:
            self.values = read_settings(self.path)
        except OSError:
            pass
        except ValueError as error:
            log(f"settings.txt was emptied: {error}.")
            self.save()
        old = builder_home() / SETTINGS_NAME  # where older builders kept it
        if old != self.path and old.is_file():
            try:
                self.values.update(read_settings(old))
                self.save()
            except (OSError, ValueError) as error:
                log(f"The old settings.txt next to the builder was not taken ({error}).")
            try:
                old.unlink()
            except OSError:
                pass

    def get(self, name):
        return self.values.get(name) or None

    def set(self, name, value):
        self.values[name] = str(value)
        self.save()

    def unset(self, name):
        """Forgets an answer, so the builder asks again."""
        if self.values.pop(name, None) is not None:
            self.save()

    def save(self):
        lines = ["# GalaxyCTR builder settings. The builder writes this file. You can edit it, or delete it to be",
                 "# asked again.", ""]
        lines += [f"{name} = {value}" for name, value in self.values.items()]
        text = "\n".join(lines) + "\n"
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.path.write_text(text, encoding="utf-8")
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


# The builder program for Windows has no terminal window of its own when it shows its window, so the
# helper programs (terminal programs) are started without one, or each would open one.
NO_WINDOW = 0x08000000 if os.name == "nt" else 0  # CREATE_NO_WINDOW


def details(text):
    """A program's output as the details of a log entry (log_format indents them)."""
    return "".join(f"\n{DETAIL}{line.strip()}" for line in str(text).splitlines() if line.strip())


def run_tool(name, *args):
    """Runs a helper program and returns its output; a failure stops the build with its message."""
    tool = find_tool(name)
    try:
        result = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True,
                                errors="replace", stdin=subprocess.DEVNULL, creationflags=NO_WINDOW)
    except OSError as error:
        log(f"Running {name} {' '.join(map(str, args))}\n{DETAIL}It could not start: {error!r}")
        raise BuildError(f"The helper program {name} could not start ({error}). An antivirus may have "
                         f"blocked it. Allow {tool.parent} in it, or download the builder again.")
    log(f"Running {name} {' '.join(map(str, args))}" + details(result.stdout + result.stderr))
    if result.returncode != 0:
        raise BuildError(f"{name} failed:\n{(result.stderr or result.stdout).strip()[-800:]}")
    return result.stdout + result.stderr


# ############################################################################
# # Step 1: the disc image                                                  #
# #                                                                         #
# # nodtool reads ISO, RVZ, WIA, WBFS, CISO, GCZ and NFS images alike, and  #
# # prints the disc's ID and revision. Only the USA release, revision 0,    #
# # is accepted: the port's code and the converter are made for it.        #
# #                                                                         #
# # Testers saw a good USA image refused once and taken after restarting    #
# # the builder. Images that cannot be read *yet* explain that: one still   #
# # being copied or unpacked, or one that a cloud folder (OneDrive on       #
# # Windows' Desktop and Documents) keeps online and downloads only when    #
# # opened. So nodtool is tried a few times, those cases are recognised and #
# # named, and the refusal says nodtool's own reason (all of it in the log).#
# ############################################################################

WII_DISC_SIZE = 4_699_979_776  # a whole single-layer Wii disc as an ISO, in bytes
DISC_TRIES = 3

# Windows file attributes of a file kept online by OneDrive and the like (st_file_attributes).
CLOUD_ATTRIBUTES = 0x1000 | 0x40000 | 0x400000  # OFFLINE, RECALL_ON_OPEN, RECALL_ON_DATA_ACCESS


def disc_not_ready(path):
    """Why an image cannot be read right now, or None."""
    try:
        first = path.stat()
    except OSError as error:
        return f"The file cannot be opened ({error.strerror or error})."
    if getattr(first, "st_file_attributes", 0) & CLOUD_ATTRIBUTES:
        return ("This file is still in the cloud (OneDrive or similar). "
                "Right-click it and choose 'Always keep on this device'. Try again once it is downloaded.")
    time.sleep(2)
    try:
        second = path.stat()
    except OSError:
        second = first
    if second.st_size != first.st_size or second.st_mtime != first.st_mtime:
        return "This file is still being copied or unpacked. Wait until that is finished, then try again."
    if path.suffix.lower() == ".iso" and first.st_size < WII_DISC_SIZE:
        return (f"This ISO has {first.st_size / (1 << 30):.2f} GB, a whole Wii disc 4.38 GB. "
                "It is incomplete: still copying, or a broken download. Copy or unpack it again.")
    return None


def read_disc_info(image):
    """nodtool's description of the image, tried again when it fails: images still being written or
    fetched from a cloud folder read fine moments later."""
    for attempt in range(DISC_TRIES):
        try:
            text = run_tool("nodtool", "info", image)
            break
        except BuildError:
            if attempt == DISC_TRIES - 1:
                raise
            log(f"  nodtool info failed, trying again ({attempt + 2}/{DISC_TRIES})")
            time.sleep(1.5)
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


def disc_problem(path):
    """Why a file is not a disc image the build takes (a sentence), or None when it is fine;
    the image's description goes to the output."""
    if path.is_dir():
        return "That is a folder. Choose the disc image file itself (.iso, .wbfs, .rvz and the like)."
    if not path.is_file():
        return f"The file {path} does not exist. Try again."
    try:
        game_id, revision, title, fmt = read_disc_info(path)
    except BuildError as error:
        reason = disc_not_ready(path)
        if reason is None:
            said = [line.strip() for line in str(error).splitlines()[1:] if line.strip()]
            reason = ("This file is not a Wii disc image that can be read" +
                      (f" (nodtool: {said[-1][-200:]})" if said else "") + ". Try another one.")
        return reason
    say(f"  {fmt} image: {title} ({game_id}, revision {revision})")
    if game_id != GAME_ID:
        region = {"RMGJ01": "the Japanese release", "RMGP01": "the European release",
                  "RMGK01": "the Korean release"}.get(game_id)
        return (f"This is {region} of Super Mario Galaxy. Only the USA release (RMGE01) is supported."
                if region else "This is not Super Mario Galaxy. Only the USA release (RMGE01) is supported.")
    if revision != GAME_REVISION:
        return f"Revision {revision} of the disc is not supported, only revision {GAME_REVISION}."
    return None


def choose_disc(given=None, default=None):
    """The disc image, checked; default (the one chosen last time) is offered first."""
    while True:
        prompt = "Disc image of Super Mario Galaxy (USA), or drag it here: "
        text = clean_path_text(given) if given else ask(prompt, "disc", default if default and Path(default).is_file() else None)
        if given:
            say(f"  {text}")
            given = None  # a refused --disc falls back to asking
        if not text:
            problem("  Choose the disc image file.")
            continue
        path = Path(text).expanduser()
        reason = disc_problem(path)
        if reason:
            problem(f"  {reason}")
            continue
        return path


def free_space(path):
    return shutil.disk_usage(path).free


def extraction_ok(target):
    """An extraction with the game's files and the original main.dol."""
    dol = target / "sys" / "main.dol"
    return (target / "files" / "ObjectData").is_dir() and dol.is_file() \
        and hashlib.sha1(dol.read_bytes()).hexdigest() == MAIN_DOL_SHA1


def extract_disc(image, work, name="disc", check_space=True):
    if check_space and free_space(work) < WORK_SPACE:
        raise BuildError(f"Not enough free space in {work}. About {WORK_SPACE >> 30} GB are needed, "
                         f"{free_space(work) >> 30} GB are free. Free some space and run the builder again.")
    target = work / name
    # Extracted beside it first and renamed once whole: an extraction cut short (the window closed,
    # the computer off) never leaves a half disc under the name that kept_folder() trusts, whose
    # check (ObjectData and main.dol) the first files out already pass.
    partial = work / f"{name}.partial"
    shutil.rmtree(partial, ignore_errors=True)
    run_tool("nodtool", "extract", "-q", image, partial)
    dol = partial / "sys" / "main.dol"
    if not (partial / "files" / "ObjectData").is_dir() or not dol.is_file():
        raise BuildError("The disc was extracted, but its files are not where they should be.")
    if not extraction_ok(partial):
        raise BuildError("This disc's main.dol is not the original. The disc image is modified or damaged. "
                         "Make a new copy of your disc.")
    shutil.rmtree(target, ignore_errors=True)
    partial.rename(target)
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

# What the converter says for its own command line, not for the builder: where to copy the cache
# (the builder copies it) and where its files went (said by the builder).
CONVERTER_NOT_LOGGED = ("Copy the cache folder to sdmc:/smg/cache together with files and native.",
                        "Done. The game's files were overwritten in this folder.")


# Its headings, in the log's words.
CONVERTER_WORDS = {"Converted:": "Files converted in this build:"}


class ConverterOutput:
    """Stands in for stdout while the converter runs."""

    def __init__(self, stream):
        self.stream, self.pending = stream, ""

    def write(self, text):
        self.pending += text
        lines = self.pending.split("\n")
        self.pending = lines.pop()
        for line in lines:
            if not line.startswith("@@PROGRESS ") and line.strip() not in CONVERTER_NOT_LOGGED:
                said = CONVERTER_WORDS.get(line.strip(), line.strip())
                log(DETAIL + said if said else "")  # details of "The converter's report"
            if line.startswith("@@PROGRESS "):
                # "@@PROGRESS step steps percent name|detail" (convert_smg_files._progress)
                parts = line.split(" ", 4)
                name, _, detail = (parts[4] if len(parts) > 4 else "").partition("|")
                percent = float(parts[3]) if len(parts) > 3 else -1
                part = f"part {parts[1]} of {parts[2]}" if len(parts) > 2 and parts[1] != "0" else ""
                UI.progress(name, percent, part if isinstance(UI, TextUI) else ", ".join(filter(None, (part, detail))))
            elif line.startswith(("Warning", "Error", "Native conversion found", "Preflight found", "Conversion stopped")):
                UI.say("  " + line)
        return len(text)

    def flush(self):
        if self.stream:
            self.stream.flush()

    def isatty(self):
        return False

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
    log("The converter's report:")
    try:
        code = converter.main()
    except SystemExit as stop:
        code = stop.code
    finally:
        sys.argv, sys.stdout, sys.stderr = saved_argv, saved_out, saved_err
    end_progress()
    if code not in (0, None):
        raise BuildError(f"The conversion stopped with an error ({code}). The details are in the log.")
    missing = [name for name in SD_ITEMS if name not in OPTIONAL_SD_ITEMS and not (disc / name).exists()]
    if missing:
        raise BuildError(f"The conversion finished without writing {', '.join(missing)}.")
    say("  The game files are ready for the 3DS.")


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


def windows_drives():
    """(letter, removable) of every drive on Windows, for the window's SD card list."""
    import ctypes
    mask = ctypes.windll.kernel32.GetLogicalDrives()
    system = Path(os.environ.get("SystemDrive", "C:")).drive.rstrip(":").upper()
    return [(chr(65 + n), removable_drive(chr(65 + n))) for n in range(26) if mask >> n & 1 and chr(65 + n) != system]


def choose_sd_card(settings):
    say("Insert your 3DS's SD card into this computer.")
    last = settings.get("last_sd_letter")
    while True:
        if os.name == "nt":
            prompt = f"Drive letter of the SD card (Enter for {last}): " if last else "Drive letter of the SD card (for example E): "
            letter = (ask(prompt, "sd", last) or last or "").rstrip(":\\/").upper()
            if len(letter) != 1 or not letter.isalpha():
                problem("  Type just the letter.")
                continue
            card = Path(f"{letter}:\\")
            if not card.is_dir():
                problem(f"  There is no drive {letter}:. Check the letter in File Explorer.")
                continue
            if letter == Path(os.environ.get("SystemDrive", "C:")).drive.rstrip(":").upper():
                problem("  That is this computer's system drive, not an SD card.")
                continue
            if not removable_drive(letter) and not ask_yes(f"  {letter}: is not a removable drive. Use it anyway?"):
                continue
        else:
            drives = mounted_drives()
            for number, drive in enumerate(drives, 1):
                say(f"  {number}. {drive}")
            answer = ask("Number of the SD card above, or its full path: ", "sd")
            card = drives[int(answer) - 1] if answer.isdigit() and 0 < int(answer) <= len(drives) else Path(answer).expanduser()
            if not answer or not card.is_dir():
                problem("  That folder does not exist.")
                continue
        try:
            probe = card / ".galaxyctr-write-test"
            probe.write_bytes(b"")
            probe.unlink()
        except OSError:
            problem("  The card cannot be written. Check its lock switch and try again.")
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
    # An update removes the old converted files first (saves are in smg/nand and stay), with its
    # progress; a first copy says so instead (owner, 26 Nov 2026).
    (smg / SENT_LIST).unlink(missing_ok=True)  # written again at the end (SENT_LIST)
    old_folders = [smg / item for item in SD_ITEMS[:-1] if (smg / item).is_dir()]
    if old_folders:
        say("  Updating the game on the SD card: its old files are removed first. Your saves stay.")
        old_files = [f for folder in old_folders for f in folder.rglob("*") if f.is_file()]
        for number, path in enumerate(old_files, 1):
            path.unlink(missing_ok=True)
            if number % 25 == 0 or number == len(old_files):
                progress("Deleting the old files", 100 * number / len(old_files), f"{number}/{len(old_files)} files")
        for folder in old_folders:
            shutil.rmtree(folder, ignore_errors=True)
        end_progress()
    else:
        say("  First copy to this SD card.")
    files = sum(1 for item in SD_ITEMS if (disc / item).is_dir() for f in (disc / item).rglob("*") if f.is_file())
    copied, sent, started = 0, 0, time.monotonic()
    for item in SD_ITEMS:
        source, target = disc / item, smg / item
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
            sent += 1
            progress("Copying", 100 * copied / needed,
                     ", ".join(filter(None, (f"{sent}/{files} files", speed_text(copied, time.monotonic() - started, needed)))))
    end_progress()
    (card / "cias").mkdir(exist_ok=True)
    shutil.copyfile(cia, card / "cias" / CIA_NAME)
    sent_files = {"/smg/" + path.relative_to(disc).as_posix(): path.stat().st_size for item in SD_ITEMS
                  for path in ([disc / item] if (disc / item).is_file() else (disc / item).rglob("*")) if path.is_file()}
    (smg / SENT_LIST).write_text(sent_list_text(sent_files), encoding="utf-8")
    say(f"  Copied in {duration(time.monotonic() - started)}. The game files are in {smg}.")
    say(f"  The CIA is in {card / 'cias' / CIA_NAME}.")


def check_dsp_firmware(card):
    """The 3DS's sound needs the console's own DSP firmware, which nobody may distribute."""
    if (card / "3ds" / "dspfirm.cdc").is_file():
        return True
    say("  Note: sdmc:/3ds/dspfirm.cdc is missing, so the game would have no sound.")
    say("  Run the homebrew app 'DSP1' once on your 3DS. It saves that file from your own console.")
    return False


# ############################################################################
# # Step 5 over Wi-Fi: FTP                                                  #
# #                                                                         #
# # The same files sent to an FTP server app running on the 3DS (ftpd and   #
# # the like), at the address it shows (ip:port). As on a card in this      #
# # computer, the old smg folders (SD_ITEMS) are deleted first            #
# # and smg/nand (the saves) is left alone.                                 #
# #                                                                         #
# # The game is thousands of files, and over Wi-Fi each one costs a pause   #
# # (opening it on the 3DS's SD card, the server's answers), so several     #
# # FTP connections work at once (ftp_connections, 5 unless the user picks  #
# # 1 to 10): one connection lists and makes the folders, then every        #
# # connection takes the next file from a shared queue, largest first, both #
# # to delete the old files and to send the new ones. A file whose transfer #
# # breaks is sent again on a new connection (FTP_TRIES times); a           #
# # connection the server refuses (too many at once) is simply not used.    #
# ############################################################################

FTP_DEFAULT_PORT = 5000  # ftpd's
FTP_DEFAULT_CONNECTIONS = 5
FTP_MAX_CONNECTIONS = 10
FTP_TRIES = 3


def parse_ftp_address(text):
    """'192.168.1.10:5000' -> ('192.168.1.10', 5000); None when it is not an address."""
    host, _, port = text.strip().partition(":")
    parts = host.split(".")
    if len(parts) != 4 or not all(p.isdigit() and int(p) < 256 for p in parts):
        return None
    if port and not (port.isdigit() and 0 < int(port) < 65536):
        return None
    return host, int(port) if port else FTP_DEFAULT_PORT


def parse_connections(text, default):
    """The number of FTP connections typed (Enter for the default); None when it is not 1 to 10."""
    text = str(text).strip()
    if not text:
        return default
    return int(text) if text.isdigit() and 1 <= int(text) <= FTP_MAX_CONNECTIONS else None


def ftp_connections(settings):
    value = parse_connections(settings.get("ftp_connections") or "", FTP_DEFAULT_CONNECTIONS)
    return value or FTP_DEFAULT_CONNECTIONS


def choose_ftp_address(settings):
    say("Start an FTP app on your 3DS, for example ftpd. Connect it to the same Wi-Fi as this computer.")
    last = settings.get("ftp_address")
    connections = ftp_connections(settings)
    while True:
        prompt = f"Address it shows, as ip:port (Enter for {last}): " if last else "Address it shows, as ip:port (for example 192.168.1.10:5000): "
        text, count = UI.ask_ftp(prompt, last, connections)
        text = str(text or "").strip() or last or ""
        log(f"FTP address {text}, connections {count}")
        address = parse_ftp_address(text)
        if address is None:
            problem("  Type the address and the port, for example 192.168.1.10:5000.")
            continue
        count = parse_connections(count, connections)
        if count is None:
            problem(f"  Type a number of connections from 1 to {FTP_MAX_CONNECTIONS}.")
            continue
        settings.set("ftp_address", f"{address[0]}:{address[1]}")
        settings.set("ftp_connections", count)
        return address, count


def ftp_connect(ftplib, address):
    ftp = ftplib.FTP()
    ftp.connect(address[0], address[1], timeout=30)
    ftp.login()
    ftp.set_pasv(True)
    return ftp


def ftp_close(ftp):
    try:
        ftp.quit()
    except Exception:  # noqa: BLE001 - a broken connection is closed all the same
        ftp.close()


def ftp_list(ftp, ftplib, path):
    """(name, is_folder) of everything in a folder on the 3DS; None when it does not exist. The
    folder is entered and then listed: some servers list their root for a listing given a path."""
    try:
        ftp.cwd(path)
    except ftplib.error_perm:
        return None
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
    return [(name, folder) for name, folder in entries if name not in (".", "..")]


def ftp_walk(ftp, ftplib, path, files, folders, looked=None):
    """Adds every file under a folder on the 3DS to files, and the folders to folders, each one
    before what it holds. looked(): called after each folder is listed (the progress)."""
    entries = ftp_list(ftp, ftplib, path)
    if entries is None:
        return
    folders.append(path)
    if looked is not None:
        looked()
    for name, is_folder in entries:
        if is_folder:
            ftp_walk(ftp, ftplib, f"{path}/{name}", files, folders, looked)
        else:
            files.append(f"{path}/{name}")


# ############################################################################
# # The list of the files sent (smg/galaxyctr_files.txt)                   #
# #                                                                         #
# # Written on the card at the end of every copy (FTP or SD card): each    #
# # game file sent and its size, signed. The next update reads it instead #
# # of looking through the 3DS's 1834 folders one by one, which took       #
# # minutes over FTP (owner, 26 Nov 2026). It is never trusted blindly: a  #
# # list edited by hand fails its signature, and twenty of its files,      #
# # picked at random, must be on the card with their sizes; a list that    #
# # fails, or is gone, means the folders are looked through as before. It  #
# # is deleted when an update starts and written again when it ends, so a  #
# # copy cut short never leaves a list of what is not there.               #
# ############################################################################
SENT_LIST = "galaxyctr_files.txt"
SENT_KEY = b"GalaxyCTR: the game files the builder sent to this card"
SENT_HEADER = "GalaxyCTR builder: the game files sent to this card. Written by the builder, do not edit."


def sent_list_text(entries):
    """The list's text for {path on the card: size}, the paths as /smg/..."""
    import hmac
    body = SENT_HEADER + "\n" + "".join(f"{size} {path}\n" for path, size in sorted(entries.items()))
    seal = hmac.new(SENT_KEY, body.encode("utf-8"), hashlib.sha256).hexdigest()
    return body + f"seal {seal}\n"


def read_sent_list(text):
    """{path: size} from the list's text, or None when it is not one the builder wrote."""
    import hmac
    if not text or not text.startswith(SENT_HEADER + "\n"):
        return None
    body, _, last = text.rstrip("\n").rpartition("\n")
    if not last.startswith("seal "):
        return None
    body += "\n"
    seal = hmac.new(SENT_KEY, body.encode("utf-8"), hashlib.sha256).hexdigest()
    if not hmac.compare_digest(seal, last[5:].strip()):
        return None
    entries = {}
    for line in body.splitlines()[1:]:
        size, _, path = line.partition(" ")
        if not size.isdigit() or not path.startswith("/smg/"):
            return None
        entries[path] = int(size)
    return entries


def sent_folders(paths):
    """The folders under smg's game folders that hold paths, each before what it holds."""
    folders = set()
    for path in paths:
        parts = path.strip("/").split("/")
        for depth in range(3, len(parts)):  # /smg/<item> and deeper
            folders.add("/" + "/".join(parts[:depth]))
    return sorted(folders, key=lambda folder: (folder.count("/"), folder))


def ftp_sent_list(ftp, ftplib):
    """The list on the 3DS when it is the builder's and still true there, else None. Deleted
    either way: it is written again when this copy ends."""
    import io
    import random
    data = io.BytesIO()
    try:
        ftp.retrbinary(f"RETR /smg/{SENT_LIST}", data.write)
    except ftplib.all_errors:
        return None
    try:
        ftp.delete(f"/smg/{SENT_LIST}")
    except ftplib.all_errors:
        pass
    entries = read_sent_list(data.getvalue().decode("utf-8", "replace"))
    if not entries:
        if data.getvalue():
            log("  The list of the files sent last time is not the builder's: the 3DS's folders are looked through.")
        return None
    try:
        ftp.voidcmd("TYPE I")
        for path in random.sample(sorted(entries), min(20, len(entries))):
            if ftp.size(path) != entries[path]:
                log(f"  {path} differs from the list of the files sent last time: the 3DS's folders are looked through.")
                return None
    except ftplib.all_errors as error:
        log(f"  A file of the list of the files sent last time is not on the 3DS ({error}): its folders are looked through.")
        return None
    return entries


def ftp_walk_parallel(ftplib, address, count, roots, files, folders, looked=None):
    """ftp_walk on up to count connections at once (chosen in the copy's window, as the copy
    itself): the 3DS lists a folder at a time per connection, and one connection took over
    13 minutes for a whole game (owner, 26 Nov 2026). Each folder is still added before what it
    holds: its subfolders are only queued once it is listed."""
    waiting = queue.Queue()
    for root in roots:
        waiting.put(root)
    lock = threading.Lock()
    state = {"left": len(roots), "workers": 0}
    errors = []

    def worker():
        ftp = None
        try:
            while True:
                with lock:
                    if state["left"] == 0 or errors:
                        return
                try:
                    path = waiting.get(timeout=0.2)
                except queue.Empty:
                    continue
                for attempt in range(1, FTP_TRIES + 1):
                    try:
                        if ftp is None:
                            ftp = ftp_connect(ftplib, address)
                        entries = ftp_list(ftp, ftplib, path)
                        break
                    except (OSError, EOFError, ftplib.Error) as error:
                        ftp_close(ftp) if ftp is not None else None
                        ftp = None
                        with lock:
                            others = state["workers"] - 1
                        if attempt == FTP_TRIES or (others > 0 and attempt > 1):
                            waiting.put(path)  # another connection lists it, or the walk fails
                            if others <= 0:
                                errors.append(error)
                            return
                        time.sleep(attempt)
                with lock:
                    if entries is not None:
                        folders.append(path)
                        for name, is_folder in entries:
                            if is_folder:
                                waiting.put(f"{path}/{name}")
                                state["left"] += 1
                            else:
                                files.append(f"{path}/{name}")
                    state["left"] -= 1
                if entries is not None and looked is not None:
                    looked()
        finally:
            if ftp is not None:
                ftp_close(ftp)
            with lock:
                state["workers"] -= 1

    threads = []
    for _ in range(max(1, count)):
        with lock:
            state["workers"] += 1
        thread = threading.Thread(target=worker, daemon=True)
        thread.start()
        threads.append(thread)
        time.sleep(0.05)  # servers on the 3DS take kindly to connections a moment apart
    for thread in threads:
        thread.join()
    if errors:
        raise errors[0]
    if state["left"]:
        raise OSError("the FTP server stopped answering")


def ftp_make_folder(ftp, ftplib, path):
    try:
        ftp.mkd(path)
    except ftplib.error_perm:
        pass  # already there


def plural(count, word):
    """'1 connection', '5 connections'."""
    return f"{count} {word}" + ("" if count == 1 else "s")


def speed_text(done, seconds, total):
    """'2.4 MB/s, about 3 min left' for a copy in progress."""
    if seconds < 1 or done <= 0:
        return ""
    speed = done / seconds
    left = (total - done) / speed if speed else 0
    eta = f"{int(left)} s" if left < 90 else f"{round(left / 60)} min"
    return f"{speed / (1 << 20):.1f} MB/s, about {eta} left"


def ftp_parallel(ftplib, address, count, jobs, work, label, sizes=None, unit="files"):
    """Runs work(ftp, job, sent) for every job on up to count connections at once. sent(n) counts n
    bytes sent; with sizes (one per job) progress is in bytes, otherwise in jobs. Raises BuildError
    when a job still fails after FTP_TRIES tries, or no connection could be opened."""
    order = sorted(range(len(jobs)), key=lambda i: -sizes[i]) if sizes else range(len(jobs))
    waiting = queue.Queue()
    for index in order:
        waiting.put(index)
    total = sum(sizes) if sizes else len(jobs)
    lock = threading.Lock()
    state = {"done": 0, "open": 0, "opened": 0, "jobs_done": 0}
    errors, refused = [], []
    stop = threading.Event()

    def count_done(amount):
        with lock:
            state["done"] += amount

    def worker(number):
        ftp = None
        try:
            try:
                ftp = ftp_connect(ftplib, address)
            except (OSError, EOFError, ftplib.Error) as error:
                refused.append(f"connection {number + 1}: {error}")
                return
            with lock:
                state["open"] += 1
                state["opened"] += 1
            while not stop.is_set():
                try:
                    index = waiting.get_nowait()
                except queue.Empty:
                    return
                for attempt in range(1, FTP_TRIES + 1):
                    part = [0]

                    def sent(amount, part=part):
                        part[0] += amount
                        if sizes:
                            count_done(amount)

                    try:
                        work(ftp, jobs[index], sent)
                        count_done(0 if sizes else 1)
                        with lock:
                            state["jobs_done"] += 1
                        break
                    except (OSError, EOFError, ftplib.Error) as error:
                        if sizes:
                            count_done(-part[0])
                        with lock:
                            others = state["open"] - 1
                        if str(error)[:3] in ("421", "425") and others > 0:
                            # The server has too many connections: this one steps back and leaves
                            # the file to the others.
                            waiting.put(index)
                            log(f"  FTP connection {number + 1} closes, the server is busy ({error}); the others go on")
                            return
                        log(f"  FTP connection {number + 1}: {jobs[index]!r} failed ({error}), try {attempt} of {FTP_TRIES}")
                        if attempt == FTP_TRIES:
                            errors.append(error)
                            stop.set()
                            return
                        ftp_close(ftp)
                        time.sleep(attempt)
                        try:
                            ftp = ftp_connect(ftplib, address)
                        except (OSError, EOFError, ftplib.Error) as again:
                            with lock:
                                others = state["open"] - 1
                            if others > 0:  # the file goes back to the queue for another connection
                                waiting.put(index)
                                log(f"  FTP connection {number + 1} could not reconnect ({again}); the others go on")
                                return
                            errors.append(again)
                            stop.set()
                            return
        finally:
            if ftp is not None:
                ftp_close(ftp)
            with lock:
                state["open"] = max(0, state["open"] - (1 if ftp is not None else 0))

    threads = [threading.Thread(target=worker, args=(n,), daemon=True) for n in range(min(count, max(1, len(jobs))))]
    started = time.monotonic()
    for thread in threads:
        thread.start()
        time.sleep(0.05)  # servers on the 3DS take kindly to connections a moment apart
    samples = deque([(started, 0)])
    while any(thread.is_alive() for thread in threads):
        time.sleep(0.25)
        with lock:
            done, connections, files = state["done"], state["open"], state["jobs_done"]
        now = time.monotonic()
        samples.append((now, done))
        while len(samples) > 2 and now - samples[0][0] > 6:
            samples.popleft()  # the speed of the last few seconds
        if sizes:
            then, before = samples[0]
            speed = (done - before) / max(now - then, 0.001)
            left = (total - done) / speed if speed > 0 else 0
            eta = "" if speed <= 0 else (f", about {int(left)} s left" if left < 90 else f", about {round(left / 60)} min left")
            # The files sent too, as the copy to the SD card says (owner, 26 Nov 2026).
            detail = f"{files}/{len(jobs)} files, {speed / (1 << 20):.1f} MB/s, {plural(connections, 'connection')}{eta}"
        else:
            detail = f"{done}/{total} {unit}, {plural(connections, 'connection')}"
        progress(label, 100 * done / total if total else 100.0, detail)
    end_progress()
    for thread in threads:
        thread.join()
    if refused:
        log(f"  {len(refused)} of {len(threads)} FTP connections were refused: " + "; ".join(refused))
    if errors:
        raise errors[0]
    if state["jobs_done"] < len(jobs):
        raise OSError(refused[0] if refused else "the FTP server stopped answering")
    log(f"  {label}: {len(jobs)} files with {state['opened']} connections in {time.monotonic() - started:.1f} s")


def copy_over_ftp(disc, cia, address, connections=FTP_DEFAULT_CONNECTIONS):
    try:
        import ftplib
    except ImportError:
        raise BuildError("This builder program cannot copy over FTP. Copy to the SD card in this computer, "
                         f"or download the latest builder. {NEW_PROGRAM}")
    files = [(f, f"/smg/{item}" + ("" if f == disc / item else "/" + f.relative_to(disc / item).as_posix()))
             for item in SD_ITEMS
             for f in ([disc / item] if (disc / item).is_file() else sorted((disc / item).rglob("*")))
             if f.is_file()]
    files.append((cia, f"/cias/{CIA_NAME}"))
    host, port = address
    say(f"  Connecting to {host}:{port}...")
    started = time.monotonic()
    try:
        ftp = ftp_connect(ftplib, address)
        say(f"  Connected. Copying with up to {plural(connections, 'connection')} at once. Over Wi-Fi this can take a "
            "while. Keep the 3DS on and near the router.")

        # The old converted files (saves are in smg/nand): listed here, deleted by every connection.
        # Only for an update: on a first copy smg holds none of them (or is not there), and this
        # step is not shown (owner, 26 Nov 2026). Listing them is long on the 3DS (a folder at a
        # time), so it shows its progress: the folders looked at, of about as many as this build has.
        old_files, old_folders = [], []
        there = {name for name, _ in (ftp_list(ftp, ftplib, "/smg") or [])}
        update = [item for item in SD_ITEMS[:-1] if item in there]
        if update:
            expected = max(1, sum(1 for item in SD_ITEMS[:-1] if (disc / item).is_dir()
                                  for f in [disc / item, *(disc / item).rglob("*")] if f.is_dir()))
            looked = [0]

            def count_folder():
                looked[0] += 1
                progress("Looking at the old files on the 3DS", min(99.0, 100 * looked[0] / expected),
                         f"{looked[0]}/{max(expected, looked[0])}")

            say("  Updating the game on the 3DS. Your saves stay.")
            lock = threading.Lock()

            def count_safely():
                with lock:
                    count_folder()

            listed = ftp_sent_list(ftp, ftplib)
            if listed is not None:
                old_files = [path for path in listed if any(path.startswith(f"/smg/{item}/") for item in update)]
                old_folders = [folder for folder in sent_folders(old_files)]
                log(f"  The list of the files sent last time: {len(old_files)} files, no need to look through the folders.")
            else:
                ftp_close(ftp)  # the walk's own connections, no more than the user chose
                ftp_walk_parallel(ftplib, address, connections, [f"/smg/{item}" for item in update],
                                  old_files, old_folders, count_safely)
                end_progress()
                ftp = ftp_connect(ftplib, address)
        else:
            say("  First copy to this 3DS.")
        # Only what this build no longer has is deleted: every other file is written over when
        # sent. Deleting all of them first took as long as the copy (owner, 26 Nov 2026).
        sent_paths = {remote for _, remote in files}
        needed = {"/smg", "/cias"} | {"/" + "/".join(remote.strip("/").split("/")[:depth])
                                     for remote in sent_paths
                                     for depth in range(1, remote.strip("/").count("/") + 1)}
        unused = [remote for remote in old_files if remote not in sent_paths]
        if unused:
            ftp_close(ftp)

            def delete(connection, remote, sent):
                try:
                    connection.delete(remote)
                except ftplib.error_perm:
                    pass  # already gone

            ftp_parallel(ftplib, address, connections, unused, delete, "Deleting the files no longer used")
            ftp = ftp_connect(ftplib, address)
        for folder in reversed(old_folders):  # each folder after what it holds
            if folder not in needed:
                try:
                    ftp.rmd(folder)
                except ftplib.error_perm:
                    pass
        ftp_close(ftp)

        # The folders still missing, a level at a time (each after the one that holds it), on all
        # the chosen connections; one at a time over one connection took minutes on the 3DS.
        missing = sorted(needed - set(old_folders) - {"/"})
        for depth in sorted({folder.count("/") for folder in missing}):
            level = [folder for folder in missing if folder.count("/") == depth]

            def make(connection, folder, sent):
                ftp_make_folder(connection, ftplib, folder)

            ftp_parallel(ftplib, address, connections, level, make, "Making the folders", unit="folders")

        def upload(connection, job, sent):
            path, remote = job
            with open(path, "rb") as source:
                connection.storbinary(f"STOR {remote}", source, blocksize=1 << 16, callback=lambda block: sent(len(block)))

        ftp_parallel(ftplib, address, connections, files, upload, "Copying", [f.stat().st_size for f, _ in files])

        ftp = ftp_connect(ftplib, address)
        import io
        listing = sent_list_text({remote: path.stat().st_size for path, remote in files if remote.startswith("/smg/")})
        ftp.storbinary(f"STOR /smg/{SENT_LIST}", io.BytesIO(listing.encode("utf-8")))
        try:
            ftp.size("/3ds/dspfirm.cdc")
            sound = True
        except ftplib.all_errors:
            sound = False
        ftp_close(ftp)
    except (OSError, EOFError, ftplib.Error) as error:
        end_progress()
        raise BuildError(f"The FTP copy stopped ({error}). Check the address and that the FTP app is still running.")
    say(f"  Copied in {duration(time.monotonic() - started)}. The game files are in smg, the CIA in cias.")
    if not sound:
        say("  Note: the SD card has no 3ds/dspfirm.cdc, so the game would have no sound.")
        say("  Run the homebrew app 'DSP1' once on your 3DS. It saves that file from your own console.")
    return sound


# ############################################################################
# # When the files are not copied now                                       #
# #                                                                         #
# # They stay where they were made, and the steps to copy them by hand are  #
# # printed and saved as install_guide.txt next to the builder.             #
# ############################################################################

def write_install_guide(disc, cia, work, keep):
    items = ", ".join(SD_ITEMS)
    names = [item for item in SD_ITEMS if "." not in item]
    folders = ", ".join(names[:-1]) + " and " + names[-1]
    lines = [
        "How to put GalaxyCTR on your 3DS",
        "",
        "Your files",
        f"  Game files: {disc}",
        f"  CIA: {cia}",
        "",
        "1. Put the 3DS's SD card into this computer. Or connect to an FTP app on the 3DS. "
        "The folders below are the same.",
        "2. On the SD card, open the folder smg (create it if it is missing).",
        # The folders the copies replace (copy_to_sd, copy_over_ftp), from the same list.
        f"3. In smg, delete the old folders {folders} if they are there. "
        "Do not touch the folder nand: your saves are in it.",
        f"4. Copy {items} from the game files folder into smg.",
        f"5. Copy {CIA_NAME} into the folder cias on the SD card. Create it if it is missing.",
        "6. For the sound, the SD card needs the file 3ds/dspfirm.cdc. If it is missing, run the "
        "homebrew app DSP1 once on your 3DS.",
        f"7. Put the SD card back into the 3DS. Install cias/{CIA_NAME} with FBI or another CIA installer.",
    ]
    if keep:
        # The folder's name only: its whole path is in "Game files" above (the folder that holds
        # game), and with it the step wrapped on the result page (owner, 26 Nov 2026).
        lines.append(f"8. Keep the folder {Path(work).name}: the next build updates it and is much faster.")
    else:
        lines.append(f"8. When everything is copied, delete the folder {work} to free the space.")
    say()
    for line in lines:
        say(line)
    guide = builder_home() / GUIDE_NAME
    RESULT["guide"] = guide
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
    say(f"The builder can keep the game files it makes after each build (about {KEEP_SPACE >> 30} GB).")
    say("The next updates then convert only what changed, without the disc. So they are much faster.")
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
        raise BuildError(f"Not enough free space for {folder}. About {-(-needed >> 30)} GB more are needed, "
                         f"{free >> 30} GB are free. Free some space, or move the folder and change work_folder "
                         f"in {SETTINGS_NAME}.")


def choose_keep_folder(settings, default):
    """Asked when the files are kept for the first time (or the kept folder is gone): where."""
    say(f"  The kept game files need about {KEEP_SPACE >> 30} GB. Choose a drive with that much free space.")
    while True:
        answer = ask(f"Folder for the kept game files (Enter for {default}): ", "folder", str(default))
        folder = Path(answer).expanduser() if answer else default
        try:
            folder.mkdir(parents=True, exist_ok=True)
            probe = folder / ".galaxyctr-write-test"
            probe.write_bytes(b"")
            probe.unlink()
        except OSError:
            problem("  That folder cannot be made or written. Choose another one.")
            continue
        needed, free = keep_space_needed(folder), free_space(folder)
        if free < needed:
            problem(f"  That drive has {free >> 30} GB free, about {-(-needed >> 30)} GB are needed. Choose another one.")
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
        return True, copy_over_ftp(disc, cia, *choose_ftp_address(settings))
    card = choose_sd_card(settings)
    copy_to_sd(disc, cia, card)
    return True, check_dsp_firmware(card)


# How the last build ended, for the window's last page: copied (bool), sound (the DSP firmware was
# found), work (the folder of the files), guide (install_guide.txt when written), keep.
RESULT = {}


def build(disc_arg=None, sd_arg=None, work_arg=None):
    RESULT.clear()
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
        say("  Using the game files kept from the last build. The disc image is not needed.")
        say(f"  They are in {kept}.")
        work = kept
    else:
        image = choose_disc(disc_arg, settings.get("disc_image"))
        settings.set("disc_image", image)
        # Work next to the disc image by default: it is on a drive that already holds 4.7 GB of game.
        parent = work_arg or (image.parent if os.access(image.parent, os.W_OK) else None)
        if keep:
            work = choose_keep_folder(settings, Path(parent or data_home()) / "GalaxyCTR-files")
        else:
            work = Path(tempfile.mkdtemp(prefix="galaxyctr-", dir=parent))
    if not kept:  # with the kept files, the line before already said where they are
        say(f"  {'The game files will be kept in' if keep else 'Working in the temporary folder'} {work}.")
    RESULT.update(work=work, keep=keep)
    if kept:
        check_keep_space(work)

    copied, sound, failed = False, None, True
    try:
        if keep:
            # The disc as extracted stays in original/, the converted game in game/: each build
            # converts again only what the new release changed.
            if image is None:
                step(2, total, "Extracting the disc (already done in an earlier build)")
                originals = work / "original"
            else:
                step(2, total, "Extracting the disc (about a minute)")
                originals = extract_disc(image, work, "original", check_space=False)  # checked for the whole folder
            disc = work / "game"
            make_icon_and_banner(originals, work)
            make_window_graphics(originals / "files")
            make_loading_screen(originals / "files", disc)
            refresh_look_disc(originals / "files", image)
            step(3, total, "Converting the game files for the 3DS (only what changed)")
            convert_files(disc, originals)
        else:
            step(2, total, "Extracting the disc (about a minute)")
            disc = extract_disc(image, work)
            make_icon_and_banner(disc, work)
            make_window_graphics(disc / "files")
            make_loading_screen(disc / "files", disc)
            refresh_look_disc(disc / "files", image)
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
        RESULT.update(copied=copied, sound=sound)
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
    say("If the SD card is in this computer, put it back into your 3DS.")
    say(f"Then install cias/{CIA_NAME} with FBI or another CIA installer.")


# ############################################################################
# # Starting                                                                #
# #                                                                         #
# # main() opens the log, then shows the window when it can                 #
# # (builder_gui.available: Qt is in the builder program, and on Linux a    #
# # display works), or runs in the terminal. run_build() is the build with  #
# # its error reporting; the window runs it in a thread and can run it      #
# # again. On Windows the builder program with a window has no terminal, so #
# # the terminal mode opens one (open_console).                             #
# ############################################################################

LOG_PATH = Path.home() / "galaxyctr-builder-log.txt"
NEW_PROGRAM = "https://github.com/Im-Monee/galaxyctr-beta/releases/latest"


# ############################################################################
# # Replacing a builder program from before the window                      #
# #                                                                         #
# # The first builder programs (launcher version 1) carry no Qt, but they   #
# # still download each release's source code and run this file. Without a  #
# # window, main() then downloads the latest release's program for this     #
# # system and puts it in place of the running one: the running program is  #
# # renamed to <name>.replaced (Windows lets a running program be renamed,  #
# # not deleted), the new one takes its name, starts, and this one closes.  #
# # The new launcher waits for this process to end and deletes the         #
# # .replaced file (launcher.finish_replacement). Anything that fails puts  #
# # the old program back and the terminal build runs as before.             #
# ############################################################################

LATEST_RELEASE = "https://api.github.com/repos/Im-Monee/galaxyctr-beta/releases/latest"
REPLACED_SUFFIX = ".replaced"  # launcher.REPLACED_SUFFIX
PROGRAM_STARTS = (b"MZ", b"\x7fELF", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe")  # Windows, Linux, macOS


def https_context():
    import ssl
    try:
        import certifi  # in every builder program: macOS has no certificates for Python otherwise
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()


def program_environment():
    """This process's environment without what this program and its launcher added: the new
    program's own launcher (PyInstaller's too) must start as if from the desktop."""
    env = {k: v for k, v in os.environ.items() if not k.startswith(("_MEI", "_PYI"))
           and k not in ("GALAXYCTR_TOOLS", "GALAXYCTR_ELF", "GALAXYCTR_RELEASE", "GALAXYCTR_GUI")}
    env["PYINSTALLER_RESET_ENVIRONMENT"] = "1"
    for name in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):  # PyInstaller keeps the user's own as *_ORIG
        if name + "_ORIG" in env:
            env[name] = env.pop(name + "_ORIG")
        else:
            env.pop(name, None)
    return env


def replace_program():
    """Puts the latest release's program in place of this one and starts it. True when it started."""
    import json
    import urllib.request
    exe = Path(sys.executable).resolve()
    name = f"GalaxyCTR-Builder-{system_key()}" + (".exe" if os.name == "nt" else "")
    new, old = exe.with_name(exe.name + ".download"), exe.with_name(exe.name + REPLACED_SUFFIX)
    say("A new builder program with a window is out. Downloading it: it then starts in place of this one.")
    try:
        context = https_context()
        ask = urllib.request.Request(LATEST_RELEASE, headers={"Accept": "application/vnd.github+json",
                                                              "User-Agent": "GalaxyCTR-builder"})
        with urllib.request.urlopen(ask, timeout=30, context=context) as answer:
            release = json.load(answer)
        asset = next((a for a in release.get("assets", []) if a.get("name") == name), None)
        if asset is None:
            raise ValueError(f"release {release.get('tag_name')} has no {name}")
        ask = urllib.request.Request(asset["browser_download_url"], headers={"User-Agent": "GalaxyCTR-builder"})
        size, done, shown = int(asset.get("size") or 0), 0, -1
        with urllib.request.urlopen(ask, timeout=60, context=context) as answer, new.open("wb") as out:
            while True:
                block = answer.read(1 << 20)
                if not block:
                    break
                out.write(block)
                done += len(block)
                if size and done * 10 // size > shown:
                    shown = done * 10 // size
                    print(f"  {shown * 10}% of {size >> 20} MB", flush=True)
        if (size and done != size) or done < (1 << 20):
            raise ValueError(f"the download stopped at {done} bytes")
        with new.open("rb") as check:
            if not check.read(4).startswith(PROGRAM_STARTS):
                raise ValueError("the download is not a program")
        if old.exists():
            old.unlink()
        os.replace(exe, old)
        try:
            os.replace(new, exe)
        except OSError:
            os.replace(old, exe)
            raise
        if os.name != "nt":
            exe.chmod(0o755)
    except Exception as error:  # noqa: BLE001 - the terminal build runs anyway
        try:
            new.unlink()
        except OSError:
            pass
        log(f"Replacing the builder program failed ({error!r}).")
        say(f"  It could not be put in place ({error}). Download it from {NEW_PROGRAM}")
        return False
    env = program_environment()
    env["GALAXYCTR_REPLACED"] = f"{os.getpid()},{os.getppid()}"  # this process and its onefile parent
    extra = {"creationflags": subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" \
        else {"start_new_session": True}
    try:
        subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env, close_fds=True, stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **extra)
    except OSError as error:
        say(f"  The new program is in place but could not start ({error}). Open {exe} again.")
        return True
    say("  Done: the new builder opens now, and this window closes.")
    time.sleep(2)  # long enough to read the line
    return True


def options():
    """Developer options, for testing without questions: --disc <image> --sd <folder> --work <folder>."""
    args = sys.argv[1:]

    def option(name):
        return args[args.index(name) + 1] if name in args and args.index(name) + 1 < len(args) else None
    return option("--disc"), option("--sd"), option("--work")


# ############################################################################
# # The window's game graphics                                              #
# #                                                                         #
# # builder_assets.py makes the game's buttons, windows, icons and sky for  #
# # the window from an extracted disc, into GalaxyCTR-game-ui next to the   #
# # builder (the user's computer only, like the kept game files). A build   #
# # makes them right after extracting when they are missing; on the first  #
# # start the window can ask for the disc only for them (run_setup), which  #
# # extracts it into a temporary folder and deletes it again. Terminal mode #
# # does neither: it would not show them.                                   #
# ############################################################################

def game_ui_folder():
    import builder_assets
    return data_home() / builder_assets.GAME_UI_NAME


def refresh_look_disc(files, image):
    """The window's look keeps the Power Star of the disc image it was read from, which the
    builder program also shows as its icon (builder_exeicon). When the build reads another disc
    image, the pause menu's layout (where the star is) is copied again from it."""
    if image is None or isinstance(UI, TextUI):
        return
    import builder_exeicon
    identity = builder_exeicon.disc_identity(image)
    settings = Settings()
    if not identity or settings.get("look_disc") == identity:
        return
    try:
        import builder_assets
        folder = game_ui_folder()
        if builder_assets.load(folder):
            builder_assets.export_layout(Path(files) / "LayoutData" / "PauseMenu.arc", folder / "layouts" / "PauseMenu")
            builder_assets.seal(folder)  # the set as it is now
        settings.set("look_disc", identity)
    except Exception:  # noqa: BLE001 - only the window's star: the build goes on
        log(traceback.format_exc())


def make_window_graphics(files):
    """Makes the window's game graphics from an extracted disc when they are missing."""
    if isinstance(UI, TextUI):
        return
    try:
        import builder_assets
        folder = game_ui_folder()
        if builder_assets.load(folder):
            return
        builder_assets.make(files, folder, lambda name, done, total: progress("The game's look for this window",
                                                                              100 * done / total, name))
        end_progress()
        say("  The game's buttons, windows and sky are ready for this window.")
    except Exception:  # noqa: BLE001 - only the window's looks: the build goes on
        log(traceback.format_exc())
        say("  The game's look for this window could not be made. The details are in the log.")


def make_loading_screen(files, disc):
    """The port's loading screen (builder_loading.py) into disc/loading, from the disc's own files
    before they are converted. It needs Qt's OpenGL: a builder program without Qt (the first ones,
    run in terminal mode) leaves it out, and the port then starts without it, as before."""
    out = Path(disc) / "loading"
    try:
        import builder_loading
        if builder_loading.is_current(out):
            say("  The loading screen is already made (an earlier build).")
            return
        from PySide6.QtGui import QGuiApplication
        app = QGuiApplication.instance() or QGuiApplication(["galaxyctr-builder"])  # noqa: F841 - OpenGL needs one
        progress("The loading screen", -1.0)
        builder_loading.make(files, Path(disc) / "loading-work", out)
        end_progress()
        say("  The loading screen is ready.")
    except Exception:  # noqa: BLE001 - only the loading screen: the build goes on
        end_progress()
        log(traceback.format_exc())
        say("  The loading screen could not be made, see the log. The game starts without it.")
    finally:
        shutil.rmtree(Path(disc) / "loading-work", ignore_errors=True)


# The window's look needs a few of the disc's files (builder_assets.needed: LayoutData, AudioRes,
# ObjectData, ParticleData). nodtool extracts a disc in the order of its file table, which is by
# name, so once anything after the last of those folders (builder_assets.LAST_FOLDER) has come
# out they are complete and the rest of the disc is not waited for. LOOK_BYTES is about how much
# has come out by then.
LOOK_BYTES = 3_500_000_000


def look_files_ready(files):
    import builder_assets
    return all(path.is_file() for path in builder_assets.needed(files))


def past_look_files(files):
    import builder_assets
    try:
        return any(entry.name.lower() > builder_assets.LAST_FOLDER.lower() for entry in files.iterdir())
    except OSError:
        return False


def folder_bytes(folder):
    total = 0
    for root, _dirs, names in os.walk(folder):
        for name in names:
            try:
                total += os.path.getsize(os.path.join(root, name))
            except OSError:
                pass
    return total


def extract_for_look(image, target, report=None):
    """Extracts the disc into target until the window's files are out; report(bytes) meanwhile."""
    tool = find_tool("nodtool")
    errors = target.parent / f"{target.name}-nodtool.txt"
    files = target / "files"
    with open(errors, "w", encoding="utf-8", errors="replace") as output:
        process = subprocess.Popen([str(tool), "extract", "-q", str(image), str(target)], stdin=subprocess.DEVNULL,
                                   stdout=output, stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        stopped = False
        while process.poll() is None:
            time.sleep(0.25)
            if report:
                report(folder_bytes(target))
            if look_files_ready(files) and past_look_files(files):
                process.terminate()
                process.wait()
                stopped = True
    said = errors.read_text(encoding="utf-8", errors="replace")
    errors.unlink(missing_ok=True)
    log(f"Running nodtool extract -q {image} {target}"
        f"\n{DETAIL}{'Stopped once the needed files were out.' if stopped else f'Finished with code {process.returncode}.'}"
        + details(said))
    if not look_files_ready(files):
        raise BuildError(f"nodtool failed:\n{said.strip()[-800:]}" if said.strip() else
                         "The disc was read, but its files are not where they should be.")


def run_setup():
    """The first start's setup, from the window: reads the disc once for the game graphics.
    Returns 0 when they are ready."""
    code = 0
    try:
        settings = Settings()
        kept = kept_folder(settings)
        if kept:
            make_window_graphics(kept / "original" / "files")
        else:
            say("The builder reads your disc once, to dress this window in the game's look.")
            image = choose_disc(None, settings.get("disc_image"))
            settings.set("disc_image", image)
            parent = image.parent if os.access(image.parent, os.W_OK) else None
            temp = Path(tempfile.mkdtemp(prefix="galaxyctr-look-", dir=parent))
            try:
                if free_space(temp) < (5 << 30):
                    raise BuildError(f"Reading the disc needs about 5 GB free in {temp.parent}. "
                                     f"{free_space(temp) >> 30} GB are free.")
                extract_for_look(image, temp / "disc", lambda done: progress(
                    "Reading your disc", min(99.0, 100.0 * done / LOOK_BYTES)))
                make_window_graphics(temp / "disc" / "files")
                refresh_look_disc(temp / "disc" / "files", image)
            finally:
                shutil.rmtree(temp, ignore_errors=True)
        import builder_assets
        if not builder_assets.load(game_ui_folder()):
            raise BuildError("The game's look could not be made. The details are in the log.")
    except BuildError as error:
        end_progress()
        say(f"ERROR: {error}")
        code = 1
    except (KeyboardInterrupt, EOFError):
        end_progress()
        say("ERROR: Stopped.")
        code = 1
    except Exception:  # noqa: BLE001
        end_progress()
        log(traceback.format_exc())
        say("ERROR: something unexpected went wrong. The details are in the log.")
        code = 1
    return code


def run_build(disc_arg=None, sd_arg=None, work_arg=None):
    """One whole build; returns 0 when it went well."""
    code = 0
    try:
        build(disc_arg, sd_arg, work_arg)
    except BuildError as error:
        end_progress()
        say()
        say(f"ERROR: {error}")
        code = 1
    except (KeyboardInterrupt, EOFError):
        end_progress()
        say()
        say("Stopped.")
        code = 1
    except Exception:  # noqa: BLE001 - every unexpected failure is reported, with its details in the log
        end_progress()
        log(traceback.format_exc())
        say()
        say("ERROR: something unexpected went wrong. Please report it with the log file below.")
        code = 1
    say(f"Log file: {LOG_PATH}")
    return code


# ############################################################################
# # The window's build, in a process of its own                             #
# #                                                                         #
# # The window runs a build (or the first start's setup) through worker()  #
# # in another process: the conversion is Python at work for minutes, and  #
# # in the window's process it held the interpreter (one thread at a time)  #
# # away from the window's drawing, its sky and its music, which stuttered. #
# # The worker runs below normal priority, its helpers too (they inherit   #
# # it), so the window keeps its share of the processor. PipeUI sends what #
# # the build says, asks and reaches to the window (builder_window's       #
# # BuildProcess), which answers the questions through the same pipe.      #
# ############################################################################

class PipeUI:
    """The build's UI in the worker process: each call is a message to the window."""

    def __init__(self, conn):
        self.conn = conn
        self.last_progress = 0.0

    def say(self, text=""):
        self.conn.send(("say", text))

    def problem(self, text):
        self.conn.send(("problem", text))

    def step(self, number, total, title):
        self.conn.send(("step", number, total, title))

    def progress(self, label, percent=-1.0, detail=""):
        moment = time.monotonic()
        if moment - self.last_progress < 0.05 and 0 <= percent < 100:
            return  # at most 20 a second, as the window shows them
        self.last_progress = moment
        self.conn.send(("progress", label, float(percent), detail))

    def end_progress(self):
        pass

    def _ask(self, kind, question, default=None, **extra):
        self.conn.send(("ask", kind, question, default, extra))
        reply = self.conn.recv()
        if reply[0] == "cancel":
            raise KeyboardInterrupt
        return reply[1]

    def ask(self, question, kind="text", default=None):
        return self._ask(kind, question, default)

    def ask_yes(self, question):
        return self._ask("yes", question)

    def ask_ftp(self, question, last, connections):
        return self._ask("ftp", question, last, connections=connections)


def lower_priority():
    """This process (and the programs it starts) below normal priority."""
    try:
        if os.name == "nt":
            import ctypes
            kernel32 = ctypes.windll.kernel32
            kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x00004000)  # BELOW_NORMAL_PRIORITY_CLASS
        else:
            os.nice(5)
    except (OSError, AttributeError):
        pass


def worker(conn, job):
    """The worker process: the setup ("setup") or a build, then ("finished", code, RESULT)."""
    global LOG, UI
    LOG = LOG_PATH.open("a", encoding="utf-8")  # the window's process writes to it too, also appending
    UI = PipeUI(conn)
    lower_priority()
    code = 1
    try:
        code = run_setup() if job == "setup" else run_build(*options())
    finally:
        LOG.flush()
        conn.send(("finished", code, dict(RESULT)))
        LOG.close()


def open_console():
    """A terminal for the terminal mode of the Windows builder program, which starts without one.
    True when one was opened."""
    if os.name != "nt":
        return False
    import ctypes
    kernel32 = ctypes.windll.kernel32
    if kernel32.GetConsoleWindow() or not kernel32.AllocConsole():
        return False
    sys.stdout = sys.stderr = open("CONOUT$", "w", encoding="utf-8", errors="replace")
    sys.stdin = open("CONIN$", "r", encoding="utf-8", errors="replace")
    return True


# ############################################################################
# # No OpenGL: a message, and the builder does not start                   #
# #                                                                         #
# # The window's game look, its skies and the 3DS loading screen are drawn #
# # with OpenGL (2.1, the shaders' GLSL 1.20). Without it the builder used #
# # to go on in a plain look and leave the loading screen out (the owner's #
# # choice, 26 Nov 2026: it now stops). A popup says how to install it on  #
# # this system and the program ends. The popup is Qt's when Qt loads, and #
# # the system's own otherwise (on Linux a Qt that cannot load its OpenGL  #
# # libraries cannot show anything): MessageBoxW on Windows, osascript on  #
# # macOS, zenity, kdialog or xmessage on Linux, and the terminal and the  #
# # log always. Checked by builder_firststart.prepare (before the update   #
# # check) and main() (a builder program updated from an older release).   #
# # The first builders, without Qt, are not stopped: they replace          #
# # themselves (replace_program).                                          #
# ############################################################################
GRAPHICS_TITLE = "GalaxyCTR Builder: OpenGL is missing"
GRAPHICS_HELP = {
    "windows": ("The builder needs OpenGL 2.1 or newer to draw its window and the 3DS loading screen.\n\n"
                "Install or update your graphics card's driver: from NVIDIA, AMD or Intel's website, or "
                "with Windows Update (Settings, Windows Update, Advanced options, Optional updates).\n\n"
                "In a virtual machine, turn on 3D acceleration in its display settings.\n\n"
                "Then start the builder again."),
    "macos": ("The builder needs OpenGL 2.1 or newer to draw its window and the 3DS loading screen. "
              "Every Mac has it, so this Mac's graphics did not answer.\n\n"
              "Update macOS (System Settings, General, Software Update) and restart the Mac.\n\n"
              "In a virtual machine, turn on 3D acceleration in its display settings.\n\n"
              "Then start the builder again."),
    "linux": ("The builder needs OpenGL 2.1 or newer to draw its window and the 3DS loading screen.\n\n"
              "Install Mesa's OpenGL libraries, which also draw without a graphics card.\n\n"
              "Ubuntu, Debian, Mint:\n"
              "    sudo apt install libgl1 libegl1 libgl1-mesa-dri\n"
              "Fedora:\n"
              "    sudo dnf install mesa-libGL mesa-libEGL mesa-dri-drivers\n"
              "Arch:\n"
              "    sudo pacman -S mesa\n"
              "openSUSE:\n"
              "    sudo zypper install Mesa-libGL1 Mesa-libEGL1 Mesa-dri\n\n"
              "With an NVIDIA card, its own driver works too.\n\n"
              "Then start the builder again."),
}
GL_LIBRARIES = ("libGL.so", "libEGL.so", "libOpenGL.so", "libGLX.so")  # in Qt's error when it cannot load


def graphics_help():
    return GRAPHICS_HELP[{"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")]


def opengl_problem():
    """Why OpenGL cannot draw the builder's look here, or "" when it can. Needs a Qt application
    (one is made when there is none: the window takes it over)."""
    from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
    from PySide6.QtWidgets import QApplication
    if QApplication.instance() is None:
        QApplication(sys.argv[:1])
    context = QOpenGLContext()
    if not context.create():
        return "no OpenGL context could be made"
    surface = QOffscreenSurface()
    surface.create()
    try:
        if not context.makeCurrent(surface):
            return "the OpenGL context could not be used"
        version = (context.format().majorVersion(), context.format().minorVersion())
        if context.isOpenGLES():
            return f"only OpenGL ES {version[0]}.{version[1]}"
        if version < (2, 1):
            return f"OpenGL {version[0]}.{version[1]} is older than 2.1"
        return ""
    finally:
        context.doneCurrent()


def stop_for_graphics(reason, qt=True):
    """Shows the popup, logs why and ends the program (SystemExit). qt: Qt can show the popup."""
    text = graphics_help()
    try:
        log(f"OpenGL is missing ({reason}): the builder does not start.")
    except Exception:  # noqa: BLE001 - the log may not be open yet (the launcher's first start)
        pass
    print(f"\n{GRAPHICS_TITLE}\n{text}\n", flush=True)
    shown = False
    if qt:
        try:
            from PySide6.QtCore import Qt
            from PySide6.QtWidgets import QApplication, QMessageBox
            if QApplication.instance() is None:
                QApplication(sys.argv[:1])
            box = QMessageBox(QMessageBox.Icon.Critical, GRAPHICS_TITLE, popup_html(text))
            box.setTextFormat(Qt.TextFormat.RichText)
            box.setTextInteractionFlags(selectable_text())  # the commands can be copied
            box.exec()
            shown = True
        except Exception:  # noqa: BLE001 - the system's own popup below
            shown = False
    if not shown:
        system_popup(text)
    raise SystemExit(1)


def popup_html(text):
    """The popup's text for Qt: paragraphs wrap, the indented command lines never do (a wrapped
    command cannot be copied whole)."""
    import html
    lines = []
    for line in text.split("\n"):
        if line.startswith("    "):
            lines.append(f'<pre style="margin:0 0 0 16px">{html.escape(line.strip())}</pre>')
        elif line:
            lines.append(f"<div>{html.escape(line)}</div>")
        else:
            lines.append("<div>&nbsp;</div>")
    return "".join(lines)


def selectable_text():
    from PySide6.QtCore import Qt
    return Qt.TextInteractionFlag.TextSelectableByMouse | Qt.TextInteractionFlag.TextSelectableByKeyboard


def system_popup(text):
    """The system's own message window, for when Qt cannot show one."""
    import shutil
    try:
        if os.name == "nt":
            import ctypes
            ctypes.windll.user32.MessageBoxW(None, text, GRAPHICS_TITLE, 0x10)  # MB_ICONERROR
            return
        if sys.platform == "darwin":
            quoted = text.replace("\\", "\\\\").replace('"', '\\"')
            subprocess.run(["osascript", "-e", f'display dialog "{quoted}" with title "{GRAPHICS_TITLE}" '
                            'buttons {"OK"} default button "OK" with icon stop'], timeout=600)
            return
        for command in (["zenity", "--error", "--no-markup", "--width", "620", "--title", GRAPHICS_TITLE, "--text", text],
                        ["kdialog", "--title", GRAPHICS_TITLE, "--error", text],
                        ["xmessage", "-center", text]):
            if shutil.which(command[0]):
                subprocess.run(command, timeout=600, env=program_environment())
                return
    except (OSError, subprocess.SubprocessError):
        pass


def require_graphics(app=None):
    """Ends the program with the popup when OpenGL cannot draw here (see above)."""
    if os.environ.get("GALAXYCTR_SKIP_GL_CHECK"):  # the developer's tests only
        return
    try:
        reason = opengl_problem()
    except Exception as error:  # noqa: BLE001 - Qt's OpenGL modules do not load
        reason = repr(error)
    if reason:
        stop_for_graphics(reason)


def main():
    global LOG, UI
    LOG_PATH.open("w", encoding="utf-8").close()
    # Appending: the window's build process (worker) writes to the same file.
    LOG = LOG_PATH.open("a", encoding="utf-8")
    LOG.write(f"== GalaxyCTR builder log ==\n"
              f"- Started on {time.strftime('%Y-%m-%d at %H:%M:%S')}\n"
              f"- Builder version: {RELEASE}\n"
              f"- Computer: {platform.platform()} ({system_key()})\n\nStarting\n")
    for line in LAUNCHER_LINES:
        # The launcher indents what it found ("  Downloading ...") under "Checking for a newer version".
        LOG.write(log_format(DETAIL + line.strip() if line.startswith("  ") else line) + "\n")
    LOG.flush()
    gui = None
    if "--console" not in sys.argv[1:]:
        try:
            import builder_gui
            if builder_gui.available():
                gui = builder_gui
            else:
                log(f"No window ({builder_gui.UNAVAILABLE}); terminal mode.")
        except Exception as error:  # noqa: BLE001 - the terminal always works
            log(f"No window ({error!r}); terminal mode.")
        # A Qt that is in the program but cannot load the system's OpenGL libraries (Linux): the
        # builder stops with the system's own popup, as it does below when OpenGL cannot draw.
        missing = getattr(sys.modules.get("builder_gui"), "UNAVAILABLE", "")
        if gui is None and any(name in missing for name in GL_LIBRARIES):
            LOG.flush()
            stop_for_graphics(missing, qt=False)
    if gui is not None:
        require_graphics()
        code = gui.run(sys.modules[__name__])
        LOG.close()
        return code

    UI = TextUI()
    if open_console():
        for line in LAUNCHER_LINES:  # said before there was a terminal to show them
            print(line)
    if FROZEN and "--console" not in sys.argv[1:] and getattr(sys.modules.get("builder_gui"), "QT", True) is None:
        if replace_program():  # a program from before the window: the new one takes over
            LOG.close()
            return 0
        say()
    code = run_build(*options())
    LOG.close()
    # Only Windows' program opens a terminal of its own (open_console), which would vanish at once;
    # elsewhere the builder was started from a terminal that stays. No answer (no terminal at all)
    # just closes.
    if FROZEN and os.name == "nt":
        try:
            input("Press Enter to close this window.")
        except (EOFError, OSError):
            pass
    return code


if __name__ == "__main__":
    multiprocessing.freeze_support()  # the converter encodes music in several processes
    sys.exit(main())
