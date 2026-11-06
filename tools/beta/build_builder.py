"""Packs the GalaxyCTR builder into one executable for the system it runs on (PyInstaller).

Usage: python tools/beta/build_builder.py --tools <folder> [--elf <program>] [--release-tag <tag>]

<folder> holds this system's helper programs: nodtool, bannertool, makerom (with .exe
on Windows). Without --elf, the public beta's program is built first (build_release_elf).
--release-tag names the release the executable is built for (default "local"): the launcher
compares it with GitHub's latest release. The executable lands in build/beta/dist/. Run it
once per system (Windows, Linux, macOS): PyInstaller only builds for the system it runs on.

What goes inside, and nothing else (no game data, see DETAILS.md):
  launcher.py                  the program that starts: updates, then runs the builder
  source/                      the builtin copy of the source files the builder runs
                               (SOURCE_FILES, at their places in the repository)
  source.json                  the release that copy belongs to
  payload/smg.elf              the port's program, built without game data
  tools/<system>/              the helper programs
  Python's standard modules    every one the source files import, plus a margin (EXTRA_MODULES)
                               so that later releases, downloaded by the launcher, run too
"""

import argparse
import ast
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "beta"))
from launcher import system_key  # noqa: E402 - the same name the builder looks for

TOOLS = ("nodtool", "bannertool", "makerom")

# The files the builder runs (the launcher finds them in a whole source tree, so the builtin copy
# keeps their places): the builder, the one-file converter, the format modules the builder reads
# the save icon with, the CIA's metadata and the port's version.
SOURCE_FILES = (
    "tools/beta/galaxyctr_builder.py",
    "tools/3ds/convert_smg_files.py",
    "tools/3ds/cia_showcase.rsf",
    "tools/3ds/builder/*.py",
    "platform/3ds/host/build_version.h",
)


# ##############################################################################################
# # Python modules inside the executable                                                       #
# #                                                                                            #
# # PyInstaller only packs the modules the program it starts from imports. The source files    #
# # are packed as plain files (so a downloaded release can take their place), so their         #
# # standard-library imports are listed here by reading them, the converter's embedded        #
# # modules (tools/3ds/builder, tools/3ds/native) included. EXTRA_MODULES adds the ones a       #
# # later release is likely to start using; one it needs beyond these raises LAUNCHER_NEEDED   #
# # in galaxyctr_builder.py. Tkinter (the converter's own window) is left out: the builder     #
# # never opens it.                                                                            #
# ##############################################################################################
SCANNED = ("tools/beta/*.py", "tools/3ds/convert_smg_files.py", "tools/3ds/builder/*.py", "tools/3ds/native/*.py")
EXTRA_MODULES = (
    "argparse", "array", "base64", "bisect", "bz2", "collections", "concurrent.futures", "contextlib",
    "copy", "csv", "ctypes", "dataclasses", "datetime", "decimal", "difflib", "enum", "fnmatch",
    "fractions", "functools", "glob", "gzip", "hashlib", "heapq", "hmac", "html", "http.client",
    "io", "itertools", "json", "logging", "lzma", "math", "mmap", "multiprocessing", "operator",
    "pickle", "platform", "queue", "random", "re", "secrets", "selectors", "shlex", "shutil",
    "signal", "socket", "sqlite3", "ssl", "statistics", "string", "struct", "subprocess",
    "tarfile", "tempfile", "textwrap", "threading", "time", "traceback", "typing", "unicodedata",
    "urllib.error", "urllib.parse", "urllib.request", "uuid", "wave", "weakref", "xml.etree.ElementTree",
    "zipfile", "zlib",
)


def stdlib_imports():
    found = set(EXTRA_MODULES)
    for pattern in SCANNED:
        for path in REPO.glob(pattern):
            for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
                if isinstance(node, ast.Import):
                    found.update(alias.name for alias in node.names)
                elif isinstance(node, ast.ImportFrom) and node.module and node.level == 0:
                    found.add(node.module)
    return sorted(name for name in found
                  if name.split(".")[0] in sys.stdlib_module_names and name.split(".")[0] != "tkinter")


# ##############################################################################################
# # The public beta's program                                                                  #
# #                                                                                            #
# # The same switches the developer's CIA script sets for the title-screen CIAs                #
# # (platform/3ds/host/showcase.h), plus SMG_PUBLIC_BETA for the beta's bottom screen text     #
# # (main.c). The header is put back as it was afterwards, and the normal build rebuilt.       #
# ##############################################################################################
BETA_SWITCHES = {"SMG_SHOWCASE": "1", "SMG_SHOWCASE_NATIVE": "1", "SMG_SHOWCASE_PROFILE": "1", "SMG_PUBLIC_BETA": "1"}


