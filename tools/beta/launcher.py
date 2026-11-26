"""GalaxyCTR builder launcher: the program inside each GalaxyCTR-Builder-<system> executable.

The executable carries Python, the helper programs (nodtool, bannertool, makerom) and a copy of
the source code and port program of the release it was built for. Each time it starts, it:
  1. keeps the source code and the port's program it runs in GalaxyCTR-Builder-files, in the
     user's settings folder (%APPDATA%\\GalaxyCTR on Windows), so the builder's own folder holds
     only the program and install_guide.txt (move_out_of_program_folder moves what older
     builders kept there);
  2. asks GitHub for the repository's latest release and, when that release is not the one in the
     folder, downloads its source code and program into the folder first;
  3. runs the builder (tools/beta/galaxyctr_builder.py) from the folder.

So a release with new fixes reaches every user the next time they run the builder they already
have: only a change to the launcher itself or to the helper programs needs a new executable
(LAUNCHER_VERSION below, LAUNCHER_NEEDED in galaxyctr_builder.py).

Since launcher version 2 the executable also carries Qt (PySide6): while it checks for updates it
shows a small window, and the builder then shows its own (tools/beta/builder_gui.py). On Windows it
is a program without a terminal; the builder opens one when it has to run in terminal mode. A
program from before Qt (launcher version 1) is replaced by this one on its own: the builder it
downloads puts the new program in its place and starts it (galaxyctr_builder.replace_program,
finish_replacement here).

Developer options: --update-only (check and download, then stop), --console (no windows, as the
first builders), --gui-probe (opens Qt and closes it: on Linux the builder asks a second copy of the
program this before showing its window), and the GALAXYCTR_TOKEN environment variable (a GitHub
token, for testing while the repository is private).
"""

import json
import math
import multiprocessing
import os
import platform
import re
import shutil
import ssl
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

LAUNCHER_VERSION = 2
REPOSITORY = "Im-Monee/galaxyctr-beta"
API = f"https://api.github.com/repos/{REPOSITORY}"
FOLDER_NAME = "GalaxyCTR-Builder-files"
GAME_UI_NAME = "GalaxyCTR-game-ui"  # builder_assets.GAME_UI_NAME

FROZEN = getattr(sys, "frozen", False)
BUNDLE = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parents[2]))
REPO = Path(__file__).resolve().parents[2]

LINES = []  # what the launcher said, handed to the builder for its log
STATUS = {"text": "", "percent": -1.0}  # the last line and the download's progress, for the splash


def say(text="", shown=None):
    """A line for the terminal and the log. shown: the plain words the small window shows for it
    instead (the log keeps versions, folders and reasons; the window does not)."""
    print(text, flush=True)
    LINES.append(text)
    if shown or text.strip():
        STATUS["text"], STATUS["percent"] = (shown or text).strip(), -1.0


def note(text):
    """A line for the terminal and the log only."""
    print(text, flush=True)
    LINES.append(text)


def system_key():
    """windows-x86_64, linux-x86_64, macos-arm64 and so on (galaxyctr_builder.system_key)."""
    name = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")
    machine = platform.machine().lower()
    machine = {"amd64": "x86_64", "x64": "x86_64", "aarch64": "arm64"}.get(machine, machine)
    return f"{name}-{machine}"


def version_key(tag):
    """v0.21.3 -> (0, 21, 3), for choosing the newer of two copies."""
    return tuple(int(n) for n in re.findall(r"\d+", tag or "")) or (0,)


class Copy:
    """A source tree and the port's program that goes with it."""

    def __init__(self, tag, source, elf, where):
        self.tag, self.source, self.elf, self.where = tag, Path(source), Path(elf), where

    def usable(self):
        return (self.source / "tools" / "beta" / "galaxyctr_builder.py").is_file() and self.elf.is_file() \
            and needed_launcher(self.source) <= LAUNCHER_VERSION


