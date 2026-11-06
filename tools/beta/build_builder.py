"""Packs the GalaxyCTR builder into one executable for the system it runs on (PyInstaller).

Usage: python tools/beta/build_builder.py --tools <folder> [--elf <program>]

<folder> holds this system's helper programs: nodtool, bannertool, makerom (with .exe
on Windows). Without --elf, the public beta's program is built first (build_release_elf).
The executable lands in build/beta/dist/. Run it once per system (Windows, Linux,
macOS): PyInstaller only builds for the system it runs on.

What goes inside, and nothing else (no game data, see DETAILS.md):
  galaxyctr_builder.py         the builder itself (tools/beta)
  convert_smg_files.py         the converter, which carries its own modules (tools/3ds)
  texreduce.py, yaz0.py, ...   the format modules the builder reads the save icon with
  tools/<system>/              the helper programs
  payload/smg.elf              the port's program, built without game data
  payload/cia.rsf              the CIA's metadata (tools/3ds/cia_showcase.rsf)
  payload/build_version.h      the port's version
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "beta"))
from galaxyctr_builder import system_key  # noqa: E402 - the same name the builder looks for

TOOLS = ("nodtool", "bannertool", "makerom")


# ############################################################################
# # The public beta's program                                              #
# #                                                                         #
# # The same switches the developer's CIA script sets for the title-screen #
# # CIAs (platform/3ds/host/showcase.h), plus SMG_PUBLIC_BETA              #
# # for the beta's bottom screen text (main.c). The header is put back as  #
# # it was afterwards, and the normal build rebuilt.                        #
# ############################################################################

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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", help="the port's program; by default the public beta's, built now")
    parser.add_argument("--tools", help="folder with this system's helper programs")
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
    added = []
    for tool in TOOLS:
        matches = sorted(Path(args.tools).glob(f"{tool}*{exe}"))
        if not matches:
            sys.exit(f"{tool}{exe} not found in {args.tools}")
        # Each tool under its plain name, which find_tool() asks for.
        staged = REPO / "build" / "beta" / "stage" / "tools" / key / f"{tool}{exe}"
        staged.parent.mkdir(parents=True, exist_ok=True)
        staged.write_bytes(matches[0].read_bytes())
        if exe == "":
            staged.chmod(0o755)
    added.append(f"{REPO / 'build' / 'beta' / 'stage' / 'tools'}{sep}tools")
    elf = Path(args.elf).resolve() if args.elf else build_release_elf()
    added.append(f"{elf}{sep}payload")
    stage = REPO / "build" / "beta" / "stage" / "payload"
    stage.mkdir(parents=True, exist_ok=True)
    (stage / "cia.rsf").write_bytes((REPO / "tools" / "3ds" / "cia_showcase.rsf").read_bytes())
    (stage / "build_version.h").write_bytes((REPO / "platform" / "3ds" / "host" / "build_version.h").read_bytes())
    added.append(f"{stage / 'cia.rsf'}{sep}payload")
    added.append(f"{stage / 'build_version.h'}{sep}payload")

    command = [sys.executable, "-m", "PyInstaller", "--onefile", "--console", "--clean", "--noconfirm",
               "--name", f"GalaxyCTR-Builder-{key}",
               "--distpath", str(REPO / "build" / "beta" / "dist"),
               "--workpath", str(REPO / "build" / "beta" / "pyinstaller"),
               "--specpath", str(REPO / "build" / "beta"),
               "--paths", str(REPO / "tools" / "3ds"), "--paths", str(REPO / "tools" / "3ds" / "builder"),
               "--hidden-import", "convert_smg_files", "--hidden-import", "texreduce", "--hidden-import", "yaz0"]
    for item in added:
        command += ["--add-data", item]
    command.append(str(REPO / "tools" / "beta" / "galaxyctr_builder.py"))
    return subprocess.call(command)


if __name__ == "__main__":
    sys.exit(main())
