"""The port's loading screen, made from the user's own disc: the game's saving window.

While the port loads, before the title screen, the console's top screen shows the window the game
shows while it saves (SaveDataHandleSequence::trySave): the small system window
(SysInfoWindowMini) with the star that turns in its corner (SaveIcon, layout IconSave), here with
the words "GalaxyCTR is loading...", and the game's saving sounds. Nothing of it is in the builder
or in the repository: like the builder window's look, it is drawn on the user's computer from the
disc, by builder_layout.py (the game's layouts, pane by pane and animation by animation) and
builder_sounds.py (the game's sound driver).

What the game does, and so what is drawn here (60 frames a second):
  appear   the window's ButtonAppear on InfoWindowM and WinInfoWindowM (SysInfoWindow::appear),
           until it stops (exeAppear)
  wait     ButtonWait on both (exeWait), and the star appears with Rotate (SaveIcon::appear, once
           the window waits), placed and scaled at the window's SaveIconPosition pane
           (setLayoutScalePosAtPaneScaleTrans); from step 20 SE_SY_LV_SAVING sounds, a level
           sound that repeats while it is asked for
  end      the star goes (trySaveWindowDisappear kills it), the window's ButtonEnd until it stops,
           then SE_SY_SAVE_SUCCESS (executeSaveFinish, once the window is gone)
The console plays the wait frames in a loop until the title is ready (platform/3ds/host/loading.c).

On the console's screen the 608 x 456 layout is stretched across the 400 x 240 top screen and a
pane with the location-adjust flag is narrowed by 0.8 in its own x (LayoutManager::calcAnim on the
3DS), as builder_game.save_icon draws it.

Output, in <sd>/smg/loading (read by platform/3ds/host/loading.c):
  frames.bin   "GCLS", version, frame counts and the box, then every frame as RGB565, compressed
               (see write_frames)
  saving.pcm   SE_SY_LV_SAVING: one second of it, which repeats exactly (32 kHz, mono, PCM16)
  success.pcm  SE_SY_SAVE_SUCCESS (32 kHz, mono, PCM16)
"""

import struct
from pathlib import Path

TEXT = "GalaxyCTR is loading..."
SCREEN = (400, 240)
LAYOUT = (608.0, 456.0)
LOCATION_ADJUST = 0.8  # LayoutManager::calcAnim on the 3DS: (4:3) / (5:3)
SAVING_START = 20  # trySave: SE_SY_LV_SAVING from this step on
SAVING_LOOP = 32000  # SE_SY_LV_SAVING repeats every second exactly (checked 25 Nov 2026)
FORMAT_VERSION = 1
LAYOUTS = ("SysInfoWindowMini", "IconSave")
WINDOW_PANES = ("InfoWindowM", "WinInfoWindowM")


def _scenes(layouts):
    import builder_layout as L
    window = L.Scene(Path(layouts) / "SysInfoWindowMini")
    star = L.Scene(Path(layouts) / "IconSave")
    star.pane("IconSave").visible = True
    text = window.pane("TxtConfirm")
    # TextPos_Center (setTextBoxVerticalPositionCenterRecursive), and centred across as the game's
    # saving message is: System_Save01 starts with the tag that centres it (display group, tag 3,
    # CustomTagProcessor::exeDisplayGroupCenter).
    text.position = 4
    for scene in (window, star):
        for pane in scene.layout.root.walk():
            if pane.flag & 4:  # Pane::IsLocationAdjust
                pane.scale[0] *= LOCATION_ADJUST
    return window, star


def _place_star(window, star):
    """SaveIcon::calcAnim: the star's layout at SaveIconPosition's place (its global translation)
    and scale (its own)."""
    import builder_layout as L
    for pane, mtx, _ in L.walk_draw(window.pane("RootPane")):
        if pane.name == "SaveIconPosition":
            root = star.pane("RootPane")
            root.origin = (0.0, 0.0)  # walk_draw keeps the root's translation from there
            root.translate[0], root.translate[1] = mtx[2], mtx[5]
            root.scale[0], root.scale[1] = pane.scale[0], pane.scale[1]
            return


