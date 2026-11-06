<p align="center">
  <img src="branding/title.png" alt="GalaxyCTR" width="640">
</p>

<p align="center"><i>A Native port of Super Mario Galaxy for Nintendo 3DS.</i></p>

<p align="center">
  <a href="https://discord.gg/Fftp79uaJ7"><img src="https://img.shields.io/badge/Discord-Join%20the%20server-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Join the Discord"></a>
</p>

---

**Contents:** [Overview](#overview) · [Builder](#builder) · [Source Code](#source-code) ·
[Contributing](#contributing) · [Credits](#credits)

# Overview

A homebrew native port of *Super Mario Galaxy* (USA, `RMGE01`) for New 3DS, New 3DS XL and
New 2DS XL, built on the [Petari](https://github.com/SMGCommunity/Petari) decompilation. The
game's own code runs on the 3DS: it is not an emulator.

> [!IMPORTANT]
> This is **not** a project of SMGCommunity, and it is not endorsed by them. The port's code is
> written with AI assistance, which I state openly in [DETAILS.md](DETAILS.md#2-ai-use). Petari
> itself is written by hand and does not accept AI contributions.

> [!NOTE]
> **No game data is included.** You need your own copy of the game: the builder makes
> everything from it, on your computer.

| | |
|---|---|
| Stage | first public beta |
| Consoles | New 3DS, New 3DS XL, New 2DS XL only ([why](DETAILS.md#10-new-3ds-only)) |
| Game | USA release only (`RMGE01`) |
| Performance | 30 FPS target; busiest areas still below full speed |

**What you need**

| | |
|---|---|
| Console | a New 3DS family console with custom firmware ([Luma3DS](https://github.com/LumaTeam/Luma3DS)) and a CIA installer such as FBI |
| Game | your own disc image of *Super Mario Galaxy* (USA) |
| Computer | Windows, Linux or macOS, with about 9 GB free while the builder works |
| SD card | about 2 GB free |
| Sound | the console's own DSP firmware, `sdmc:/3ds/dspfirm.cdc`: run the homebrew app DSP1 once |

# Builder

The builder turns your disc image into everything the 3DS needs and copies it to the SD card.
Download it for your system from the [Releases](../../releases) page and run it.

> [!TIP]
> - **Windows:** if SmartScreen stops it, choose *More info*, then *Run anyway*.
> - **macOS:** the app is not signed: right-click it and choose *Open*, or run
>   `xattr -d com.apple.quarantine GalaxyCTR-Builder-macos-*` in Terminal.
> - **Linux:** make it executable first: `chmod +x GalaxyCTR-Builder-linux-x86_64`.

It works in six steps; every one of them is in
[`tools/beta/galaxyctr_builder.py`](tools/beta/galaxyctr_builder.py) (function names in the
last column).

| Step | What it does | Source code |
|---|---|---|
| 1. Disc image | Asks for the disc image (drag it into the window), in any format [nod-rs](https://github.com/encounter/nod-rs) reads (ISO, WBFS, RVZ and others). Checks that it is *Super Mario Galaxy*, USA release (`RMGE01`), revision 0, and says so if it is another game or region. | `choose_disc`, `read_disc_info` |
| 2. Extracting | Checks the free space, extracts the disc with nodtool and checks that `main.dol` is the original (SHA-1). Takes the HOME Menu icon and banner from the game's own Wii save icon (`SaveIconBanner.arc`). | `extract_disc`, `make_icon_and_banner` |
| 3. Converting | Converts the game files for the 3DS: byte order, textures, models into the port's `.n3d` format, music, movies into H.264 for the New 3DS video decoder (ffmpeg, downloaded if missing), the game data kept inside `main.dol`, and the SD file cache. The longest step. | `convert_files`, which runs [`tools/3ds/convert_smg_files.py`](tools/3ds/convert_smg_files.py) with [`tools/3ds/builder/`](tools/3ds/builder/) and [`tools/3ds/native/`](tools/3ds/native/) |
| 4. Building the CIA | Makes the icon and banner with bannertool, then the CIA with makerom from the port's program, built in advance and inside the builder. | `make_cia`, [`tools/3ds/cia_showcase.rsf`](tools/3ds/cia_showcase.rsf) |
| 5. Copying to the SD card | Asks for the SD card (a drive letter on Windows, a folder on Linux and macOS) and checks that it is writable and has room. Replaces the converted files in `smg/`, keeps your saves (`smg/nand`), copies the CIA to `cias/GalaxyCTR.cia`, and warns if the DSP firmware for the sound is missing. | `choose_sd_card`, `copy_to_sd`, `check_dsp_firmware` |
| 6. Cleaning up | Deletes the temporary folder, also when a step failed. | `build` |

At the end, put the SD card back into the 3DS and install `cias/GalaxyCTR.cia` with FBI. If
something goes wrong, the builder says what and why; everything it printed is also in
`galaxyctr-builder-log.txt` in your user folder.

How the builder itself is packed (PyInstaller, with nodtool, bannertool and makerom inside):
[`tools/beta/build_builder.py`](tools/beta/build_builder.py), run for every system by
[`.github/workflows/builder.yml`](.github/workflows/builder.yml).

# Source Code

This repository holds Petari's decompiled game together with the port:

| Path | Holds |
|---|---|
| [`src/`](src/), [`include/`](include/), [`libs/`](libs/) | Petari's decompiled game. Every change the port makes to it is under `#ifdef __3DS__`, so the Wii build stays byte-identical, and is listed with its reason in [PENDING_DECOMP_CHANGES.md](platform/3ds/PENDING_DECOMP_CHANGES.md). |
| [`platform/3ds/`](platform/3ds/) | the port: the 3DS side, the native renderer, the Wii SDK replacements |
| [`tools/3ds/`](tools/3ds/) | the PC converter of the game files and the build configuration |
| [`tools/beta/`](tools/beta/) | the builder and its packer |

To build the port's program, with [devkitPro](https://devkitpro.org/) installed (3DS toolchain,
libctru, citro3d):

```sh
python tools/3ds/configure.py
ninja -f build/3ds/build.ninja
```

## For programmers

Read **[DETAILS.md](DETAILS.md)** before the code: every major design decision and the reason
behind it, the checks that keep the port honest, how to read the code, a map of the repository
and the known compromises.

| Document | Contents |
|---|---|
| [DETAILS.md](DETAILS.md) | design decisions, checks, reading the code, repository map, known compromises |
| [platform/3ds/PENDING_DECOMP_CHANGES.md](platform/3ds/PENDING_DECOMP_CHANGES.md) | every change to the decompiled code, with reasons |
| [platform/3ds/HARDWARE_LIMITS.md](platform/3ds/HARDWARE_LIMITS.md) | Wii effects the 3DS GPU cannot match exactly |
| [Petari's README](https://github.com/SMGCommunity/Petari#readme) | the decompilation itself, and its tools to build it for Wii (not included here) |

## License

The port's own code is under the [MIT License](LICENSE). Petari's decompiled code keeps Petari's
own licence, CC0 ([LICENSE-Petari](LICENSE-Petari)).

# Contributing

The most useful help is a good bug report. Open an
[issue](../../issues) here on GitHub or, even better, post in the **#bug-reports** channel of
the [Discord server](https://discord.gg/Fftp79uaJ7), with the files below.

> [!IMPORTANT]
> The game rewrites its log every time it starts. After a crash or a bug, **copy the log and the
> crash file to your computer before you play again**, or they are lost.

| File | Where on the SD card | When |
|---|---|---|
| Game log | `sdmc:/smg/log.txt` | always |
| Crash file | `sdmc:/luma/dumps/arm11/`, the newest `.dmp` file | when the game crashed (Luma3DS shows an exception screen and saves this file) |
| Screenshots | `sdmc:/luma/screenshots/` | strongly recommended: they show what went wrong |
| Builder log | `galaxyctr-builder-log.txt` in your user folder on the computer | only for problems with the builder |

**Taking a screenshot.** When the problem is on screen, press **L + Down + Select** to open the
Rosalina menu, choose **Take screenshot**, then press **B** to go back to the game. Rosalina
saves both screens.

**Sending the files.**

1. Turn the console off and put the SD card into your computer (or copy the files over the
   network with an FTP homebrew app such as ftpd).
2. Copy the files from the paths above.
3. In **#bug-reports**, or in a new GitHub issue, write what you were doing, what went wrong, your
   console model, and the CIA version (it is on the first lines of `log.txt`). Attach the files:
   drag them into the message. On GitHub, put the `.dmp` file in a `.zip` first.

# Credits

| | |
|---|---|
| [SMGCommunity / Petari](https://github.com/SMGCommunity/Petari) | the decompilation this port is built on |
| [devkitPro](https://devkitpro.org/): libctru, citro3d | the 3DS toolchain and libraries |
| [3dbrew](https://www.3dbrew.org/) | 3DS hardware documentation |
| [Dolphin](https://dolphin-emu.org/) | the reference for Wii GPU behaviour used to check the port's graphics |
| [nod-rs](https://github.com/encounter/nod-rs) | reading the disc image in the builder |
| [Project_CTR](https://github.com/3DSGuy/Project_CTR) (makerom), [bannertool](https://github.com/diasurgical/bannertool) | building the CIA in the builder |
| [FFmpeg](https://ffmpeg.org/) | encoding the movies |

Super Mario Galaxy is a trademark of Nintendo. This project is not affiliated with Nintendo.
