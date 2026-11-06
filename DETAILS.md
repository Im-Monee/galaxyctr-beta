# Super Mario Galaxy for New 3DS: design decisions

How the 3DS port is built, and why. Written for people who know nothing about the port yet.
If some code makes you think "why is it done this way?", the answer should be here. If it is
not, please report the gap.

**Contents**

1. [What this is](#1-what-this-is)
2. [AI use](#2-ai-use)
3. [Wii build untouched](#3-wii-build-untouched)
4. [Two worlds](#4-two-worlds)
5. [Graphics: zero GX](#5-graphics-zero-gx)
6. [Game files](#6-game-files)
7. [Wii SDK replaced](#7-wii-sdk-replaced)
8. [Reading the code](#8-reading-the-code)
9. [Repository map](#9-repository-map)
10. [New 3DS only](#10-new-3ds-only)
11. [Known compromises](#11-known-compromises)

---

## 1. What this is

| It is | It is not |
|---|---|
| A homebrew native port of *Super Mario Galaxy* (USA, `RMGE01`) for New 3DS, New 3DS XL and New 2DS XL. | A project of SMGCommunity, or endorsed by them. |
| Built on [Petari](https://github.com/SMGCommunity/Petari), the community decompilation: the game logic is Petari's C++ compiled for the 3DS's ARM CPU. | An emulator: no Wii machine code or GPU commands are interpreted at run time. |
| | A source of game data: no textures, models, sounds or code from the disc are in this repository or the CIA. Bring your own copy of the game. |

> [!IMPORTANT]
> Petari is written by hand and does not accept AI-generated contributions. This port is a
> separate project: nothing from it has been or will be sent to Petari.

## 2. AI use

The port's own code is written by an AI assistant (Claude Code). That is `platform/3ds/`,
`tools/3ds/`, and the `#ifdef __3DS__` branches in the decompiled sources. I direct the work,
decide what is acceptable and test every build on real hardware. I am not a programmer.

I would rather you know this before reading the code than find out later. Since I cannot vouch
for the code line by line, the project relies on checks that do not depend on trusting its
author:

| Check | What it guarantees |
|---|---|
| **Wii build bit-identical** | After every decomp change, the Wii `main.dol` must keep SHA-1 `9a71008ae1ee9010e267fa67d1f0b0d4f0e895dd`. Any accidental change to Wii behaviour fails. This repository holds only the game's code, not Petari's Wii build tools: to repeat the check, build `src/`, `include/` and `libs/` with the tools of [Petari's repository](https://github.com/SMGCommunity/Petari). |
| **Every decomp change listed** | [`PENDING_DECOMP_CHANGES.md`](platform/3ds/PENDING_DECOMP_CHANGES.md) records each 3DS-only change to `src/`, `include/` and `libs/`, with its reason. |
| **Real hardware only** | Every build is tested on a real New 2DS XL. Emulators never declare something working: one GPU hang never showed up in one. |
| **Independent audits** | Audit scripts in my development repository check every converted resource (materials, textures, models, particles, layouts) against the decomp's structures and Dolphin's GX rules, never against the port's own code. Fixed categories: identical, minor difference, reduced on purpose, approximated, not supported, not checked. Nothing is rounded up to "identical". They are tools, not part of the port, so they are not published here. |

Readable code is part of the same answer: see [section 8](#8-reading-the-code).

---

## 3. Wii build untouched

**Rule.** The decompiled sources (`src/`, `include/`, `libs/`) change only inside
`#ifdef __3DS__`. Everything the Wii compiler sees stays as Petari wrote it.

```cpp
#ifdef __3DS__
    // 3DS-only code: what it does and why.
#else
    // Petari's original code, untouched.
#endif
```

**Why.**

| Reason | |
|---|---|
| The Wii build is the reference | If it still matches the disc exactly, the logic compiled for the 3DS is the real game's. |
| The port stays separable | Anyone can see what the port changed and why. |
| Updates stay possible | New Petari code merges without untangling port edits. |

**Checked by** the SHA-1 above, and by a development script that shows each touched file as
the Wii compiler sees it and must report zero differences.

## 4. Two worlds

| Half | Contents | Compiled with |
|---|---|---|
| **Game** | Petari (`src/`), Wii SDK replacements (`platform/3ds/rvl/`), renderer (`platform/3ds/gx/`) | Wii conventions: `-fno-short-enums -fshort-wchar -fexec-charset=CP932`, the decomp's own standard headers |
| **Host** | `platform/3ds/host/`: graphics, audio, files, threads, input, via libctru and Citro3D | normal 3DS homebrew settings |

They talk only through the C header [`host.h`](platform/3ds/host/host.h).

**Why.** The game expects the Wii compiler's enum sizes, 2-byte `wchar_t`, Shift-JIS strings and
C headers; the 3DS libraries expect others. Mixed in one file, structure sizes silently
disagree. A thin C boundary makes every crossing explicit.

## 5. Graphics: zero GX

| | Wii (GX) | 3DS (PICA200) |
|---|---|---|
| Colour stages | 16 TEV stages | 6 combiners |
| Indirect textures | yes | no |
| Clipping | all sides | near plane only |
| Depth as a texture | yes | no |

**Rule.** No Wii GPU command is interpreted at run time. Draws go straight to the PICA through
Citro3D:

| Drawn | How |
|---|---|
| **Models** | Materials and vertices compiled on the PC (`tools/3ds/native/mat.py`, `n3d.py`) into PICA-ready `.n3d` files. `native.cpp` loads them; `native_submit.cpp` draws them with the game's matrices and animated values. |
| **Hand-written draws** (effects, HUD, water...) | `TDDraw` (`DirectDraw.*`): 3DS branches setting Citro3D state, through one GX-to-PICA table ([`GXToCitro3D.hpp`](platform/3ds/include/GXToCitro3D.hpp)). |
| **Particles** (JPA) | batched by `particle.cpp` |
| **2D layouts** (nw4r) | colour stages compiled by `tev_compile.cpp` |

**Why not emulate GX?** An earlier version did, interpreting GX commands on the CPU every
frame: far too slow. And work that never changes (materials, vertex and texture formats) is
better done once on the PC than 30 times a second on the console.

> [!NOTE]
> **The cost.** A draw not moved to Citro3D is invisible, so missing objects are bugs fixed one
> by one. Effects the PICA cannot express are approximated: see
> [`HARDWARE_LIMITS.md`](platform/3ds/HARDWARE_LIMITS.md) and the audit categories.

<details>
<summary><b>PICA traps the code works around</b> (each commented where handled)</summary>

| Trap | Workaround |
|---|---|
| Clipping on the near plane only | Huge triangles crossing the camera plane are clipped on the CPU (`native_clip.cpp`, `host/clip.c`). |
| Projected textures warp on large triangles | Those triangles are subdivided on the CPU. |
| The real GPU hangs unless a vertex shader writes every output component on every path | All shaders in `platform/3ds/host/shaders/` do. Emulators do not show this hang. |

</details>

## 6. Game files

**Rule.** A PC converter ([`convert_smg_files.py`](tools/3ds/convert_smg_files.py)) turns the
disc's files into 3DS-ready files on the SD card, once. The console never converts anything
heavy.

| Converted | Into |
|---|---|
| Byte order | little-endian (Wii is big-endian) |
| Textures | PICA formats, smaller where memory demands |
| Models | `.n3d` |
| Streamed music | PCM16 as on the disc, byte order only (a smaller AFC ADPCM version was dropped: with it the music stopped for good at random moments) |
| Movies | H.264, for the New 3DS hardware decoder |

**Why.** The 3DS CPU is far slower than a PC, SD writes in game run at about 45 KB/s, and
converting once keeps loading and memory down.

**Updates.** File formats carry a version number; when a release changes one, only the
affected files are redone.

## 7. Wii SDK replaced

`platform/3ds/rvl/` replaces what the game calls in the Wii SDK:

| Area | Replacement |
|---|---|
| Threads, interrupts, timers (OS) | 3DS threads; interrupts delivered at safe points |
| Disc reads (DVD) | SD card files |
| Saves (NAND) | SD card files |
| Wii Remote, Nunchuk (KPAD) | buttons, circle pad, touch screen |
| Audio DSP | JAudio2 runs unchanged; DSP mixing done in software on the game's core (`dsp.cpp`): on the third core the sound broke up whenever the game was busy |
| Mii (RFL) | heads built from the console's own Mii data |

**Performance (New 3DS):**

| | |
|---|---|
| CPU | 804 MHz, L2 cache on |
| Frame rate | 30 FPS, as on Wii |
| When behind | fewer frames drawn, game speed kept |
| Draw distances | as on Wii |

## 8. Reading the code

1. **Block headers.** Each block adapting part of the game opens with a separator and a
   plain-English summary of what and why:

   ```cpp
   // ############################################################
   // # Subdivision of projected textures                         #
   // # The PICA interpolates texture coordinates per triangle,   #
   // # so large triangles are cut on the CPU first ...           #
   // ############################################################
   ```

   Python uses `#`; C and C++ use `//`, since a line starting with `#` is a preprocessor
   directive there.
2. **Comments explain intent** on every line whose purpose is not obvious.
3. **Cross-references** name the related file and block, so you know where to look next.
4. **Commit messages** say what changed and why, in full sentences.

> [!NOTE]
> Older port code is being brought up to this standard area by area. Meanwhile, start from
> the [repository map](#9-repository-map).

## 9. Repository map

| Path | Holds |
|---|---|
| [`src/`](src/), [`include/`](include/), [`libs/`](libs/) | Petari's decompiled game; the port's changes to it are all under `#ifdef __3DS__` and listed in `PENDING_DECOMP_CHANGES.md` |
| [`platform/3ds/host/`](platform/3ds/host/) | the 3DS side: graphics state (`gfx.c`), audio, files, threads, startup (`main.c`) |
| [`platform/3ds/gx/`](platform/3ds/gx/) | native renderer: models, draw submission, clipping, particles, layouts |
| [`platform/3ds/rvl/`](platform/3ds/rvl/) | Wii SDK replacements |
| [`platform/3ds/include/`](platform/3ds/include/) | headers shared by the game half and the renderer |
| [`platform/3ds/PENDING_DECOMP_CHANGES.md`](platform/3ds/PENDING_DECOMP_CHANGES.md) | every decomp change, with reasons |
| [`platform/3ds/HARDWARE_LIMITS.md`](platform/3ds/HARDWARE_LIMITS.md) | Wii effects the 3DS GPU cannot match exactly |
| [`tools/3ds/convert_smg_files.py`](tools/3ds/convert_smg_files.py) | PC converter for the game files |
| [`tools/3ds/builder/`](tools/3ds/builder/), [`tools/3ds/native/`](tools/3ds/native/) | format converters, material compiler |
| [`tools/beta/`](tools/beta/) | the builder (disc image to SD card) and its packer |
| [`branding/`](branding/) | the project's logo |

## 10. New 3DS only

| Limit | On the original 3DS |
|---|---|
| Memory | 96 MB mode does not fit: about 13 MB over, even with reduced textures and no movies |
| CPU | about 28-40% of game speed in tests at its clock |

A low-memory mode exists but has never run on a real original 3DS, so the beta is New only.

> [!WARNING]
> A CIA installer cannot refuse a console model, so the game checks it at startup: on an original
> 3DS it shows why it cannot run and closes (`platform/3ds/host/main.c`).

## 11. Known compromises

- Textures smaller than on Wii where memory requires, mostly without mipmaps.
- Indirect-texture effects (water refraction, heat haze, distortions) approximated: the PICA has
  no indirect stage.
- Depth-of-field and screen blur, which read the depth buffer, approximated.
- Busiest areas, such as the Comet Observatory, still below full speed.

Per-resource numbers are in the audit results.
