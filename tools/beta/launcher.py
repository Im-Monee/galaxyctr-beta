"""GalaxyCTR builder launcher: the program inside each GalaxyCTR-Builder-<system> executable.

The executable carries Python, the helper programs (nodtool, bannertool, makerom) and a copy of
the source code and port program of the release it was built for. Each time it starts, it:
  1. keeps the source code and the port's program it runs in a folder next to itself,
     GalaxyCTR-Builder-files;
  2. asks GitHub for the repository's latest release and, when that release is not the one in the
     folder, downloads its source code and program into the folder first;
  3. runs the builder (tools/beta/galaxyctr_builder.py) from the folder.

So a release with new fixes reaches every user the next time they run the builder they already
have: only a change to the launcher itself or to the helper programs needs a new executable
(LAUNCHER_VERSION below, LAUNCHER_NEEDED in galaxyctr_builder.py).

Developer options: --update-only (check and download, then stop), and the GALAXYCTR_TOKEN
environment variable (a GitHub token, for testing while the repository is private).
"""

import json
import multiprocessing
import os
import platform
import re
import shutil
import ssl
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

LAUNCHER_VERSION = 1
REPOSITORY = "Im-Monee/galaxyctr-beta"
API = f"https://api.github.com/repos/{REPOSITORY}"
FOLDER_NAME = "GalaxyCTR-Builder-files"

FROZEN = getattr(sys, "frozen", False)
BUNDLE = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parents[2]))
REPO = Path(__file__).resolve().parents[2]

LINES = []  # what the launcher said, handed to the builder for its log


def say(text=""):
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


def files_folder():
    """GalaxyCTR-Builder-files next to the executable, or in the home folder when that is not
    writable. Run from the repository, build/beta/launcher-test stands in for the executable's folder."""
    here = Path(sys.executable).resolve().parent if FROZEN else REPO / "build" / "beta" / "launcher-test"
    for base in (here, Path.home()):
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


def download(url, target, label, accept="application/octet-stream"):
    with request(url, accept) as response, open(target, "wb") as out:
        total = int(response.headers.get("Content-Length") or 0)
        done = 0
        while True:
            block = response.read(1 << 16)
            if not block:
                break
            out.write(block)
            done += len(block)
            shown = f"{100 * done / total:5.1f}%" if total else f"{done >> 20} MB"
            print(f"\r  {label}: {shown}", end="", flush=True)
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


def install(release, folder):
    tag = release["tag_name"]
    elf_asset = next((a for a in release.get("assets", []) if a["name"] == "smg.elf"), None)
    if elf_asset is None:
        raise ValueError(f"release {tag} has no smg.elf yet")
    work = folder / "download"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    download(release["zipball_url"], work / "source.zip", f"Source code {tag}", "application/vnd.github+json")
    private = bool(os.environ.get("GALAXYCTR_TOKEN"))
    download(elf_asset["url"] if private else elf_asset["browser_download_url"], work / "smg.elf", f"Port program {tag}")
    if (work / "smg.elf").read_bytes()[:4] != b"\x7fELF":
        raise ValueError("the downloaded smg.elf is not a program")
    extract_source(work / "source.zip", work / "source")
    if not (work / "source" / "tools" / "beta" / "galaxyctr_builder.py").is_file():
        raise ValueError("the downloaded source code has no builder")
    if needed_launcher(work / "source") > LAUNCHER_VERSION:
        shutil.rmtree(work, ignore_errors=True)
        raise NeedsNewBuilder(tag)

    shutil.rmtree(folder / "source", ignore_errors=True)
    (work / "source").rename(folder / "source")
    os.replace(work / "smg.elf", folder / "smg.elf")
    (folder / "release.json").write_text(json.dumps({
        "tag": tag, "name": release.get("name"), "published": release.get("published_at"),
        "downloaded": time.strftime("%Y-%m-%d %H:%M:%S"),
    }, indent=1), encoding="utf-8")
    shutil.rmtree(work, ignore_errors=True)