def build_release_elf():
    import re

    header = REPO / "platform" / "3ds" / "host" / "showcase.h"
    original = header.read_bytes()
    text = original.decode("utf-8")
    for name, value in BETA_SWITCHES.items():
        text, count = re.subn(rf"#define {name} \d", f"#define {name} {value}", text)
        if count != 1:
            sys.exit(f"{name} not found in {header}")
    text = re.sub(r'#define SMG_START_STAGE ".*"', '#define SMG_START_STAGE ""', text)
    text = re.sub(r'#define SMG_START_DEMO ".*"', '#define SMG_START_DEMO ""', text)
    ninja = ["ninja", "-C", str(REPO), "-f", "build/3ds/build.ninja"]
    target = REPO / "build" / "beta" / "stage" / "payload" / "smg.elf"
    try:
        header.write_bytes(text.encode("utf-8"))
        subprocess.check_call([sys.executable, str(REPO / "tools" / "3ds" / "configure.py")])
        # The objects that read these switches, rebuilt even if a header dependency is missed.
        subprocess.check_call(ninja + ["-t", "clean"] + [f"build/3ds/obj/platform/3ds/host/{name}.o" for name in ("main", "debug", "gfx")])
        subprocess.check_call(ninja + ["build/3ds/smg.elf"])
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((REPO / "build" / "3ds" / "smg.elf").read_bytes())
    finally:
        header.write_bytes(original)
        subprocess.check_call(ninja + ["-t", "clean"] + [f"build/3ds/obj/platform/3ds/host/{name}.o" for name in ("main", "debug", "gfx")])
        subprocess.check_call(ninja)
    return target


# ##############################################################################################
# # Packing                                                                                    #
# #                                                                                            #
# # Everything is staged in build/beta/stage first: the helper programs under their plain      #
# # names (find_tool in galaxyctr_builder.py asks for those), the source files at their        #
# # repository places, source.json. Then one PyInstaller run from launcher.py.                 #
# ##############################################################################################
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", help="the port's program; by default the public beta's, built now")
    parser.add_argument("--tools", help="folder with this system's helper programs")
    parser.add_argument("--release-tag", default="local", help="the release this executable is built for")
    parser.add_argument("--elf-only", action="store_true",
                        help="only build the public beta's program, for a release (see .github/workflows/builder.yml)")
    args = parser.parse_args()
    if args.elf_only:
        print(build_release_elf())
        return 0
    if not args.tools:
        parser.error("--tools is needed")

    exe = ".exe" if os.name == "nt" else ""
    sep = os.pathsep  # PyInstaller's "source<sep>destination" for added files
    key = system_key()
    stage = REPO / "build" / "beta" / "stage"
    shutil.rmtree(stage / "tools", ignore_errors=True)
    shutil.rmtree(stage / "source", ignore_errors=True)
    added = []
    for tool in TOOLS:
        matches = sorted(Path(args.tools).glob(f"{tool}*{exe}"))
        if not matches:
            sys.exit(f"{tool}{exe} not found in {args.tools}")
        staged = stage / "tools" / key / f"{tool}{exe}"
        staged.parent.mkdir(parents=True, exist_ok=True)
        staged.write_bytes(matches[0].read_bytes())
        if exe == "":
            staged.chmod(0o755)
    added.append(f"{stage / 'tools'}{sep}tools")

    for name in SOURCE_FILES:
        paths = sorted(REPO.glob(name))
        if not paths:
            sys.exit(f"{name} not found")
        for path in paths:
            target = stage / "source" / path.relative_to(REPO)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(path.read_bytes())
    (stage / "source.json").write_text(json.dumps({"tag": args.release_tag}), encoding="utf-8")
    added.append(f"{stage / 'source'}{sep}source")
    added.append(f"{stage / 'source.json'}{sep}.")
    elf = Path(args.elf).resolve() if args.elf else build_release_elf()
    added.append(f"{elf}{sep}payload")

    command = [sys.executable, "-m", "PyInstaller", "--onefile", "--console", "--clean", "--noconfirm",
               "--name", f"GalaxyCTR-Builder-{key}",
               "--distpath", str(REPO / "build" / "beta" / "dist"),
               "--workpath", str(REPO / "build" / "beta" / "pyinstaller"),
               "--specpath", str(REPO / "build" / "beta")]
    for module in stdlib_imports():
        command += ["--hidden-import", module]
    try:
        import certifi  # noqa: F401 - only checked for: the launcher's HTTPS certificates on macOS
        command += ["--hidden-import", "certifi", "--collect-data", "certifi"]
    except ImportError:
        print("Note: certifi is not installed (pip install certifi); macOS builders need it for HTTPS.")
    for item in added:
        command += ["--add-data", item]
    command.append(str(REPO / "tools" / "beta" / "launcher.py"))
    return subprocess.call(command)


if __name__ == "__main__":
    sys.exit(main())