def needed_launcher(source):
    """The LAUNCHER_NEEDED a source tree's builder declares (1 when it says nothing)."""
    try:
        text = (source / "tools" / "beta" / "galaxyctr_builder.py").read_text(encoding="utf-8")
    except OSError:
        return 1
    found = re.search(r"^LAUNCHER_NEEDED = (\d+)", text, re.M)
    return int(found.group(1)) if found else 1


def builtin_copy():
    """The copy inside the executable (or, run from the repository, the repository itself)."""
    if not FROZEN:
        return Copy(os.environ.get("GALAXYCTR_RELEASE", "dev"), REPO, REPO / "build" / "3ds" / "smg.elf", "this repository")
    info = json.loads((BUNDLE / "source.json").read_text(encoding="utf-8"))
    return Copy(info["tag"], BUNDLE / "source", BUNDLE / "payload" / "smg.elf", "this builder")


def user_folder():
    """The user's settings folder for GalaxyCTR (galaxyctr_builder.settings_folder)."""
    if os.environ.get("GALAXYCTR_HOME"):  # tests
        return Path(os.environ["GALAXYCTR_HOME"])
    if os.name == "nt":
        base = Path(os.environ.get("APPDATA") or Path.home() / "AppData" / "Roaming")
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    return base / "GalaxyCTR"


def data_home():
    """Where GalaxyCTR-Builder-files and the game look are (galaxyctr_builder.data_home). Run from
    the repository, build/beta/launcher-test stands in for it."""
    if FROZEN or os.environ.get("GALAXYCTR_HOME"):
        return user_folder()
    return REPO / "build" / "beta" / "launcher-test"


# ##############################################################################################
# # After replacing an older program                                                          #
# #                                                                                            #
# # A builder program from before the window puts this one in its place and starts it          #
# # (galaxyctr_builder.replace_program), with GALAXYCTR_REPLACED naming its two processes (the  #
# # Python one and PyInstaller's parent). This program waits for them to end, so the old       #
# # GalaxyCTR-Builder-files next to the program is free to move, then deletes the old program, #
# # which was left next to this one as <name>.replaced.                                        #
# ##############################################################################################
REPLACED_SUFFIX = ".replaced"  # galaxyctr_builder.REPLACED_SUFFIX


def process_running(pid):
    if os.name == "nt":
        import ctypes
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
        if not handle:
            return False
        try:
            return kernel32.WaitForSingleObject(handle, 0) == 0x102  # WAIT_TIMEOUT: still running
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def finish_replacement():
    pids = os.environ.pop("GALAXYCTR_REPLACED", "")
    if not FROZEN:
        return
    if pids:
        note("  This builder program replaced an older one.")
        waits = [int(p) for p in pids.split(",") if p.strip().isdigit() and int(p) > 1]
        give_up = time.monotonic() + 20
        while time.monotonic() < give_up and any(process_running(pid) for pid in waits):
            time.sleep(0.1)
    old = Path(sys.executable).resolve()
    old = old.with_name(old.name + REPLACED_SUFFIX)
    if old.exists():
        try:
            patiently(old.unlink, tries=10)
            note(f"  Deleted the old program ({old.name}).")
        except OSError as error:
            note(f"  Could not delete the old program {old} ({error}).")


def move_out_of_program_folder():
    """Older builders kept GalaxyCTR-Builder-files and the game look next to the program: they
    move to the user's settings folder (or go, when it already has them)."""
    if not FROZEN:
        return
    here = Path(sys.executable).resolve().parent
    home = data_home()
    for name in (FOLDER_NAME, GAME_UI_NAME):
        old, new = here / name, home / name
        if not old.is_dir() or old.resolve() == new.resolve():
            continue
        try:
            if new.exists():
                shutil.rmtree(old)
                note(f"  Removed the old {name} next to the builder ({home} has it).")
            else:
                home.mkdir(parents=True, exist_ok=True)
                shutil.move(str(old), str(new))
                note(f"  Moved {name} to {home}.")
        except OSError as error:
            note(f"  Could not move {old} ({error}).")
            shutil.rmtree(old, ignore_errors=True)