def _draw(renderer, window, star, with_star, fonts):
    """One frame of the top screen, as an RGB888 bytes object (400 x 240, rows from the top)."""
    from PySide6.QtCore import QRectF, Qt
    from PySide6.QtGui import QImage, QPainter
    k = SCREEN[1] / LAYOUT[1]
    box = (-LAYOUT[0] / 2, -LAYOUT[1] / 2, LAYOUT[0] / 2, LAYOUT[1] / 2)
    canvas = QImage(SCREEN[0], SCREEN[1], QImage.Format.Format_ARGB32_Premultiplied)
    canvas.fill(Qt.GlobalColor.black)
    p = QPainter(canvas)
    p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
    target = QRectF(0, 0, SCREEN[0], SCREEN[1])
    p.drawImage(target, renderer.render(window, "RootPane", k, box, {"TxtConfirm": TEXT}, fonts))
    if with_star:
        _place_star(window, star)
        p.drawImage(target, renderer.render(star, "RootPane", k, box))
    p.end()
    rgb = canvas.convertToFormat(QImage.Format.Format_RGB888)
    stride = rgb.bytesPerLine()
    data = bytes(rgb.constBits())
    return b"".join(data[y * stride:y * stride + SCREEN[0] * 3] for y in range(SCREEN[1]))


def render_frames(layouts, fonts):
    """(appear, wait, end): lists of frames, each 400 x 240 RGB888 from the top."""
    import builder_layout as L
    renderer = L.Renderer.get()
    if renderer is None:
        raise RuntimeError("no OpenGL to draw the loading screen")
    window, star = _scenes(layouts)
    appear, wait, end = [], [], []
    for pane in WINDOW_PANES:
        window.start_pane_anim(pane, "ButtonAppear")
    while True:
        appear.append(_draw(renderer, window, star, False, fonts))
        window.movement()
        if window.is_pane_anim_stopped(WINDOW_PANES[0]):
            break
    for pane in WINDOW_PANES:
        window.start_pane_anim(pane, "ButtonWait")
    star.start_pane_anim("PicIconSave", "Rotate")
    # The wait loops when both loops meet again: ButtonWait (10 frames) and Rotate (120).
    a, b = window.frame_max("ButtonWait"), star.frame_max("Rotate")
    period = a * b
    while b:
        a, b = b, a % b
    period //= a
    for _ in range(period):
        wait.append(_draw(renderer, window, star, True, fonts))
        window.movement()
        star.movement()
    for pane in WINDOW_PANES:
        window.start_pane_anim(pane, "ButtonEnd")
    while True:
        end.append(_draw(renderer, window, star, False, fonts))
        window.movement()
        if window.is_pane_anim_stopped(WINDOW_PANES[0]):
            break
    return appear, wait, end


def _rgb565(frame, box):
    left, top, right, bottom = box
    out = bytearray()
    for y in range(top, bottom):
        row = frame[(y * SCREEN[0] + left) * 3:(y * SCREEN[0] + right) * 3]
        for i in range(0, len(row), 3):
            r, g, b = row[i], row[i + 1], row[i + 2]
            out += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return bytes(out)


def _compress(pixels, previous):
    """A frame as runs of 16-bit pixels against the frame before (zeros when none): a run is a
    u16 count, top bit set for pixels kept from the frame before, else that many new pixels
    follow. The console decodes it straight into its frame (platform/3ds/host/loading.c)."""
    words = struct.unpack(f"<{len(pixels) // 2}H", pixels)
    before = struct.unpack(f"<{len(previous) // 2}H", previous) if previous else (0,) * len(words)
    out = bytearray()
    i, n = 0, len(words)
    while i < n:
        j = i
        while j < n and j - i < 0x7FFF and words[j] == before[j]:
            j += 1
        if j > i:
            out += struct.pack("<H", 0x8000 | (j - i))
            i = j
            continue
        while j < n and j - i < 0x7FFF and (words[j] != before[j] or (j + 1 < n and words[j + 1] != before[j + 1])):
            j += 1
        j = max(j, i + 1)
        out += struct.pack("<H", j - i)
        out += struct.pack(f"<{j - i}H", *words[i:j])
        i = j
    return bytes(out)