def choose_copy():
    """Updates the folder when GitHub has a newer release, and returns the copy to run."""
    builtin = builtin_copy()
    folder = files_folder()
    current = folder_copy(folder) if folder else None
    if current and not current.usable():
        current = None

    say("Checking for a newer version of GalaxyCTR...")
    try:
        release = latest_release()
        tag = release["tag_name"]
        if current and current.tag == tag:
            say(f"  Up to date ({tag}).")
        elif folder is None:
            say(f"  Version {tag} is out, but there is no folder to download it to.")
        else:
            say(f"  Downloading version {tag} ({release.get('name') or tag}) into {folder}:")
            install(release, folder)
            current = folder_copy(folder)
            say(f"  Updated to {tag}.")
    except NeedsNewBuilder as needed:
        say(f"  Version {needed} needs a newer builder program. Download it from")
        say(f"  https://github.com/{REPOSITORY}/releases/latest")
    except (urllib.error.URLError, OSError, ValueError, KeyError) as error:
        reason = "no published release was found" if getattr(error, "code", None) == 404 else str(error)
        say(f"  Could not check or download ({reason}).")

    # The newer of the folder's copy and the builder's own; when the builder's own is newer (or the
    # folder is still empty), it is copied into the folder, which so always holds what runs.
    if folder and FROZEN and (current is None or version_key(builtin.tag) > version_key(current.tag)):
        try:
            place_builtin(builtin, folder)
            current = folder_copy(folder)
        except OSError as error:
            say(f"  Could not fill {folder} ({error}).")
    choice = current if current and current.usable() and version_key(current.tag) >= version_key(builtin.tag) else builtin
    say(f"  Using version {choice.tag}, from {choice.where}.")
    return choice


def place_builtin(builtin, folder):
    shutil.rmtree(folder / "source", ignore_errors=True)
    shutil.copytree(builtin.source, folder / "source")
    shutil.copyfile(builtin.elf, folder / "smg.elf")
    (folder / "release.json").write_text(json.dumps({
        "tag": builtin.tag, "name": "the copy inside the builder program",
        "downloaded": time.strftime("%Y-%m-%d %H:%M:%S"),
    }, indent=1), encoding="utf-8")


# ##############################################################################################
# # Running the builder                                                                        #
# #                                                                                            #
# # The builder is imported from the chosen source tree, with the helper programs and the      #
# # port's program handed over through the environment (GALAXYCTR_TOOLS, GALAXYCTR_ELF),      #
# # which the converter's worker processes inherit too. Child processes the converter starts   #
# # run this executable again: freeze_support() hands them over before anything else happens.  #
# # If the downloaded builder cannot even start, the copy inside the executable runs instead.  #
# ##############################################################################################
def main():
    args = sys.argv[1:]
    try:
        copy = choose_copy()
    except Exception as error:  # noqa: BLE001 - an update problem never stops the build
        say(f"  The update check failed ({error}); using the version inside this builder.")
        copy = builtin_copy()
    if "--update-only" in args:
        return 0
    if FROZEN:
        os.environ["GALAXYCTR_TOOLS"] = str(BUNDLE / "tools" / system_key())
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
    return builder.main()


def load_builder(copy):
    os.environ["GALAXYCTR_ELF"] = str(copy.elf)
    os.environ["GALAXYCTR_RELEASE"] = copy.tag or "?"
    for name in [n for n in sys.modules if n in ("galaxyctr_builder", "texreduce", "yaz0", "convert_smg_files")]:
        del sys.modules[name]  # a failed first try must not leave half-imported modules behind
    sys.path.insert(0, str(copy.source / "tools" / "beta"))
    import galaxyctr_builder
    return galaxyctr_builder


if __name__ == "__main__":
    multiprocessing.freeze_support()
    sys.exit(main())
