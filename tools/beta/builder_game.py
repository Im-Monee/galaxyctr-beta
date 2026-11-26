"""The game's own buttons, windows, icons and sounds in the builder window.

builder_assets.make() copies them from the user's disc into the game UI folder; builder_layout
draws the layouts with OpenGL. This module puts them in Qt widgets:

  Skin              the game UI folder, as builder_window uses it: icons, window frames, the
                    sky, and the sounds (play)
  ButtonController  Game/Screen/ButtonPaneController.cpp, nerve by nerve: a button appears,
                    waits, grows when the pointer is on it (ButtonSelectIn), shrinks back when it
                    leaves (ButtonSelectOut, from the frame the other one had reached), flashes
                    when pressed (ButtonDecide) and lets go (ButtonSelectOut at 1.5 times speed)
  GameButton        one button: its layout's panes (the shadow, the frame, the face, the text,
                    the shine), the controller, and the sounds the game plays for that button
  Ticker            one 60 Hz timer for every button that is moving

Which layout each kind of button takes, and which sounds, is in BUTTONS, with the game code each
one comes from.
"""

import re
import subprocess
import sys
from pathlib import Path

from PySide6.QtCore import QObject, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QFont, QFontMetrics, QImage

import builder_layout as L

UNIT = 0.74  # logical pixels per layout unit for buttons: the pause menu's button is 225 x 52


# ##############################################################################################
# # Buttons                                                                                    #
# #                                                                                            #
# # (layout, pane the animations play on, bounding pane, text panes, sound when the pointer   #
# # comes on, sound when pressed). The owner chose one look for all of them: the galaxy's     #
# # Start button.                                                                              #
# ##############################################################################################
START = ("GalaxySelect", "StartButton", "BoxStartButton", ("TxtStart", "ShaStart"))
BUTTONS = {
    # Every button is the Start button of a galaxy (GalaxyConfirmLayout); each keeps the sounds
    # of the game's button it stands for.
    "primary": START + ("button_on", "galaxy_ok"),  # GalaxyConfirmLayout
    "glass": START + ("pause_item", "pause_off"),  # PauseMenu
    "danger": START + ("pause_item", "pause_off"),
    "yes": START + ("cursor", "yes"),  # YesNoController
    "no": START + ("cursor", "no"),
    "back": START + ("button_on", "cancel"),  # BackButton
    # The pause menu's small button with Luigi's letter (PauseMenu::init: ButtonPaneController on
    # LetterButton, bounded by BoxButton3), its picture changed for the builder's icon
    # (set_icon). Its sounds: SE_SY_SELECT_PAUSE_ITEM when pointed at, the letter's
    # SE_SY_FILE_SEL_UPPER_DECIDE when pressed (PauseMenu::exeDecided).
    "icon": ("PauseMenu", "LetterButton", "BoxButton3", (), "pause_item", "letter"),
    # MiiSelect's page arrows (MiiSelect::createButtons): no sound when the pointer comes on, the
    # page sound when pressed. They step the builder's sliders.
    "arrow_left": ("MiiSelect", "Left", "PicArrowL", (), None, "mii_scroll"),
    "arrow_right": ("MiiSelect", "Right", "PicArrowR", (), None, "mii_scroll"),
}
# Shadows. Each of the game's screens keeps one shadow for all its buttons: the file select and
# the galaxy's Start button draw it in full (their Sha pane at alpha 255, the shadow pictures at
# 80), the pause menu, the back button and the map buttons through a Sha pane at 128 (half as
# dark). The builder shows the letter button beside the Start button, so its shadow pane is given
# the Start button's alpha: every button of a page casts the same shadow, as on one game screen.
SHADOW_PANES = {"icon": "ShaLButton"}
START_SHADOW_ALPHA = 255  # GalaxySelect's ShaSButton
# Buttons whose screen acts at once when they are pressed (MiiSelect::callbackLeft/Right: the
# sound and the page turn come with the press). The game then leaves the arrow alone while its
# page turns (_24 = false); the builder has no page to turn, so the controller goes on as it was
# made (ButtonPaneController with _18 set): ButtonDecide, ButtonSelectOut at 1.5, and waiting
# again, so every press shows and the arrow lets go as soon as the pointer leaves.
ACT_AT_ONCE = ("arrow_left", "arrow_right")

# Icons: (layout, pane), drawn as the layout draws that pane; or a texture alone (raw).
ICONS = {
    "a_button": ("IconAButton", "PicIconA"),
    "comet": ("IconComet", "PicIconComet"),
    "question": ("ScenarioSelect", "PicQuestion1"),
    "back": ("BackButton", "PicArrow"),
    "list": ("MapButton", "PicList"),
    "save": ("IconSave", "PicIconSave"),
    "star": ("PauseMenu", "ShaStarA"),
    "power_star": ("PauseMenu", "PicStarA"),  # the star alone, without its place
    # The camera's red "no" sign with its shadow (CameraInfo: the camera cannot turn there).
    "forbid": ("CameraInfo", "Forbid"),
}
# Windows. The game's system window (SysInfoWindow, the one that asks "Create a new save file on
# this planet?"): its group pane holds the window (WinInfo), the same frame once more as its shadow
# (ShaInfo, down and to the right), the screen's dimming (BG) and where the A button goes. text is
# the text box whose colours the window's words take (TxtConfirm).
WINDOWS = {"window": {"layout": "SysInfoWindow", "group": "WinInfoWindow", "frame": "WinInfo",
                      "shadow": "ShaInfo", "text": "TxtConfirm", "backdrop": "BG"},
           # MiiSelect's page counter (PageNumber: the small window WinPage and its text TxtPage):
           # the plate for short words, choices and fields.
           "plate": {"layout": "MiiSelect", "group": "PageNumber", "frame": "WinPage", "shadow": None,
                     "text": "TxtPage"},
           # The co-star's manual (P2Manual): the paler panel inside its window (WinTitle, white at
           # 100 over the system window's blue), where the manual writes its page. It holds the
           # install guide inside the result page's window.
           "page": {"layout": "P2Manual", "group": "Window", "frame": "WinTitle", "shadow": None,
                    "text": None}}
