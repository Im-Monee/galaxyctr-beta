# Building From Source

## The Port's Program

Install [devkitPro](https://devkitpro.org/) (3DS toolchain, libctru, citro3d),
[ninja](https://ninja-build.org/) and Python 3, then run from the repository's root:

```sh
python tools/3ds/configure.py
ninja -f build/3ds/build.ninja
```

`configure.py` looks for devkitPro in `C:/devkitPro` on Windows and in `/opt/devkitpro` on Linux
and macOS. Pass `--devkitpro <folder>` if it is elsewhere. The result is `build/3ds/smg.elf`.

The game files are not part of the build. The builder converts them from the user's disc
([BUILDER.md](BUILDER.md)).

## The Repository

| Path | Holds |
|---|---|
| [`src/`](../src/), [`include/`](../include/), [`libs/`](../libs/) | Petari's decompiled game. Every change the port makes to it is under `#ifdef __3DS__`, so the Wii build stays byte-identical, and is listed in [PENDING_DECOMP_CHANGES.md](../platform/3ds/PENDING_DECOMP_CHANGES.md). |
| [`platform/3ds/host/`](../platform/3ds/host/) | the host world: startup (`main.c`), graphics state (`gfx.c`), CPU clipping (`clip.c`), audio, movies, files, threads, the diagnostics log (`debug.c`) |
| [`platform/3ds/gx/`](../platform/3ds/gx/) | the native renderer: models, draw submission, clipping, particles, layouts |
| [`platform/3ds/rvl/`](../platform/3ds/rvl/) | the Wii SDK replacements |
| [`platform/3ds/include/`](../platform/3ds/include/) | headers shared by the game and the renderer |
| [`tools/3ds/configure.py`](../tools/3ds/configure.py) | the build configuration (writes `build/3ds/build.ninja`) |
| [`tools/3ds/convert_smg_files.py`](../tools/3ds/convert_smg_files.py) | the PC converter of the game files |
| [`tools/3ds/builder/`](../tools/3ds/builder/), [`tools/3ds/native/`](../tools/3ds/native/) | the converter's format modules and the material compiler |
| [`tools/beta/`](../tools/beta/) | the builder, its launcher and its packer |
| [`docs/`](README.md) | this documentation |
| [`branding/`](../branding/) | the project's logo |

Why the code is split this way is in [DETAILS.md](DETAILS.md).