def files_folder():
    """GalaxyCTR-Builder-files in the user's settings folder, or in the home folder when that is
    not writable."""
    for base in (data_home(), Path.home()):
        folder = base / FOLDER_NAME
        try:
            folder.mkdir(parents=True, exist_ok=True)
            probe = folder / ".write-test"
            probe.write_bytes(b"")
            probe.unlink()
            return folder
        except OSError:
            continue
    return None


def folder_copy(folder):
    try:
        info = json.loads((folder / "release.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return Copy(info.get("tag"), folder / "source", folder / "smg.elf", folder)


# ##############################################################################################
# # Talking to GitHub                                                                          #
# #                                                                                            #
# # The latest release (GitHub's "latest": the newest one that is not a pre-release), its      #
# # source code archive and its smg.elf. With no token everything goes through the public      #
# # addresses; GALAXYCTR_TOKEN adds the token to GitHub's own requests only, never to the      #
# # storage servers GitHub redirects downloads to. certifi, bundled in the executable, gives   #
# # Python the certificates macOS does not.                                                    #
# ##############################################################################################
def ssl_context():
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()


def request(url, accept="application/vnd.github+json"):
    req = urllib.request.Request(url, headers={"User-Agent": "GalaxyCTR-Builder", "Accept": accept})
    token = os.environ.get("GALAXYCTR_TOKEN")
    if token:
        req.add_unredirected_header("Authorization", f"Bearer {token}")
    return urllib.request.urlopen(req, timeout=60, context=ssl_context())


def latest_release():
    with request(f"{API}/releases/latest") as response:
        return json.load(response)


def download(url, target, label, accept="application/octet-stream", shown=None):
    status = shown
    with request(url, accept) as response, open(target, "wb") as out:
        total = int(response.headers.get("Content-Length") or 0)
        done, last = 0, None
        while True:
            block = response.read(1 << 16)
            if not block:
                break
            out.write(block)
            done += len(block)
            shown = f"{100 * done / total:3.0f}%" if total else f"{done >> 20} MB"
            if shown != last:  # GitHub's source archives have no size: megabytes then
                print(f"\r  {label}: {shown}", end="", flush=True)
                STATUS["text"] = status or f"{label}: {shown.strip()}"
                STATUS["percent"] = 100 * done / total if total else -1.0
                last = shown
    print()


def extract_source(archive, target):
    """GitHub's archive holds one top folder (<repository>-<tag>); its content goes to target."""
    with zipfile.ZipFile(archive) as zipped:
        for member in zipped.infolist():
            parts = Path(member.filename).parts[1:]
            if not parts or member.is_dir():
                continue
            if any(part in ("..", "") for part in parts) or Path(member.filename).is_absolute():
                raise ValueError(f"unsafe path in the archive: {member.filename}")
            destination = target.joinpath(*parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            with zipped.open(member) as source, open(destination, "wb") as out:
                shutil.copyfileobj(source, out)


# ##############################################################################################
# # Updating the folder                                                                        #
# #                                                                                            #
# # The new release is downloaded into GalaxyCTR-Builder-files/download and checked (a whole   #
# # archive, the builder inside it, an ELF program) before it replaces the folder's source/    #
# # and smg.elf, so a failed or interrupted download leaves the previous copy as it was. A     #
# # release that needs a newer launcher is not installed: the user is sent to download the     #
# # new executable.                                                                            #
# ##############################################################################################
class NeedsNewBuilder(Exception):
    pass


def patiently(action, tries=20):
    """On Windows a folder whose files were just written is often held for a moment (an
    antivirus scanning them): "access denied". Retried for up to ten seconds."""
    for attempt in range(tries):
        try:
            return action()
        except PermissionError:
            if attempt == tries - 1:
                raise
            time.sleep(0.5)


def install(release, folder):
    tag = release["tag_name"]
    elf_asset = next((a for a in release.get("assets", []) if a["name"] == "smg.elf"), None)
    if elf_asset is None:
        raise ValueError(f"release {tag} has no smg.elf yet")
    work = folder / "download"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    download(release["zipball_url"], work / "source.zip", f"Source code {tag}", "application/vnd.github+json",
             shown=f"Downloading version {tag}...")
    private = bool(os.environ.get("GALAXYCTR_TOKEN"))
    download(elf_asset["url"] if private else elf_asset["browser_download_url"], work / "smg.elf", f"Port program {tag}",
             shown=f"Downloading version {tag}...")
    if (work / "smg.elf").read_bytes()[:4] != b"\x7fELF":
        raise ValueError("the downloaded smg.elf is not a program")
    extract_source(work / "source.zip", work / "source")
    if not (work / "source" / "tools" / "beta" / "galaxyctr_builder.py").is_file():
        raise ValueError("the downloaded source code has no builder")
    if needed_launcher(work / "source") > LAUNCHER_VERSION:
        shutil.rmtree(work, ignore_errors=True)
        raise NeedsNewBuilder(tag)

    swap_in(folder, work / "source", work / "smg.elf", {
        "tag": tag, "name": release.get("name"), "published": release.get("published_at")})
    shutil.rmtree(work, ignore_errors=True)


def swap_in(folder, source, elf, info):
    """Puts a checked source tree and program in place of the folder's. The old copy is moved
    aside first and deleted last, so the swap itself is only renames."""
    old = folder / "source.old"
    shutil.rmtree(old, ignore_errors=True)
    if (folder / "source").exists():
        patiently(lambda: (folder / "source").rename(old))
    patiently(lambda: source.rename(folder / "source"))
    patiently(lambda: os.replace(elf, folder / "smg.elf"))
    shutil.rmtree(old, ignore_errors=True)
    info["downloaded"] = time.strftime("%Y-%m-%d %H:%M:%S")
    (folder / "release.json").write_text(json.dumps(info, indent=1), encoding="utf-8")


def choose_copy():
    """Updates the folder when GitHub has a newer release, and returns the copy to run."""
    builtin = builtin_copy()
    folder = files_folder()
    current = folder_copy(folder) if folder else None
    if current and not current.usable():
        current = None

    say("Checking for a newer version of GalaxyCTR...", "Looking for updates...")
    try:
        release = latest_release()
        tag = release["tag_name"]
        if current and current.tag == tag:
            say(f"  Up to date ({tag}).", "Everything is up to date!")
        elif folder is None:
            say(f"  Version {tag} is out, but there is no folder to download it to.",
                f"Version {tag} is out, but it cannot be saved on this computer.")
        else:
            say(f"  Downloading {release.get('name') or tag} into {folder}.", f"Downloading version {tag}...")
            install(release, folder)
            current = folder_copy(folder)
            say(f"  Updated to {tag}.", f"Updated to version {tag}!")
    except NeedsNewBuilder as needed:
        say(f"  Version {needed} needs a newer builder program. Download it from",
            "The new version needs a new builder program. Download it from GitHub.")
        note(f"  https://github.com/{REPOSITORY}/releases/latest")
    except (urllib.error.URLError, OSError, ValueError, KeyError) as error:
        reason = "no published release was found" if getattr(error, "code", None) == 404 else str(error)
        say(f"  Could not check or download ({reason}).", "Could not look for updates. Starting anyway...")

    # The newer of the folder's copy and the builder's own; when the builder's own is newer (or the
    # folder is still empty), it is copied into the folder, which so always holds what runs.
    if folder and FROZEN and (current is None or version_key(builtin.tag) > version_key(current.tag)):
        try:
            place_builtin(builtin, folder)
            current = folder_copy(folder)
        except OSError as error:
            note(f"  Could not fill {folder} ({error}).")
    choice = current if current and current.usable() and version_key(current.tag) >= version_key(builtin.tag) else builtin
    say(f"  Running version {choice.tag}, from {choice.where}.", "Starting...")
    return choice


def place_builtin(builtin, folder):
    work = folder / "download"
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(builtin.source, work / "source")
    shutil.copyfile(builtin.elf, work / "smg.elf")
    swap_in(folder, work / "source", work / "smg.elf", {"tag": builtin.tag, "name": "the copy inside the builder program"})
    shutil.rmtree(work, ignore_errors=True)


# ##############################################################################################
# # Running the builder                                                                        #
# #                                                                                            #
# # The builder is imported from the chosen source tree, with the helper programs and the      #
# # port's program handed over through the environment (GALAXYCTR_TOOLS, GALAXYCTR_ELF),       #
# # which the converter's worker processes inherit too. Child processes the converter starts   #
# # run this executable again: freeze_support() hands them over before anything else happens.  #
# # If the downloaded builder cannot even start, the copy inside the executable runs instead.  #
# ##############################################################################################
def main():
    args = sys.argv[1:]
    if "--gui-probe" in args:
        return gui_probe()
    app = start_qt(args)
    if FROZEN:
        os.environ["GALAXYCTR_TOOLS"] = str(BUNDLE / "tools" / system_key())
    finish_replacement()
    move_out_of_program_folder()
    card = first_start(app) if app and "--update-only" not in args else None
    try:
        copy = with_splash(app, choose_copy, card) if app else choose_copy()
    except Exception as error:  # noqa: BLE001 - an update problem never stops the build
        say(f"  The update check failed ({error}); using the version inside this builder.",
            "Could not look for updates. Starting anyway...")
        copy = builtin_copy()
    if "--update-only" in args:
        return 0
    try:
        builder = load_builder(copy)
    except Exception as error:  # noqa: BLE001 - e.g. a module this executable does not carry
        builtin = builtin_copy()
        if copy.source == builtin.source:
            raise
        say(f"  Version {copy.tag} could not start ({error!r}); using {builtin.tag}, from {builtin.where}.")
        copy = builtin
        builder = load_builder(copy)
    builder.LAUNCHER_LINES = list(LINES)
    if not hasattr(builder, "run_build"):
        open_console()  # a builder from before the window: it only talks in a terminal
    return builder.main()


# ##############################################################################################
# # The window while checking for updates                                                     #
# #                                                                                            #
# # Qt starts here, so the user sees something at once: a small frameless card with the        #
# # launcher's last line, a progress bar for downloads and a spinning ring. The check runs in   #
# # a thread while the card is redrawn. The builder then opens its window in the same Qt       #
# # application. No window (GALAXYCTR_GUI=0 for the builder) with --console, without Qt, or on  #
# # Linux when a second copy of the program cannot open one (Qt would end the program).       #
# ##############################################################################################
def start_qt(args):
    if "--console" in args or "--update-only" in args:
        os.environ["GALAXYCTR_GUI"] = "0"
        return None
    try:
        from PySide6.QtWidgets import QApplication
    except Exception:  # noqa: BLE001 - an executable without Qt
        return None
    if sys.platform.startswith("linux"):
        command = [sys.executable, "--gui-probe"] if FROZEN else [sys.executable, str(Path(__file__).resolve()), "--gui-probe"]
        try:
            works = (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")) and subprocess.run(
                command, stdin=subprocess.DEVNULL, capture_output=True, timeout=60).returncode == 0
        except (OSError, subprocess.SubprocessError):
            works = False
        if not works:
            os.environ["GALAXYCTR_GUI"] = "0"
            return None
    try:
        app = QApplication(sys.argv[:1])
        app.setApplicationName("GalaxyCTR Builder")
        app.setDesktopFileName("galaxyctr-builder")  # Linux: builder_window.DESKTOP_ID, for the splash too
        return app
    except Exception:  # noqa: BLE001
        os.environ["GALAXYCTR_GUI"] = "0"
        return None


def gui_probe():
    from PySide6.QtWidgets import QApplication, QWidget
    app = QApplication(sys.argv[:1])
    window = QWidget()
    window.show()
    app.processEvents()
    window.close()
    return 0


def installed_copy():
    """The copy that runs now, without asking GitHub: the folder's when it is usable and not older
    than the builder's own, else the builder's own (choose_copy decides the same way)."""
    builtin = builtin_copy()
    folder = files_folder()
    current = folder_copy(folder) if folder else None
    if current and current.usable() and version_key(current.tag) >= version_key(builtin.tag):
        return current
    return builtin


def first_start(app):
    """Before the update check: the disc image in Windows' own dialog and the builder made ready
    from it (builder_firststart.prepare), when the game's graphics are not there yet; then the
    painter that draws the update check in the game's look (builder_window.paint_card), or None."""
    builder = None
    try:
        builder = load_builder(installed_copy())
        if hasattr(builder, "check_settings"):
            builder.check_settings()  # before anything reads it, the disc's question first of all
        import builder_firststart
        import builder_window
        builder_window.B = builder
        builder_window.pick_fonts()
        builder_firststart.prepare(app, builder)
        return builder_window.splash_painter()
    except Exception as error:  # noqa: BLE001 - the plain update window, the builder asks later
        note(f"  The first start could not run before the update check ({error!r}).")
        return None
    finally:
        early = getattr(builder, "EARLY", None)
        if early:  # into the builder's log, with the launcher's own lines
            LINES.extend(early)
            early.clear()


def with_splash(app, action, card=None):
    """Runs action() in a thread while a small window shows STATUS; returns its result. card: a
    painter for the game's look (builder_window.paint_card), when the game's graphics are there."""
    from PySide6.QtCore import QPointF, QRectF, Qt
    from PySide6.QtGui import QColor, QFont, QLinearGradient, QPainter, QPen
    from PySide6.QtWidgets import QWidget

    class Splash(QWidget):
        def __init__(self):
            super().__init__(None, Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint)
            self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
            self.setWindowTitle("GalaxyCTR Builder")
            self.resize(460, 190)

        def paintEvent(self, event):
            p = QPainter(self)
            percent = STATUS["percent"]
            if card is not None and card(p, self, STATUS["text"] or "Starting...", percent / 100 if percent >= 0 else None):
                return
            p.setRenderHint(QPainter.RenderHint.Antialiasing)
            box = QRectF(self.rect()).adjusted(2, 2, -2, -2)
            fill = QLinearGradient(box.topLeft(), box.bottomRight())
            fill.setColorAt(0, QColor(24, 34, 90, 245))
            fill.setColorAt(1, QColor(6, 8, 28, 245))
            edge = QLinearGradient(box.topLeft(), box.bottomRight())
            edge.setColorAt(0, QColor(63, 216, 255, 200))
            edge.setColorAt(1, QColor(140, 92, 255, 160))
            p.setBrush(fill)
            p.setPen(QPen(edge, 1.5))
            p.drawRoundedRect(box, 22, 22)
            t = time.monotonic()
            p.setPen(QPen(QColor(63, 216, 255), 3, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            p.drawArc(QRectF(34, 38, 34, 34), int(-(t * 320) % 360 * 16), 110 * 16)
            heading = QFont()
            heading.setPixelSize(22)
            heading.setWeight(QFont.Weight.Bold)
            heading.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, 4)
            p.setFont(heading)
            p.setPen(QColor(236, 243, 255))
            p.drawText(QPointF(86, 63), "GALAXYCTR")
            small = QFont()
            small.setPixelSize(13)
            p.setFont(small)
            p.setPen(QColor(150, 166, 206))
            text = p.fontMetrics().elidedText(STATUS["text"] or "Starting...", Qt.TextElideMode.ElideRight, int(box.width()) - 70)
            p.drawText(QPointF(36, 118), text)
            track = QRectF(36, 140, box.width() - 70, 6)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(255, 255, 255, 26))
            p.drawRoundedRect(track, 3, 3)
            percent = STATUS["percent"]
            if percent >= 0:
                part = QRectF(track.left(), track.top(), track.width() * min(percent, 100) / 100, track.height())
            else:  # not known: a glow going to and fro
                k = (math.sin(t * 2.2) + 1) / 2
                part = QRectF(track.left() + k * track.width() * 0.72, track.top(), track.width() * 0.28, track.height())
            bar = QLinearGradient(part.topLeft(), part.topRight())
            bar.setColorAt(0, QColor(47, 123, 255))
            bar.setColorAt(1, QColor(63, 216, 255))
            p.setBrush(bar)
            p.drawRoundedRect(part, 3, 3)

    splash = Splash()
    screen = app.primaryScreen()
    if screen is not None:
        splash.move(screen.availableGeometry().center() - splash.rect().center())
    splash.show()
    outcome = {}

    def work():
        try:
            outcome["value"] = action()
        except BaseException as error:  # noqa: BLE001 - raised again in the main thread
            outcome["error"] = error

    thread = threading.Thread(target=work, daemon=True)
    thread.start()
    shown = time.monotonic()
    while thread.is_alive() or time.monotonic() - shown < 0.6:  # long enough to be seen, not to wait
        app.processEvents()
        splash.update()
        time.sleep(0.016)
    splash.close()
    app.processEvents()
    if "error" in outcome:
        raise outcome["error"]
    return outcome["value"]


def open_console():
    """A terminal for builders without the window, in the Windows program that starts without one."""
    if os.name != "nt" or not FROZEN:
        return
    import ctypes
    if ctypes.windll.kernel32.GetConsoleWindow() or not ctypes.windll.kernel32.AllocConsole():
        return
    sys.stdout = sys.stderr = open("CONOUT$", "w", encoding="utf-8", errors="replace")
    sys.stdin = open("CONIN$", "r", encoding="utf-8", errors="replace")
    for line in LINES:
        print(line)


LOADED = {}  # the copy whose modules are imported now: {"source": Path}


def load_builder(copy):
    """The builder module of copy. The first start (first_start) imports the copy that is there
    before the update check; a newer one after it replaces every module of the old one."""
    os.environ["GALAXYCTR_ELF"] = str(copy.elf)
    os.environ["GALAXYCTR_RELEASE"] = copy.tag or "?"
    old = LOADED.get("source")
    if old is not None and old == copy.source and "galaxyctr_builder" in sys.modules:
        return sys.modules["galaxyctr_builder"]
    if old is not None:
        root = str(old.resolve())
        for name, module in list(sys.modules.items()):
            where = getattr(module, "__file__", None)
            if name not in ("__main__", "__mp_main__") and where and str(Path(where).resolve()).startswith(root):
                del sys.modules[name]
        sys.path[:] = [p for p in sys.path if not str(Path(p).resolve()).startswith(root)]
    for name in [n for n in sys.modules if n in ("galaxyctr_builder", "texreduce", "yaz0", "convert_smg_files")]:
        del sys.modules[name]  # a failed first try must not leave half-imported modules behind
    sys.path.insert(0, str(copy.source / "tools" / "beta"))
    LOADED["source"] = copy.source
    import galaxyctr_builder
    return galaxyctr_builder


if __name__ == "__main__":
    # The Windows program has no terminal: no stdout at all. The converter's worker processes (this
    # program again, through freeze_support) and some modules write to it, so it goes nowhere.
    if sys.stdout is None:
        sys.stdout = open(os.devnull, "w", encoding="utf-8")
    if sys.stderr is None:
        sys.stderr = sys.stdout
    multiprocessing.freeze_support()
    sys.exit(main())
