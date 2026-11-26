# The Builder

The builder turns your disc image into everything the 3DS needs and copies it to the SD card.
It is one program per system, downloaded from [Releases](../../../releases/latest).

It opens a window: a start page, *Settings*, the build with its six steps, its progress and
everything it says, and a last page that says how it ended. Its questions open small dialogs
with the matching picker (a file, a folder, the SD card's drive). When the system asks for less
motion, its background and effects stand still. Without a display, or with
`--console`, it asks the same questions in a terminal. The window is
[`builder_window.py`](../tools/beta/builder_window.py), drawn with Qt (PySide6).

## The Game's Look

On the first start, before anything else, Windows' own file dialog asks for your disc image,
and a small window reads it once. That window appears, in the game's look, as soon as the game's
layouts have come out of the disc (a few seconds), and the update check and everything after it
wear the game's own look too. When the builder already knows the disc, it does not ask. From the disc it takes
the game's own layouts, font, menu sounds, music, particle effects, the GalaxySky, CometNearOrbitSky and PowerStarHalo models. The window then draws them as
the game does. Each button is the game's own: its panes in the same order (the shadow, the frame,
the face, the text, the shine), its size, and its animations and sounds when the pointer comes on
it, leaves it and presses it. Every button casts the same shadow, the one of the galaxy's Start
button, as all the buttons of one game screen do. The steps of a build show the pause menu's star places: an empty
dark place until the step is done, then the star on it, and the running step spins the star
the game shows while it saves, once the build is at work (not while a question waits for
you). A step that fails shows the same red "no" sign as a stopped build. The name of the
page is at the top left on the same plate as the main menu's notes, with Mario's head. Every graphic, colour and animation comes from the game, as it
is: the game's sky behind every page, the font of its messages, the Mii screen's page counter
for short words, fields and choices. The panels and the questions are the
game's system window, the one that asks to create a new save file: its frame and shadow, its
text colour, and the striped dimming behind it. The choice of keeping the game files and the
number of FTP connections move with the Mii screen's blue arrows, one step a press, with their
animations and page sound. A
finished build shows the Power Star with the sound of a star collected. A stopped one shows
the camera's red "no" sign with the sound of the file select's refused choice. Both pages lead back to the main menu with the same
button. The Comet Observatory's music, from the disc too, plays behind every page. The
small button in the top right corner pauses it, and it goes on from the same moment. That
button is the pause menu's button for Luigi's letter, with the music note of the game's
message font in place of the letter, drawn as the letter is, and while the music is off
the camera's small red "no" sign on its corner, moving with the button. The same
button with the font's open hand chooses a file or a folder. Short messages come up from
the bottom of the window in the game's system window, and in *Settings* they stay between
*Back* and *Save*. The mouse
pointer is the game's pointing hand, as in its menus. Behind the logo on the main menu
shines the PowerStarHalo, the light the game shows where a Power Star is while the camera
is still far from it, fading in with its own animation, its glow flickering as in the game. The window opens on the game's
title screen: its sky with the comet, its small shooting stars and the planet, the logo with its animations and
lights, and its music. Where the game asks to press A and B, it asks to press the Space bar,
and that leads to the main menu, each of its parts coming in from its own side. The button at the bottom of the main menu goes back to the title
screen: the parts of the menu leave the way they came, while the music fades, and the title
screen starts again from the beginning with its music. The title screen's sky stays behind
every page and keeps turning, also when the title screen comes back. When Windows' Animation effects setting is off, the window stands still, its sky
too, and the main menu says so with a button that turns the setting back on. Everything
moves again at once. Settings has a General part and a Video part, the Video one with the frame rate (60 by default, the
screen's own, a number up to 240, or no limit), VSync, which graphics card draws the window
on a computer with two, the motion (follow the system, always or still) and a frame rate
counter. The game's own frames stay 60 a second, as in the game, and past 60 its layouts are
drawn in between, so the title screen and the buttons move smoothly at any rate. Everything is kept in
`GalaxyCTR-game-ui`, in your user settings folder (see [Your Settings](#your-settings)), on your
computer only. Nothing of the game is in the builder or this repository. Every file of it is
checked at each start: if one was changed (a texture drawn over, for example) or deleted, the
builder makes the whole set again from your disc. Changed graphics are not used. You can choose *Later* and build straight away. The build reads the
disc anyway and makes them too. Without OpenGL the window keeps its own drawn look. The build runs in a
process of its own, below normal priority, so the window, its sky and its music stay smooth
while the game files are converted.

| File | What it does |
|---|---|
| [`builder_firststart.py`](../tools/beta/builder_firststart.py) | the first start's small window: asks for the disc and gets the builder ready in a process of its own |
| [`builder_assets.py`](../tools/beta/builder_assets.py) | copies the game's layouts (`LayoutData`), writes their textures and the font as PNG, and writes the sky model (`ObjectData/GalaxySky.arc`) for OpenGL |
| [`builder_font.py`](../tools/beta/builder_font.py) | traces the game's message font (`LayoutData/Font.arc`) into a small TrueType font, so the window's own words are in the game's font too |
| [`builder_loading.py`](../tools/beta/builder_loading.py) | makes the port's loading screen from your disc: the game's saving window with its turning star, the words "GalaxyCTR is loading..." and the saving sounds, shown on the 3DS before the title screen |
| [`builder_layout.py`](../tools/beta/builder_layout.py) | reads layouts (`.brlyt`), their animations (`.brlan`) and fonts (`.brfnt`), and draws them with OpenGL as `nw4r::lyt` does |
| [`builder_game.py`](../tools/beta/builder_game.py) | the game's buttons, windows and icons in the window, with the game's button controller (`ButtonPaneController`) |
| [`builder_sounds.py`](../tools/beta/builder_sounds.py) | plays the menu sound effects' sequences (`AudioRes/SMR.szs`) through a small copy of the game's sound driver (JAudio2) and writes them as WAV files |
| [`builder_music.py`](../tools/beta/builder_music.py) | turns the game's music streams (`AudioRes/Stream`) into WAV files and plays them round and round from their loop point, pausing where they are |
| [`builder_particles.py`](../tools/beta/builder_particles.py) | copies the game's particle effects (`ParticleData/Effect.arc`) the title screen shows and runs and draws them as the game's particle system (JParticle) does |
| [`builder_exeicon.py`](../tools/beta/builder_exeicon.py) | on Windows, gives the builder program's own file the game's Power Star as its icon once your disc is read, and its first icon back when the disc image is gone. The new icon shows once the builder is closed, and no other file is left next to the program. Nothing of the game is in the published program |
| [`builder_halo.py`](../tools/beta/builder_halo.py) | draws the PowerStarHalo model (`ObjectData/PowerStarHalo.arc`) with its joints, its colour animation and its texture animation, behind the main menu's logo |
| [`builder_sky.py`](../tools/beta/builder_sky.py) | draws the skies with OpenGL, the title screen's too with its joints, texture animations and projection-mapped textures. Each material becomes a shader that does what the Wii's pixel stages do, with its hidden faces, its depth test and the title screen's lights from `ObjectData/LightData.arc`, in the order the game draws them (the opaque materials, then the translucent ones, each by name). The title screen's sky also has its small shooting stars, the particle effect the game puts on it. Without OpenGL the window keeps its own drawn sky |

## Steps

| Step | What it does | Source code |
|---|---|---|
| Update | Checks GitHub for a newer release. If there is one, downloads its source code and port program into `GalaxyCTR-Builder-files`. Without internet, uses the newest copy it already has. | [`launcher.py`](../tools/beta/launcher.py) |
| 1. Disc image | Asks for the disc image, in any format [nod-rs](https://github.com/encounter/nod-rs) reads (ISO, WBFS, RVZ and others). Checks that it is *Super Mario Galaxy*, USA, revision 0. An image that cannot be read yet is named as such: one still being copied or unpacked, an incomplete ISO, or one a cloud folder such as OneDrive keeps online. | `choose_disc`, `read_disc_info`, `disc_not_ready` |
| 2. Extracting | Checks the free space, extracts the disc and checks that `main.dol` is the original (SHA-1). Takes the HOME Menu icon and banner from the game's own save icon. | `extract_disc`, `make_icon_and_banner` |
| 3. Converting | Converts the game files for the 3DS: byte order, textures, models, music, movies (H.264, with FFmpeg, downloaded if missing), the game data inside `main.dol`, and the SD file cache. The longest step. | `convert_files`, which runs [`convert_smg_files.py`](../tools/3ds/convert_smg_files.py) |
| 4. Building the CIA | Makes the icon and banner with bannertool, then the CIA with makerom from the port's program. | `make_cia`, [`cia_showcase.rsf`](../tools/3ds/cia_showcase.rsf) |
| Settings | Asks once whether to keep the game files between builds, and saves the answer in `settings.txt`. | `ask_keep_files`, `Settings` |
| 5. Copying to the 3DS | Says the build is complete and asks whether to copy now. Then asks how: over Wi-Fi with FTP (the address an FTP app on the 3DS shows, as `ip:port`, and how many files to send at once, 1 to 10, 5 by default) or to the SD card in this computer (its drive letter). Replaces the game files in `smg/`, **keeps your saves** (`smg/nand`), copies the CIA to `cias/GalaxyCTR.cia`, and warns if the sound firmware is missing. If you copy later, it prints the steps to do it by hand and saves them in `install_guide.txt`. | `copy_now`, `copy_over_ftp`, `ftp_parallel`, `choose_sd_card`, `copy_to_sd`, `write_install_guide` |
| 6. Cleaning up | Deletes the temporary folder, also when a step failed. It keeps the folder when you copy later, or when you chose to keep the game files. | `build` |

Steps 1 to 6 are in [`galaxyctr_builder.py`](../tools/beta/galaxyctr_builder.py). The last
column names its functions.

If something goes wrong, the builder says what and why. Everything it printed is also in
`galaxyctr-builder-log.txt`, in your user folder.

## Where Everything Goes

On the SD card, the builder writes only these, and leaves everything else as it is:

| On the SD card | What it is |
|---|---|
| `smg/files/` | the game files, converted for the 3DS |
| `smg/native/` | the game's models, compiled for the 3DS's GPU (`.n3d`) |
| `smg/port/` | the game data that is inside `main.dol` on the disc |
| `smg/cache/` | the file cache, so the game finds its files faster |
| `smg/loading/` | the loading screen shown before the title screen |
| `smg/files_version.txt` | which version of the converted files this is, written last |
| `cias/GalaxyCTR.cia` | the game, to install with FBI |

`smg/nand/` holds your saves and `smg/log.txt` the game's log. The builder never touches them.

On the computer:

| On the computer | What it is |
|---|---|
| your user settings folder (see below) | `settings.txt`, the game's look (`GalaxyCTR-game-ui`) and the release in use (`GalaxyCTR-Builder-files`) |
| the kept files' folder (`GalaxyCTR-files` by default) | only with `keep_files = yes`: `original/` (the disc as extracted), `game/` (the converted game) and the CIA |
| a temporary folder | without `keep_files`: the same, deleted at the end |
| next to the builder | `install_guide.txt`, only when you copy later |
| your user folder | `galaxyctr-builder-log.txt`, the log of the last run |

## Your Settings

| File | |
|---|---|
| `settings.txt` | your answers: `keep_files` (yes or no), `work_folder` (where the kept files are), `ftp_address`, `ftp_connections` (1 to 10, 5 when missing), `last_sd_letter`, and `remember_window` (yes or no) with `window_geometry` (where the window was left: its position, its size as you dragged its borders, whether it was maximized, and the screen). Change them in *Settings*, edit the file, or delete it to be asked again. |
| `install_guide.txt` | the steps to copy the files by hand, when you chose to copy later. It is the only file made next to the builder. |

Everything else the builder keeps is in your user settings folder (`%APPDATA%\GalaxyCTR` on
Windows, `~/Library/Application Support/GalaxyCTR` on macOS, `~/.config/GalaxyCTR` on Linux):
`settings.txt`, the game's look (`GalaxyCTR-game-ui`) and the release in use
(`GalaxyCTR-Builder-files`). A builder program downloaded again into another folder finds them
there. What an older builder left next to the program is moved there the first time.

You can edit `settings.txt`, but each line must be a comment (starting with `#`), empty, or
`name = value` with a name from the list above (or one of the graphics settings) written once,
and a value of the right kind: `yes` or `no`, a full path with its brackets closed, an address
such as `192.168.1.10:5000`, a number from 1 to 10, a single letter. If any line is wrong, the
builder empties the file at once, before it asks anything, and asks its questions again.

**Over Wi-Fi**, the game is thousands of files and each one costs a pause on the 3DS. So the
builder opens several FTP connections and each one sends the next file: one list of files,
largest first. A file whose transfer breaks is sent again on a new connection. If the FTP app
refuses some connections, the builder carries on with the ones it accepted.

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

In your user settings folder the builder keeps `GalaxyCTR-Builder-files`:

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
program. The builder then tells you so, with the link.

A builder program from before the window (the first releases, which ran in a terminal) replaces
itself: it downloads the new program for your system, puts it in its own place under the same
name, starts it and closes. If that fails, it builds in the terminal as before and gives the
link. How this works inside is in [DETAILS.md](DETAILS.md#8-the-builder-and-its-updates).

## Packing the Builder Yourself

Each release's source code (the *Source code* archives on the release page) holds everything
needed. With Python 3.12:

1. Install the packer and Qt: `pip install pyinstaller==6.22.3 certifi==2026.7.22 PySide6-Essentials==6.11.2`.
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
