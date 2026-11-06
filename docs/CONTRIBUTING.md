# Contributing

To report a bug, post in the **#bug-reports** channel of the
[Discord server](https://discord.gg/Fftp79uaJ7) (preferred), or open an [issue](../../../issues)
on GitHub.

> [!IMPORTANT]
> The game rewrites its log every time it starts. After a crash or a bug, **copy the log and the
> crash file to your computer before you play again**, or they are lost.

## Files to Send

| File | Where | When |
|---|---|---|
| **Game log** | `sdmc:/smg/log.txt` | always |
| **Crash file** | the newest `.dmp` file in `sdmc:/luma/dumps/arm11/` | after a crash (Luma3DS shows an exception screen and saves it) |
| **Screenshots** | `sdmc:/luma/screenshots/` | strongly recommended |
| **Builder log** | `galaxyctr-builder-log.txt` in your user folder on the computer | only for problems with the builder |

## Taking a Screenshot

When the problem is on screen, press **L + Down + Select** to open the Rosalina menu, choose
**Take screenshot**, then press **B** to go back to the game. Both screens are saved.

## Sending the Files

1. Turn the console off and put the SD card into your computer. You can also copy the files
   over the network with an FTP app such as ftpd.
2. Copy the files from the paths above.
3. Write what you were doing, what went wrong, your console model and the version. The version
   is on the first lines of `log.txt`.
4. Attach the files by dragging them into the message. On GitHub, put the `.dmp` file in a
   `.zip` first.

## Code

The port's code and its design are described in [the documentation](README.md). Every change to
Petari's decompiled code must stay under `#ifdef __3DS__` and be listed in
[PENDING_DECOMP_CHANGES.md](../platform/3ds/PENDING_DECOMP_CHANGES.md), as explained in
[DETAILS.md](DETAILS.md#3-the-wii-build-stays-identical).