def write_frames(path, appear, wait, end):
    """frames.bin: header, then each frame's compressed RGB565 in the box every frame fits in.
    Header: "GCLS", u16 version, u16 appear/wait/end counts, u16 box left/top/width/height
    (screen pixels, rows from the top), then a u32 size per frame, then the frames."""
    frames = appear + wait + end
    left, top, right, bottom = SCREEN[0], SCREEN[1], 0, 0
    for frame in frames:
        for y in range(SCREEN[1]):
            row = frame[y * SCREEN[0] * 3:(y + 1) * SCREEN[0] * 3]
            if any(row):
                top, bottom = min(top, y), max(bottom, y + 1)
                first = next(i for i in range(0, len(row), 3) if row[i] or row[i + 1] or row[i + 2]) // 3
                last = max(i for i in range(0, len(row), 3) if row[i] or row[i + 1] or row[i + 2]) // 3
                left, right = min(left, first), max(right, last + 1)
    if right <= left:
        raise RuntimeError("the loading screen drew nothing")
    left, right = left & ~1, min(SCREEN[0], (right + 1) & ~1)
    box = (left, top, right, bottom)
    blobs, previous = [], None
    for index, frame in enumerate(frames):
        pixels = _rgb565(frame, box)
        # Each part starts from black: the console may start a part without the frame before.
        if index in (0, len(appear), len(appear) + len(wait)):
            previous = None
        blobs.append(_compress(pixels, previous))
        previous = pixels
    header = b"GCLS" + struct.pack("<7H", FORMAT_VERSION, len(appear), len(wait), len(end),
                                   left, top, right - left) + struct.pack("<H", bottom - top)
    header += b"".join(struct.pack("<I", len(b)) for b in blobs)
    Path(path).write_bytes(header + b"".join(blobs))
    return box, sum(len(b) for b in blobs)


def write_sounds(audio_res, out):
    """saving.pcm (one second that repeats) and success.pcm, PCM16 little-endian, 32 kHz mono, at
    the level the game's mixer gives them (not builder_sounds.GAIN, the builder window's own)."""
    import builder_sounds as S
    archive = S.Archive(audio_res)
    saving = S.render(archive, "SE_SY_LV_SAVING", limit_seconds=3.0)
    success = S.render(archive, "SE_SY_SAVE_SUCCESS")
    if len(saving) < 2 * SAVING_LOOP or not success:
        raise RuntimeError("the saving sounds did not render")

    def pcm(samples):
        return b"".join(struct.pack("<h", max(-32768, min(32767, int(round(v))))) for v in samples)

    (Path(out) / "saving.pcm").write_bytes(pcm(saving[SAVING_LOOP:2 * SAVING_LOOP]))
    (Path(out) / "success.pcm").write_bytes(pcm(success))


def make(files, work, out, report=None):
    """Draws the loading screen from the disc's files into out (the SD's smg/loading). work: a
    folder for the exported layouts and font. Raises when it cannot (no OpenGL, files missing):
    the port then starts without it, as before."""
    import builder_assets as A
    files, work, out = Path(files), Path(work), Path(out)
    for name in LAYOUTS:
        A.export_layout(files / "LayoutData" / f"{name}.arc", work / "layouts" / name)
    fonts = A.read_archive(files / "LayoutData" / "Font.arc")
    (work / "fonts").mkdir(parents=True, exist_ok=True)
    A.export_font(fonts[A.UI_FONT], work / "fonts", Path(A.UI_FONT).stem)
    if report:
        report("frames")
    appear, wait, end = render_frames(work / "layouts", work / "fonts")
    out.mkdir(parents=True, exist_ok=True)
    box, size = write_frames(out / "frames.bin", appear, wait, end)
    if report:
        report("sounds")
    write_sounds(files / "AudioRes", out)
    return {"appear": len(appear), "wait": len(wait), "end": len(end), "box": box, "bytes": size}


def is_current(out):
    """Whether out already holds this version's loading screen (made by an earlier build)."""
    out = Path(out)
    try:
        head = (out / "frames.bin").read_bytes()[:6]
    except OSError:
        return False
    return head[:4] == b"GCLS" and struct.unpack("<H", head[4:6])[0] == FORMAT_VERSION and \
        (out / "saving.pcm").is_file() and (out / "success.pcm").is_file()
