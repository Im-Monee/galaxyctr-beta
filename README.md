<p align="center">
  <img src="branding/title.png" alt="GalaxyCTR" width="640">
</p>

<p align="center"><i>A native port of Super Mario Galaxy for New Nintendo 3DS.</i></p>

<p align="center">
  <a href="https://discord.gg/Fftp79uaJ7"><img src="https://img.shields.io/badge/Discord-Join%20the%20server-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Join the Discord"></a>
</p>

---

**Contents:** [Overview](#overview) · [Builder](#builder) · [Source Code](#source-code) ·
[Contributing](#contributing) · [Credits](#credits)

# Overview

A homebrew native port of *Super Mario Galaxy* (USA, `RMGE01`) for the New 3DS family, built on
the [Petari](https://github.com/SMGCommunity/Petari) decompilation. The game's own code runs on
the 3DS: it is not an emulator.

> [!IMPORTANT]
> This is **not** a project of SMGCommunity, and it is not endorsed by them. The port's code is
> written with AI assistance, as explained in [DETAILS.md](DETAILS.md#2-ai-use). Petari itself is
> written by hand and does not accept AI contributions.

> [!NOTE]
> **No game data is included.** You need your own copy of the game: the builder makes
> everything from it, on your computer.

| | |
|---|---|
| **Stage** | first public beta |
| **Consoles** | New 3DS, New 3DS XL, New 2DS XL only ([why](DETAILS.md#11-new-3ds-only)) |
| **Game** | USA release only (`RMGE01`) |
| **Performance** | 30 FPS target; the busiest areas are still below full speed |

**What you need**

| | |
|---|---|
| **Console** | a New 3DS family console with [Luma3DS](https://github.com/LumaTeam/Luma3DS) and a CIA installer such as FBI |
| **Game** | your own disc image of *Super Mario Galaxy* (USA) |
| **Computer** | Windows, Linux or macOS, with about 9 GB free while the builder works |
| **SD card** | about 2 GB free |
| **Sound** | the console's DSP firmware, `sdmc:/3ds/dspfirm.cdc`: run the homebrew app DSP1 once |

# Builder

The builder turns your disc image into everything the 3DS needs and copies it to the SD card.
Download it for your system from [Releases](../../releases/latest) and run it.

> [!TIP]
> - **Windows:** if SmartScreen stops it, choose *More info*, then *Run anyway*.
> - **macOS:** the app is not signed: right-click it and choose *Open*, or run
>   `xattr -d com.apple.quarantine GalaxyCTR-Builder-macos-*` in Terminal.
> - **Linux:** make it executable first: `chmod +x GalaxyCTR-Builder-linux-x86_64`.

**It updates itself.** Next to itself the builder keeps a folder, `GalaxyCTR-Builder-files`,
with the source code and the port program it uses. At every start it checks for a newer
release and, if there is one, downloads it before doing anything else. Keep that folder next to
the builder; deleting it only means downloading it again.

| Step | What it does | Source code |
|---|---|---|
| Update | Checks GitHub for a newer release and downloads its source code and port program into `GalaxyCTR-Builder-files`. Without internet, it uses the newest copy it already has. | [`launcher.py`](tools/beta/launcher.py) |
| 1. Disc image | Asks for the disc image (you can drag it into the window), in any format [nod-rs](https://github.com/encounter/nod-rs) reads: ISO, WBFS, RVZ and others. Checks that it is *Super Mario Galaxy*, USA, revision 0. | `choose_disc`, `read_disc_info` |
| 2. Extracting | Checks the free space, extracts the disc and checks that `main.dol` is the original (SHA-1). Takes the HOME Menu icon and banner from the game's own save icon. | `extract_disc`, `make_icon_and_banner` |
| 3. Converting | Converts the game files for the 3DS: byte order, textures, models, music, movies (H.264, with FFmpeg, downloaded if missing), the game data inside `main.dol`, and the SD file cache. The longest step. | `convert_files`, which runs [`convert_smg_files.py`](tools/3ds/convert_smg_files.py) |
| 4. Building the CIA | Makes the icon and banner with bannertool, then the CIA with makerom from the port's program. | `make_cia`, [`cia_showcase.rsf`](tools/3ds/cia_showcase.rsf) |
| 5. Copying to the SD card | Asks for the SD card and checks that it is writable and has room. Replaces the game files in `smg/`, **keeps your saves** (`smg/nand`), copies the CIA to `cias/GalaxyCTR.cia`, and warns if the sound firmware is missing. | `choose_sd_card`, `copy_to_sd`, `check_dsp_firmware` |
| 6. Cleaning up | Deletes the temporary folder, also when a step failed. | `build` |

Steps 1 to 6 are in [`galaxyctr_builder.py`](tools/beta/galaxyctr_builder.py) (function names
in the last column).

At the end, put the SD card back into the 3DS and install `cias/GalaxyCTR.cia` with FBI. If
something goes wrong, the builder says what and why; everything it printed is also in
`galaxyctr-builder-log.txt`, in your user folder.

<details>
<summary><b>Building the builder yourself</b></summary>

Each release's source code (the *Source code* archives on the release page) holds everything
needed. With Python 3.12:

1. Install the packer: `pip install pyinstaller==6.22.3 certifi==2026.7.22`.
2. Put the three helper programs for your system in one folder:
   [nodtool](https://github.com/encounter/nod-rs/releases/tag/v2.0.0-alpha.12) (v2.0.0-alpha.12),
   [makerom](https://github.com/3DSGuy/Project_CTR/releases/tag/makerom-v0.19.0) (v0.19.0) and
   [bannertool](https://github.com/diasurgical/bannertool/releases/tag/1.2.0) (1.2.0; on macOS,
   compile it from its source).
3. Get the port's program: `smg.elf` from the same release, or build it yourself with
   devkitPro: `python tools/beta/build_builder.py --elf-only`.
4. Pack the builder:

   ```sh
   python tools/beta/build_builder.py --tools <helpers folder> --elf smg.elf --release-tag <release tag>
   ```

The executable is written to `build/beta/dist/`. The release's own builders are made the same
way by [`.github/workflows/builder.yml`](.github/workflows/builder.yml).

</details>

# Source Code

This repository holds Petari's decompiled game together with the port:

| Path | Holds |
|---|---|
| [`src/`](src/), [`include/`](include/), [`libs/`](libs/) | Petari's decompiled game. Every change the port makes to it is under `#ifdef __3DS__`, so the Wii build stays byte-identical, and is listed with its reason in [PENDING_DECOMP_CHANGES.md](platform/3ds/PENDING_DECOMP_CHANGES.md). |
| [`platform/3ds/`](platform/3ds/) | the port: the 3DS side, the native renderer, the Wii SDK replacements |
| [`tools/3ds/`](tools/3ds/) | the build configuration and the PC converter of the game files |
| [`tools/beta/`](tools/beta/) | the builder, its launcher and its packer |

To build the port's program, install [devkitPro](https://devkitpro.org/) (3DS toolchain,
libctru, citro3d) and [ninja](https://ninja-build.org/), then:

```sh
python tools/3ds/configure.py
ninja -f build/3ds/build.ninja
```

`configure.py` finds devkitPro in `C:/devkitPro` on Windows and in `/opt/devkitpro` on Linux and
macOS; pass `--devkitpro <folder>` if it is elsewhere.

## For Programmers

Start from **[DETAILS.md](DETAILS.md)**: every major design decision and its reason, the checks
that keep the port honest, how the builder updates itself, how to read the code, and a map of
the repository.

| Document | Contents |
|---|---|
| [DETAILS.md](DETAILS.md) | design decisions, checks, the builder, reading the code, repository map, known compromises |
| [PENDING_DECOMP_CHANGES.md](platform/3ds/PENDING_DECOMP_CHANGES.md) | every change to the decompiled code, with its reason |
| [HARDWARE_LIMITS.md](platform/3ds/HARDWARE_LIMITS.md) | Wii effects the 3DS GPU cannot match exactly |
| [Petari's README](https://github.com/SMGCommunity/Petari#readme) | the decompilation itself, and its Wii build tools (not included here) |

## License

The port's own code is under the [MIT License](LICENSE). Petari's decompiled code keeps Petari's
licence, CC0 ([LICENSE-Petari](LICENSE-Petari)).

# Contributing

The most useful help is a good bug report. Open an [issue](../../issues) here on GitHub or,
better, post in the **#bug-reports** channel of the [Discord server](https://discord.gg/Fftp79uaJ7),
with the files below.

> [!IMPORTANT]
> The game rewrites its log every time it starts. After a crash or a bug, **copy the log and the
> crash file to your computer before you play again**, or they are lost.

| File | Where | When |
|---|---|---|
| **Game log** | `sdmc:/smg/log.txt` | always |
| **Crash file** | `sdmc:/luma/dumps/arm11/`, the newest `.dmp` | when the game crashed (Luma3DS shows an exception screen and saves it) |
| **Screenshots** | `sdmc:/luma/screenshots/` | strongly recommended: they show what went wrong |
| **Builder log** | `galaxyctr-builder-log.txt`, in your user folder on the computer | only for problems with the builder |

**Taking a screenshot.** When the problem is on screen, press **L + Down + Select** to open the
Rosalina menu, choose **Take screenshot**, then press **B** to go back to the game. Both screens
are saved.

**Sending the files.**

1. Turn the console off and put the SD card into your computer (or copy the files over the
   network with an FTP app such as ftpd).
2. Copy the files from the paths above.
3. In **#bug-reports** or a new GitHub issue, write what you were doing, what went wrong, your
   console model and the version (on the first lines of `log.txt`). Attach the files by dragging
   them into the message; on GitHub, put the `.dmp` file in a `.zip` first.

# Credits

| | |
|---|---|
| [SMGCommunity / Petari](https://github.com/SMGCommunity/Petari) | the decompilation this port is built on |
| [devkitPro](https://devkitpro.org/): libctru, citro3d | the 3DS toolchain and libraries |
| [3dbrew](https://www.3dbrew.org/) | 3DS hardware documentation |
| [Dolphin](https://dolphin-emu.org/) | the reference for Wii GPU behaviour used to check the port's graphics |
| [nod-rs](https://github.com/encounter/nod-rs) | reading the disc image |
| [Project_CTR](https://github.com/3DSGuy/Project_CTR) (makerom), [bannertool](https://github.com/diasurgical/bannertool) | building the CIA |
| [FFmpeg](https://ffmpeg.org/) | encoding the movies |

Super Mario Galaxy is a trademark of Nintendo. This project is not affiliated with Nintendo.
