# GalaxyCTR: Design Details

How the port is built, and why, for someone who has never seen the code. If a design question
is not answered here, report it.

**Contents**

1. [What This Is](#1-what-this-is)
2. [AI Use](#2-ai-use)
3. [The Wii Build Stays Identical](#3-the-wii-build-stays-identical)
4. [Two Worlds: Game and Host](#4-two-worlds-game-and-host)
5. [Graphics Without GX](#5-graphics-without-gx)
6. [Game Files](#6-game-files)
7. [The Wii SDK, Replaced](#7-the-wii-sdk-replaced)
8. [The Builder and Its Updates](#8-the-builder-and-its-updates)
9. [Reading the Code](#9-reading-the-code)
10. [Repository Map](#10-repository-map)
11. [New 3DS Only](#11-new-3ds-only)
12. [Known Compromises](#12-known-compromises)

---

## 1. What This Is

| It is | It is not |
|---|---|
| A homebrew native port of *Super Mario Galaxy* (USA, `RMGE01`) for New 3DS, New 3DS XL and New 2DS XL. | A project of SMGCommunity, or endorsed by them. |
| Built on [Petari](https://github.com/SMGCommunity/Petari). The game's logic is Petari's C++, compiled for the 3DS's ARM CPU. | An emulator. No Wii machine code or GPU command is interpreted while the game runs. |
| | A source of game data. No texture, model, sound or code from the disc is in this repository or in the CIA. |

> [!IMPORTANT]
> Petari is written by hand and does not accept AI-generated contributions. This port is a
> separate project. Nothing from it has been or will be sent to Petari.

## 2. AI Use

The port's own code is written by an AI assistant (Claude Code). That covers `platform/3ds/`,
`tools/` and the `#ifdef __3DS__` branches in the decompiled sources. I direct the work, decide
what is acceptable and test every build on real hardware. I am not a programmer.

Since I cannot vouch for the code line by line, the project relies on checks that do not depend
on trusting its author:

| Check | What it guarantees |
|---|---|
| **Wii build bit-identical** | After every change to the decompiled code, the Wii `main.dol` must still have SHA-1 `9a71008ae1ee9010e267fa67d1f0b0d4f0e895dd`, so no change can alter the Wii game. To repeat the check, build `src/`, `include/` and `libs/` with the tools of [Petari's repository](https://github.com/SMGCommunity/Petari). They are not included here. |
| **Every decomp change listed** | [`PENDING_DECOMP_CHANGES.md`](../platform/3ds/PENDING_DECOMP_CHANGES.md) records each 3DS-only change, with its reason. |
| **Real hardware only** | Every build is tested on a real New 2DS XL. An emulator never decides that something works. One GPU hang never appeared in one. |
| **Independent audits** | Private scripts compare every converted resource (materials, textures, models, particles, layouts) with the decomp's structures and Dolphin's GX rules, never with the port's own code. Each result is identical, minor difference, reduced on purpose, approximated, not supported or not checked. Nothing is rounded up. They are development tools, so they are not published. |

---

## 3. The Wii Build Stays Identical

**Rule.** The decompiled sources (`src/`, `include/`, `libs/`) change only inside
`#ifdef __3DS__`, with a comment above each block saying what the 3DS does there and why.
Everything the Wii compiler sees stays as Petari wrote it.

```cpp
// 3DS: what this block does, and why.
#ifdef __3DS__
    // the 3DS code
#else
    // Petari's original code, untouched
#endif
```

| Why | |
|---|---|
| The Wii build is the reference | If it still matches the disc byte for byte, the logic compiled for the 3DS is the real game's. |
| The port stays separable | Anyone can see what the port changed, and why. |
| Petari updates stay possible | New Petari code merges without untangling port edits. |

**Checked by** the SHA-1 above, and by a script that shows every touched file as the Wii
compiler sees it and must report zero differences.

## 4. Two Worlds: Game and Host

| World | Contents | Compiled with |
|---|---|---|
| **Game** | Petari (`src/`), the Wii SDK replacements (`platform/3ds/rvl/`), the renderer (`platform/3ds/gx/`) | the Wii compiler's conventions: `-fno-short-enums -fshort-wchar -fexec-charset=CP932`, the decomp's own standard headers |
| **Host** | `platform/3ds/host/`: graphics, audio, files, threads, input, through libctru and Citro3D | normal 3DS homebrew settings |

The two worlds talk only through one C header, [`host.h`](../platform/3ds/host/host.h).

**Why.** The game expects the Wii compiler's enum sizes, a 2-byte `wchar_t`, Shift-JIS strings
and its own C headers. The 3DS libraries expect others. Mixed in one file, structure sizes
would silently disagree. A thin C boundary makes every crossing explicit. The build
configuration, [`configure.py`](../tools/3ds/configure.py), keeps the two sets of flags apart.

## 5. Graphics Without GX

| | Wii (GX) | 3DS (PICA200) |
|---|---|---|
| Colour stages | 16 TEV stages | 6 combiners |
| Indirect textures | yes | no |
| Clipping | all sides | near plane only |
| Depth as a texture | yes | no |

**Rule.** No Wii GPU command is interpreted while the game runs. Everything is drawn straight
on the PICA through Citro3D:

| What | How |
|---|---|
| **Models** | Materials and vertices are compiled on the PC (`tools/3ds/native/mat.py`, `n3d.py`) into PICA-ready `.n3d` files. [`native.cpp`](../platform/3ds/gx/native.cpp) loads them. [`native_submit.cpp`](../platform/3ds/gx/native_submit.cpp) draws them with the game's matrices and animated values. |
| **Hand-written draws** (effects, HUD, water...) | `TDDraw` (`DirectDraw.*`). The game's draw code has 3DS branches that set Citro3D state, through one GX-to-PICA table ([`GXToCitro3D.hpp`](../platform/3ds/include/GXToCitro3D.hpp)). |
| **Particles** (JPA) | batched by [`particle.cpp`](../platform/3ds/gx/particle.cpp) |
| **2D layouts** (nw4r) | colour stages compiled by [`tev_compile.cpp`](../platform/3ds/gx/tev_compile.cpp) |

**Why not emulate GX?** An early version did, interpreting GX commands on the CPU every frame,
and it was far too slow. Work that never changes (materials, vertex and texture formats) is
better done once on the PC than 30 times a second on the console.

> [!NOTE]
> **The cost.** A draw that was not moved to Citro3D is invisible, so a missing object
> is a bug to fix on its own. Effects the PICA cannot express are approximated, and
> [`HARDWARE_LIMITS.md`](../platform/3ds/HARDWARE_LIMITS.md) lists them.

**PICA traps the code works around.** Each one is commented where it is handled.

| Trap | Workaround |
|---|---|
| Clipping on the near plane only | Huge triangles that cross the camera plane are clipped on the CPU ([`native_clip.cpp`](../platform/3ds/gx/native_clip.cpp), [`clip.c`](../platform/3ds/host/clip.c)). |
| Projected textures warp on large triangles | Those triangles are subdivided on the CPU first (`native_clip.cpp`). |
| The real GPU hangs unless a vertex shader writes every output component, on every path | All shaders in `platform/3ds/host/shaders/` do. Emulators do not show this hang. |

## 6. Game Files

**Rule.** A PC converter, [`convert_smg_files.py`](../tools/3ds/convert_smg_files.py), turns
the disc's files into 3DS-ready files on the SD card, once. The console never converts anything
heavy.

| Converted | Into |
|---|---|
| Byte order | little-endian (the Wii is big-endian) |
| Textures | PICA formats, smaller where memory demands it |
| Models | `.n3d` (section 5) |
| Streamed music | PCM16 as on the disc, byte order only. A smaller ADPCM version was dropped because with it the music stopped for good at random moments. |
| Movies | H.264, for the New 3DS's hardware decoder |

**Why.** The 3DS CPU is far slower than a PC, writing to the SD card during the game runs at
about 45 KB/s, and converting once keeps loading times and memory down.

**Updates.** Each file format carries a version number (`FILES_VERSION` in the converter). When
a release changes one, only the affected files are converted again.

The converter is one self-contained file. The format modules of
[`tools/3ds/builder/`](../tools/3ds/builder/) and [`tools/3ds/native/`](../tools/3ds/native/)
are packed inside it by [`bundle_converter.py`](../tools/3ds/bundle_converter.py), which must
be run after changing them. Run on its own, the converter opens a small window. The builder
runs it without one.

## 7. The Wii SDK, Replaced

[`platform/3ds/rvl/`](../platform/3ds/rvl/) replaces what the game calls in the Wii SDK:

| Area | Replacement |
|---|---|
| Threads, interrupts, timers (OS) | 3DS threads, with interrupts delivered at safe points |
| Disc reads (DVD) | files on the SD card |
| Saves (NAND) | files on the SD card (`sdmc:/smg/nand`) |
| Wii Remote and Nunchuk (KPAD) | buttons, Circle Pad, touch screen |
| Audio DSP | JAudio2 runs unchanged. The DSP's mixing is done in software on the game's own core ([`dsp.cpp`](../platform/3ds/rvl/dsp.cpp)). On the third core the sound broke up whenever the game was busy. |
| Mii (RFL) | heads built from the console's own Mii data |

**Performance on New 3DS.** The CPU runs at 804 MHz with the L2 cache on. The game targets
30 FPS as on Wii. When it falls behind, fewer frames are drawn but the game speed is kept. Draw
distances are as on Wii.

## 8. The Builder and Its Updates

The builder turns the user's disc image into the CIA and the SD card's files. What it does step
by step is in [BUILDER.md](BUILDER.md). It is one program per system, made of two parts:

| Part | Source | Updated by |
|---|---|---|
| **Launcher**: Python itself, the helper programs (nodtool, bannertool, makerom), the update check | [`launcher.py`](../tools/beta/launcher.py), packed by [`build_builder.py`](../tools/beta/build_builder.py) | downloading a new program |
| **Builder**: the build steps, the converter, the CIA metadata, the port's program (`smg.elf`) | [`galaxyctr_builder.py`](../tools/beta/galaxyctr_builder.py) and the files it uses | itself, at every start |

**How it updates.** The launcher keeps the folder `GalaxyCTR-Builder-files` next to itself,
with the source code and the `smg.elf` of the release in use. At every start it asks GitHub for
the latest release. If that is not the one in the folder, it downloads it into
`GalaxyCTR-Builder-files/download`, checks it (a whole archive, the builder inside it, an ELF
program) and only then swaps it in with two renames. A failed download leaves the old copy as
it was. It then imports the builder from the folder and hands it the helper programs and the
port's program through the environment (`GALAXYCTR_TOOLS`, `GALAXYCTR_ELF`). Without internet,
it runs the newest copy it has, the folder's or the one inside the program.

**Why.** A release with fixes then reaches every user the next time they run the builder they
already have. Only a change to the launcher or the helper programs needs a new program. In that
case the builder declares a higher `LAUNCHER_NEEDED`, and an older launcher tells the user to
download the new one instead of running it.

**How a release is made.**

1. The developer builds the public beta's program (`python tools/beta/build_builder.py --elf-only`)
   and tests it on the console.
2. A GitHub release is created with that `smg.elf`.
3. The [`builder.yml`](../.github/workflows/builder.yml) workflow, run by hand with the
   release's tag, packs the program for Windows, Linux and macOS and attaches them to the
   release.

## 9. Reading the Code

1. **Section boxes.** Every block of port code opens with a box that sums up, in plain English,
   what it does and why:

   ```cpp
   // ##############################################################################
   // # Subdivision of projected textures                                          #
   // #                                                                            #
   // # The PICA interpolates texture coordinates per triangle, so large triangles #
   // # are cut on the CPU first ...                                               #
   // ##############################################################################
   ```

   Python files use `#`, the shader `;`, C and C++ `//`.
2. **Comments explain intent** wherever a line's purpose is not obvious.
3. **Cross-references** name the related file and section, so you know where to look next.
4. **Commit messages** say what changed and why, in full sentences.

## 10. Repository Map

What each folder holds is in [BUILDING.md](BUILDING.md#the-repository).

## 11. New 3DS Only

| Limit | On the original 3DS |
|---|---|
| Memory | the 96 MB mode does not fit: about 13 MB over, even with reduced textures and no movies |
| CPU | about 28-40% of game speed in tests at its clock |

A low-memory mode exists in the code but has never run on a real original 3DS, so the beta is
for the New models only.

> [!WARNING]
> A CIA installer cannot refuse a console model, so the game checks it at startup. On an
> original 3DS it says why it cannot run, and closes (`platform/3ds/host/main.c`).

## 12. Known Compromises

- Textures are smaller than on Wii where memory requires it, mostly without mipmaps.
- Indirect-texture effects (water refraction, heat haze, distortions) are approximated, since
  the PICA has no indirect stage.
- Depth of field and screen blur, which read the depth buffer, are approximated.
- The busiest areas, such as the Comet Observatory, still run below full speed.
