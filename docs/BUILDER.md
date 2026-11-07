# The Builder

The builder turns your disc image into everything the 3DS needs and copies it to the SD card.
It is one program per system, downloaded from [Releases](../../../releases/latest).

## Steps

| Step | What it does | Source code |
|---|---|---|
| Update | Checks GitHub for a newer release. If there is one, downloads its source code and port program into `GalaxyCTR-Builder-files`. Without internet, uses the newest copy it already has. | [`launcher.py`](../tools/beta/launcher.py) |
| 1. Disc image | Asks for the disc image, in any format [nod-rs](https://github.com/encounter/nod-rs) reads (ISO, WBFS, RVZ and others). Checks that it is *Super Mario Galaxy*, USA, revision 0. | `choose_disc`, `read_disc_info` |
| 2. Extracting | Checks the free space, extracts the disc and checks that `main.dol` is the original (SHA-1). Takes the HOME Menu icon and banner from the game's own save icon. | `extract_disc`, `make_icon_and_banner` |
| 3. Converting | Converts the game files for the 3DS: byte order, textures, models, music, movies (H.264, with FFmpeg, downloaded if missing), the game data inside `main.dol`, and the SD file cache. The longest step. | `convert_files`, which runs [`convert_smg_files.py`](../tools/3ds/convert_smg_files.py) |
| 4. Building the CIA | Makes the icon and banner with bannertool, then the CIA with makerom from the port's program. | `make_cia`, [`cia_showcase.rsf`](../tools/3ds/cia_showcase.rsf) |
| Settings | Asks once whether to keep the game files between builds, and saves the answer in `settings.txt`. | `ask_keep_files`, `Settings` |
| 5. Copying to the 3DS | Says the build is complete and asks whether to copy now. Then asks how: over Wi-Fi with FTP (the address an FTP app on the 3DS shows, as `ip:port`) or to the SD card in this computer (its drive letter). Replaces the game files in `smg/`, **keeps your saves** (`smg/nand`), copies the CIA to `cias/GalaxyCTR.cia`, and warns if the sound firmware is missing. If you copy later, it prints the steps to do it by hand and saves them in `install_guide.txt`. | `copy_now`, `copy_over_ftp`, `choose_sd_card`, `copy_to_sd`, `write_install_guide` |
| 6. Cleaning up | Deletes the temporary folder, also when a step failed. It keeps the folder when you copy later, or when you chose to keep the game files. | `build` |

Steps 1 to 6 are in [`galaxyctr_builder.py`](../tools/beta/galaxyctr_builder.py). The last
column names its functions.

If something goes wrong, the builder says what and why. Everything it printed is also in
`galaxyctr-builder-log.txt`, in your user folder.

## Your Settings

Next to the builder, two text files are made:

| File | |
|---|---|
| `settings.txt` | your answers: `keep_files` (yes or no), `work_folder` (where the kept files are), `ftp_address` and `last_sd_letter`. Edit it, or delete it to be asked again. |
| `install_guide.txt` | the steps to copy the files by hand, when you chose to copy later |

With `keep_files = yes` the builder keeps the extracted disc and the converted game in a folder
you choose the first time (by default `GalaxyCTR-files`, next to the disc image). The next builds
do not ask for the disc image again, and they convert only the files a new release changes, so they
are much faster. The folder takes about 5 GB, and up to 9 GB while converting: the builder checks
the free space on its drive before it starts.

## Running It on macOS

The program is not signed. Right-click it and choose *Open*, or run this in Terminal:

```sh
xattr -d com.apple.quarantine GalaxyCTR-Builder-macos-*
```

## Updates

Next to itself the builder keeps a folder, `GalaxyCTR-Builder-files`:

| In the folder | |
|---|---|
| `source/` | the source code of the release in use |
| `smg.elf` | that release's port program |
| `release.json` | which release that is |

At every start it asks GitHub for the latest release (pre-releases do not count). If that is
not the one in the folder, it downloads it, checks it, and only then replaces the folder's
copy. A failed download leaves the old copy as it was. Deleting the folder only means
downloading it again.

Only a change to the launcher or to the helper programs needs a new download of the builder
program. The builder then tells you so, with the link. How this works inside is in
[DETAILS.md](DETAILS.md#8-the-builder-and-its-updates).

## Packing the Builder Yourself

Each release's source code (the *Source code* archives on the release page) holds everything
needed. With Python 3.12:

1. Install the packer: `pip install pyinstaller==6.22.3 certifi==2026.7.22`.
2. Put the three helper programs for your system in one folder:
   [nodtool](https://github.com/encounter/nod-rs/releases/tag/v2.0.0-alpha.12) (v2.0.0-alpha.12),
   [makerom](https://github.com/3DSGuy/Project_CTR/releases/tag/makerom-v0.19.0) (v0.19.0) and
   [bannertool](https://github.com/diasurgical/bannertool/releases/tag/1.2.0) (1.2.0). On macOS,
   bannertool has no ready-made program, so compile it from its source.
3. Get the port's program: `smg.elf` from the same release, or build it yourself
   ([BUILDING.md](BUILDING.md)) with `python tools/beta/build_builder.py --elf-only`.
4. Pack the builder:

   ```sh
   python tools/beta/build_builder.py --tools <helpers folder> --elf smg.elf --release-tag <release tag>
   ```

The program is written to `build/beta/dist/`. The release's own builders are made the same way
by [`builder.yml`](../.github/workflows/builder.yml).