# Words drawn over the game's sky take the colour of MiiSelect's title (TxtMiiChange), which the
# game writes over its dimmed screen.
SKY_TEXT = ("MiiSelect", "TxtMiiChange")
# The shadow the game puts under white words over its screens: a copy of the text box drawn
# first in its own material, a little down and right (PressStart: ShaStart under TxtStart, as
# ScenarioSelect's ShaGalaxy under TxtGalaxy). The font itself (messagefont26, GX I4) has no
# outline.
SKY_SHADOW = ("PressStart", "ShaStart", "TxtStart")

# The builder's words: a new line after every full stop (owner, 25 Nov 2026). Only a stop that
# ends a sentence: after a letter or a closing bracket or quote, not part of "...", and followed
# by a space. Versions (v0.21.4), file names (settings.txt), numbered steps ("1. ") and "..." stay.
SENTENCE_END = re.compile(r"(?:(?<=\w)|(?<=[)\]\"'’]))\.(?!\.)[ \t]+(?=\S)")
# The "1" of a numbered step: at the start of a line or right after the sentence before it.
STEP_NUMBER = re.compile(r"(?:^|\n|[.!?:][ \t]+)[ \t]*\d+$")


def sentence_lines(text):
    """text with a line break after each sentence's full stop (SENTENCE_END), but not after a
    step's number ("1. Put the card in": STEP_NUMBER)."""
    if not isinstance(text, str):
        return text
    return SENTENCE_END.sub(lambda m: m.group(0) if STEP_NUMBER.search(text, 0, m.start()) else ".\n", text)


def fitted_lines(base_font, text, width, smallest=9):
    """(font, lines) for a box of fixed width: a line per sentence, never broken inside one (owner,
    25 Nov 2026); when the widest sentence is wider than the box, the font gets smaller, down to
    `smallest` pixels, and only past that is the line cut short with '...'."""
    lines = [line.strip() for line in sentence_lines(text or "").split("\n") if line.strip()]
    used = QFont(base_font)
    size = used.pixelSize() if used.pixelSize() > 0 else round(used.pointSizeF() * 96 / 72)
    while True:
        used.setPixelSize(size)
        metrics = QFontMetrics(used)
        if size <= smallest or all(metrics.horizontalAdvance(line) <= width for line in lines):
            break
        size -= 1
    return used, [metrics.elidedText(line, Qt.TextElideMode.ElideRight, int(width)) for line in lines]


def inked(image, shift):
    """image as a picture pane draws it when its material's colour 0 (what black becomes) moves
    by shift and colour 1 (what white becomes) stays: each channel lerp(C0, C1, value) moved by
    the same amount, value + shift * (1 - value / 255). White stays, the dark moves the most."""
    image = image.convertToFormat(QImage.Format.Format_ARGB32)
    data = bytearray(bytes(image.constBits()))
    # Format_ARGB32 is B, G, R, A in memory on little-endian machines.
    for offset, amount in ((2, shift[0]), (1, shift[1]), (0, shift[2])):
        table = bytes(max(0, min(255, int(round(v + amount * (1 - v / 255.0))))) for v in range(256))
        data[offset::4] = bytes(data[offset::4]).translate(table)
    out = QImage(bytes(data), image.width(), image.height(), image.bytesPerLine(), QImage.Format.Format_ARGB32)
    return out.copy()


class Skin:
    """The game UI folder: what builder_window draws with. None of it works without OpenGL."""

    def __init__(self, folder, manifest, renderer):
        self.folder = Path(folder)
        self.manifest = manifest
        self.renderer = renderer
        self.layouts = set(manifest["layouts"])
        self.images = {}
        self.save_scene = None  # (IconSave scene, the box of every turn)

    def has_layout(self, name):
        return name in self.layouts

    def has(self, name):
        if name in ICONS:
            return ICONS[name][0] in self.layouts
        if name in WINDOWS:
            return WINDOWS[name]["layout"] in self.layouts
        if name == "button":
            return all(BUTTONS[kind][0] in self.layouts for kind in BUTTONS if kind not in ACT_AT_ONCE)
        if name == "arrows":
            return all(BUTTONS[kind][0] in self.layouts for kind in ACT_AT_ONCE)
        return name in self.manifest["raw"] or name in self.manifest["skies"]

    def layout_folder(self, name):
        return self.folder / "layouts" / name

    def fonts_folder(self):
        return self.folder / "fonts"

    # Sounds.
    def play(self, key):
        file = self.manifest["sounds"].get(key)
        if file:
            play_sound(self.folder / file)

    def preload_sounds(self):
        preload_sounds(self.folder / file for file in self.manifest["sounds"].values())

    # Icons.
    def icon(self, p, rect, name, opacity=1.0, star_done=True, shift=None):
        """An icon fitted into rect (QRectF), at the painter's pixel density. shift: (r, g, b)
        the dark of the icon moves by, as a picture pane's black moves when its material's
        colour 0 changes (inked)."""
        ratio = p.device().devicePixelRatioF() if p.device() else 1.0
        size = max(1, int(round(max(rect.width(), rect.height()) * ratio)))
        shift = tuple(shift) if shift and any(shift) else None
        key = ("icon", name, size, star_done, shift)
        image = self.images.get(key)
        if image is None:
            image = self._icon_image(name, size, star_done)
            if shift is not None and image is not None and not image.isNull():
                image = inked(image, shift)
            if len(self.images) > 400:
                self.images = {k: v for k, v in self.images.items() if k[0] != "icon" or k[4] is None}
            self.images[key] = image
        if image is None or image.isNull():
            return
        scale = min(rect.width() / image.width(), rect.height() / image.height()) * 1.0
        w, h = image.width() * scale, image.height() * scale
        old = p.opacity()
        p.setOpacity(old * opacity)
        p.drawImage(QRectF(rect.center().x() - w / 2, rect.center().y() - h / 2, w, h), image)
        p.setOpacity(old)

    def _icon_image(self, name, size, star_done):
        if name in self.manifest["raw"]:
            image = QImage(str(self.folder / self.manifest["raw"][name]))
            if image.isNull():
                return None
            return image.scaled(size, size, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)
        layout, pane = ICONS[name]
        scene = L.Scene(self.layout_folder(layout))
        root = scene.pane(pane)
        if root is None:
            return None
        root.visible = True
        root.alpha = 255
        if name == "star":
            # PauseMenu::updateStarPane: a star not collected yet shows its dark place (ShaStar)
            # alone; a collected one shows the star (PicStar) on it. Both are fitted by the box of
            # the collected one, so the empty place keeps the size it has under the star.
            for child in root.children:
                child.visible = True
            box = L.extent(root)
            for child in root.children:
                child.visible = star_done
        else:
            box = L.extent(root)
        side = max(box[2] - box[0], box[3] - box[1]) or 1.0
        return self.renderer.render(scene, pane, size / side, box, fonts_folder=self.fonts_folder())

    # The save icon (SaveIcon: IconSave, its Rotate animation looping).
    def save_icon(self, p, rect, seconds):
        """The star the game spins while it saves, fitted into rect, at the frame of Rotate that
        seconds (60 frames a second) reaches. The box holds every turn, so it does not wobble.

        Drawn as the wide screen shows it: the 608-unit layout is stretched across the screen
        (WIDE_STRETCH), and a pane with the location-adjust flag (IconSave has it) is narrowed
        first in its own x (LayoutManager::calcAnim's LocationAdjustScale). The star is tilted in
        depth, so the two do not cancel: on screen it lies flatter than in the 4:3 layout."""
        ratio = p.device().devicePixelRatioF() if p.device() else 1.0
        size = max(1, int(round(max(rect.width(), rect.height()) * ratio)))
        if self.save_scene is None:
            scene = L.Scene(self.layout_folder("IconSave"))
            scene.pane("IconSave").visible = True
            if scene.pane("IconSave").flag & 4:  # Pane::IsLocationAdjust
                scene.pane("IconSave").scale[0] /= WIDE_STRETCH
            scene.start_pane_anim("PicIconSave", "Rotate")
            boxes = []
            for f in range(0, scene.frame_max("Rotate"), 5):
                scene.set_pane_anim_frame("PicIconSave", f)
                boxes.append(L.extent(scene.pane("IconSave")))
            box = (min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes), max(b[3] for b in boxes))
            self.save_scene = (scene, box)
        scene, box = self.save_scene
        frames = scene.frame_max("Rotate")
        frame = int(seconds * 60) % frames
        key = ("save icon", size, frame)
        image = self.images.get(key)
        if image is None:
            scene.set_pane_anim_frame("PicIconSave", frame)
            side = max(box[2] - box[0], box[3] - box[1]) or 1.0
            image = self.renderer.render(scene, "IconSave", size / side, box)
            self._keep(key, image)
        wide = image.width() * WIDE_STRETCH  # the screen's stretch
        scale = min(rect.width() / wide, rect.height() / image.height())
        w, h = wide * scale, image.height() * scale
        p.drawImage(QRectF(rect.center().x() - w / 2, rect.center().y() - h / 2, w, h), image)

    # The pointer (Game/Screen/StarPointerLayout.cpp).
    def hand_cursor(self, ratio=1.0):
        """(QImage, hot x, hot y) of the game's hand pointer as the menus show it: the file
        select, the pause menu and the yes/no questions use the finger pointer
        (StarPointerOnOffController -> startFingerPointer), which is DPDPointer's HandPointer
        group with HandType stopped at frame 0 (startAnimHandFinger) and the first player's star
        on the hand (changeToHandPointer). The hot spot is the layout's origin, where the game
        puts the pointer's position (setTrans). None without the layout."""
        if "DPDPointer" not in self.layouts:
            return None
        scene = L.Scene(self.layout_folder("DPDPointer"))
        for name in ("StarPointer", "PicNozzle", "HandArrow", "PicPlayer2"):  # hideAll, changeToHandPointer
            if scene.pane(name) is not None:
                for pane in scene.pane(name).walk():
                    pane.visible = False
        scene.start_pane_anim("HandPointer", "HandType")
        scene.set_pane_anim_frame("HandPointer", 0.0)
        root = scene.pane("DPDPointer")
        left, bottom, right, top = L.extent(root)
        left, bottom, right, top = min(left, 0.0), min(bottom, 0.0), max(right, 0.0), max(top, 0.0)  # the hot spot inside
        scale = CURSOR_UNIT * ratio
        image = self.renderer.render(scene, "DPDPointer", scale, (left, bottom, right, top))
        image.setDevicePixelRatio(ratio)
        return image, -left * CURSOR_UNIT, top * CURSOR_UNIT

    # Windows.
    def window(self, p, rect, name="window"):
        """A game window at rect's size, with its shadow inside rect: Window::DrawSelf draws the
        frame and the content for any size, so nothing is stretched."""
        ratio = p.device().devicePixelRatioF() if p.device() else 1.0
        w, h = int(round(rect.width())), int(round(rect.height()))
        key = ("window", name, w, h, ratio)
        image = self.images.get(key)
        if image is None:
            spec = WINDOWS[name]
            scene = L.Scene(self.layout_folder(spec["layout"]))
            group = scene.pane(spec["group"])
            frame = scene.pane(spec["frame"])
            shadow = scene.pane(spec["shadow"]) if spec["shadow"] else None
            group.visible, group.alpha = True, 255  # the appear animation brings it from 0
            for child in group.children:  # the window and its shadow, without what goes on them
                child.visible = child in (frame, shadow)
            dx = abs(shadow.translate[0] - frame.translate[0]) if shadow else 0.0
            dy = abs(shadow.translate[1] - frame.translate[1]) if shadow else 0.0
            for pane in (frame, shadow) if shadow else (frame,):
                pane.size = [max(1.0, w / WINDOW_UNIT - dx), max(1.0, h / WINDOW_UNIT - dy)]
            image = self.renderer.render(scene, spec["group"], WINDOW_UNIT * ratio, L.extent(group))
            image.setDevicePixelRatio(ratio)
            self._keep(key, image)
        p.drawImage(QRectF(rect.left(), rect.top(), rect.width(), rect.height()), image)

    def window_colours(self, name="window"):
        """(text, fill) of a game window, as QColors: the colour its text box gives white glyphs
        (TextBox: the material's white colour, times the top colour) and the window's own."""
        key = ("window colours", name)
        if key not in self.images:
            from PySide6.QtGui import QColor
            spec = WINDOWS[name]
            scene = L.Scene(self.layout_folder(spec["layout"]))
            text = scene.pane(spec["text"])
            r, g, b, _a = (c * t // 255 for c, t in zip(text.material.tev[1], text.colours[0]))
            fill = scene.pane(spec["frame"]).material.tev[1]
            self.images[key] = (QColor(r, g, b), QColor(fill[0], fill[1], fill[2]))
        return self.images[key]

    def text_colour(self, layout, pane_name):
        """The colour a layout's text box gives white glyphs (its material's white colour times
        its top colour), as a QColor."""
        key = ("text colour", layout, pane_name)
        if key not in self.images:
            from PySide6.QtGui import QColor
            pane = L.Scene(self.layout_folder(layout)).pane(pane_name)
            r, g, b, _a = (c * t // 255 for c, t in zip(pane.material.tev[1], pane.colours[0]))
            self.images[key] = QColor(r, g, b)
        return self.images[key]

    def text_shadow(self, layout, shadow_name, text_name):
        """(QColor, dx, dy) of a shadow text box: its colour and alpha (the material's white
        colour times its top colour and the pane's alpha), and how far it sits from the text
        box as shares of the font's height (x right, y down)."""
        key = ("text shadow", layout, shadow_name, text_name)
        if key not in self.images:
            from PySide6.QtGui import QColor
            scene = L.Scene(self.layout_folder(layout))
            sha, txt = scene.pane(shadow_name), scene.pane(text_name)
            r, g, b, a = (c * t // 255 for c, t in zip(sha.material.tev[1], sha.colours[0]))
            height = txt.font_size[1] or 1.0
            self.images[key] = (QColor(r, g, b, a * sha.alpha // 255), (sha.translate[0] - txt.translate[0]) / height,
                                -(sha.translate[1] - txt.translate[1]) / height)
        return self.images[key]

    def backdrop(self, p, rect, name="window"):
        """The screen's dimming under the window (its BG picture), filling rect: the game's
        screen height is the picture's height, and it is as wide as rect."""
        ratio = p.device().devicePixelRatioF() if p.device() else 1.0
        w, h = int(round(rect.width())), int(round(rect.height()))
        key = ("backdrop", name, w, h, ratio)
        image = self.images.get(key)
        if image is None:
            spec = WINDOWS[name]
            scene = L.Scene(self.layout_folder(spec["layout"]))
            pane = scene.pane(spec["backdrop"])
            pane.visible, pane.alpha = True, 255
            k = h / max(1.0, pane.size[1])
            pane.size = [w / k, pane.size[1]]
            image = self.renderer.render(scene, spec["backdrop"], k * ratio, L.extent(pane))
            image.setDevicePixelRatio(ratio)
            self._keep(key, image)
        p.drawImage(QRectF(rect.left(), rect.top(), rect.width(), rect.height()), image)

    def _keep(self, key, image):
        if len(self.images) > 200:
            self.images = {k: v for k, v in self.images.items() if k[0] in ("window colours", "text colour")}
        self.images[key] = image

# The wide screen's stretch of the 608-unit layout: the 3DS port's 760 / 608 (its location adjust is
# 0.8, Game/Screen/LayoutManager.cpp); the Wii in 16:9 is 4 / 3 with 0.75.
WIDE_STRETCH = 760.0 / 608.0

WINDOW_UNIT = 1.0  # logical pixels per layout unit for windows
CURSOR_UNIT = UNIT  # the pointer at the buttons' scale


def load(folder, manifest):
    renderer = L.Renderer.get()
    if renderer is None:
        return None
    return Skin(folder, manifest, renderer)


# ##############################################################################################
# # Sounds                                                                                     #
# #                                                                                            #
# # WAV files played without waiting, each to its end, over one another as the game mixes     #
# # them: through Qt's QSoundEffect, one per sound playing (winsound, used before, stopped    #
# # the last sound whenever a new one began). Without Qt Multimedia: winsound on Windows,     #
# # afplay on macOS, and the first of paplay, pw-play or aplay on Linux.                      #
# ##############################################################################################
_player = None
_effects = {}  # path -> the QSoundEffects made for it, reused once they are quiet


def _new_effect(path):
    """A QSoundEffect loading path, or None without Qt Multimedia or an application."""
    try:
        from PySide6.QtCore import QUrl, QCoreApplication
        from PySide6.QtMultimedia import QSoundEffect
    except ImportError:
        return None
    app = QCoreApplication.instance()
    if app is None:
        return None
    effect = QSoundEffect(app)
    effect.setSource(QUrl.fromLocalFile(path))
    effect.setVolume(1.0)
    _effects.setdefault(path, []).append(effect)
    return effect


def _qt_play(path):
    """True when Qt plays the sound."""
    try:
        from PySide6.QtMultimedia import QSoundEffect
    except ImportError:
        return False
    effects = _effects.get(path, [])
    if any(e.status() == QSoundEffect.Status.Error for e in effects):
        return False
    effect = next((e for e in effects if e.status() == QSoundEffect.Status.Ready and not e.isPlaying()), None)
    effect = effect or _new_effect(path)
    if effect is None:
        return False
    effect.play()  # one still loading plays once it is loaded
    return True


def preload_sounds(paths):
    """Loads every sound once ahead, so the first press is not late."""
    try:
        for path in map(str, paths):
            if path not in _effects and _new_effect(path) is None:
                return
    except Exception:  # noqa: BLE001 - no sound is no harm
        pass


def play_sound(path):
    global _player
    path = str(path)
    try:
        if _qt_play(path):
            return
        if sys.platform == "win32":
            import winsound
            winsound.PlaySound(path, winsound.SND_FILENAME | winsound.SND_ASYNC | winsound.SND_NODEFAULT)
            return
        if _player is None:
            import shutil
            if sys.platform == "darwin":
                _player = ["afplay"]
            else:
                found = next((tool for tool in ("paplay", "pw-play", "aplay") if shutil.which(tool)), "")
                _player = [found, "-q"] if found == "aplay" else [found] if found else []
        if _player:
            subprocess.Popen(_player + [path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:  # noqa: BLE001 - no sound is no harm
        pass


# ##############################################################################################
# # The button controller (Game/Screen/ButtonPaneController.cpp)                               #
# #                                                                                            #
# # A nerve runs once per frame; a nerve set while running starts on the next frame at step 0  #
# # (Spine). The animations are started at a nerve's first step, as the game does.            #
# ##############################################################################################
DECIDED_TO_DISAPPEAR_RATE = 1.5


class ButtonController:
    def __init__(self, scene, pane):
        self.scene, self.pane = scene, pane
        self.nerve, self.next, self.step = "Hidden", "Hidden", 0
        self.pointing_anim_start = 0.0
        self.is_pointing = False  # the screen asks this frame (trySelect)
        self.pointer_on = False  # the pointer is over the bounding pane
        self.selected = False
        self.anims = {"appear": "ButtonAppear", "wait": "ButtonWait", "decide": "ButtonDecide",
                      "in": "ButtonSelectIn", "out": "ButtonSelectOut", "end": "ButtonEnd"}

    # NerveExecutor.
    def set_nerve(self, nerve):
        self.next = nerve

    def is_nerve(self, nerve):
        return self.nerve == nerve

    def first_step(self):
        return self.step == 0

    def update(self):
        if self.next is not None:
            self.nerve, self.next, self.step = self.next, None, 0
        self.ran = (self.nerve, self.step)  # what the screen sees after this frame's update
        getattr(self, "exe" + self.nerve)()
        if self.next is None:
            self.step += 1
        self.is_pointing = False

    # What the screen calls.
    def appear(self):
        self.selected = False
        self.pointing_anim_start = 0.0
        self.set_nerve("Appear")

    def disappear(self):
        self.set_nerve("Disappear")

    def force_to_wait(self):
        self.selected = False
        self.pointing_anim_start = 0.0
        self.set_nerve("Wait")

    def try_select(self, decide_trigger):
        self.is_pointing = True
        if self.is_nerve("Pointing") and decide_trigger:
            self.decide()
            return True
        return False

    def decide(self):
        self.selected = True
        self.set_nerve("Decided")

    ran = ("Hidden", 0)

    def is_pointing_trigger(self):
        """isPointingTrigger: the first step of Pointing ran this frame."""
        return self.ran == ("Pointing", 0)

    def is_timing_for_selected_se(self):
        return self.ran == ("Decided", 1)

    # Helpers.
    def _pointing_pane(self):
        return self.is_pointing and self.pointer_on

    def _start_at_first_step(self, anim):
        if self.first_step():
            self.scene.start_pane_anim(self.pane, anim)
            return True
        return False

    def _start_pointing_at_first_step(self, anim):
        if self._start_at_first_step(anim):
            self.scene.set_pane_anim_frame(self.pane, self.pointing_anim_start)

    def _next_when_stopped(self, nerve):
        if self.scene.is_pane_anim_stopped(self.pane):
            self.set_nerve(nerve)

    def _pointing_anim_start_frame(self):
        if self.scene.is_pane_anim_stopped(self.pane):
            return 0.0
        frame_max = self.scene.frame_max(self.anims["in"])
        frame = frame_max - self.scene.pane_anim_frame(self.pane)
        return max(0.0, min(float(frame_max), frame))

    def _try_pointing(self, on):
        if on and (self.is_nerve("Wait") or self.is_nerve("NotPointing")):
            self.pointing_anim_start = self._pointing_anim_start_frame() if self.is_nerve("NotPointing") else 0.0
            self.set_nerve("Pointing")
            return True
        return False

    def _try_not_pointing(self, on):
        if not on and self.is_nerve("Pointing"):
            self.pointing_anim_start = self._pointing_anim_start_frame()
            self.set_nerve("NotPointing")
            return True
        return False

    # Nerves.
    def exeHidden(self):
        if self.first_step():
            self.scene.show_pane(self.pane, False)

    def exeAppear(self):
        if self._start_at_first_step(self.anims["appear"]):
            self.scene.show_pane(self.pane, True)
        self._next_when_stopped("Wait")

    def exeWait(self):
        if self.first_step():
            self.scene.show_pane(self.pane, True)
        self._start_at_first_step(self.anims["wait"])
        self._try_pointing(self._pointing_pane())

    def exePointing(self):
        self._start_pointing_at_first_step(self.anims["in"])
        self._try_not_pointing(self._pointing_pane())

    def exeNotPointing(self):
        self._start_pointing_at_first_step(self.anims["out"])
        if self._try_pointing(self._pointing_pane()):
            return
        self._next_when_stopped("Wait")

    def exeDecided(self):
        self._start_at_first_step(self.anims["decide"])
        self._next_when_stopped("DecidedToDisappear")

    def exeDecidedToDisappear(self):
        if self._start_at_first_step(self.anims["out"]):
            self.scene.set_pane_anim_rate(self.pane, DECIDED_TO_DISAPPEAR_RATE)
        # The game goes on to ButtonEnd and hides the button; the builder's buttons stay, so
        # it waits again (forceToWait), as a screen that shows its buttons again does.
        if self.scene.is_pane_anim_stopped(self.pane):
            self.force_to_wait()

    def exeDecidedWait(self):
        pass

    def exeDisappear(self):
        self._start_at_first_step(self.anims["end"])
        self._next_when_stopped("Hidden")


# ##############################################################################################
# # One game button                                                                            #
# ##############################################################################################
class GameButton:
    """What a builder_window.Button draws and plays when the game's graphics are there.

    The button's own pane sits at the centre of an image big enough for its largest pose; the
    bounding pane is where it takes the pointer."""

    def __init__(self, skin, kind, text):
        spec = BUTTONS.get(kind, BUTTONS["glass"])
        self.skin = skin
        self.layout, self.pane, self.box_pane, self.text_panes, self.point_se, self.decide_se = spec
        self.at_once = kind in ACT_AT_ONCE
        self.partner = None  # the other arrow: their ButtonWait sways keep together
        self.scene = L.Scene(skin.layout_folder(self.layout))
        self.controller = ButtonController(self.scene, self.pane)
        self.cache = {}
        self.texts = {}
        self.set_text(text)
        root = self.scene.pane(self.pane)
        root.visible = True
        root.origin = (root.translate[0], root.translate[1])  # the layout's place: moves are kept
        shadow = self.scene.pane(SHADOW_PANES.get(kind, ""))
        if shadow is not None:  # the Start button's shadow (SHADOW_PANES)
            shadow.alpha = START_SHADOW_ALPHA
        # Rest pose (ButtonWait's values) for the size; the largest pose is 1.15 times it (the
        # arrows: 1.1 / 0.8, and their sway).
        self.scene.start_pane_anim(self.pane, "ButtonWait")
        left, bottom, right, top = L.extent(root)
        cx, cy = (left + right) / 2, (bottom + top) / 2
        grow, sway = (1.45, 8) if self.at_once else (1.2, 0)
        half_w, half_h = (right - left) / 2 * grow + 4 + sway, (top - bottom) / 2 * grow + 8
        self.box = (cx - half_w, cy - half_h, cx + half_w, cy + half_h)
        self.hit = L.pane_box(self.scene, self.pane, self.box_pane) or (left, bottom, right, top)
        self.scene.players.clear()
        self.entered = False
        self.clicked = None  # set by the widget: called when the press is over
        self.controller.appear()
        picture = self.scene.pane(self.ICON_PANE)
        # The picture's colour 0 as the layout has it: ButtonSelectIn and ButtonDecide move it
        # (the letter darkens), and ink_shift says by how much.
        self.ink_rest = list(picture.material.tev[0][:3]) if picture is not None and picture.material else None

    def size(self):
        """The widget size in logical pixels."""
        return ((self.box[2] - self.box[0]) * UNIT, (self.box[3] - self.box[1]) * UNIT)

    def hit_rect(self):
        """The bounding pane in widget coordinates (QRectF)."""
        left, bottom, right, top = self.hit
        return QRectF((left - self.box[0]) * UNIT, (self.box[3] - top) * UNIT, (right - left) * UNIT, (top - bottom) * UNIT)

    def set_text(self, text):
        """The text, in both text panes (the text and its shadow); a text wider than the box is
        narrowed to fit (the game's own messages are written to fit)."""
        font = self.skin.renderer.font(self.skin.fonts_folder(), self.scene.layout.fonts[0]) if self.scene.layout.fonts else None
        for name in self.text_panes:
            pane = self.scene.pane(name)
            if pane is None:
                continue
            self.texts[name] = text
            if font is not None:
                if not hasattr(pane, "base_font_size"):
                    pane.base_font_size, pane.base_char_space = tuple(pane.font_size), pane.char_space
                pane.font_size, pane.char_space = list(pane.base_font_size), pane.base_char_space
                quads = L.layout_text(pane, font, text)
                if quads:
                    width = max(q[1] + q[3] for q in quads) - min(q[1] for q in quads)
                    if width > pane.size[0] * 0.96:
                        k = pane.size[0] * 0.96 / width
                        pane.font_size = [pane.base_font_size[0] * k, pane.base_font_size[1]]
                        pane.char_space = pane.base_char_space * k
        self.cache.clear()

    ICON_PANE = "PicLetter"  # the picture of the "icon" button

    def set_icon(self, path):
        """Puts the picture at path (a PNG) where the button's own picture is."""
        pane = self.scene.pane(self.ICON_PANE)
        if pane is None or pane.material is None or not pane.material.textures:
            return
        pane.material.textures[0][3] = str(path)
        # The picture at its own proportions: the pane keeps its height (48, as the letter's
        # 64 x 64), its width follows the picture's.
        from PySide6.QtGui import QImage
        image = QImage(str(path))
        if not image.isNull() and image.height():
            if not hasattr(pane, "own_size"):
                pane.own_size = list(pane.size)
            pane.size = [pane.own_size[1] * image.width() / image.height(), pane.own_size[1]]
            # Centred as the letter is in its 64 x 64: the pane moves by the distance between the
            # picture's middle and the middle of what it draws (a font cell keeps its glyph where
            # the line puts it, the note low and on the left). The picture stays as it is.
            if not hasattr(pane, "own_translate"):
                pane.own_translate = list(pane.translate)
            seen = [(x, y) for y in range(image.height()) for x in range(image.width())
                    if image.pixelColor(x, y).alpha() > 0]
            dx = dy = 0.0
            if seen:
                xs, ys = [x for x, _ in seen], [y for _, y in seen]
                mid_x, mid_y = (min(xs) + max(xs) + 1) / 2.0, (min(ys) + max(ys) + 1) / 2.0
                dx = (image.width() / 2.0 - mid_x) / image.width() * pane.size[0] * pane.scale[0]
                dy = (mid_y - image.height() / 2.0) / image.height() * pane.size[1] * pane.scale[1]
            pane.translate = [pane.own_translate[0] + dx, pane.own_translate[1] + dy] + list(pane.own_translate[2:])
        self.cache.clear()

    def tick(self, pointer_on, pressed, enabled):
        """One frame: the controller (with the screen's trySelect), the sounds, the animation.
        True when the picture changed."""
        before = self.scene.state_key(self.pane)
        c = self.controller
        c.pointer_on = pointer_on and enabled
        if enabled and c.try_select(pressed) and self.at_once:
            self._act()
        elif (enabled and pressed and self.at_once and c.pointer_on
              and c.nerve in ("Decided", "DecidedToDisappear")):
            c.decide()  # a quick second press: the flash again, and one more step
            self._act()
        c.update()
        partner = self.partner
        if (partner is not None and c.ran == ("Wait", 0) and partner.controller.nerve == "Wait"
                and partner.controller.ran != ("Wait", 0)):
            # MiiSelect::control: an arrow that starts waiting takes the other one's sway frame.
            self.scene.set_pane_anim_frame(self.pane, partner.scene.pane_anim_frame(partner.pane))
        if c.is_pointing_trigger() and self.entered:
            self.skin.play(self.point_se)
            self.entered = False
        if c.is_timing_for_selected_se() and not self.at_once:
            self.skin.play(self.decide_se)
        if c.ran == ("DecidedToDisappear", 0) and self.clicked and not self.at_once:
            QTimer.singleShot(0, self.clicked)
        self.scene.movement()
        return self.scene.state_key(self.pane) != before

    def decide_now(self):
        """A press the pointer was not seen on first (a touch screen, or the keyboard)."""
        c = self.controller
        if c.nerve not in ("Wait", "Pointing", "NotPointing"):
            return
        c.decide()
        if self.at_once:
            self._act()

    def _act(self):
        """MiiSelect::callbackLeft/Right: the sound and the step with the press."""
        self.skin.play(self.decide_se)
        if self.clicked:
            QTimer.singleShot(0, self.clicked)

    def busy(self):
        """False when nothing moves until the pointer does: waiting, all animations stopped. The
        arrows sway while they wait (a looping ButtonWait that moves)."""
        c = self.controller
        if c.next is not None or c.nerve not in ("Wait", "Pointing", "Hidden", "DecidedWait"):
            return True
        if self.at_once and c.nerve == "Wait":
            return True
        return any(not p.stopped and not p.anim.loop for p in self.scene.players.values())

    def place(self, name, x, y, ahead=0.0):
        """Where the point (x, y) of pane name's own space is drawn now, in widget pixels, with
        how much that pane is scaled and its alpha (0 to 1): what follows the button's
        animations has to be drawn there (IconButton's badge)."""
        if ahead:
            self.scene.apply(ahead)
        try:
            for pane, mtx, alpha in L.walk_draw(self.scene.pane(self.pane)):
                if pane.name == name:
                    px, py = L._apply(mtx, x, y)
                    scale = (mtx[0] ** 2 + mtx[3] ** 2) ** 0.5
                    return QRectF((px - self.box[0]) * UNIT, (self.box[3] - py) * UNIT, 0, 0).topLeft(), scale, alpha
        finally:
            if ahead:
                self.scene.apply()
        return None

    def ink_shift(self, ahead=0.0):
        """How far the picture's colour 0 is now from its rest, (r, g, b): what the button's
        animations do to the picture's dark, for a badge to follow."""
        if self.ink_rest is None:
            return None
        if ahead:
            self.scene.apply(ahead)
        try:
            now_ = self.scene.pane(self.ICON_PANE).material.tev[0][:3]
            return tuple(int(n) - int(r) for n, r in zip(now_, self.ink_rest))
        finally:
            if ahead:
                self.scene.apply()

    def image(self, ratio, ahead=0.0):
        """The button drawn at the device ratio, ahead (0 to 1) of a frame further on."""
        if ahead:
            self.scene.apply(ahead)
        try:
            return self._image(ratio)
        finally:
            if ahead:
                self.scene.apply()

    def _image(self, ratio):
        key = (self.scene.state_key(self.pane), ratio)
        image = self.cache.get(key)
        if image is None:
            image = self.skin.renderer.render(self.scene, self.pane, UNIT * ratio, self.box, self.texts,
                                              self.skin.fonts_folder())
            image.setDevicePixelRatio(ratio)
            if len(self.cache) > 160:
                self.cache.clear()
            self.cache[key] = image
        return image


# ##############################################################################################
# # The window's frame rate (Settings > Graphics)                                              #
# #                                                                                            #
# # The game thinks at 60 frames a second: its nerves, its button controllers and its          #
# # animations count whole frames. The window draws as often as the chosen frame rate asks    #
# # (FRAME_RATE: a number, 0 for no limit, None for the screen's own rate): every timer that    #
# # draws runs on one beat (Beat), the game's frames are counted against the clock            #
# # (GameClock: whole frames to think, and the share of the next one), and a layout is drawn   #
# # that share of a frame further on (Scene.apply(ahead)), so its animations move smoothly    #
# # past 60. Qt's own animations are capped at 60 by Qt, so the window's fades are            #
# # ClockAnimations, run by the same clock.                                                    #
# ##############################################################################################
FRAME_RATE = None


def frame_rate():
    """The frames a second the window draws: the chosen number, or the screen's own rate."""
    if FRAME_RATE is None:
        from PySide6.QtGui import QGuiApplication
        screen = QGuiApplication.primaryScreen()
        rate = screen.refreshRate() if screen is not None else 60.0
        return rate if rate and rate > 1 else 60.0
    return float(FRAME_RATE)


def frame_interval():
    """The time between frames at the frame rate, in milliseconds (0: as often as Qt can)."""
    if FRAME_RATE == 0:
        return 0
    return max(1, int(round(1000.0 / frame_rate())))


# ##############################################################################################
# # One beat for every drawing timer (FrameTimer)                                              #
# #                                                                                            #
# # Each moving part of the window (the space, the title, the skies, the fades, the game       #
# # buttons) once had its own QTimer at the frame rate. Those timers did not keep time with    #
# # each other, so the window was drawn whenever any of them fired: at 30 it drew about 55     #
# # times a second, and the counter showed that. Now one timer (Beat) fires at the frame rate  #
# # and wakes all the FrameTimers that run together, deepest part first, so their redraws   #
# # make one frame (a window asked first and a part after it is drawn twice by Qt). Its       #
# # times are kept against the clock (a 60 frame rate is 16.7 ms, not 17), so the rate holds. #
# ##############################################################################################
class Beat(QObject):
    _instance = None

    @classmethod
    def get(cls):
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    def __init__(self):
        super().__init__()
        self.running = []  # [FrameTimer] started
        self.timer = QTimer(self)
        self.timer.setSingleShot(True)
        self.timer.setTimerType(Qt.TimerType.PreciseTimer)
        self.timer.timeout.connect(self.fire)
        self.next = 0.0  # the clock time the next beat is due (time.perf_counter)

    def join(self, frame_timer):
        import time
        if frame_timer not in self.running:
            self.running.append(frame_timer)
        if not self.timer.isActive():
            self.next = time.perf_counter()
            self.schedule()

    def leave(self, frame_timer):
        if frame_timer in self.running:
            self.running.remove(frame_timer)
        if not self.running:
            self.timer.stop()

    def schedule(self):
        """The next beat: one frame on from the last one due. A beat late by more than half a
        frame (a slow drawing) is not caught up with a burst; the count starts again from now."""
        import time
        now_ = time.perf_counter()
        # No limit: as fast as the drawing goes, but never under 2 ms, so a beat with nothing
        # to draw (a still sky, a hidden page) does not spin thousands of times a second.
        period = 0.002 if FRAME_RATE == 0 else 1.0 / frame_rate()
        self.next += period
        if self.next < now_ - period / 2:
            self.next = now_
        self.timer.start(max(0, int(round((self.next - now_) * 1000.0))))

    @staticmethod
    def depth(frame_timer):
        """How deep in the window the timer's part lies (no part: deepest)."""
        part, depth = frame_timer.parent(), 0
        if part is None:
            return 1000
        while part is not None:
            part, depth = part.parent(), depth + 1
        return depth

    def fire(self):
        # The deepest parts first and the window last: a window asked to redraw before one of
        # its parts is drawn twice by Qt (6.12, measured: 60 full paints a second at 30).
        try:
            order = sorted(self.running, key=self.depth, reverse=True)
        except RuntimeError:  # one was deleted with its widget: the emits below drop it
            order = list(self.running)
        for frame_timer in order:
            try:
                frame_timer.timeout.emit()
            except RuntimeError:  # deleted with its widget
                self.leave(frame_timer)
        if self.running:
            self.schedule()


class FrameTimer(QObject):
    """A drawing timer at the window's frame rate: the QTimer calls the window's parts use
    (timeout, start, stop, isActive), on the one Beat."""

    timeout = Signal()

    def start(self, *_):
        Beat.get().join(self)

    def stop(self):
        Beat.get().leave(self)

    def isActive(self):
        return self in Beat.get().running

    def interval(self):
        return frame_interval()


def frame_timer(parent):
    """A drawing timer for parent's moving part (FrameTimer)."""
    return FrameTimer(parent)


def set_frame_rate(rate):
    """Settings > Video: the beat takes the new rate at its next frame."""
    global FRAME_RATE
    FRAME_RATE = rate
    beat = Beat.get()
    if beat.timer.isActive():
        import time
        beat.next = time.perf_counter()
        beat.schedule()


class GameClock:
    """The game's 60 frames a second against the clock: step() gives the whole frames gone by
    since the last call (at most 30, after a stall) and keeps the share of the next one."""

    def __init__(self):
        self.last = None
        self.share = 0.0

    def step(self):
        import time
        t = time.perf_counter()
        if self.last is None:
            self.last, self.share = t, 0.0
            return 1
        gone = (t - self.last) * 60.0 + self.share
        self.last = t
        frames = int(gone)
        self.share = gone - frames
        if frames > 30:
            frames, self.share = 30, 0.0
        return frames

    def reset(self):
        self.last, self.share = None, 0.0


class ClockAnimation(QObject):
    """QVariantAnimation's part the window uses (a number from a start to an end value over a
    duration, with an easing curve), run by the frame rate's timer instead of Qt's 60 Hz one."""

    valueChanged = Signal(object)
    finished = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        from PySide6.QtCore import QEasingCurve
        self.duration, self.start_value, self.end_value = 250, 0.0, 1.0
        self.curve = QEasingCurve(QEasingCurve.Type.Linear)
        self.began = None
        self.timer = frame_timer(self)
        self.timer.timeout.connect(self._step)

    def setDuration(self, ms):
        self.duration = max(1, int(ms))

    def setEasingCurve(self, curve):
        from PySide6.QtCore import QEasingCurve
        self.curve = QEasingCurve(curve)

    def setStartValue(self, value):
        self.start_value = float(value)

    def setEndValue(self, value):
        self.end_value = float(value)

    def state(self):
        from PySide6.QtCore import QAbstractAnimation
        return QAbstractAnimation.State.Running if self.timer.isActive() else QAbstractAnimation.State.Stopped

    def start(self):
        import time
        self.began = time.perf_counter()
        self.timer.start()
        self._step()

    def stop(self):
        self.timer.stop()

    def _step(self):
        import time
        k = min(1.0, (time.perf_counter() - self.began) * 1000.0 / self.duration)
        self.valueChanged.emit(self.start_value + (self.end_value - self.start_value) * self.curve.valueForProgress(k))
        if k >= 1.0:
            self.timer.stop()
            self.finished.emit()


class Ticker(QObject):
    """The game buttons' frames: they think 60 times a second (GameClock) and are drawn at the
    window's frame rate, each with the share of the next frame; it sleeps when none moves."""

    _instance = None

    @classmethod
    def get(cls):
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    def __init__(self):
        super().__init__()
        self.widgets = set()
        self.clock = GameClock()
        self.timer = frame_timer(self)
        self.timer.timeout.connect(self.tick)

    def wake(self, widget):
        self.widgets.add(widget)
        if not self.timer.isActive():
            self.clock.reset()
            self.timer.start()

    def tick(self):
        frames = self.clock.step()
        for widget in list(self.widgets):
            try:
                if not widget.game_tick(frames, self.clock.share):
                    self.widgets.discard(widget)
            except RuntimeError:  # the widget is gone
                self.widgets.discard(widget)
        if not self.widgets:
            self.timer.stop()
