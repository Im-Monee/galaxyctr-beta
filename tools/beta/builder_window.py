"""The GalaxyCTR builder's window (Qt, PySide6): the same build as in the terminal, with pages.

  Start     the logo, what the builder needs, Start the build and Settings
  Settings  every answer settings.txt keeps, editable (galaxyctr_builder.Settings)
  Build     the six steps, the current one's progress and the activity log
  Done      how it ended: copied, the copy steps for later, or what went wrong

The build is galaxyctr_builder.run_build(), run in a thread of its own. It talks to the window
through GuiUI below: its lines, steps and progress arrive as Qt signals, and each question opens a
dialog in the window while the build thread waits for the answer. Questions are the same ones the
terminal asks, with the matching picker (a file, a folder, the SD card's drive, the FTP address and
the number of connections).

Everything is drawn here, with no image files besides the logo (branding/title.png): an animated
star field with drifting nebulae, and frosted glass panels that show a blurred copy of it.

The game's own look. Once the user's disc has been read (builder_assets.make, run by the first
start's setup or by a build), the window wears Super Mario Galaxy's buttons, windows and icons
(Skin, SKIN) and shows GalaxySky behind the start page (builder_sky.SkyView, OpenGL). Until then,
or when a file of them is missing, everything is drawn by this file as below.

Design rules (from a review against the Hallmark design skill, kept where they fit a desktop
window): one accent family, the logo's blues, and no purple-to-blue blends; labels in sentence
case, untracked, never spaced capitals; no decorative dots, no " · " strings; no panel inside a
panel (text areas sit flat in their glass); a focus ring that shows at once on every control; and
when the system asks for less motion (REDUCED), the background, glows, pulses and page fades stand
still, leaving only the indicators that show work in progress.
"""

import math
import os
import random
import re
import sys
import threading
import time
from pathlib import Path

from PySide6.QtCore import (QAbstractAnimation, QEasingCurve, QEvent, QObject, QPoint, QPointF, QRect, QRectF, QSize, Qt,
                            QTimer, QUrl, Signal)
from PySide6.QtGui import (QColor, QCursor, QDesktopServices, QFont, QFontDatabase, QFontMetrics, QIcon,
                           QImage, QLinearGradient, QPainter, QPainterPath, QPen, QPixmap,
                           QRadialGradient)
from PySide6.QtWidgets import (QApplication, QFileDialog, QGraphicsOpacityEffect,
                               QGridLayout, QHBoxLayout, QLabel, QLineEdit, QPlainTextEdit, QTextEdit,
                               QPushButton, QScrollArea, QSizePolicy, QSlider, QStackedLayout,
                               QVBoxLayout, QWidget)

import builder_firststart
import builder_game

B = None  # the galaxyctr_builder module, set by run()


# ##############################################################################################
# # Look                                                                                       #
# #                                                                                            #
# # Deep space blues with the logo's cyan, violet and a touch of pink. Headings use the first  #
# # of a few technical-looking fonts the system has (Bahnschrift on Windows), text the         #
# # system's own.                                                                              #
# ##############################################################################################
INK = QColor(236, 243, 255)
MUTED = QColor(150, 166, 206)
CYAN = QColor(63, 216, 255)
BLUE = QColor(47, 123, 255)
DEEP = QColor(28, 58, 170)     # the logo's dark outline blue, for the nebulae and edges
ICE = QColor(160, 225, 255)    # the logo's pale highlight
GOOD = QColor(80, 255, 180)
BAD = QColor(255, 93, 115)
GOLD = QColor(255, 214, 90)

HEADING_FONTS = ("Orbitron", "Exo 2", "Rajdhani", "Bahnschrift", "SF Pro Display", "Segoe UI Variable Display",
                 "Helvetica Neue", "Ubuntu", "Cantarell", "Noto Sans", "DejaVu Sans")
TEXT_FONTS = ("Segoe UI Variable Text", "Segoe UI", "SF Pro Text", ".AppleSystemUIFont", "Helvetica Neue",
              "Ubuntu", "Cantarell", "Noto Sans", "DejaVu Sans")
MONO_FONTS = ("Cascadia Mono", "Consolas", "SF Mono", "Menlo", "JetBrains Mono", "Ubuntu Mono",
              "DejaVu Sans Mono", "Liberation Mono")
WEIGHTS = {400: QFont.Weight.Normal, 500: QFont.Weight.Medium, 600: QFont.Weight.DemiBold,
           700: QFont.Weight.Bold, 800: QFont.Weight.ExtraBold}
FAMILIES = {}


def pick_fonts():
    families = set(QFontDatabase.families())
    fallback = QApplication.font().family()
    FAMILIES["heading"] = next((name for name in HEADING_FONTS if name in families), fallback)
    FAMILIES["text"] = next((name for name in TEXT_FONTS if name in families), fallback)
    FAMILIES["mono"] = next((name for name in MONO_FONTS if name in families),
                            QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont).family())


def use_game_font():
    """The game's message font (made from the user's disc, builder_font.py) for every word of the
    window, the log too. The game's font has one weight, already round and heavy, so weights are
    not asked of it."""
    if SKIN is None or "ui_font" not in SKIN.manifest:
        return False
    path = SKIN.folder / SKIN.manifest["ui_font"]
    try:
        import builder_font
        if builder_font.repair(path):  # a font made before the side bearings were kept
            B.log("The game's font got its side bearings back.")
    except Exception as error:  # noqa: BLE001 - the font as it is
        B.log(f"Font not repaired: {error!r}")
    index = QFontDatabase.addApplicationFont(str(path))
    families = QFontDatabase.applicationFontFamilies(index) if index >= 0 else []
    if not families:
        return False
    FAMILIES["heading"] = FAMILIES["text"] = FAMILIES["mono"] = families[0]
    FAMILIES["game"] = families[0]
    QApplication.setFont(font(14))
    return True


GAME_CHAR_SPACE = 1.0 / 29.7  # charSpace over the font height, SysInfoWindow TxtConfirm


# The smallest text's pixel size. The game's font has round and pointed letters (c, e, o, s, v,
# w) a little past the line, as type often does; at 11 and 12 pixels that became a whole pixel and
# the letters stood at different heights (owner, 26 Nov 2026). From 13 the system's hinting lines
# them up (only x still differs), from 15 all of them.
SMALL_TEXT = 13


def font(size, weight=400, kind="text", spacing=0.0):
    result = QFont(FAMILIES.get(kind) or QApplication.font().family())
    result.setPixelSize(size)
    game = FAMILIES.get("game") and FAMILIES.get(kind) == FAMILIES.get("game")
    result.setWeight(QFont.Weight.Normal if game else WEIGHTS.get(weight, QFont.Weight.Normal))
    if game and not spacing:
        # The game's space between characters (TextBox charSpace): SysInfoWindow's TxtConfirm,
        # the window the builder's words sit on, has 1 at a font height of 29.7. Here a line of
        # the font (its 33-pixel height) is 1.25 times the pixel size (builder_font.make).
        spacing = GAME_CHAR_SPACE * 1.25 * size
    if spacing:
        result.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, spacing)
    return result


def alpha(color, value):
    result = QColor(color)
    result.setAlpha(int(max(0, min(255, value))))
    return result


def mix(a, b, k):
    return QColor(*(int(getattr(a, c)() + (getattr(b, c)() - getattr(a, c)()) * k) for c in ("red", "green", "blue")))


def now():
    return time.monotonic()


REDUCED = False  # the system asks for less motion: set by run() from reduce_motion()
MOTION = "system"  # Settings > Video > Motion (graphics_motion)


def reduce_motion():
    """Whether the system's accessibility settings ask apps to animate less."""
    try:
        if os.name == "nt":
            import ctypes
            enabled = ctypes.c_int(1)
            # SPI_GETCLIENTAREAANIMATION: Settings > Accessibility > Visual effects > Animation effects
            if ctypes.windll.user32.SystemParametersInfoW(0x1042, 0, ctypes.byref(enabled), 0):
                return not enabled.value
        elif sys.platform == "darwin":
            import subprocess
            answer = subprocess.run(["defaults", "read", "com.apple.universalaccess", "reduceMotion"],
                                    capture_output=True, text=True, timeout=5)
            return answer.stdout.strip() == "1"
        else:
            import subprocess
            answer = subprocess.run(["gsettings", "get", "org.gnome.desktop.interface", "enable-animations"],
                                    capture_output=True, text=True, timeout=5, env=system_environment())
            return answer.stdout.strip() == "false"
    except Exception:  # noqa: BLE001 - no answer means the usual motion
        pass
    return False


def windows_animations_off():
    """True when Windows' own "Animation effects" setting is what stills the window (not the
    GALAXYCTR_REDUCED_MOTION variable): the main menu then offers to turn it back on."""
    return os.name == "nt" and os.environ.get("GALAXYCTR_REDUCED_MOTION") != "1" and reduce_motion()


def turn_windows_animations_on():
    """SPI_SETCLIENTAREAANIMATION to on, saved and told to the other programs, as the switch in
    Settings > Accessibility > Visual effects does. True when Windows now animates."""
    try:
        import ctypes
        # The new value goes in pvParam itself; SPIF_UPDATEINIFILE | SPIF_SENDCHANGE.
        ctypes.windll.user32.SystemParametersInfoW(0x1043, 0, ctypes.c_void_p(1), 0x01 | 0x02)
    except Exception as error:  # noqa: BLE001 - the setting stays as it was
        B.log(f"Animation effects not turned on: {error!r}")
        return False
    return not reduce_motion()


# ##############################################################################################
# # Graphics settings (Settings > Video, kept in settings.txt)                                 #
# #                                                                                            #
# #   graphics_fps      the frame rate: "screen" (the screen's own), a number, or "0" (no limit)  #
# #   graphics_vsync    "on" or "off": drawing waits for the screen (QSurfaceFormat swap interval) #
# #   graphics_gpu      on Windows "auto", "saving" or "fast": the per-program choice Windows     #
# #                     keeps in UserGpuPreferences; on Linux "auto" or "second" (PRIME offload)  #
# #   graphics_motion   "system" (follow the system's animation setting), "always" or "still"    #
# #   graphics_counter  "on" shows the frames drawn each second                                 #
# # VSync and the card are read when the builder starts; the rest change at once.               #
# ##############################################################################################
FRAME_RATES = (("Match the screen", "screen"), ("30", "30"), ("60", "60"), ("120", "120"), ("144", "144"),
               ("165", "165"), ("240", "240"), ("No limit", "0"))
GPU_CHOICES_WINDOWS = (("Windows decides", "auto"), ("Power saving", "saving"), ("High performance", "fast"))
GPU_CHOICES_LINUX = (("System default", "auto"), ("Second card (PRIME)", "second"))
GPU_PREFERENCE = {"auto": None, "saving": 1, "fast": 2}  # UserGpuPreferences GpuPreference=
MOTION_CHOICES = (("Follow Windows" if os.name == "nt" else "Follow the system", "system"), ("Always", "always"),
                  ("Still", "still"))


def graphics(settings, name):
    defaults = {"graphics_fps": "60", "graphics_vsync": "on", "graphics_gpu": "auto",
                "graphics_motion": "system", "graphics_counter": "off"}
    return settings.get(name) or defaults[name]


def frame_rate_of(value):
    """builder_game.FRAME_RATE for a graphics_fps value: None for the screen's, 0 for no limit."""
    if value == "screen":
        return None
    try:
        return max(0, int(value))
    except ValueError:
        return None


def motion_still(value):
    """REDUCED for a graphics_motion value."""
    if value == "always":
        return False
    if value == "still":
        return True
    return reduce_motion() or os.environ.get("GALAXYCTR_REDUCED_MOTION") == "1"


def graphics_cards():
    """The names of the graphics cards, as the system calls them (empty when not known)."""
    names = []
    try:
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes

            class DisplayDevice(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD), ("DeviceName", wintypes.WCHAR * 32),
                            ("DeviceString", wintypes.WCHAR * 128), ("StateFlags", wintypes.DWORD),
                            ("DeviceID", wintypes.WCHAR * 128), ("DeviceKey", wintypes.WCHAR * 128)]
            index = 0
            while True:
                device = DisplayDevice()
                device.cb = ctypes.sizeof(device)
                if not ctypes.windll.user32.EnumDisplayDevicesW(None, index, ctypes.byref(device), 0):
                    break
                if device.DeviceString and device.DeviceString not in names:
                    names.append(device.DeviceString)
                index += 1
        elif sys.platform.startswith("linux"):
            import subprocess
            out = subprocess.run(["lspci"], capture_output=True, text=True, timeout=5, env=system_environment()).stdout
            names = [line.split(": ", 1)[1] for line in out.splitlines()
                     if ("VGA" in line or "3D controller" in line or "Display controller" in line) and ": " in line]
    except Exception:  # noqa: BLE001 - the list is only shown
        pass
    return names


def short_card_name(name):
    """A graphics card's name without what makes it long and says nothing to the user: the maker's
    company words and lspci's revision ("NVIDIA Corporation AD107M [GeForce RTX 4050] (rev a1)"
    becomes "NVIDIA AD107M [GeForce RTX 4050]")."""
    import re
    name = re.sub(r"\s*\(rev [0-9a-f]+\)$", "", name.strip())
    name = re.sub(r"\b(Corporation|Corp\.|Inc\.|Co\., Ltd\.|\[AMD/ATI\]|Advanced Micro Devices, Inc\.)\s*", "", name)
    return re.sub(r"\s{2,}", " ", name).strip()


def graphics_card_in_use():
    """The OpenGL renderer the window draws with (GL_RENDERER), or an empty text."""
    try:
        base = builder_game.L.Renderer.get()
        if base is None:
            return ""
        base.context.makeCurrent(base.surface)
        return base.gl.glGetString(0x1F01) or ""
    except Exception:  # noqa: BLE001 - only shown
        return ""


def apply_graphics_card(value):
    """Windows: the card Windows gives this program, kept by Windows per program
    (Settings > System > Display > Graphics). Only for the builder program itself: from the
    source, the choice would apply to every Python program, so it is left alone."""
    if os.name != "nt" or not getattr(sys, "frozen", False):
        return
    try:
        import winreg
        key = winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\DirectX\UserGpuPreferences")
        preference = GPU_PREFERENCE.get(value)
        if preference is None:
            try:
                winreg.DeleteValue(key, sys.executable)
            except FileNotFoundError:
                pass
        else:
            winreg.SetValueEx(key, sys.executable, 0, winreg.REG_SZ, f"GpuPreference={preference};")
        winreg.CloseKey(key)
    except Exception as error:  # noqa: BLE001 - the card stays Windows' choice
        B.log(f"Graphics card choice not kept: {error!r}")


def apply_graphics_at_start():
    """What must be set before the first OpenGL drawing: VSync, the card, the frame rate."""
    from PySide6.QtGui import QSurfaceFormat
    settings = B.Settings()
    fmt = QSurfaceFormat.defaultFormat()
    fmt.setSwapInterval(1 if graphics(settings, "graphics_vsync") == "on" else 0)
    # Desktop OpenGL, whose GLSL 1.20 every shader of the window is written in. Left to Qt, an
    # NVIDIA card through PRIME offload on Wayland gave OpenGL ES, where they do not compile
    # (Linux, Settings > Video > Graphics card: second, 25 Nov 2026).
    fmt.setRenderableType(QSurfaceFormat.RenderableType.OpenGL)
    QSurfaceFormat.setDefaultFormat(fmt)
    card = graphics(settings, "graphics_gpu")
    if os.name == "nt":
        apply_graphics_card(card)
    elif card == "second":
        if any("NVIDIA" in name for name in graphics_cards()):
            # NVIDIA's own offload (its library would break OpenGL on a computer without one).
            os.environ.setdefault("__NV_PRIME_RENDER_OFFLOAD", "1")
            os.environ.setdefault("__GLX_VENDOR_LIBRARY_NAME", "nvidia")
        else:
            os.environ.setdefault("DRI_PRIME", "1")  # Mesa's offload to the second card
    builder_game.set_frame_rate(frame_rate_of(graphics(settings, "graphics_fps")))
    return settings


def sway():
    """The clock for decorative motion: still when the system asks for less motion."""
    return 0.0 if REDUCED else now()


def ease_out(k):
    k = max(0.0, min(1.0, k))
    return 1 - (1 - k) ** 3


STYLE = """
QLabel { color: #ecf3ff; background: transparent; }
QLineEdit { color: #ecf3ff; background: rgba(255,255,255,16); border: 1px solid rgba(120,190,255,80);
            border-radius: 12px; padding: 10px 14px; selection-background-color: #2f7bff; }
QLineEdit:focus { border: 2px solid #a0e1ff; background: rgba(63,216,255,24); }
QPlainTextEdit, QTextEdit { color: #b4c2e6; background: transparent; border: none;
                 border-top: 1px solid rgba(120,190,255,45); padding: 10px 0 0 0;
                 selection-background-color: #2f7bff; }
QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 8px 2px; }
QScrollBar::handle:vertical { background: rgba(140,200,255,70); border-radius: 3px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: rgba(63,216,255,160); }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
QScrollBar:horizontal { height: 0; }
QSlider::groove:horizontal { height: 6px; border-radius: 3px; background: rgba(255,255,255,26); }
QSlider::sub-page:horizontal { border-radius: 3px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2f7bff, stop:1 #3fd8ff); }
QSlider::handle:horizontal { width: 18px; height: 18px; margin: -7px 0; border-radius: 9px;
    background: #ecf3ff; border: 3px solid #3fd8ff; }
QSlider:focus { border: 2px solid #a0e1ff; border-radius: 10px; }
QPlainTextEdit:focus, QTextEdit:focus { border: none; }
QComboBox { color: #ecf3ff; background: rgba(255,255,255,16); border: 1px solid rgba(120,190,255,80);
            border-radius: 12px; padding: 9px 14px; }
QComboBox::drop-down { border: none; width: 26px; }
QComboBox QAbstractItemView { background: #0d1230; color: #ecf3ff; border: 1px solid rgba(63,216,255,120);
                              selection-background-color: #2f7bff; outline: 0; }
QToolTip { background: #0d1230; color: #ecf3ff; border: 1px solid #3fd8ff; padding: 6px; }
"""


class Text(QLabel):
    """A label in one of the window's colours (INK, MUTED, CYAN...), shown as tone() gives it for
    where the label sits: on the game's light windows it takes the window's text colour."""

    def __init__(self, text, color):
        super().__init__(builder_game.sentence_lines(text))
        self.color = color
        self.recolor()

    def setText(self, text):
        super().setText(builder_game.sentence_lines(text))  # a new line after each sentence

    def setWordWrap(self, on):
        """Never wrapped by width (owner, 25 Nov 2026): lines break only after a sentence
        (sentence_lines), and the layout gives the label the width of its longest one."""
        super().setWordWrap(False)

    def ink_room(self):
        """The room the words' ink takes past their box: the game's letters reach a little past
        their advance, and the sky shadow lies a tenth of the font's height down and right. Without
        it the last character was cut, as the % of the build's percentage (owner, 26 Nov 2026)."""
        size = self.font().pixelSize()
        if size <= 0:
            size = round(self.font().pointSizeF() * 4 / 3) or 14
        return math.ceil(size * 0.15) + 1

    def sizeHint(self):
        room = self.ink_room()
        return super().sizeHint() + QSize(room, room)

    def minimumSizeHint(self):
        room = self.ink_room()
        return super().minimumSizeHint() + QSize(room, room)

    def recolor(self):
        c = tone(self, self.color)
        self.setStyleSheet(f"color: rgba({c.red()},{c.green()},{c.blue()},{c.alpha()});")

    def showEvent(self, event):
        self.recolor()  # it may sit on a game window now
        super().showEvent(event)

    def paintEvent(self, event):
        """The game's shadow first when the words are over its sky (sky_shadow), then the words."""
        if self.text():
            flags = int(self.alignment().value) | (int(Qt.TextFlag.TextWordWrap.value) if self.wordWrap() else 0)
            box = QRectF(self.contentsRect()).adjusted(self.margin(), self.margin(), -self.margin(), -self.margin())
            p = QPainter(self)
            p.setFont(self.font())
            sky_shadow(p, self, tone(self, self.color), lambda d: p.drawText(box.translated(d), flags, self.text()))
            p.end()
        super().paintEvent(event)


def sky_shadow(p, widget, colour, draw):
    """The shadow the game puts under words over its screens (builder_game.SKY_SHADOW: a copy in
    the shadow box's colour, a tenth of the font's height down and right), drawn by
    draw(offset) with p's font; nothing on the game's windows, whose words have none."""
    if SKIN is None or on_game_window(widget):
        return
    shade, dx, dy = SKIN.text_shadow(*builder_game.SKY_SHADOW)
    shade = QColor(shade)
    shade.setAlpha(int(shade.alpha() * QColor(colour).alphaF()))
    height = QFontMetrics(p.font()).height()
    pen = p.pen()
    p.setPen(shade)
    draw(QPointF(dx * height, dy * height))
    p.setPen(pen)


def label(text, size=14, weight=400, color=INK, kind="text", spacing=0.0, wrap=False):
    widget = Text(text, color)
    widget.setFont(font(size, weight, kind, spacing))
    widget.setWordWrap(wrap)
    widget.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse if wrap else Qt.TextInteractionFlag.NoTextInteraction)
    return widget


def set_color(widget, color):
    widget.color = color
    widget.recolor()


def style_sheet():
    """STYLE, and on the game's windows (Glass) fields, lists and sliders in the window's own
    colours: its text colour on a paler fill, as the game writes on its windows."""
    if SKIN is None or not SKIN.has("window"):
        return STYLE
    text, _fill = SKIN.window_colours()
    plate = plate_text()
    plate_fill = SKIN.window_colours("plate")[1]
    ink = f"{text.red()},{text.green()},{text.blue()}"
    on_plate = f"{plate.red()},{plate.green()},{plate.blue()}"
    # Fields sit on the game's plate (GameLineEdit paints it), the log on the game's window;
    # nothing of Qt's own frames, fills or scroll bars shows.
    return STYLE + f"""
QLineEdit, QLineEdit:focus {{ color: rgb({on_plate}); background: transparent; border: none; padding: 6px 18px;
    selection-background-color: rgb({ink}); selection-color: rgb({plate_fill.red()},{plate_fill.green()},{plate_fill.blue()}); }}
QPlainTextEdit, QPlainTextEdit:focus, QTextEdit, QTextEdit:focus {{ color: rgb({ink}); background: transparent; border: none; padding: 0; }}
QScrollBar:vertical {{ width: 0px; }}
QScrollBar:horizontal {{ height: 0px; }}
"""


def game_icon():
    """The window's icon with the game's graphics: the pause menu's collected star."""
    pixmap = QPixmap(256, 256)
    pixmap.fill(Qt.GlobalColor.transparent)
    p = QPainter(pixmap)
    p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
    SKIN.icon(p, QRectF(0, 0, 256, 256), "star")
    p.end()
    return QIcon(pixmap)


# ##############################################################################################
# # The program's icon on Linux                                                               #
# #                                                                                            #
# # On Wayland the desktop (GNOME and the others) takes a window's icon from the .desktop file #
# # named by the window's app id, never from the window itself, so it showed a generic gear.  #
# # The Linux builder program registers itself at every start, as the owner chose (25 Nov     #
# # 2026): galaxyctr-builder.desktop in the user's applications (which also lists "GalaxyCTR   #
# # Builder" among them) and the window's icon next to the user's icons, rewritten only when  #
# # they change (the program moved, the star came). Not from the repository: the entry would   #
# # start Python, not the builder.                                                             #
# ##############################################################################################
GENERIC_ICON = "application-x-executable"  # the icon theme's program icon (freedesktop name)
DESKTOP_ID = "galaxyctr-builder"  # the app id (QGuiApplication.setDesktopFileName) and the files' names


def desktop_exec(path):
    """A path as a .desktop file's Exec wants it: in double quotes, with " ` $ and \\ escaped by a
    backslash, which the file's own string escapes write as two (the Desktop Entry spec)."""
    # The escaping backslash is written \\; a backslash itself is escaped, then both doubled: \\\\.
    return '"' + "".join("\\\\" + (c * 2 if c == "\\" else c) if c in '"`$\\' else c for c in str(path)) + '"'


def disc_present():
    """Whether the disc image named in settings.txt is there (builder_exeicon.disc_identity)."""
    try:
        import builder_exeicon
        return bool(builder_exeicon.disc_identity(B.Settings().get("disc_image")))
    except Exception:  # noqa: BLE001 - a broken settings file is no disc image
        return False


def follow_disc_icon():
    """Linux: the desktop's icon follows the disc image, as the Windows program's own icon does
    (Window.update_program_icon): the game's star while the disc image the look was read from is
    there, the builder's own icon when it is missing or moved, while its dialog asks for it, and
    when that dialog is closed without one (owner, 25 Nov 2026). Here it changes at once."""
    if not sys.platform.startswith("linux"):
        return
    star = SKIN is not None and SKIN.has("star") and disc_present()
    # GNOME's list of applications reads an entry once and keeps it until the next login (the
    # dock follows at once). So the first entry is written only with the game's star, after the
    # first start made it: the plain icon, written first, stayed in that list (owner, 26 Nov
    # 2026). Until then the dock shows the desktop's generic icon. Without the star (the disc
    # image missing) the entry names the desktop's generic program icon: the builder's own blue
    # star is never shown (owner, 26 Nov 2026).
    if not star and not linux_desktop_entry().is_file():
        return
    register_linux_desktop(game_icon() if star else None)


def linux_desktop_entry():
    data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share")
    return data / "applications" / f"{DESKTOP_ID}.desktop"


def register_linux_desktop(icon):
    if not sys.platform.startswith("linux") or not getattr(sys, "frozen", False):
        return
    try:
        import hashlib
        from PySide6.QtCore import QBuffer, QIODevice
        data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share")
        icons = data / "icons" / "hicolor" / "256x256" / "apps"
        desktop = data / "applications" / f"{DESKTOP_ID}.desktop"
        old = sorted(icons.glob(f"{DESKTOP_ID}*.png")) if icons.is_dir() else []
        if icon is None:  # the desktop's own generic program icon, from its icon theme
            png, name = None, GENERIC_ICON
        else:
            buffer = QBuffer()
            buffer.open(QIODevice.OpenModeFlag.WriteOnly)
            icon.pixmap(256, 256).save(buffer, "PNG")
            picture = bytes(buffer.data())
            # The picture's name carries its content's fingerprint: a new icon is a new path in
            # the .desktop file, which the dock then reloads at once; the same path with a new
            # picture stayed the old one in its cache (owner, 25 Nov 2026).
            png = icons / f"{DESKTOP_ID}-{hashlib.sha1(picture).hexdigest()[:10]}.png"
            name = str(png)
            if not png.is_file():
                write_whole(png, picture)
                B.log(f"Registered with the desktop: {png}")
        entry = ("[Desktop Entry]\n"
                 "Type=Application\n"
                 "Name=GalaxyCTR Builder\n"
                 "Comment=Super Mario Galaxy for New Nintendo 3DS, made from your own disc\n"
                 f"Exec={desktop_exec(Path(sys.executable).resolve())}\n"
                 # The picture's own path: by its name alone (the icon theme's lookup) GNOME
                 # showed the gear, in the dock and among the applications (owner, 25 Nov 2026).
                 f"Icon={name}\n"
                 "Terminal=false\n"
                 "Categories=Game;Utility;\n"
                 f"StartupWMClass={DESKTOP_ID}\n").encode("utf-8")
        if not desktop.is_file() or desktop.read_bytes() != entry:
            write_whole(desktop, entry)
            B.log(f"Registered with the desktop: {desktop}")
        for path in old:  # the pictures the entry no longer names
            if path != png:
                path.unlink(missing_ok=True)
    except Exception as error:  # noqa: BLE001 - the generic icon stays
        B.log(f"Could not register the program's icon with the desktop: {error!r}")


def write_whole(path, content):
    """A file written beside and moved into place: never seen half written."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(content)
    os.replace(temporary, path)


# ##############################################################################################
# # The game's graphics (Skin)                                                                 #
# #                                                                                            #
# # builder_assets made them from the user's disc, at twice the layout size: buttons are cut   #
# # into three parts (round ends, a middle that stretches), windows into nine (corners, edges  #
# # and a middle). Icons are drawn as they are, scaled.                                        #
# ##############################################################################################
SKIN = None  # builder_game.Skin: the game's graphics, when they are there and OpenGL draws them


def skin_icon(name):
    return SKIN is not None and SKIN.has(name)


def on_game_window(widget):
    """True when widget sits on one of the game's windows (a Glass panel, with SKIN)."""
    if SKIN is None or not SKIN.has("window"):
        return False
    while widget is not None:
        if isinstance(widget, Glass):
            return True
        widget = widget.parentWidget()
    return False


def tone(widget, color):
    """color as widget shows it. With the game's graphics every word takes a colour of the
    game's layouts, unchanged: on the game's windows the window's text colour (SysInfoWindow's
    TxtConfirm), over the game's sky the colour of MiiSelect's title (TxtMiiChange)."""
    if SKIN is None:
        return color
    if on_game_window(widget):
        return QColor(SKIN.window_colours()[0])
    return QColor(SKIN.text_colour(*builder_game.SKY_TEXT))


def plate_text():
    """The colour of the words on the plate (MiiSelect's TxtPage)."""
    return QColor(SKIN.window_colours("plate")[0])


class IconLabel(QWidget):
    """One of the game's icons, size pixels square, or nothing without the game's graphics."""

    def __init__(self, name, size=28, parent=None):
        super().__init__(parent)
        self.name = name
        self.setFixedSize(size, size)
        self.setVisible(skin_icon(name))

    def paintEvent(self, event):
        if SKIN:
            p = QPainter(self)
            p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            SKIN.icon(p, QRectF(self.rect()), self.name)


def star_path(center, outer, inner):
    path = QPainterPath()
    for index in range(8):
        radius = outer if index % 2 == 0 else inner
        angle = math.pi / 4 * index - math.pi / 2
        point = QPointF(center.x() + radius * math.cos(angle), center.y() + radius * math.sin(angle))
        path.moveTo(point) if index == 0 else path.lineTo(point)
    path.closeSubpath()
    return path


# ##############################################################################################
# # The background: space                                                                      #
# #                                                                                            #
# # The window itself paints it, 30 times a second: a night gradient, four soft nebulae that   #
# # drift slowly (drawn small, then stretched: cheap, and soft by nature), a perspective grid  #
# # running towards the horizon, twinkling stars moving at three depths and now and then a     #
# # shooting star. "frost" is the nebulae once more, smaller still: glass panels show the part #
# # behind them, which reads as frosted glass.                                                 #
# ##############################################################################################
NEBULAE = ((0.18, 0.25, 0.55, DEEP, 120, 0.07, 0.0), (0.86, 0.18, 0.50, BLUE, 110, 0.05, 2.0),
           (0.70, 0.88, 0.60, CYAN, 50, 0.06, 4.0), (0.22, 0.92, 0.42, DEEP, 90, 0.08, 1.0))


class Space(QWidget):
    def __init__(self):
        super().__init__()
        rnd = random.Random(7)
        self.stars = [(rnd.random(), rnd.random(), rnd.uniform(0.6, 2.3), rnd.uniform(0, 6.3),
                       rnd.uniform(0.6, 2.4), rnd.choice((0.35, 0.65, 1.0))) for _ in range(230)]
        self.started = now()
        self.t = 0.0
        self.frames_drawn = 0
        self.nebula = self.frost = None
        self.comet = None
        self.timer = builder_game.frame_timer(self)  # Settings > Video: the frame rate
        self.timer.timeout.connect(self.tick)
        self.timer.start()

    def game_sky_shows(self):
        """True when one of the game's skies covers the window, so this painted space is unseen."""
        return any(sky is not None and sky.isVisible() for sky in (getattr(self, "sky", None), getattr(self, "title_sky", None)))

    def moving_sky_shows(self):
        """True when a game sky covers the window and moves on the beat: its redraw is a frame
        of the whole window already."""
        return any(sky is not None and sky.isVisible() and sky.timer.isActive()
                   for sky in (getattr(self, "sky", None), getattr(self, "title_sky", None)))

    def tick(self):
        if self.moving_sky_shows():
            # The sky's own redraw repaints the window with everything on it. Asked here too, Qt
            # painted the window twice in that one frame (measured on Wayland: 120 paints a
            # second at 60).
            return
        if REDUCED or self.game_sky_shows():
            # The space stands still (or does not show); the window is still redrawn, for the
            # progress indicators and everything else that moves on it.
            if not self.game_sky_shows() and (self.nebula is None or self.nebula.width() != max(16, self.width() // 5)):
                self.paint_nebulae()
            self.update()
            return
        last, self.t = self.t, now() - self.started
        self.paint_nebulae()
        # A comet now and then: 0.6% a frame at the 30 frames a second this space was made for,
        # the same chance a second at any frame rate (more frames must not bring more comets).
        chance = 1.0 - (1.0 - 0.006) ** (max(0.0, self.t - last) * 30.0)
        if self.comet is None and random.random() < chance:
            self.comet = (self.t, random.uniform(0.1, 0.8), random.uniform(0.02, 0.35))
        elif self.comet and self.t - self.comet[0] > 1.4:
            self.comet = None
        self.update()

    def paint_nebulae(self):
        width, height = max(1, self.width()), max(1, self.height())
        small_w, small_h = max(16, width // 5), max(16, height // 5)
        image = QImage(small_w, small_h, QImage.Format.Format_ARGB32_Premultiplied)
        p = QPainter(image)
        night = QLinearGradient(0, 0, 0, small_h)
        night.setColorAt(0, QColor(3, 5, 16))
        night.setColorAt(1, QColor(6, 12, 34))
        p.fillRect(image.rect(), night)
        t = self.t
        for cx, cy, size, color, strength, speed, phase in NEBULAE:
            x = (cx + 0.08 * math.sin(t * speed + phase)) * small_w
            y = (cy + 0.06 * math.cos(t * speed * 0.8 + phase)) * small_h
            radius = size * max(small_w, small_h) * (1 + 0.07 * math.sin(t * speed * 1.3 + phase))
            cloud = QRadialGradient(QPointF(x, y), radius)
            cloud.setColorAt(0, alpha(color, strength))
            cloud.setColorAt(0.5, alpha(color, strength * 0.35))
            cloud.setColorAt(1, alpha(color, 0))
            p.fillRect(image.rect(), cloud)
        p.end()
        self.nebula = image
        self.frost = image.scaled(max(4, small_w // 3), max(4, small_h // 3), Qt.AspectRatioMode.IgnoreAspectRatio,
                                  Qt.TransformationMode.SmoothTransformation)

    def event(self, event):
        if event.type() == QEvent.Type.UpdateRequest:
            # The frame rate counter (FrameCounter): one frame for each time Qt draws the window
            # and shows it, whatever part of it changed (a game sky in OpenGL covers this
            # painted space, which then is not drawn at all).
            self.frames_drawn += 1
        return super().event(event)

    def paintEvent(self, event):
        if self.game_sky_shows():
            return  # the game's sky covers the window: nothing of this painted space shows
        if self.nebula is None:
            self.paint_nebulae()
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        width, height, t = self.width(), self.height(), self.t
        p.drawImage(QRectF(self.rect()), self.nebula)

        # The grid: lines to a vanishing point, and rows that come closer.
        horizon = height * 0.74
        for index in range(-14, 15):
            p.setPen(QPen(alpha(CYAN, 16), 1))
            p.drawLine(QPointF(width / 2 + index * width * 0.035, horizon), QPointF(width / 2 + index * width * 0.26, height))
        for row in range(12):
            k = ((row + (t * 0.35) % 1) / 12) ** 2
            p.setPen(QPen(alpha(CYAN, 40 * k), 1))
            y = horizon + k * (height - horizon)
            p.drawLine(QPointF(0, y), QPointF(width, y))
        fade = QLinearGradient(0, horizon - 30, 0, horizon + 40)
        fade.setColorAt(0, alpha(QColor(6, 12, 34), 0))
        fade.setColorAt(0.5, alpha(QColor(6, 12, 34), 120))
        fade.setColorAt(1, alpha(QColor(6, 12, 34), 0))
        p.fillRect(QRectF(0, horizon - 30, width, 70), fade)

        p.setPen(Qt.PenStyle.NoPen)
        for x, y, size, phase, speed, depth in self.stars:
            twinkle = 0.55 + 0.45 * math.sin(t * speed + phase)
            sx = ((x + t * 0.004 * depth) % 1.0) * width
            sy = y * height * 0.98
            color = mix(QColor(255, 255, 255), CYAN, 0.25 if phase > 3 else 0.0)
            p.setBrush(alpha(color, 255 * twinkle * depth))
            radius = size * 0.5 * (0.75 + 0.25 * twinkle) * (0.6 + 0.4 * depth)
            p.drawEllipse(QPointF(sx, sy), radius, radius)
            if size > 2.0 and depth == 1.0:
                p.setPen(QPen(alpha(color, 90 * twinkle), 1))
                p.drawLine(QPointF(sx - size * 3.5, sy), QPointF(sx + size * 3.5, sy))
                p.drawLine(QPointF(sx, sy - size * 3.5), QPointF(sx, sy + size * 3.5))
                p.setPen(Qt.PenStyle.NoPen)

        if self.comet:
            start, cx, cy = self.comet
            k = (t - start) / 1.4
            head = QPointF((cx + k * 0.45) * width, (cy + k * 0.22) * height)
            tail = QPointF(head.x() - 160, head.y() - 78)
            trail = QLinearGradient(tail, head)
            trail.setColorAt(0, alpha(CYAN, 0))
            trail.setColorAt(1, alpha(QColor(255, 255, 255), 220 * (1 - k)))
            p.setPen(QPen(trail, 2, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            p.drawLine(tail, head)


class Glass(QWidget):
    """A frosted glass panel: the blurred space behind it, a dark tint, a sheen and a glowing edge.
    With the game's graphics it is the game's system window instead, and what is on it takes the
    window's colours (tone, style_sheet)."""

    def __init__(self, parent=None, radius=22, tint=105):
        super().__init__(parent)
        self.radius, self.tint = radius, tint

    def paintEvent(self, event):
        p = QPainter(self)
        if SKIN and SKIN.has("window"):
            # The game's system window (SysInfoWindow) with its shadow, as the game draws it.
            SKIN.window(p, QRectF(self.rect()))
            return
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        box = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        shape = QPainterPath()
        shape.addRoundedRect(box, self.radius, self.radius)
        p.save()
        p.setClipPath(shape)
        space = self.window()
        frost = getattr(space, "frost", None)
        if frost is not None and space.width() > 0 and space.height() > 0:
            corner = self.mapTo(space, QPoint(0, 0))
            sx, sy = frost.width() / space.width(), frost.height() / space.height()
            p.drawImage(QRectF(self.rect()), frost, QRectF(corner.x() * sx, corner.y() * sy, self.width() * sx, self.height() * sy))
        tint = QLinearGradient(0, 0, 0, self.height())
        tint.setColorAt(0, QColor(30, 44, 100, self.tint))
        tint.setColorAt(1, QColor(8, 12, 38, min(255, self.tint + 40)))
        p.fillRect(self.rect(), tint)
        sheen = QLinearGradient(0, 0, self.width() * 0.6, self.height())
        sheen.setColorAt(0, QColor(255, 255, 255, 26))
        sheen.setColorAt(0.45, QColor(255, 255, 255, 0))
        p.fillRect(self.rect(), sheen)
        p.restore()
        edge = QLinearGradient(box.topLeft(), box.bottomRight())
        edge.setColorAt(0, alpha(CYAN, 150))
        edge.setColorAt(0.5, QColor(255, 255, 255, 26))
        edge.setColorAt(1, alpha(DEEP, 160))
        p.setPen(QPen(edge, 1.2))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawPath(shape)


# ##############################################################################################
# # Controls                                                                                   #
# ##############################################################################################
KEYBOARD_REASONS = (Qt.FocusReason.TabFocusReason, Qt.FocusReason.BacktabFocusReason,
                    Qt.FocusReason.ShortcutFocusReason)


class KeyboardFocus:
    """The focus ring shows when focus came from the keyboard (like CSS :focus-visible), at once;
    focus Qt gives by itself, or a click, shows none."""

    keyboard_focus = False

    def focusInEvent(self, event):
        self.keyboard_focus = event.reason() in KEYBOARD_REASONS
        super().focusInEvent(event)
        self.update()

    def focusOutEvent(self, event):
        self.keyboard_focus = False
        super().focusOutEvent(event)
        self.update()

    def ring(self):
        return self.keyboard_focus and self.hasFocus()


class Button(KeyboardFocus, QPushButton):
    """A pill button that glows when the pointer is on it. kind: primary, glass, danger, yes, no
    or back.

    With the game's graphics (SKIN) it is the game's own button instead (builder_game.GameButton):
    the layout's panes drawn as the game draws them, its size, its animations when the pointer
    comes on, leaves and presses, and its sounds. Then clicked comes when the press animation
    has played, as a game screen acts once its button has flashed."""

    def __init__(self, text, kind="glass", parent=None, icon=None):
        super().__init__(text, parent)
        self.kind, self.hover, self.icon_name = kind, 0.0, icon
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFont(font(14, 600))
        self.setMinimumHeight(50)
        self.animation = builder_game.ClockAnimation(self)
        self.animation.setDuration(180)
        self.animation.valueChanged.connect(self._hovered)
        self.game = None
        self.game_hidden = False  # the game button is away (set_game_shown)
        self.game_ahead = 0.0  # the share of the next game frame it is drawn at (game_tick)
        if SKIN is not None and SKIN.has("button"):
            try:
                self.game = builder_game.GameButton(SKIN, kind, text)
            except Exception as error:  # noqa: BLE001 - a missing file leaves the painted button
                B.log(f"Game button {kind!r}: {error!r}")
                self.game = None
        if self.game is not None:
            self.game.clicked = self.click
            w, h = self.game.size()
            self.setFixedSize(QSize(math.ceil(w), math.ceil(h)))
            self.setMouseTracking(True)
            self.pointer_on, self.pressed = False, False

    def side_room(self):
        """The clear room on each side of the drawn game button, in pixels (0 when painted)."""
        if self.game is None:
            return 0
        return max(0.0, (self.game.size()[0] - self.game.hit_rect().width()) / 2)

    def set_game_shown(self, shown):
        """Brings the game button in (ButtonAppear) or sends it away (ButtonEnd), as a screen
        does with a button that has nothing to do (MiiSelect::updateButtons)."""
        if self.game is None or shown != self.game_hidden:
            return
        self.game_hidden = not shown
        if self.isVisible():
            if shown:
                self.game.controller.appear()
            else:
                self.game.controller.disappear()
            self.wake()

    def setText(self, text):
        super().setText(text)
        if self.game is not None:
            self.game.set_text(text)
            self.update()

    def _hovered(self, value):
        self.hover = float(value)
        self.update()

    def _glow_to(self, target):
        self.animation.stop()
        self.animation.setStartValue(self.hover)
        self.animation.setEndValue(target)
        self.animation.start()

    # The game button's frames: builder_game.Ticker gives the game's whole frames gone by (60 a
    # second) and the share of the next one, which the drawing goes ahead by.
    def game_tick(self, frames=1, share=0.0):
        if not self.isVisible():
            return False
        c = self.game.controller
        changed = False
        for _ in range(frames):
            pressed, self.pressed = self.pressed, False
            on = (self.pointer_on or self.ring()) and self.isEnabled()
            if pressed and on and not c.is_nerve("Pointing") and c.nerve in ("Wait", "NotPointing"):
                self.game.decide_now()  # pressed before the pointer was seen on it (a touch screen)
                pressed = False
            changed = self.game.tick(on, pressed, self.isEnabled()) or changed
        busy = self.game.busy() or c.nerve not in ("Wait", "Hidden") or self.pointer_on
        ahead = share if busy and any(not p.stopped for p in self.game.scene.players.values()) else 0.0
        if changed or ahead or ahead != self.game_ahead:
            self.update()
        self.game_ahead = ahead
        return busy

    def wake(self):
        builder_game.Ticker.get().wake(self)

    def showEvent(self, event):
        super().showEvent(event)
        if self.game is not None:
            if self.game_hidden:
                self.game.controller.set_nerve("Hidden")
            else:
                self.game.controller.appear()
            self.wake()

    def mouseMoveEvent(self, event):
        if self.game is None:
            return super().mouseMoveEvent(event)
        on = self.game.hit_rect().contains(event.position())
        if on != self.pointer_on:
            self.pointer_on = on
            self.game.entered = self.game.entered or on
            self.setCursor(Qt.CursorShape.PointingHandCursor if on else Qt.CursorShape.ArrowCursor)
            self.wake()

    def mousePressEvent(self, event):
        if self.game is None:
            return super().mousePressEvent(event)
        if event.button() == Qt.MouseButton.LeftButton and self.game.hit_rect().contains(event.position()):
            self.pointer_on = True
            self.pressed = True  # MR::testDPDMenuPadDecideTrigger: the press decides
            self.wake()

    def mouseReleaseEvent(self, event):
        if self.game is None:
            return super().mouseReleaseEvent(event)

    def keyPressEvent(self, event):
        if self.game is not None and event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter, Qt.Key.Key_Space):
            self.game.decide_now()
            self.wake()
            return
        super().keyPressEvent(event)

    def focusInEvent(self, event):
        super().focusInEvent(event)
        if self.game is not None and self.ring():
            self.game.entered = True  # the keyboard's focus points at it, as the pointer would
            self.wake()

    def focusOutEvent(self, event):
        super().focusOutEvent(event)
        if self.game is not None:
            self.wake()

    def enterEvent(self, event):
        if self.game is None:
            self._glow_to(1.0)
        super().enterEvent(event)

    def leaveEvent(self, event):
        if self.game is None:
            self._glow_to(0.0)
        else:
            self.pointer_on = False
            self.wake()
        super().leaveEvent(event)

    def sizeHint(self):
        if self.game is not None:
            return self.size()
        extra = 34 if self.icon_name and skin_icon(self.icon_name) else 0
        return QSize(QFontMetrics(self.font()).horizontalAdvance(self.text()) + 64 + extra, 50)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        if self.game is not None:
            self.paint_game(p)
            return
        box = QRectF(self.rect()).adjusted(5, 5, -5, -5)
        if self.isDown():
            box.adjust(1.5, 1.5, -1.5, -1.5)
        radius = box.height() / 2
        h = self.hover
        p.setOpacity(1.0 if self.isEnabled() else 0.4)
        text_color = INK
        if self.kind == "primary":
            for ring in range(1, 4):
                p.setPen(QPen(alpha(CYAN, (35 + 70 * h) / ring), 2))
                p.setBrush(Qt.BrushStyle.NoBrush)
                p.drawRoundedRect(box.adjusted(-ring * 1.6, -ring * 1.6, ring * 1.6, ring * 1.6), radius + ring, radius + ring)
            fill = QLinearGradient(box.topLeft(), box.topRight())
            fill.setColorAt(0, BLUE.lighter(100 + int(25 * h)))
            fill.setColorAt(1, mix(CYAN, QColor(255, 255, 255), 0.15 * h))
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(fill)
            p.drawRoundedRect(box, radius, radius)
            sheen = QLinearGradient(box.topLeft(), box.bottomLeft())
            sheen.setColorAt(0, QColor(255, 255, 255, 70))
            sheen.setColorAt(0.55, QColor(255, 255, 255, 0))
            p.setBrush(sheen)
            p.drawRoundedRect(box, radius, radius)
            text_color = QColor(4, 14, 40)
        else:
            accent = BAD if self.kind in ("danger", "no") else CYAN
            p.setBrush(alpha(accent if self.kind == "danger" else QColor(255, 255, 255), 14 + 22 * h))
            p.setPen(QPen(alpha(accent, 70 + 130 * h), 1.3))
            p.drawRoundedRect(box, radius, radius)
            text_color = mix(BAD, QColor(255, 255, 255), 0.35) if self.kind == "danger" else INK
        p.setPen(text_color)
        p.setFont(self.font())
        p.drawText(box, Qt.AlignmentFlag.AlignCenter, self.text())
        if self.ring():
            p.setOpacity(1.0)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(QPen(ICE, 2))
            ring = box.adjusted(-3.5, -3.5, 3.5, 3.5)
            p.drawRoundedRect(ring, ring.height() / 2, ring.height() / 2)

    def paint_game(self, p):
        """The game's button as its layout draws it now (builder_game.GameButton.image)."""
        image = self.game.image(self.devicePixelRatioF(), self.game_ahead)
        p.drawImage(QPointF(0, 0), image)  # keyboard focus shows as the game's pointing pose


class IconButton(Button):
    """The pause menu's small square button (the one with Luigi's letter, builder_game "icon"),
    with a picture of the game's picture font in place of the letter: words only as its tooltip.
    A second picture (badge) can sit on its lower right corner, moving with the button, as the
    music's note shows the camera's red "no" sign when the music is off. Without the game's graphics it is a painted button with the
    words."""

    def __init__(self, icon, words, parent=None):
        game = SKIN is not None and SKIN.has(icon) and SKIN.has("button")
        super().__init__("" if game else words, "icon" if game else "glass", parent)
        self.picture, self.badge = None, None
        self.set_icon(icon, None, words)

    def set_icon(self, icon, badge=None, words=None):
        if words:
            self.setToolTip(words)
            self.setAccessibleName(words)
            if self.game is None:
                self.setText(words)
        if self.game is not None and icon != self.picture:
            self.game.set_icon(SKIN.folder / SKIN.manifest["raw"][icon])
        self.picture, self.badge = icon, badge
        self.update()

    # The badge in the button's own space (LButton, where the picture is): its centre on the
    # picture's lower right, and its side, in layout units.
    BADGE_AT, BADGE_SIDE = (22.0, -14.0), 18.0

    def paint_game(self, p):
        super().paint_game(p)
        if self.badge and SKIN.has(self.badge):
            # Drawn where the button's animations put it now: it grows, moves and fades with
            # the button (ButtonAppear, ButtonSelectIn and Out, ButtonDecide).
            placed = self.game.place("LButton", *self.BADGE_AT, ahead=self.game_ahead)
            if placed is None or placed[2] <= 0:
                return
            centre, scale, opacity = placed
            size = self.BADGE_SIDE * scale * builder_game.UNIT
            # Its colours change as the picture's do (the note darkens under the pointer).
            shift = self.game.ink_shift(self.game_ahead)
            SKIN.icon(p, QRectF(centre.x() - size / 2, centre.y() - size / 2, size, size), self.badge, opacity,
                      shift=shift)


def button_row(buttons, align="right"):
    """A row of buttons. A game button's widget keeps clear room on each side for its grown
    pose; neighbours share that room (one side's worth), so the drawn buttons sit that far
    apart and the places that take the pointer never overlap. The outer rooms reach into the
    card's margin, so the buttons line up with the text."""
    row = QHBoxLayout()
    room = max([b.side_room() for b in buttons if isinstance(b, Button)] or [0])
    row.setSpacing(int(-room) if room else 10)
    row.setContentsMargins(-int(room), 0, -int(room), 0)
    if align in ("right", "center"):
        row.addStretch(1)
    for b in buttons:
        row.addWidget(b)
    if align in ("left", "center"):
        row.addStretch(1)
    return row


class Segmented(KeyboardFocus, QWidget):
    """A row of choices with a sliding, glowing marker on the chosen one."""

    changed = Signal(int)

    def __init__(self, choices, index=0, parent=None):
        super().__init__(parent)
        self.choices, self.index, self.position = list(choices), index, float(index)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setFixedHeight(46)
        self.setMaximumWidth(560)
        self.setFont(font(13, 600))
        self.animation = builder_game.ClockAnimation(self)
        self.animation.setDuration(260)
        self.animation.setEasingCurve(QEasingCurve.Type.OutCubic)
        self.animation.valueChanged.connect(self._moved)

    def _moved(self, value):
        self.position = float(value)
        self.update()

    def sizeHint(self):
        metrics = QFontMetrics(self.font())
        if SKIN is not None and SKIN.has("plate"):
            return QSize(max(metrics.horizontalAdvance(c) for c in self.choices) + 56, 44)
        return QSize(sum(metrics.horizontalAdvance(c) + 40 for c in self.choices), 44)

    def minimumSizeHint(self):
        # Never narrower than its longest choice: without it the grid squeezed the plate between
        # the arrows to nothing in a narrower window (owner, 26 Nov 2026).
        return self.sizeHint()

    def set_index(self, index, notify=False):
        index = max(0, min(len(self.choices) - 1, index))
        if index == self.index and not notify:
            return
        self.index = index
        self.animation.stop()
        self.animation.setStartValue(self.position)
        self.animation.setEndValue(float(index))
        self.animation.start()
        if notify:
            self.changed.emit(index)

    def mousePressEvent(self, event):
        self.set_index(int(event.position().x() / max(1, self.width()) * len(self.choices)), True)

    def keyPressEvent(self, event):
        if event.key() in (Qt.Key.Key_Left, Qt.Key.Key_Right):
            self.set_index(self.index + (1 if event.key() == Qt.Key.Key_Right else -1), True)
        else:
            super().keyPressEvent(event)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        if SKIN is not None and SKIN.has("plate"):
            # As MiiSelect's page counter between its arrows: the plate and the chosen words.
            p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            SKIN.window(p, QRectF(self.rect()), "plate")
            p.setPen(plate_text())
            p.setFont(self.font())
            p.drawText(QRectF(self.rect()), Qt.AlignmentFlag.AlignCenter, self.choices[self.index])
            return
        box = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        radius = box.height() / 2
        p.setBrush(tone(self, QColor(255, 255, 255, 14)))
        p.setPen(QPen(tone(self, ICE), 2) if self.ring() else QPen(tone(self, alpha(CYAN, 60)), 1))
        p.drawRoundedRect(box, radius, radius)
        width = box.width() / len(self.choices)
        marker = QRectF(box.left() + self.position * width + 3, box.top() + 3, width - 6, box.height() - 6)
        fill = QLinearGradient(marker.topLeft(), marker.topRight())
        fill.setColorAt(0, alpha(BLUE, 220))
        fill.setColorAt(1, alpha(CYAN, 220))
        p.setPen(QPen(alpha(CYAN, 120), 3))
        p.setBrush(fill)
        p.drawRoundedRect(marker, marker.height() / 2, marker.height() / 2)
        p.setFont(self.font())
        for index, choice in enumerate(self.choices):
            closeness = max(0.0, 1 - abs(self.position - index))
            p.setPen(mix(tone(self, MUTED), QColor(4, 14, 40), closeness))
            p.drawText(QRectF(box.left() + index * width, box.top(), width, box.height()), Qt.AlignmentFlag.AlignCenter, choice)


class ArrowChoice(QWidget):
    """A row of choices (Segmented) between MiiSelect's two page arrows: with the game's
    graphics only the arrows change it (and the keyboard's left and right), one choice a press,
    with their animations and sound; at an end its arrow goes away as MiiSelect's does on the
    first and last page. Without them it is the plain row, chosen with a click."""

    changed = Signal(int)

    def __init__(self, choices, index=0, parent=None):
        super().__init__(parent)
        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(6)
        self.choices = Segmented(choices, index)
        self.choices.changed.connect(self._changed)
        self.left = self.right = None
        if SKIN is not None and SKIN.has("button") and SKIN.has("arrows"):
            self.left, self.right = Button("<", "arrow_left"), Button(">", "arrow_right")
            if self.left.game is not None and self.right.game is not None:
                self.left.game.partner, self.right.game.partner = self.right.game, self.left.game
                self.left.clicked.connect(lambda: self.step(-1))
                self.right.clicked.connect(lambda: self.step(1))
                self.choices.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
                self.choices.setCursor(Qt.CursorShape.ArrowCursor)
                # The game's counter keeps one size, as wide as its longest choice, whatever the
                # window's width (owner, 26 Nov 2026).
                self.choices.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
            else:
                self.left = self.right = None
        if self.left is not None:
            row.addWidget(self.left)
        row.addWidget(self.choices, 0 if self.left is not None else 1)  # compact, as the game's counter
        if self.right is not None:
            row.addWidget(self.right)
        row.addStretch(1 if self.left is not None else 0)
        self._arrows()

    @property
    def index(self):
        return self.choices.index

    def step(self, delta):
        self.set_index(self.choices.index + delta, True)

    def set_index(self, index, notify=False):
        self.choices.set_index(index, notify)
        self._arrows()

    def _changed(self, index):
        self._arrows()
        self.changed.emit(index)

    def _arrows(self):
        if self.left is not None:
            self.left.set_game_shown(self.choices.index > 0)
            self.right.set_game_shown(self.choices.index < len(self.choices.choices) - 1)


class NumberChoice(ArrowChoice):
    """A number between MiiSelect's arrows, as its page counter: the slider's calls."""

    valueChanged = Signal(int)

    def __init__(self, minimum=1, maximum=10, parent=None):
        self.minimum = minimum
        super().__init__([str(n) for n in range(minimum, maximum + 1)], 0, parent)
        self.changed.connect(lambda index: self.valueChanged.emit(self.value()))

    def setRange(self, minimum, maximum):
        value = self.value()
        self.minimum = minimum
        self.choices.choices = [str(n) for n in range(minimum, maximum + 1)]
        self.setValue(value)

    def setValue(self, value):
        self.set_index(max(0, min(len(self.choices.choices) - 1, int(value) - self.minimum)), True)

    def value(self):
        return self.minimum + self.choices.index


def make_slider():
    """The number of FTP connections: MiiSelect's arrows and counter with the game's graphics,
    a slider without them."""
    if SKIN is not None and SKIN.has("arrows") and SKIN.has("plate"):
        return NumberChoice(1, B.FTP_MAX_CONNECTIONS)
    return QSlider(Qt.Orientation.Horizontal)


class GameLineEdit(QLineEdit):
    """A text field on the game's plate (MiiSelect's page counter), with the game's graphics."""

    def paintEvent(self, event):
        if SKIN is not None and SKIN.has("plate"):
            p = QPainter(self)
            p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            SKIN.window(p, QRectF(self.rect()), "plate")
            p.end()
        super().paintEvent(event)


class Progress(QWidget):
    """A glowing bar that eases to its value, with a light sweeping along it. Below 0 it is busy:
    a glow slides to and fro, for steps whose length is not known."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.target = self.shown = 0.0
        self.busy = False
        self.setFixedHeight(40 if SKIN is not None and SKIN.has("save") else 16)

    def set_value(self, percent):
        self.busy = percent < 0
        if not self.busy:
            self.target = max(0.0, min(100.0, percent))
            if self.target < self.shown - 5:
                self.shown = self.target  # a new part starts again from the left

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        if SKIN is not None and SKIN.has("save"):
            # No bar in the game: while a part runs, the star the game spins while it works
            # (SaveIcon); the percentage is written next to it by the page.
            if self.busy or self.target < 100:
                p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
                SKIN.save_icon(p, QRectF(0, 0, self.height() * 1.4, self.height()), now())
            return
        self.shown += (self.target - self.shown) * 0.18
        box = QRectF(self.rect()).adjusted(1, 3, -1, -3)
        radius = box.height() / 2
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(tone(self, QColor(255, 255, 255, 22)))
        p.drawRoundedRect(box, radius, radius)
        if self.busy:
            band = box.width() * 0.28
            k = (math.sin(now() * 2.2) + 1) / 2
            part = QRectF(box.left() + k * (box.width() - band), box.top(), band, box.height())
            glow = QLinearGradient(part.topLeft(), part.topRight())
            glow.setColorAt(0, alpha(CYAN, 0))
            glow.setColorAt(0.5, alpha(CYAN, 230))
            glow.setColorAt(1, alpha(CYAN, 0))
            p.setBrush(glow)
            p.drawRoundedRect(part, radius, radius)
            return
        filled = QRectF(box.left(), box.top(), max(box.height(), box.width() * self.shown / 100), box.height())
        if self.shown <= 0.05:
            return
        fill = QLinearGradient(filled.topLeft(), filled.topRight())
        fill.setColorAt(0, BLUE)
        fill.setColorAt(1, CYAN)
        p.setBrush(fill)
        p.drawRoundedRect(filled, radius, radius)
        sweep = ((sway() * 0.55) % 1.4 - 0.2) * filled.width() if not REDUCED else -1000.0
        light = QLinearGradient(QPointF(filled.left() + sweep - 60, 0), QPointF(filled.left() + sweep + 60, 0))
        light.setColorAt(0, QColor(255, 255, 255, 0))
        light.setColorAt(0.5, QColor(255, 255, 255, 110))
        light.setColorAt(1, QColor(255, 255, 255, 0))
        p.setBrush(light)
        p.drawRoundedRect(filled, radius, radius)
        tip = QRadialGradient(QPointF(filled.right() - radius, filled.center().y()), 14)
        tip.setColorAt(0, alpha(QColor(255, 255, 255), 220))
        tip.setColorAt(1, alpha(CYAN, 0))
        p.setBrush(tip)
        p.drawEllipse(QPointF(filled.right() - radius, filled.center().y()), 14, 14)


def draw_partial(p, points, k):
    """The first k (0 to 1) of a line through points: a tick that draws itself."""
    lengths = [math.hypot(b.x() - a.x(), b.y() - a.y()) for a, b in zip(points, points[1:])]
    left = sum(lengths) * max(0.0, min(1.0, k))
    for (a, b), length in zip(zip(points, points[1:]), lengths):
        if left <= 0:
            break
        part = min(1.0, left / length) if length else 1.0
        p.drawLine(a, QPointF(a.x() + (b.x() - a.x()) * part, a.y() + (b.y() - a.y()) * part))
        left -= length


def draw_mark(p, center, radius, good, k):
    pen = QPen(QColor(4, 14, 40) if good else QColor(255, 255, 255), max(2.0, radius * 0.16),
               Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
    p.setPen(pen)
    x, y, r = center.x(), center.y(), radius
    if good:
        draw_partial(p, [QPointF(x - 0.42 * r, y + 0.02 * r), QPointF(x - 0.12 * r, y + 0.32 * r),
                         QPointF(x + 0.45 * r, y - 0.30 * r)], k)
    else:
        draw_partial(p, [QPointF(x - 0.3 * r, y - 0.3 * r), QPointF(x + 0.3 * r, y + 0.3 * r)], min(1.0, k * 2))
        draw_partial(p, [QPointF(x + 0.3 * r, y - 0.3 * r), QPointF(x - 0.3 * r, y + 0.3 * r)], k * 2 - 1)


QUIET_AFTER_ANSWER = 0.8  # seconds without a question before the save star turns


class StepRow(QWidget):
    """One of the six steps: waiting, running (a spinning arc and a pulse), done, failed or skipped."""

    def __init__(self, number, title, last=False, parent=None):
        super().__init__(parent)
        self.number, self.title, self.last = number, title, last
        self.state, self.detail, self.since = "wait", "", now()
        self.spin_since = None  # when the save star began to turn
        self.setMinimumHeight(62)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

    def set_state(self, state, title=None):
        if title:
            self.title = re.sub(r"\s*\(.*\)$", "", title)  # "(about a minute)" is on the big card
        if state != self.state:
            self.state, self.since = state, now()
            self.spin_since = None
            if state != "run":
                self.detail = ""
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        center, radius = QPointF(22, 24), 14.0
        done = self.state == "done"
        line = tone(self, alpha(CYAN, 150) if done else QColor(255, 255, 255, 34))
        if not self.last and SKIN is None:
            p.setPen(QPen(line, 2))
            p.drawLine(QPointF(center.x(), center.y() + radius + 4), QPointF(center.x(), self.height() + 6))
        age = now() - self.since
        p.setPen(Qt.PenStyle.NoPen)
        if SKIN and self.state == "fail" and skin_icon("forbid"):
            # A step that failed: the camera's red "no" sign, as the stopped build's page shows it.
            size = 2 * radius + 8
            p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            SKIN.icon(p, QRectF(center.x() - size / 2, center.y() - size / 2, size, size), "forbid")
        elif SKIN and self.state != "fail" and skin_icon("star"):
            # The pause menu's star places (PauseMenu::updateStarPane): a step not done yet is the
            # dark, empty place of a star (ShaStar alone); a done step has the star on it
            # (PicStar over ShaStar).
            box = QRectF(center.x() - radius - 4, center.y() - radius - 4, 2 * radius + 8, 2 * radius + 8)
            p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            # The step being made shows the star the game spins while it saves (SaveIcon), in
            # place of the step's star, only while the build works: not while it waits for an
            # answer (the disc, at the start), nor in the moment after one, while the answered
            # card fades away and the next question is on its way.
            main = self.window()
            working = self.state == "run" and not getattr(main, "asking", 0) and \
                now() - getattr(main, "answered_at", 0.0) >= QUIET_AFTER_ANSWER
            if self.state in ("wait", "skip") or (self.state == "run" and not working):
                SKIN.icon(p, box, "star", star_done=False)
            if self.state == "done":
                SKIN.icon(p, box, "star", star_done=True)
            elif working:
                if self.spin_since is None:
                    self.spin_since = now()
                SKIN.save_icon(p, box.adjusted(-4, -4, 4, 4), now() - self.spin_since)
        elif self.state == "run":
            pulse = (now() * 0.8) % 1.0 if not REDUCED else 1.0
            p.setBrush(alpha(CYAN, 90 * (1 - pulse)))
            p.drawEllipse(center, radius + 10 * pulse, radius + 10 * pulse)
            p.setBrush(QColor(10, 30, 70, 230))
            p.setPen(QPen(alpha(CYAN, 70), 2))
            p.drawEllipse(center, radius, radius)
            p.setPen(QPen(CYAN, 3, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            box = QRectF(center.x() - radius, center.y() - radius, radius * 2, radius * 2)
            p.drawArc(box, int(-(now() * 320) % 360 * 16), 110 * 16)
            p.setPen(INK)
        elif self.state in ("done", "fail"):
            k = ease_out(age / 0.35)
            good = self.state == "done"
            fill = QLinearGradient(center.x() - radius, center.y() - radius, center.x() + radius, center.y() + radius)
            fill.setColorAt(0, CYAN if good else BAD)
            fill.setColorAt(1, GOOD if good else BAD.darker(130))
            p.setBrush(fill)
            p.drawEllipse(center, radius * (0.6 + 0.4 * k), radius * (0.6 + 0.4 * k))
            draw_mark(p, center, radius, good, ease_out((age - 0.15) / 0.35))
        else:
            p.setPen(QPen(tone(self, QColor(255, 255, 255, 46 if self.state == "wait" else 22)), 2))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawEllipse(center, radius, radius)
            p.setPen(tone(self, MUTED if self.state == "wait" else alpha(MUTED, 90)))
        if self.state in ("wait", "skip", "run") and not (SKIN and skin_icon("star")):
            p.setFont(font(13, 700, "heading"))
            p.drawText(QRectF(center.x() - radius, center.y() - radius, radius * 2, radius * 2),
                       Qt.AlignmentFlag.AlignCenter, str(self.number))
        title_color = {"run": INK, "done": mix(INK, CYAN, 0.25), "fail": mix(BAD, INK, 0.3),
                       "skip": alpha(MUTED, 110)}.get(self.state, MUTED)
        p.setPen(tone(self, title_color))
        title_font, detail_font = font(15, 600 if self.state == "run" else 500), font(SMALL_TEXT)
        title_metrics, detail_metrics = QFontMetrics(title_font), QFontMetrics(detail_font)
        text_width = self.width() - 54
        # The words in the middle of the icon's height: the title alone, or the title and the
        # detail under it as one block.
        block = title_metrics.ascent() + title_metrics.descent()
        if self.detail:
            block += 4 + detail_metrics.ascent() + detail_metrics.descent()
        top = center.y() - block / 2
        title_at = QPointF(50, top + title_metrics.ascent())
        p.setFont(title_font)
        title = title_metrics.elidedText(self.title, Qt.TextElideMode.ElideRight, text_width)
        sky_shadow(p, self, p.pen().color(), lambda d: p.drawText(title_at + d, title))
        p.drawText(title_at, title)
        if self.detail:
            p.setFont(detail_font)
            p.setPen(tone(self, alpha(CYAN, 210) if self.state == "run" else MUTED))
            detail_at = QPointF(50, title_at.y() + title_metrics.descent() + 4 + detail_metrics.ascent())
            detail = detail_metrics.elidedText(self.detail, Qt.TextElideMode.ElideRight, text_width)
            sky_shadow(p, self, p.pen().color(), lambda d: p.drawText(detail_at + d, detail))
            p.drawText(detail_at, detail)


class Chip(QWidget):
    """A small pill with a few words, its edge in the given colour. height and size: the plate's
    height and the words' size (the header's plate is larger than the main menu's)."""

    def __init__(self, text, color=CYAN, parent=None, icon=None, height=32, size=13, weight=500):
        super().__init__(parent)
        self.text, self.color, self.icon_name = text, color, icon
        self.setFont(font(size, weight))
        self.side = height
        self.setFixedHeight(height)

    def set_text(self, text, color=None):
        self.text = text
        self.color = color or self.color
        self.updateGeometry()
        self.update()

    def sizeHint(self):
        extra = self.side - 6 if self.icon_name and skin_icon(self.icon_name) else 0
        return QSize(QFontMetrics(self.font()).horizontalAdvance(self.text) + self.side + extra, self.side)

    def minimumSizeHint(self):
        return self.sizeHint()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        box = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        if SKIN is not None and SKIN.has("plate"):
            # On the game's plate (MiiSelect's page counter), in its text colour.
            p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            SKIN.window(p, QRectF(self.rect()), "plate")
            if self.icon_name and skin_icon(self.icon_name):
                SKIN.icon(p, QRectF(box.left() + 9, box.top() + 4, box.height() - 8, box.height() - 8), self.icon_name)
                box = box.adjusted(self.side - 6, 0, 0, 0)
            p.setPen(plate_text())
            p.setFont(self.font())
            p.drawText(box, Qt.AlignmentFlag.AlignCenter, self.text)
            return
        p.setBrush(QColor(255, 255, 255, 14))
        p.setPen(QPen(alpha(self.color, 80), 1))
        p.drawRoundedRect(box, box.height() / 2, box.height() / 2)
        if self.icon_name and skin_icon(self.icon_name):
            p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            SKIN.icon(p, QRectF(box.left() + 9, box.top() + 4, box.height() - 8, box.height() - 8), self.icon_name)
            box = box.adjusted(self.side - 6, 0, 0, 0)
        p.setPen(mix(INK, self.color, 0.25))
        p.setFont(self.font())
        p.drawText(box, Qt.AlignmentFlag.AlignCenter, self.text)


class Logo(QWidget):
    """The GalaxyCTR logo, floating gently up and down. It is redrawn 60 times a second while it
    shows, and drawn at its exact height (not rounded to whole pixels), so it glides."""

    BOB, SPEED = 4.0, 1.2  # pixels up and down, radians a second

    def __init__(self, parent=None):
        super().__init__(parent)
        path = Path(B.REPO) / "branding" / "title.png"
        self.source = QPixmap(str(path)) if path.is_file() else QPixmap()
        self.scaled = None
        self.setMinimumHeight(150)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        self.timer = builder_game.frame_timer(self)
        self.timer.timeout.connect(self.tick)

    def sizeHint(self):
        return QSize(620, 270)

    def tick(self):
        """The logo and the Power Star's halo behind it (HaloLayer) move together."""
        self.update()
        layer = getattr(self.window(), "halo_layer", None)
        if layer is not None:
            layer.update()

    def bob(self):
        return self.BOB * math.sin(sway() * self.SPEED)

    def resizeEvent(self, event):
        self.scaled = None

    def showEvent(self, event):
        super().showEvent(event)
        if not REDUCED:
            self.timer.start()

    def hideEvent(self, event):
        super().hideEvent(event)
        self.timer.stop()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        center = QPointF(self.width() / 2, self.height() / 2 + self.bob())
        if SKIN is None:
            # A round glow squashed into an ellipse, so it fades out on every side.
            p.save()
            p.translate(center)
            p.scale(1.0, self.height() * 0.55 / max(1.0, self.width() * 0.42))
            glow = QRadialGradient(QPointF(0, 0), self.width() * 0.42)
            glow.setColorAt(0, alpha(BLUE, 80 + 20 * math.sin(sway() * 1.7)))
            glow.setColorAt(0.6, alpha(BLUE, 25))
            glow.setColorAt(1, alpha(BLUE, 0))
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(glow)
            p.drawEllipse(QPointF(0, 0), self.width() * 0.42, self.width() * 0.42)
            p.restore()
        if self.source.isNull():
            p.setPen(tone(self, INK))
            p.setFont(font(64, 800, "heading", 4))
            p.drawText(QRectF(0, center.y() - 60, self.width(), 120), Qt.AlignmentFlag.AlignCenter, "GalaxyCTR")
            return
        if self.scaled is None or self.scaled.width() > self.width() * self.devicePixelRatioF():
            ratio = self.devicePixelRatioF()
            target = QSize(int(min(self.width(), self.height() * 2.25) * ratio), int(self.height() * ratio))
            self.scaled = self.source.scaled(target, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)
            self.scaled.setDevicePixelRatio(ratio)
        size = self.scaled.deviceIndependentSize()
        # Drawn at a fractional height and a hair larger than the pixmap: the painter then
        # filters it (smooth transforms are on) instead of taking its fast path, which rounds
        # the position to whole pixels and makes the logo move in jumps.
        w, h = size.width() + 0.25, size.height() + 0.25
        p.drawPixmap(QRectF(center.x() - w / 2, center.y() - h / 2, w, h), self.scaled, QRectF(self.scaled.rect()))


class Badge(QWidget):
    """The mark of the last page: the game's Power Star on a finished build, the camera's red
    "no" sign (CameraInfo's Forbid, with its shadow) on a stopped one, each with its sound when
    the page shows."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.good, self.since = True, now()
        # Smaller than it was (170 x 150, the star 130 pixels): the install guide below gets the
        # height, to show more of its steps in a small window (owner, 26 Nov 2026).
        self.setFixedSize(130, 106)

    def show_result(self, good):
        """good: the Power Star with SE_SY_STAR_GET (as PowerStar plays when it is collected);
        otherwise the red "no" sign with SE_SY_FILE_SEL_NG. Both start when the page shows."""
        self.good, self.since = good, now()
        self.sound = "star_get" if good else "failure"

    sound = None
    def showEvent(self, event):
        super().showEvent(event)
        self.since = now()
        if SKIN is not None and self.sound:
            SKIN.play(self.sound)
        self.sound = None

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        center, radius = QPointF(self.width() / 2, self.height() / 2), 38.0
        age = 10.0 if REDUCED else now() - self.since
        grow = ease_out(age / 0.5)
        if self.good and skin_icon("star"):  # the pause menu's collected star, as it is
            size = 2 * radius * 1.35
            SKIN.icon(p, QRectF(center.x() - size / 2, center.y() - size / 2, size, size), "star")
            return
        if not self.good and skin_icon("forbid"):  # the camera's "no" sign, as it is
            size = 2 * radius * 1.1
            SKIN.icon(p, QRectF(center.x() - size / 2, center.y() - size / 2, size, size), "forbid")
            return
        fill = QLinearGradient(center.x() - radius, center.y() - radius, center.x() + radius, center.y() + radius)
        fill.setColorAt(0, CYAN if self.good else BAD)
        fill.setColorAt(1, GOOD if self.good else BAD.darker(130))
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(fill)
        p.drawEllipse(center, radius * grow, radius * grow)
        draw_mark(p, center, radius, self.good, ease_out((age - 0.35) / 0.5))


class Toast(QWidget):
    """A short message at the bottom of the window. It comes up from below the window's edge,
    fading in, and goes back down the same way (as MotionNotice comes down from the top), on
    the clock. Given two buttons (between), it stays in the room between them, its words on more
    lines when they need it; otherwise it is centred above the window's bottom edge."""

    SLIDE_IN, SLIDE_OUT, HOLD = 450, 300, 3200  # milliseconds
    GAP = 14  # pixels kept clear on each side of the room between the buttons

    def __init__(self, parent):
        super().__init__(parent)
        self.text, self.color = "", CYAN
        self.between = None
        self.pending = None  # words waiting for the toast that is up to go down
        self.rest = QRect()
        self.shown = False
        self.offset = 0.0  # 0 below the window's edge and clear, 1 in place
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setFont(font(13, 600))
        self.slide = builder_game.ClockAnimation(self)
        self.slide.setEasingCurve(QEasingCurve.Type.OutCubic)
        self.slide.valueChanged.connect(self._slid)
        self.slide.finished.connect(self._slide_over)
        self.hold = QTimer(self)
        self.hold.setSingleShot(True)
        self.hold.timeout.connect(self.slide_out)
        self.hide()

    def show_text(self, text, color=CYAN, between=None):
        """The same words while they are up: they stay, and the time they stay starts again.
        Other words: the toast that is up goes down first, then the new one comes up."""
        text = builder_game.sentence_lines(text)  # a new line after each sentence
        different = (text, color) != (self.text, self.color)
        if different and not REDUCED and self.isVisible() and self.offset > 0.0:
            self.pending = (text, color, between)
            self.hold.stop()
            self.slide_out()  # _slide_over brings the new words up
            return
        self.pending = None
        self.text, self.color, self.between = text, color, between
        self.measure()
        self.show()
        self.raise_()
        self.hold.start(self.HOLD)
        if REDUCED:  # the system asks for less motion: it is just there
            self.shown, self.offset = True, 1.0
            self.place()
            self.update()
            return
        if not self.shown:
            self.shown = True
            self._slide_to(1.0, self.SLIDE_IN)
        self.place()
        self.update()

    def slide_out(self):
        if not self.shown:
            return
        self.shown = False
        if REDUCED:
            self.offset = 0.0
            self.hide()
            return
        self._slide_to(0.0, self.SLIDE_OUT)

    def _slide_to(self, value, duration):
        self.slide.stop()
        self.slide.setDuration(duration)
        self.slide.setStartValue(self.offset)
        self.slide.setEndValue(value)
        self.slide.start()

    def _slid(self, value):
        self.offset = float(value)
        self.place()
        self.update()

    def _slide_over(self):
        if not self.shown:
            self.hide()
            if self.pending:
                text, color, between = self.pending
                self.pending = None
                self.show_text(text, color, between)

    def measure(self):
        """Where the toast rests (self.rest), from its words and the room it has."""
        parent = self.parentWidget()
        metrics = QFontMetrics(self.font())
        base = 64 if SKIN is not None else 48
        pad = 40
        if self.between is not None and all(w.isVisible() for w in self.between):
            left, right = self.between
            room = left.side_room() if isinstance(left, Button) else 0
            x0 = left.mapTo(parent, QPoint(left.width(), 0)).x() - room + self.GAP
            room = right.side_room() if isinstance(right, Button) else 0
            x1 = right.mapTo(parent, QPoint(0, 0)).x() + room - self.GAP
            centre_y = left.mapTo(parent, QPoint(0, left.height() // 2)).y()
            room_w = max(160, x1 - x0)
            centre_x = (x0 + x1) / 2
        else:
            room_w = parent.width() - 80
            centre_x = parent.width() / 2
            centre_y = parent.height() - 40 - base / 2
        # A line per sentence, never broken inside one (owner, 25 Nov 2026): as wide as the
        # longest, as tall as their number.
        lines = self.text.split("\n")
        width = max(metrics.horizontalAdvance(line) for line in lines) + 2 * pad
        height = max(base, metrics.lineSpacing() * len(lines) + base - metrics.height())
        self.rest = QRect(int(centre_x - width / 2), int(centre_y - height / 2), int(width), int(height))
        self.place()

    def text_margins(self):
        return 26

    def place(self):
        """At the rest place, or that far below it as the slide says (the window's edge first)."""
        parent = self.parentWidget()
        below = parent.height() - self.rest.y()
        y = self.rest.y() + below * (1.0 - self.offset)
        self.setGeometry(self.rest.x(), int(y), self.rest.width(), self.rest.height())

    def paintEvent(self, event):
        p = QPainter(self)
        p.setOpacity(max(0.0, min(1.0, self.offset)))
        flags = int(Qt.AlignmentFlag.AlignCenter)  # lines only after a sentence (measure)
        margin = self.text_margins()
        if SKIN is not None and SKIN.has("window"):
            # The game's system window with its words.
            p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            SKIN.window(p, QRectF(self.rect()))
            p.setPen(QColor(SKIN.window_colours()[0]))
            p.setFont(self.font())
            p.drawText(QRectF(self.rect()).adjusted(margin, 0, -margin - 6, -6), flags, self.text)
            return
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        box = QRectF(self.rect()).adjusted(2, 2, -2, -2)
        p.setBrush(QColor(12, 20, 54, 235))
        p.setPen(QPen(alpha(self.color, 200), 1.3))
        radius = min(box.height() / 2, 24)
        p.drawRoundedRect(box, radius, radius)
        p.setPen(INK)
        p.setFont(self.font())
        p.drawText(box.adjusted(margin, 0, -margin, 0), flags, self.text)


def transparent_scroll(content):
    area = QScrollArea()
    area.setWidgetResizable(True)
    area.setFrameShape(QScrollArea.Shape.NoFrame)
    area.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
    area.setWidget(content)
    area.viewport().setAutoFillBackground(False)
    content.setAutoFillBackground(False)
    EdgeFade(area.viewport())
    if content.layout() is not None:  # room for the fade, so nothing is faded at the list's two ends
        m = content.layout().contentsMargins()
        content.layout().setContentsMargins(m.left(), m.top() + EdgeFade.FADE, m.right(), m.bottom() + EdgeFade.FADE)
    return area


class EdgeFade(QObject):
    """A fade of fixed height at the top and bottom of a scrolling list (owner, 25 Nov 2026): what
    scrolls past an edge fades out instead of being cut. An opacity mask on the viewport, from
    clear at the edge to full at FADE pixels in, measured again whenever the viewport resizes."""

    FADE = 28

    def __init__(self, viewport):
        super().__init__(viewport)
        self.viewport = viewport
        self.effect = QGraphicsOpacityEffect(viewport)
        self.effect.setOpacity(1.0)
        viewport.setGraphicsEffect(self.effect)
        viewport.installEventFilter(self)
        self.update_mask()

    def eventFilter(self, watched, event):
        if event.type() == QEvent.Type.Resize:
            self.update_mask()
        return False

    def update_mask(self):
        height = max(1, self.viewport.height())
        edge = min(0.5, self.FADE / height)
        mask = QLinearGradient(0, 0, 0, height)
        # Eased (the square of the way in), not straight: near the edge almost nothing shows, so a
        # panel's dark shadow passing there no longer stood out as a thin black line (owner, 26
        # Nov 2026); the same 28 pixels to full.
        steps = 8
        for step in range(steps + 1):
            t = step / steps
            opacity = round(255 * t * t)
            mask.setColorAt(edge * t, QColor(0, 0, 0, opacity))
            mask.setColorAt(1.0 - edge * t, QColor(0, 0, 0, opacity))
        self.effect.setOpacityMask(mask)


# ##############################################################################################
# # The system's own programs                                                                  #
# #                                                                                            #
# # The Linux program runs with its own libraries first (LD_LIBRARY_PATH, PyInstaller), and   #
# # the system's programs then fail to load theirs: xdg-open opened nothing ("file ''"), so the #
# # result page's Open the log and Open the files folder did nothing (owner, 26 Nov 2026).    #
# # They run with the system's own environment (galaxyctr_builder.program_environment).        #
# ##############################################################################################
def system_environment():
    """The environment for the system's own programs: the desktop's, without this program's
    libraries. None (this process's) when not the frozen Linux or macOS program."""
    if not getattr(sys, "frozen", False) or os.name == "nt" or B is None:
        return None
    return B.program_environment()


def open_path(path):
    """Opens a file or folder in the system's program for it."""
    if not path:
        return
    if sys.platform.startswith("linux") and getattr(sys, "frozen", False):
        import subprocess
        try:
            subprocess.Popen(["xdg-open", str(path)], env=system_environment(), stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            return
        except OSError as error:
            B.log(f"Could not open {path} ({error!r}).")
    QDesktopServices.openUrl(QUrl.fromLocalFile(str(path)))


# ##############################################################################################
# # Talking to the build thread                                                                #
# #                                                                                            #
# # GuiUI is the build's UI (galaxyctr_builder.UI) while the window runs it. It is called from #
# # the build thread, so it only emits signals; Qt delivers them in the window's thread. A     #
# # question is a Request: the window opens its dialog, and the build thread waits until the   #
# # answer is set (or the user stops the build, which the builder reports as "Stopped.").      #
# ##############################################################################################
class Bridge(QObject):
    said = Signal(str)
    stepped = Signal(int, int, str)
    progressed = Signal(str, float, str)
    asked = Signal(object)
    finished = Signal(int)


class Request:
    def __init__(self, kind, question, default=None, **extra):
        self.kind, self.question, self.default, self.extra = kind, question, default, extra
        self.problem = None
        self.answer = None
        self.cancelled = False
        self.answered = threading.Event()

    def reply(self, answer):
        self.answer = answer
        self.answered.set()

    def cancel(self):
        self.cancelled = True
        self.answered.set()


class GuiUI:
    def __init__(self, bridge):
        self.bridge = bridge
        self.pending_problem = None
        self.last_progress = 0.0

    def say(self, text=""):
        self.bridge.said.emit(text)

    def problem(self, text):
        self.pending_problem = text.strip()
        self.say(text)

    def step(self, number, total, title):
        self.bridge.stepped.emit(number, total, title)

    def progress(self, label_text, percent=-1.0, detail=""):
        moment = now()
        if moment - self.last_progress < 0.05 and 0 <= percent < 100:
            return
        self.last_progress = moment
        self.bridge.progressed.emit(label_text, float(percent), detail)

    def end_progress(self):
        pass

    def _ask(self, kind, question, default=None, **extra):
        request = Request(kind, question, default, **extra)
        request.problem, self.pending_problem = self.pending_problem, None
        self.bridge.asked.emit(request)
        request.answered.wait()
        if request.cancelled:
            raise KeyboardInterrupt
        return request.answer

    def ask(self, question, kind="text", default=None):
        return self._ask(kind, question, default)

    def ask_yes(self, question):
        return self._ask("yes", question)

    def ask_ftp(self, question, last, connections):
        return self._ask("ftp", question, last, connections=connections)


class BuildProcess:
    """A build (job "build") or the first start's setup ("setup") in a process of its own
    (galaxyctr_builder.worker), so its Python work never holds the window's drawing, sky and
    music back. A thread here reads the worker's messages and hands them to GuiUI as the build
    thread did; a question's answer (or the stop) goes back through the pipe. At the end the
    worker's RESULT becomes the builder's, and the bridge says finished."""

    def __init__(self, bridge, job):
        import multiprocessing
        self.bridge = bridge
        self.ui = GuiUI(bridge)
        context = multiprocessing.get_context("spawn")
        self.conn, child = context.Pipe()
        if B.LOG:
            B.LOG.flush()
        self.process = context.Process(target=B.worker, args=(child, job), name=f"galaxyctr-{job}")
        self.process.start()
        child.close()
        threading.Thread(target=self.listen, daemon=True).start()

    def listen(self):
        code = 1
        try:
            while True:
                message = self.conn.recv()
                kind = message[0]
                if kind == "say":
                    self.ui.say(message[1])
                elif kind == "problem":
                    self.ui.problem(message[1])
                elif kind == "step":
                    self.ui.step(*message[1:])
                elif kind == "progress":
                    self.ui.progress(*message[1:])
                elif kind == "ask":
                    _kind, question_kind, question, default, extra = message
                    try:
                        reply = ("answer", self.ui._ask(question_kind, question, default, **extra))
                    except KeyboardInterrupt:
                        reply = ("cancel",)
                    self.conn.send(reply)
                elif kind == "finished":
                    code = message[1]
                    B.RESULT.clear()
                    B.RESULT.update(message[2])
                    break
        except (EOFError, OSError) as error:
            B.log(f"The build's process ended without its result: {error!r}")
            self.ui.say("ERROR: the build stopped unexpectedly. The details are in the log.")
        finally:
            self.process.join(5)
            self.bridge.finished.emit(code)

    def kill(self):
        """Ends the worker and the programs it started (the window closes during the build)."""
        if not self.process.is_alive():
            return
        try:
            if os.name == "nt":
                import subprocess
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(self.process.pid)], capture_output=True,
                               creationflags=B.NO_WINDOW, timeout=10)
            else:
                self.process.kill()
        except Exception as error:  # noqa: BLE001 - the window closes anyway
            B.log(f"Could not end the build's process: {error!r}")


# ##############################################################################################
# # Dialogs                                                                                   #
# #                                                                                            #
# # The Overlay dims the window and holds the dialog cards, newest on top. QuestionCard is     #
# # one question of the build, with the picker its kind asks for; it replies to the Request.  #
# ##############################################################################################
class PageChange(QObject):
    """One page change: OUT seconds of the old page fading out, IN of the new one fading in."""

    OUT, IN = 0.22, 0.30

    def __init__(self, window, old, page):
        super().__init__(window)
        self.window, self.old, self.page = window, old, page
        self.switched = False
        self.old_effect = QGraphicsOpacityEffect(old)
        self.old_effect.setOpacity(1.0)  # an effect starts at 0.7: the page would dip at once
        old.setGraphicsEffect(self.old_effect)
        old.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)  # no clicks while it goes
        self.animation = builder_game.ClockAnimation(self)
        self.animation.setDuration(int((self.OUT + self.IN) * 1000))
        self.animation.setStartValue(0.0)
        self.animation.setEndValue(1.0)
        self.animation.valueChanged.connect(self._step)
        self.animation.finished.connect(self.finish)
        self.animation.start()

    def _switch(self):
        self.switched = True
        self.old.setGraphicsEffect(None)
        self.old.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, False)
        self.new_effect = QGraphicsOpacityEffect(self.page)
        self.new_effect.setOpacity(0.0)
        self.page.setGraphicsEffect(self.new_effect)
        self.window.stack.setCurrentWidget(self.page)
        self.window.title_page_shown(self.page)
        self.window.update_sky()

    def _step(self, value):
        split = self.OUT / (self.OUT + self.IN)
        k = float(value)
        if k < split:
            self.old_effect.setOpacity(1.0 - k / split)  # the effect is gone once switched
            return
        if not self.switched:
            self._switch()
        x = (k - split) / (1.0 - split)
        self.new_effect.setOpacity(x * x * (3 - 2 * x))  # smoothstep

    def finish(self):
        """Ends now (the next change starts, or the animation is over)."""
        if self.animation.state() == QAbstractAnimation.State.Running:
            self.animation.stop()
        if not self.switched:
            self._switch()
        self.page.setGraphicsEffect(None)
        self.window.page_changed(self)
        self.deleteLater()


class Entrance(QObject):
    """The main menu's first showing, after the title screen: each part fades in while it comes
    to its place from its own side, one shortly after the other. parts: [(widget, (dx, dy))],
    the direction it comes from (-1, 0 from the left, 0, -1 from above...). Later showings of
    the menu use the plain page fade (PageChange).

    reverse: the same entrance played backwards, for going back to the title screen
    (Window.back_to_title): the parts fade out while they go back to their sides, the last to
    come in the first to leave; then is called once it is over."""

    DURATION, STAGGER, DISTANCE = 0.75, 0.07, 90.0  # seconds, seconds between parts, pixels

    def __init__(self, window, parts, reverse=False, then=None):
        super().__init__(window)
        self.window = window
        self.reverse, self.then = reverse, then
        self.parts = []
        for index, (widget, (dx, dy)) in enumerate(parts):
            if widget is None or not widget.isVisible():
                continue
            effect = QGraphicsOpacityEffect(widget)
            effect.setOpacity(0.0)
            widget.setGraphicsEffect(effect)
            self.parts.append((widget, widget.pos(), dx, dy, index * self.STAGGER, effect))
        total = self.DURATION + self.STAGGER * max(0, len(parts) - 1)
        self.total = total
        self.animation = builder_game.ClockAnimation(self)
        self.animation.setDuration(int(total * 1000))
        self.animation.setStartValue(0.0)
        self.animation.setEndValue(1.0)
        self.animation.valueChanged.connect(self._step)
        self.animation.finished.connect(self.finish)
        self._step(0.0)
        self.animation.start()

    def opacity(self, widget):
        """How far widget has come in (1 when it is not part of the entrance)."""
        for part in self.parts:
            if part[0] is widget:
                return part[5].opacity()
        return 1.0

    def _step(self, value):
        t = (1.0 - float(value) if self.reverse else float(value)) * self.total
        for widget, base, dx, dy, delay, effect in self.parts:
            x = min(1.0, max(0.0, (t - delay) / self.DURATION))
            eased = 1.0 - (1.0 - x) ** 3  # out cubic: fast, then settling
            effect.setOpacity(min(1.0, x * 1.6))
            left = (1.0 - eased) * self.DISTANCE
            widget.move(int(base.x() + dx * left), int(base.y() + dy * left))
        layer = getattr(self.window, "halo_layer", None)
        if layer is not None:
            layer.update()

    def finish(self):
        """Puts every part in its place at once (the animation ends, or something interrupts)."""
        if self.animation.state() == QAbstractAnimation.State.Running:
            self.animation.stop()
        if self.window.entrance is self:
            self.window.entrance = None
        then, self.then = self.then, None
        if then is not None:
            then()  # backwards: the next screen first, so the parts are not seen coming back
        for widget, base, _dx, _dy, _delay, _effect in self.parts:
            widget.setGraphicsEffect(None)
            widget.move(base)
        self.parts = []
        self.deleteLater()


class Overlay(QWidget):
    """The screen's dimming and the window (card) on it. Both fade in when a card opens and fade
    out when the last one closes, as the pages do."""

    def __init__(self, parent):
        super().__init__(parent)
        self.cards = []
        self.leaving = None  # the last card while it fades out
        self.fade = 0.0
        self.animation = builder_game.ClockAnimation(self)
        self.animation.setDuration(260)
        self.animation.setEasingCurve(QEasingCurve.Type.OutCubic)
        self.animation.valueChanged.connect(self._faded)
        self.animation.finished.connect(self._fade_over)
        self.hide()

    def _faded(self, value):
        self.fade = float(value)
        card = self.leaving or (self.cards[-1] if self.cards else None)
        if card is not None and isinstance(card.graphicsEffect(), QGraphicsOpacityEffect):
            card.graphicsEffect().setOpacity(self.fade)
        self.place()
        self.update()

    def _fade_over(self):
        if self.leaving is not None:
            self._gone()
        elif self.cards:
            self.cards[-1].setGraphicsEffect(None)  # drawn straight again once in

    def _gone(self):
        """The faded-out card goes, and the dimming with it."""
        card, self.leaving = self.leaving, None
        card.hide()
        card.deleteLater()
        if not self.cards:
            self.hide()

    def park_focus(self):
        """Hiding a widget that holds the focus makes Qt hand it to the next one with Tab's
        reason (QWidget::focusNextPrevChild), and a Button takes that for the keyboard: a button
        of the page behind would light up and point as if chosen. The window takes it instead,
        as after a click."""
        self.parentWidget().setFocus(Qt.FocusReason.OtherFocusReason)

    def _fade(self, start, end):
        self.animation.stop()
        self.animation.setStartValue(start)
        self.animation.setEndValue(end)
        self.animation.start()

    def open(self, card):
        if self.leaving is not None:  # a new question right after an answer: the old card goes now
            self.animation.stop()
            self._gone()
        if self.cards:
            self.park_focus()
            self.cards[-1].hide()
        self.cards.append(card)
        card.setParent(self)
        self.setGeometry(self.parentWidget().rect())
        self.show()
        self.raise_()
        effect = QGraphicsOpacityEffect(card)
        effect.setOpacity(self.fade)
        card.setGraphicsEffect(effect)
        card.show()
        card.adjustSize()
        self._fade(self.fade, 1.0)
        card.focus_first()

    def close_card(self, card):
        if card in self.cards:
            self.cards.remove(card)
        self.park_focus()
        if self.cards:
            card.hide()
            card.deleteLater()
            self.cards[-1].show()
            self.place()
            return
        # The last card fades out with the dimming; it takes no more clicks meanwhile.
        card.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        effect = QGraphicsOpacityEffect(card)
        effect.setOpacity(self.fade)
        card.setGraphicsEffect(effect)
        self.leaving = card
        if REDUCED:
            self._gone()
            return
        self._fade(self.fade, 0.0)

    def place(self):
        card = self.leaving or (self.cards[-1] if self.cards else None)
        if card is None:
            return
        card.adjustSize()
        hint = card.sizeHint()  # within the card's own limits, so it is centred as it is drawn
        width = min(max(hint.width(), card.minimumWidth()), card.maximumWidth(), self.width() - 40)
        wanted = card.heightForWidth(width) if card.hasHeightForWidth() else -1  # wrapped text
        height = min(max(hint.height(), wanted, card.minimumHeight()), card.maximumHeight(), self.height() - 40)
        card.setGeometry((self.width() - width) // 2, int((self.height() - height) // 2 + 24 * (1 - self.fade)), width, height)

    def resizeEvent(self, event):
        self.place()

    def mousePressEvent(self, event):
        event.accept()

    def paintEvent(self, event):
        p = QPainter(self)
        if SKIN and SKIN.has("window"):  # the system window's own dimming (its BG picture)
            p.setOpacity(self.fade)
            SKIN.backdrop(p, QRectF(self.rect()))
            return
        p.fillRect(self.rect(), QColor(2, 4, 14, int(165 * self.fade)))


DISC_FILTER = "Wii disc images (*.iso *.wbfs *.rvz *.wia *.ciso *.gcz *.nfs *.wdf *.wad);;All files (*)"

# Words for some of the builder's questions, shown under them in the dialogs.
EXPLAINED = {
    "Keep the game files after each build?":
        "About 9 GB stay on this computer. The next builds then convert only what changed, without the "
        "disc. So they are much faster. You can change this in Settings.",
    "Copy the files to your 3DS now?":
        "Choose Later to copy them yourself. The steps are shown at the end and saved in install_guide.txt.",
}


def clean_question(text):
    text = re.sub(r"\s*\((Enter for|for example)[^)]*\)", "", text.strip())
    return text.rstrip(" :")


class QuestionCard(Glass):
    def __init__(self, request, window):
        super().__init__(radius=26, tint=150)
        self.request, self.main = request, window
        self.edit = self.slider = None
        self.setAcceptDrops(request.kind == "disc")
        self.setMinimumWidth(560)  # no maximum: as wide as its longest sentence (Text.setWordWrap)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(34, 30, 34, 28)
        layout.setSpacing(14)
        kind = request.kind
        ftp_choice = kind == "yes" and request.question.startswith("Copy over Wi-Fi with FTP")
        caption = {"yes": "Question", "disc": "Your game disc", "folder": "Kept game files", "sd": "SD card",
                   "ftp": "Copy over Wi-Fi", "text": "Question"}[kind if kind in ("yes", "disc", "folder", "sd", "ftp") else "text"]
        head = QHBoxLayout()
        head.setSpacing(10)
        head.addWidget(IconLabel({"disc": "star", "folder": "list", "sd": "save", "ftp": "comet"}.get(kind, "question"), 30))
        head.addWidget(label(caption, 13, 600, CYAN))
        head.addStretch(1)
        layout.addLayout(head)
        title = "How do you want to copy the files?" if ftp_choice else clean_question(request.question)
        if kind == "disc":
            title = "Choose your Super Mario Galaxy disc image"
        elif kind == "sd" and os.name != "nt":
            title = "Folder of the SD card"  # no drive letters here: the terminal's question asks for a number
        layout.addWidget(label(title, 22, 600, INK, "heading", wrap=True))
        explanation = EXPLAINED.get(request.question)
        if ftp_choice:
            explanation = ("An FTP app on the 3DS receives the files over Wi-Fi, no card needed. "
                           "Or put the 3DS's SD card into this computer.")
        elif kind == "disc":
            explanation = ("The USA release (RMGE01), as ISO, WBFS, RVZ, WIA, CISO, GCZ or NFS. "
                           "You can also drop the file on this window.")
        elif kind == "folder":
            explanation = "About 9 GB are needed there. The drive that holds the disc image is a good choice."
        elif kind == "ftp":
            explanation = ("Start ftpd or another FTP app on the 3DS, on the same Wi-Fi as this computer. "
                           "It shows the address. More connections copy the many small files faster. "
                           "Lower the number if the app refuses them.")
        elif kind == "sd":
            explanation = "Insert the 3DS's SD card into this computer, then choose it. Your saves on it are kept."
        if explanation:
            layout.addWidget(label(explanation, 14, 400, MUTED, wrap=True))
        if request.problem:
            warning = label(request.problem, 13, 600, mix(BAD, INK, 0.25), wrap=True)
            layout.addWidget(warning)

        if kind in ("disc", "folder", "text", "sd", "ftp"):
            self.add_inputs(layout, kind)

        stop = Button("Stop the build", "danger")
        stop.clicked.connect(self.stop)
        layout.addSpacing(6)
        if kind == "yes":
            # The answers on their own row (the game's yes/no: no left, yes right), and
            # stopping under them: three game buttons do not fit in one row of a card.
            no_text, yes_text = ("SD card", "Wi-Fi (FTP)") if ftp_choice else \
                ("Later", "Copy now") if request.question.startswith("Copy the files") else ("No", "Yes")
            no = Button(no_text, "no")
            no.clicked.connect(lambda: self.reply(False))
            yes = Button(yes_text, "yes", icon="a_button")
            yes.clicked.connect(lambda: self.reply(True))
            layout.addLayout(button_row([no, yes], "center"))
            layout.addLayout(button_row([stop], "center"))
            self.first = yes
        else:
            go = Button("Continue", "yes", icon="a_button")
            go.clicked.connect(self.submit)
            row = button_row([stop, go], "right")
            row.insertStretch(1, 1)  # stop on the left, continue on the right
            layout.addLayout(row)
            self.first = self.edit or go
            if self.edit is not None:
                self.edit.returnPressed.connect(self.submit)

    def add_inputs(self, layout, kind):
        request = self.request
        row = QHBoxLayout()
        row.setSpacing(10)
        self.edit = GameLineEdit()
        self.edit.setMinimumHeight(46)
        default = request.default or ""
        if kind == "disc":
            self.edit.setPlaceholderText("Disc image file")
        elif kind == "ftp":
            self.edit.setPlaceholderText("192.168.1.10:5000")
        elif kind == "sd" and os.name == "nt":
            self.edit.setPlaceholderText("Drive letter, for example E")
            self.edit.setMaxLength(3)
        elif kind == "sd":
            self.edit.setPlaceholderText("Folder of the SD card")
        self.edit.setText(str(default))
        row.addWidget(self.edit, 1)
        if kind in ("disc", "folder") or (kind == "sd" and os.name != "nt"):
            browse = IconButton("hand", "Choose a file" if kind == "disc" else "Choose a folder")
            browse.clicked.connect(lambda: self.browse(kind))
            row.addWidget(browse)
        if kind == "sd":
            # A drive's button answers at once; the field below is for typing one (owner, 25 Nov 2026).
            drives = self.drive_list()
            if drives is not None:
                layout.addWidget(label("Select a drive...", 14, 600, INK))
                layout.addWidget(drives)
                layout.addWidget(label("...or type the letter manually." if os.name == "nt" else "...or choose its folder.",
                                       14, 600, INK))
        layout.addLayout(row)
        if kind == "ftp":
            count = int(request.extra.get("connections") or B.FTP_DEFAULT_CONNECTIONS)
            line = QHBoxLayout()
            line.addWidget(label("Connections at once", 14, 600, INK))
            self.slider = make_slider()
            self.slider.setRange(1, B.FTP_MAX_CONNECTIONS)
            self.slider.setValue(count)
            self.slider.setMinimumHeight(30)
            shown = label(str(count), 18, 700, CYAN, "heading")
            shown.setMinimumWidth(30)
            self.slider.valueChanged.connect(lambda value: shown.setText(str(value)))
            line.addWidget(self.slider, 1)
            line.addWidget(shown)
            shown.setVisible(isinstance(self.slider, QSlider))  # the game's counter shows it itself
            layout.addLayout(line)

    def drive_list(self):
        """The drives to choose the SD card from: removable ones first, with their names."""
        holder = QWidget()
        rows = QVBoxLayout(holder)
        rows.setContentsMargins(0, 0, 0, 0)
        rows.setSpacing(4)
        try:
            if os.name == "nt":
                drives = sorted(B.windows_drives(), key=lambda d: (not d[1], d[0]))
                choices = [(letter, f"{letter}:  {volume_name(letter) or ('Removable' if removable else 'Drive')}")
                           for letter, removable in drives]
            else:
                choices = [(str(path), path.name or str(path)) for path in B.mounted_drives()]
        except Exception:  # noqa: BLE001 - the field below still works
            choices = []
        # Two drives a row, as button_row places them: a game button keeps its own size, and three
        # of them are wider than the card (they overlapped, owner's screenshot, 25 Nov 2026).
        buttons = []
        for value, text in choices[:6]:
            button = Button(text)
            button.setMinimumHeight(46)
            button.clicked.connect(lambda checked=False, v=value: self.choose_drive(v))
            buttons.append(button)
        for start in range(0, len(buttons), 2):
            rows.addLayout(button_row(buttons[start:start + 2], "left"))
        if not choices:
            return None  # only the field: no "Select a drive" over nothing
        return holder

    def showEvent(self, event):
        super().showEvent(event)
        if self.request.kind == "disc":
            follow_disc_icon()  # Linux: no star while the build asks for the disc image

    def choose_drive(self, value):
        """A drive's button: the copy starts on it, as Continue with its letter typed."""
        self.edit.setText(value)
        self.submit()

    def browse(self, kind):
        start = self.edit.text().strip() or str(Path.home())
        if kind == "disc":
            path, _ = QFileDialog.getOpenFileName(None, "Choose the disc image", str(Path(start).parent if Path(start).is_file() else start), DISC_FILTER)
        else:
            path = QFileDialog.getExistingDirectory(None, "Choose a folder", start)
        if path:
            self.edit.setText(str(Path(path)))

    def dragEnterEvent(self, event):
        if event.mimeData().hasUrls():
            event.acceptProposedAction()

    def dropEvent(self, event):
        urls = [url.toLocalFile() for url in event.mimeData().urls() if url.isLocalFile()]
        if urls and self.edit is not None:
            self.edit.setText(urls[0])
            self.submit()

    def focus_first(self):
        QTimer.singleShot(50, self.first.setFocus)

    def reply(self, answer):
        self.main.asking = max(0, self.main.asking - 1)
        self.main.answered_at = now()
        self.request.reply(answer)
        self.main.overlay.close_card(self)

    def submit(self):
        text = self.edit.text().strip() if self.edit is not None else ""
        if self.request.kind == "disc":
            # The build checks the image and keeps it in settings.txt: the star then, at once.
            for wait in (1500, 4000, 10000):
                QTimer.singleShot(wait, follow_disc_icon)
        if self.request.kind == "ftp":
            self.reply((text, str(self.slider.value())))
        else:
            self.reply(text)

    def stop(self):
        self.main.asking = max(0, self.main.asking - 1)
        self.main.answered_at = now()
        self.request.cancel()
        self.main.overlay.close_card(self)


def volume_name(letter):
    """A Windows drive's name, as File Explorer shows it."""
    try:
        import ctypes
        name = ctypes.create_unicode_buffer(261)
        if ctypes.windll.kernel32.GetVolumeInformationW(f"{letter}:\\", name, 261, None, None, None, None, 0):
            return name.value
    except Exception:  # noqa: BLE001 - only a label
        pass
    return ""


class ConfirmCard(Glass):
    """A yes/no question of the window itself (closing during a build)."""

    def __init__(self, window, title, text, yes_text, on_yes):
        super().__init__(radius=26, tint=160)
        self.main = window
        self.setMinimumWidth(520)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(34, 30, 34, 28)
        layout.setSpacing(14)
        layout.addWidget(label("Careful", 13, 600, BAD))
        layout.addWidget(label(title, 22, 600, INK, "heading", wrap=True))
        layout.addWidget(label(text, 14, 400, MUTED, wrap=True))
        no = Button("Keep going", "no")
        no.clicked.connect(lambda: self.main.overlay.close_card(self))
        yes = Button(yes_text, "yes")
        yes.clicked.connect(on_yes)
        layout.addSpacing(6)
        layout.addLayout(button_row([no, yes], "center"))  # the game's yes/no: no left, yes right
        self.first = no

    def focus_first(self):
        QTimer.singleShot(50, self.first.setFocus)


class SetupCard(Glass):
    """The first start's progress: reading the disc and making the game's graphics."""

    def __init__(self, window):
        super().__init__(radius=26, tint=160)
        self.main = window
        self.errors = []
        self.setMinimumWidth(560)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(34, 30, 34, 30)
        layout.setSpacing(14)
        head = QHBoxLayout()
        head.addWidget(label("Getting ready", 13, 600, CYAN))
        head.addStretch(1)
        layout.addLayout(head)
        self.title = label("Reading your disc", 22, 600, INK, "heading", wrap=True)
        layout.addWidget(self.title)
        self.text = label("About a minute. Its buttons, windows and sky stay on this computer only.",
                          14, 400, MUTED, wrap=True)
        layout.addWidget(self.text)
        self.bar = Progress()
        self.bar.set_value(-1)
        layout.addWidget(self.bar)
        self.detail = label("", 13, 400, MUTED, wrap=True)
        layout.addWidget(self.detail)

    def on_said(self, text):
        line = text.strip()
        if line.startswith("ERROR:"):
            self.errors.append(line[6:].strip())
        if line and not line.startswith("Log file"):
            self.detail.setText(line)

    def on_progress(self, name, percent, detail):
        self.bar.set_value(percent)
        self.title.setText(name)
        self.detail.setText(f"{percent:.0f}%" if percent >= 0 else "")

    def focus_first(self):
        pass


class GameCursorFilter(QObject):
    """The game's pointer only inside the windows' own area. An application-wide cursor also
    covered the title bar and the borders, where Windows shows its arrows for resizing (owner,
    25 Nov 2026): it is set when the pointer enters a window's area and taken off when it leaves
    it (Leave of the window, or a move over the title bar or the borders)."""

    def __init__(self, cursor):
        super().__init__()
        self.cursor, self.on = cursor, False

    def put_on(self):
        if not self.on:
            QApplication.setOverrideCursor(self.cursor)
            self.on = True

    def take_off(self):
        if self.on:
            QApplication.restoreOverrideCursor()
            self.on = False

    def eventFilter(self, watched, event):
        kind = event.type()
        if kind in (QEvent.Type.NonClientAreaMouseMove, QEvent.Type.NonClientAreaMouseButtonPress):
            self.take_off()
        elif isinstance(watched, QWidget) and watched.isWindow():
            if kind == QEvent.Type.Leave:
                self.take_off()
            elif kind in (QEvent.Type.Enter, QEvent.Type.MouseMove, QEvent.Type.HoverMove):
                self.put_on()
        elif kind in (QEvent.Type.Enter, QEvent.Type.MouseMove):
            self.put_on()
        return False


GAME_CURSOR_FILTER = None


def use_game_cursor():
    """The game's finger pointer (builder_game.Skin.hand_cursor) inside the window (GameCursorFilter)."""
    global GAME_CURSOR_FILTER
    if SKIN is None:
        return
    try:
        app = QApplication.instance()
        made = SKIN.hand_cursor(app.primaryScreen().devicePixelRatio() if app.primaryScreen() else 1.0)
        if made is None:
            return
        image, hot_x, hot_y = made
        cursor = QCursor(QPixmap.fromImage(image), int(round(hot_x)), int(round(hot_y)))
        if GAME_CURSOR_FILTER is not None:
            on = GAME_CURSOR_FILTER.on
            GAME_CURSOR_FILTER.take_off()
            GAME_CURSOR_FILTER.cursor = cursor
            if on:
                GAME_CURSOR_FILTER.put_on()
            return
        GAME_CURSOR_FILTER = GameCursorFilter(cursor)
        app.installEventFilter(GAME_CURSOR_FILTER)
    except Exception as error:  # noqa: BLE001 - the system's pointer stays
        B.log(f"No game pointer: {error!r}")


def load_skin():
    """The game's graphics made for this builder, or None (not made yet, or no OpenGL to draw them)."""
    try:
        import builder_assets
        folder = B.game_ui_folder()
        manifest = builder_assets.load(folder)
        if not manifest:
            return None
        skin = builder_game.load(folder, manifest)
        if skin is None:
            B.log("No game graphics: OpenGL does not draw here.")
        return skin
    except Exception as error:  # noqa: BLE001 - the painted look stays
        B.log(f"No game graphics: {error!r}")
        return None


def load_old_skin():
    """The game's graphics an older builder made (another ASSET_VERSION), or None: enough to dress
    the small window that makes them again (builder_firststart), not the builder's window."""
    try:
        import json
        import builder_assets
        folder = B.game_ui_folder()
        manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
        if not all((folder / "layouts" / name).exists() for name in ("SysInfoWindow", "IconSave")):
            return None
        if builder_assets.seal_problem(folder, manifest):  # changed graphics are not shown
            return None
        return builder_game.load(folder, manifest)
    except Exception:  # noqa: BLE001 - the plain small window
        return None


def fit_lines(metrics, text, width, most):
    """text wrapped to width in at most `most` lines, a new line after each sentence
    (builder_game.sentence_lines); a word wider than a line (a path) is broken where it must be,
    and what does not fit ends the last line with '...'."""
    lines = []
    for sentence in builder_game.sentence_lines(text).split("\n"):
        lines += fit_sentence(metrics, sentence, width)
    if len(lines) > most:
        rest = " ".join(lines[most - 1:])
        lines = lines[:most - 1] + [metrics.elidedText(rest, Qt.TextElideMode.ElideRight, width)]
    return lines


def fit_sentence(metrics, text, width):
    """One sentence of fit_lines, wrapped to width."""
    lines, line = [], ""
    for word in text.split():
        candidate = f"{line} {word}" if line else word
        if metrics.horizontalAdvance(candidate) <= width:
            line = candidate
            continue
        if line:
            lines.append(line)
            line = ""
        while metrics.horizontalAdvance(word) > width:  # break the long word
            cut = len(word)
            while cut > 1 and metrics.horizontalAdvance(word[:cut]) > width:
                cut -= 1
            lines.append(word[:cut])
            word = word[cut:]
        line = word
    if line:
        lines.append(line)
    return lines


def draw_lines(p, left, top, lines):
    metrics = p.fontMetrics()
    for number, line in enumerate(lines):
        p.drawText(QPointF(left, top + metrics.ascent() + number * metrics.lineSpacing()), line)


def paint_card(p, widget, text, fraction=None, error=""):
    """One of the builder's small windows (the update check, getting ready) in the game's look:
    the system window, the star the game spins while it works, the name, a line of words and
    the percentage when it is known (fraction, 0 to 1). False when the game's graphics are not
    there (the caller draws its own)."""
    if SKIN is None or not SKIN.has("window"):
        return False
    try:
        rect = QRectF(widget.rect()).adjusted(4, 4, -4, -4)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        SKIN.window(p, rect)
        ink, _fill = SKIN.window_colours()
        left = rect.left() + 34
        if SKIN.has("save"):
            SKIN.save_icon(p, QRectF(left - 6, rect.top() + 24, 64, 46), now())
        p.setPen(ink)
        p.setFont(font(24, 700, "heading"))
        p.drawText(QPointF(left + 70, rect.top() + 58), "GalaxyCTR")
        if fraction is not None and fraction >= 0:
            text = re.sub(r"[:,]?\s*\d+(\.\d+)?\s*%\s*$", "", text)  # the big number below says it
        width = int(rect.width() - 68)
        used, lines = builder_game.fitted_lines(font(14, 500), text, width)
        p.setFont(used)
        draw_lines(p, left, rect.top() + 84, lines[:2])
        if error:
            p.setPen(mix(BAD, ink, 0.25))
            used, lines = builder_game.fitted_lines(font(14, 500), error, width)
            p.setFont(used)
            draw_lines(p, left, rect.top() + 126, lines[:2])
        elif fraction is not None and fraction >= 0:
            p.setFont(font(22, 700, "heading"))
            p.drawText(QPointF(left, rect.bottom() - 26), f"{min(1.0, fraction) * 100:.0f}%")
        return True
    except Exception:  # noqa: BLE001 - the plain small window
        return False


def splash_painter():
    """For the launcher's update check: the game's look made from the user's disc, or None."""
    global SKIN
    if SKIN is None:
        SKIN = load_skin()
    if SKIN is None:
        return None
    pick_fonts()
    use_game_font()
    return paint_card


def opengl_works():
    """True when Qt can open an OpenGL context here (tried off screen, without a window)."""
    try:
        from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
        context = QOpenGLContext()
        if not context.create():
            return False
        surface = QOffscreenSurface()
        surface.create()
        works = context.makeCurrent(surface)
        context.doneCurrent()
        return bool(works)
    except Exception:  # noqa: BLE001
        return False


# ##############################################################################################
# # Pages                                                                                      #
# ##############################################################################################
class StarHalo:
    """The PowerStarHalo, the light the game shows where a Power Star is while the camera is still
    far from it (Game/Map/Halo.cpp, builder_halo), centred behind the logo. It fades in with its
    Appear animation each time the main menu shows, then stays. SIZE is the glow's width as a
    share of the logo's height."""

    SIZE = 1.6

    def __init__(self):
        import builder_halo
        self.halo = builder_halo.Halo(SKIN.folder)
        self.cache = None  # (key, image) once the halo has settled

    def start(self):
        self.halo.start(-1e9 if REDUCED else now())
        self.cache = None

    def paint(self, p, widget, center, logo_height):
        ratio = widget.devicePixelRatioF()
        left, bottom, right, top = self.halo.box
        glow = 3002.0  # the glow's width in the model (the streak is the rest of the box)
        k = max(1.0, logo_height * self.SIZE) / glow  # pixels per unit
        w = (right - left) * k
        h = (top - bottom) * k
        key = (int(w * ratio), int(h * ratio))
        t = now()
        if self.cache is not None and self.cache[0] == key:
            image = self.cache[1]
        else:
            half_w, half_h = w / 2 / k, h / 2 / k
            image = self.halo.image(max(1, key[0]), max(1, key[1]), (-half_w, -half_h, half_w, half_h), t)
            if image is None:
                return False
            image.setDevicePixelRatio(ratio)
            if self.halo.settled(t):
                self.cache = (key, image)
        builder_game.L.draw_lit(p, QPointF(center.x() - w / 2, center.y() - h / 2), image)
        return True


# ##############################################################################################
# # The title screen (Game/Screen/TitleSequenceProduct.cpp)                                    #
# #                                                                                            #
# # The game's title over its sky (CometNearOrbitSky, builder_sky.TitleSkyView), nerve by      #
# # nerve: the title music starts (exeBgmPrepare), the logo plays Appear (exeLogoFadein), then  #
# # Wait with its lights (exeLogoWait: the TitleLogoLight effects at the EffPosition panes),   #
# # and after 25 frames the prompt shows (exeLogoDisplay: the PressStart layout, its text     #
# # telling of the mouse). The left mouse button is the game's A, the right one its B: one    #
# # held lights the logo (ReactionA, ReactionB) and bumps the prompt (ButtonReaction), both   #
# # together start: the music fades over 75 frames, SE_SY_GAME_START, the logo plays Decide   #
# # and the prompt End, the lights go (exeDecide), then the main menu comes. Enter or Space   #
# # press both at once, for the keyboard.                                                     #
# ##############################################################################################
TITLE_LIGHTS = (("TitleLogoLightA", "EffPosition7"), ("TitleLogoLightB", "EffPosition2"),  # AutoEffectList:
                ("TitleLogoLightC", "EffPosition5"), ("TitleLogoLightD", "EffPosition6"),  # the effect and
                ("TitleLogoLightE", "EffPosition3"), ("TitleLogoLightF", "EffPosition4"),  # the pane it
                ("TitleLogoLightG", "EffPosition1"))                                        # starts at
PRESS_AB_APPEAR = 25  # sPressABAppearFrame
PRESS_TEXT = "Press the Space bar."  # the game's "Press both (A) and (B)."


def title_available():
    return SKIN is not None and SKIN.has_layout("TitleLogo") and SKIN.has_layout("PressStart") and \
        "cometnearorbitsky" in SKIN.manifest.get("skies", {})


class TriggerChecker:
    """The game's TriggerChecker: a button's level this frame, and whether it went on or off."""

    def __init__(self):
        self.level = self.last = False

    def update(self, level):
        self.last, self.level = self.level, level

    @property
    def on(self):
        return self.level and not self.last

    @property
    def off(self):
        return self.last and not self.level


class TitlePage(QWidget):
    finished = Signal()

    def __init__(self, window):
        super().__init__()
        import builder_layout
        self.main = window
        self.scene = builder_layout.Scene(SKIN.layout_folder("TitleLogo"))
        self.lights = None
        if SKIN.manifest.get("particles"):
            try:
                import builder_particles
                self.lights = builder_particles.Player(builder_particles.Library(SKIN.folder, SKIN.manifest["particles"]))
            except Exception as error:  # noqa: BLE001 - the title without its sparkles
                B.log(f"No title lights: {error!r}")
        self.press = builder_layout.Scene(SKIN.layout_folder("PressStart"))
        self.press_shown = False
        self.held = {"a": False, "b": False}  # the mouse buttons now (left: A, right: B)
        self.a_checker, self.b_checker = TriggerChecker(), TriggerChecker()
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.nerve, self.step, self.clock = "bgm", 0, None
        self.ahead = 0.0  # the share of the next game frame the layouts are drawn at
        self.timer = builder_game.frame_timer(self)  # drawn at the window's frame rate
        self.timer.timeout.connect(self.tick)

    def showEvent(self, event):
        super().showEvent(event)
        self.setFocus()  # the Space bar reaches it at once
        if self.nerve == "bgm":
            self.timer.start()

    def set_nerve(self, nerve):
        self.nerve, self.step = nerve, 0

    def tick(self):
        t = now()
        if self.clock is None:
            self.clock, frames = t, 1
        else:
            frames = min(30, int((t - self.clock) * 60.0))
            self.clock += frames / 60.0
        for _ in range(frames):
            self.frame()
        # The game thinks 60 times a second; between its frames the layouts are drawn that
        # far into the next one (Settings > Video: the frame rate).
        self.ahead = min(0.999, max(0.0, (t - self.clock) * 60.0))
        if self.lights is not None:
            self.lights.clock = None  # run by frame() here
        self.update()

    def frame(self):
        """One frame of the game: the nerve, the layout's animations, the lights."""
        first = self.step == 0
        if self.nerve == "bgm":  # exeBgmPrepare
            if self.main.music is not None:
                self.main.music.play("title")
            self.set_nerve("fadein")
            return
        if self.nerve == "fadein":  # exeLogoFadein
            if first:
                self.scene.start_pane_anim("RootPane", "Appear")
            elif self.scene.is_pane_anim_stopped("RootPane"):
                self.set_nerve("wait")
                return
        elif self.nerve == "wait":  # exeLogoWait
            if first:
                self.scene.start_pane_anim("RootPane", "Wait")
                if self.lights is not None:
                    for effect, pane in TITLE_LIGHTS:
                        x, y = self.pane_origin(pane)
                        self.lights.emit(effect, (x, y, 0.0))
            if self.step >= PRESS_AB_APPEAR:
                self.set_nerve("display")
                return
        elif self.nerve == "display":  # exeLogoDisplay
            if first:
                self.press_shown = True
                self.press.start_pane_anim("RootPane", "Appear")
            if self.press.is_pane_anim_stopped("RootPane"):
                self.press.start_pane_anim("RootPane", "Wait")
            self.a_checker.update(self.held["a"])
            self.b_checker.update(self.held["b"])
            if self.a_checker.level and self.b_checker.level:
                self.decide()
                return  # Decide starts on its first step, next frame
            else:
                self.button_reaction(self.a_checker, "ReactionA")
                self.button_reaction(self.b_checker, "ReactionB")
                if self.a_checker.on or self.b_checker.on:  # updatePressStartReaction
                    self.press.start_pane_anim("RootPane", "ButtonReaction")
        elif self.nerve == "decide":  # exeDecide
            if first:
                self.scene.start_pane_anim("RootPane", "Decide")
                if self.lights is not None:
                    self.lights.clear()  # deleteEffectAll
                self.press.start_pane_anim("RootPane", "End")
            elif self.scene.is_pane_anim_stopped("RootPane") and self.press.is_pane_anim_stopped("RootPane"):
                self.set_nerve("dead")
                self.timer.stop()
                self.finished.emit()
                return
        self.scene.movement(1.0)
        if self.press_shown:
            self.press.movement(1.0)
        if self.lights is not None:
            for emitter in self.lights.emitters:
                emitter.calc()
            self.lights.emitters = [e for e in self.lights.emitters if e.alive()]
        self.step += 1

    def button_reaction(self, checker, anim):
        """updateButtonReaction: a button going down holds the logo's light at its brightest
        (the animation's first frame, in the logo's second group); let go, it fades."""
        if checker.on:
            self.scene.start_pane_anim("RootPane", anim, 1)
            self.scene.set_anim_frame_and_stop("RootPane", 0.0, 1)
        elif checker.off:
            self.scene.start_pane_anim("RootPane", anim, 1)

    def decide(self):
        """A and B together: the music fades over 75 frames (stopStageBGM(75)) and
        SE_SY_GAME_START plays."""
        if self.main.music is not None:
            self.main.music.fade_out(75)
        SKIN.play("game_start")
        self.set_nerve("decide")

    # The Space bar is the game's A and B pressed together (the owner's choice, 23 Nov 2026);
    # the mouse only gives the title screen the keyboard.
    def mousePressEvent(self, event):
        self.setFocus()
        event.accept()

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Space and not event.isAutoRepeat():
            self.held["a"] = self.held["b"] = True
        else:
            super().keyPressEvent(event)

    def keyReleaseEvent(self, event):
        if event.key() == Qt.Key.Key_Space and not event.isAutoRepeat():
            self.held["a"] = self.held["b"] = False
        else:
            super().keyReleaseEvent(event)

    def contextMenuEvent(self, event):
        event.accept()

    def pane_origin(self, name):
        """A pane's place in the layout (its parents' matrices, then its own translation)."""
        import builder_layout as L
        pane = self.scene.pane(name)
        chain = []
        while pane is not None and pane.parent is not None:
            chain.append(pane)
            pane = pane.parent
        m = (1, 0, 0, 0, 1, 0)
        for p in reversed(chain):
            m = L._mul(m, L._local_matrix(p))
        return m[2], m[5]

    # The layout fills the window's height: 456 units, centred.
    def unit(self):
        return self.height() / 456.0

    def paintEvent(self, event):
        if self.nerve == "bgm":
            return
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        ratio = self.devicePixelRatioF()
        k = self.unit()
        half_w = self.width() / 2 / k
        box = (-half_w, -228.0, half_w, 228.0)
        # The game's wide screen: the 608-unit layout is stretched across the whole width, and
        # a pane with the location-adjust flag (the logo, SMGTitleLogo) is narrowed by as much
        # in its own x (LayoutManager's LocationAdjustScale), so it keeps its shape while the
        # flash (PicFlash) covers the screen.
        stretch = max(1.0, (self.width() / max(1, self.height())) / (608.0 / 456.0))
        self.scene.apply(self.ahead)  # first: the narrowing below works on the drawn frame's scale
        adjusted = [pane for pane in self.scene.layout.root.walk() if pane.flag & 4]
        kept = [pane.scale[0] for pane in adjusted]
        for pane in adjusted:
            pane.scale[0] /= stretch
        image = SKIN.renderer.render(self.scene, "RootPane", k * ratio, (-304.0, -228.0, 304.0, 228.0), lights=True)
        for pane, value in zip(adjusted, kept):
            pane.scale[0] = value
        self.scene.apply()  # the game's own frame again
        # The flash's blooms and shine add to the sky (draw_lit), as in the game.
        builder_game.L.draw_lit(p, QRectF(0, 0, self.width(), self.height()), image)
        if self.press_shown:
            # The prompt keeps its shape: drawn across the window's own width, not stretched.
            self.press.apply(self.ahead)
            image = SKIN.renderer.render(self.press, "RootPane", k * ratio, box,
                                         {"TxtStart": PRESS_TEXT, "ShaStart": PRESS_TEXT}, SKIN.fonts_folder())
            self.press.apply()
            p.drawImage(QRectF(0, 0, self.width(), self.height()), image)
        if self.lights is not None and self.lights.emitters:
            light = self.lights.image(max(1, int(self.width() * ratio)), max(1, int(self.height() * ratio)), box)
            if light is not None:
                light.setDevicePixelRatio(ratio)
                builder_game.L.draw_lit(p, QPointF(0, 0), light)


class HaloLayer(QWidget):
    """The whole window, over the game's sky and under the pages: the main menu's PowerStarHalo
    is drawn here, so its long streak runs to the window's edges."""

    def __init__(self, window):
        super().__init__(window)
        self.main = window
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)

    def paintEvent(self, event):
        page = getattr(self.main, "start_page", None)
        if page is not None and self.main.stack.currentWidget() is page:
            p = QPainter(self)
            page.paint_light(p, self)


class MotionNotice(Glass):
    """On the main menu when Windows' "Animation effects" is off (the window then stands still,
    the game's sky too): the game's system window at the very top of the window, over the
    header, with a word on it and a button that turns the setting on. It slides down from above
    the window's edge with the sound of the game's control guide (PlayerActionGuidance,
    SE_SY_CTRL_GUIDE_APPEAR) and back up when it goes (Window.refresh_motion_notice). Its slide
    runs on the clock even while the rest of the window stands still: it is the one thing that
    has to be seen arriving."""

    TOP, SLIDE_IN, SLIDE_OUT = 10, 450, 300  # pixels from the window's top, milliseconds

    def __init__(self, window):
        super().__init__(window, radius=18)
        self.main = window
        row = QHBoxLayout(self)
        row.setContentsMargins(24, 8, 12, 8)
        row.setSpacing(14)
        words = label("Windows animations are off, so the sky stands still.", 14, 500, INK)
        row.addWidget(words)
        turn_on = Button("Turn them on", "primary")
        turn_on.clicked.connect(self.turn_on)
        row.addLayout(button_row([turn_on]))
        self.shown = False
        self.offset = 0.0  # 0 above the window, 1 in place
        self.slide = builder_game.ClockAnimation(self)
        self.slide.setEasingCurve(QEasingCurve.Type.OutCubic)
        self.slide.valueChanged.connect(self._slid)
        self.slide.finished.connect(self._slide_over)
        self.hide()

    def _slid(self, value):
        self.offset = float(value)
        self.place()

    def _slide_over(self):
        if not self.shown:
            self.hide()

    def place(self):
        self.adjustSize()
        size = self.sizeHint()
        width = min(size.width(), self.main.width() - 20)
        y = -size.height() + (self.TOP + size.height()) * self.offset
        self.setGeometry(int((self.main.width() - width) / 2), int(y), width, size.height())

    def slide_in(self):
        if self.shown:
            return
        self.shown = True
        self.show()
        self.raise_()
        if SKIN is not None:
            SKIN.play("notice")
        self.slide.setDuration(self.SLIDE_IN)
        self.slide.setStartValue(self.offset)
        self.slide.setEndValue(1.0)
        self.slide.start()

    def slide_out(self):
        if not self.shown:
            return
        self.shown = False
        self.slide.setDuration(self.SLIDE_OUT)
        self.slide.setStartValue(self.offset)
        self.slide.setEndValue(0.0)
        self.slide.start()

    def turn_on(self):
        if turn_windows_animations_on():
            self.main.motion_back()
            self.main.toast.show_text("Windows animations are on", GOOD)
            B.log("Windows animation effects turned on from the main menu.")
        else:
            self.main.toast.show_text("Windows did not change the setting", BAD)


class StartPage(QWidget):
    def __init__(self, window):
        super().__init__()
        self.light = None
        if SKIN is not None and "powerstarhalo" in SKIN.manifest.get("skies", {}):
            try:
                self.light = StarHalo()
            except Exception as error:  # noqa: BLE001 - the logo shows without its light
                B.log(f"No Power Star halo: {error!r}")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        self.parts = []  # what comes in from where on the first showing (Entrance)
        layout.addStretch(2)
        self.logo = Logo()
        layout.addWidget(self.logo, 5)
        subtitle = label("Super Mario Galaxy for New Nintendo 3DS", 22, 600, INK, "heading")
        subtitle.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(subtitle)
        layout.addSpacing(6)
        tagline = label(f"Builder, release {B.RELEASE}", 14, 500, CYAN)
        tagline.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(tagline)
        self.parts += [(self.logo, (0, -1)), (subtitle, (-1, 0)), (tagline, (1, 0))]
        layout.addSpacing(22)
        chips = QHBoxLayout()
        chips.setSpacing(10)
        chips.addStretch(1)
        for text, color, icon in (("Your own USA disc (RMGE01)", CYAN, "star"), ("New 3DS, New 3DS XL, New 2DS XL", ICE, "mario"),
                                  ("Your saves are kept", GOOD, "save")):
            chip = Chip(text, color, icon=icon)
            chips.addWidget(chip)
            self.parts.append((chip, ((-1, 0), (0, 1), (1, 0))[len([1 for w, _ in self.parts if isinstance(w, Chip)])]))
        chips.addStretch(1)
        layout.addLayout(chips)
        layout.addSpacing(30)
        buttons = QHBoxLayout()
        buttons.setSpacing(14)
        buttons.addStretch(1)
        settings = Button("Settings", icon="list")
        settings.setMinimumWidth(170)
        settings.clicked.connect(window.show_settings)
        buttons.addWidget(settings)
        if SKIN is None:
            # First start: the disc is read once for the game's own look (the user's choice,
            # 21 Nov 2026); building straight away reads it too.
            setup = Button("Choose your disc", "primary")
            setup.setMinimumWidth(260)
            setup.clicked.connect(window.start_setup)
            later = Button("Start the build")
            later.clicked.connect(window.start_build)
            buttons.addWidget(later)
            buttons.addWidget(setup)
            start = setup
        else:
            start = Button("Start the build", "primary", icon="a_button")
            start.setMinimumWidth(260)
            start.clicked.connect(window.start_build)
            buttons.addWidget(start)
        buttons.addStretch(1)
        layout.addLayout(buttons)
        for index in range(buttons.count()):
            widget = buttons.itemAt(index).widget()
            if widget is not None:
                self.parts.append((widget, (-1, 0) if widget is settings else (1, 0)))
        layout.addSpacing(18)
        if SKIN is None:
            first = label("First, choose your disc. The builder reads it once to dress this window in the game's look. "
                          "Nothing of it leaves your computer.", 13, 400, ICE, wrap=True)
            first.setAlignment(Qt.AlignmentFlag.AlignCenter)
            first.setMinimumWidth(560)  # no maximum: as wide as its longest sentence
            row = QHBoxLayout()
            row.addStretch(1)
            row.addWidget(first)
            row.addStretch(1)
            layout.addLayout(row)
            layout.addSpacing(8)
        layout.addSpacing(4)
        # Back to the title screen (Window.back_to_title), under Settings and Start the build,
        # in the middle: only when there is a title screen to go back to.
        self.to_title = None
        if window.title_page is not None:
            self.to_title = Button("Back to Title Screen", "back", icon="back")
            self.to_title.clicked.connect(window.back_to_title)
            row = QHBoxLayout()
            row.addStretch(1)
            row.addWidget(self.to_title)
            row.addStretch(1)
            layout.addLayout(row)
            self.parts.append((self.to_title, (0, 1)))
        layout.addStretch(3)
        self.start = start

    def paint_light(self, p, layer):
        """Draws the halo on layer (HaloLayer, the whole window under the pages), centred on the
        logo, as faded as this page is while pages change."""
        if self.light is None or not self.isVisible():
            return
        opacity = 1.0
        effect = self.graphicsEffect()
        if isinstance(effect, QGraphicsOpacityEffect):
            opacity = effect.opacity()
        entrance = getattr(self.window(), "entrance", None)
        if entrance is not None:
            opacity *= entrance.opacity(self.logo)
        p.setOpacity(opacity)
        logo = self.logo.geometry()
        centre = self.logo.parentWidget().mapTo(layer.parentWidget(), QPointF(logo.center().x(), logo.top() + logo.height() / 2 + self.logo.bob()))
        if not self.light.paint(p, layer, centre, logo.height()):
            self.light = None

    def showEvent(self, event):
        super().showEvent(event)
        if self.light is not None:
            self.light.start()  # Halo::appear


def even_rows(*grids):
    """The settings' rows all as tall as the tallest (a game control with its arrows), with the
    same small gap: the same room between every two options (owner, 25 Nov 2026). Without it
    each row took its own control's height, from 37 to 87 pixels, and one was squeezed."""
    tallest = 0
    for grid in grids:
        grid.setVerticalSpacing(8)
        for row in range(grid.rowCount()):
            for column in range(grid.columnCount()):
                item = grid.itemAtPosition(row, column)
                if item is not None:
                    tallest = max(tallest, item.sizeHint().height())
    for grid in grids:
        for row in range(grid.rowCount()):
            grid.setRowMinimumHeight(row, tallest)


class SettingsPage(QWidget):
    """Every answer settings.txt keeps. Saving writes only what changed."""

    KEEP_VALUES = (None, "yes", "no")

    def __init__(self, window):
        super().__init__()
        self.main = window
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(16)

        panel = Glass(radius=24)
        grid = QGridLayout(panel)
        grid.setContentsMargins(30, 26, 30, 26)
        grid.setHorizontalSpacing(28)
        grid.setVerticalSpacing(22)
        grid.setColumnStretch(1, 1)
        self.keep = ArrowChoice(("Ask me", "Keep them", "Don't keep"))
        self.remember_window = ArrowChoice(("Don't remember", "Remember"))
        self.folder = GameLineEdit()
        self.folder.setPlaceholderText("Asked the first time the files are kept")
        folder_row = QHBoxLayout()
        folder_row.addWidget(self.folder, 1)
        browse = IconButton("hand", "Choose a folder")
        browse.clicked.connect(self.choose_folder)
        folder_row.addWidget(browse)
        self.ftp = GameLineEdit()
        self.ftp.setPlaceholderText("Asked at the first copy over Wi-Fi, for example 192.168.1.10:5000")
        self.letter = GameLineEdit()
        self.letter.setMaxLength(1)
        self.letter.setMaximumWidth(90)
        self.letter.setPlaceholderText("E")

        rows = [("Keep the game files", "About 9 GB kept between builds. Updates convert only what changed, without the disc.", self.keep),
                ("Window position and screen", "Restore the window where you left it when you reopen the builder.", self.remember_window),
                ("Folder of the kept files", "Where the kept game files are.", folder_row),
                ("3DS FTP address", "The address the FTP app on the 3DS shows, as ip:port.", self.ftp)]
        # The number of FTP connections is chosen in the copy's own window, each time (owner, 26
        # Nov 2026), which keeps the last one in settings.txt (ftp_connections).
        if os.name == "nt":
            rows.append(("SD card drive letter", "The letter offered first when copying to the SD card.", self.letter))
        for index, (name, help_text, control) in enumerate(rows):
            words = QVBoxLayout()
            words.setSpacing(3)
            words.addWidget(label(name, 15, 600, INK))
            hint = label(help_text, SMALL_TEXT, 400, MUTED, wrap=True)
            words.addWidget(hint)
            # The name and its hint together, in the middle of the row: the game's controls are
            # taller, and the extra height was split between the two (owner, 25 Nov 2026).
            grid.addLayout(words, index, 0, Qt.AlignmentFlag.AlignVCenter)
            if isinstance(control, QHBoxLayout):
                grid.addLayout(control, index, 1)
            else:
                grid.addWidget(control, index, 1)
        # Graphics: how the window draws (the frame rate, VSync, the card, motion, the counter).
        self.fps = ArrowChoice(tuple(name for name, _ in FRAME_RATES))
        self.vsync = ArrowChoice(("On", "Off"))
        self.gpu_choices = GPU_CHOICES_WINDOWS if os.name == "nt" else GPU_CHOICES_LINUX
        self.gpu = ArrowChoice(tuple(name for name, _ in self.gpu_choices))
        self.motion = ArrowChoice(tuple(name for name, _ in MOTION_CHOICES))
        self.counter = ArrowChoice(("Hidden", "Shown"))
        self.gpu_hint = label("", SMALL_TEXT, 400, MUTED, wrap=True)
        graphics_panel = Glass(radius=24)
        graphics_grid = QGridLayout(graphics_panel)
        graphics_grid.setContentsMargins(30, 26, 30, 26)
        graphics_grid.setHorizontalSpacing(28)
        graphics_grid.setVerticalSpacing(22)
        graphics_grid.setColumnStretch(1, 1)
        graphics_rows = [("Frame rate", "Frames a second the window draws. The game itself stays at 60. "
                                        "Past 60, its animations are drawn in between.", self.fps),
                         ("VSync", "Each frame waits for the screen: no tearing. Changes when the builder starts "
                                   "again.", self.vsync),
                         ("Graphics card", None, self.gpu),
                         ("Motion", "Sky and window animated, or as the system's animation setting says.",
                          self.motion),
                         ("Frame rate counter", "The frames drawn each second, in the bottom right corner.", self.counter)]
        if os.name not in ("nt",) and not sys.platform.startswith("linux"):
            graphics_rows = [row for row in graphics_rows if row[0] != "Graphics card"]
        for index, (name, help_text, control) in enumerate(graphics_rows):
            words = QVBoxLayout()
            words.setSpacing(3)
            words.addWidget(label(name, 15, 600, INK))
            if help_text is None:
                words.addWidget(self.gpu_hint)
            else:
                hint = label(help_text, SMALL_TEXT, 400, MUTED, wrap=True)
                words.addWidget(hint)
            graphics_grid.addLayout(words, index, 0, Qt.AlignmentFlag.AlignVCenter)  # as in the grid above
            graphics_grid.addWidget(control, index, 1)
        self.grids = (grid, graphics_grid)
        even_rows(*self.grids)
        # Every choice between the game's arrows as wide as the longest of them all: the arrows
        # stand in one column, whatever the window's width (owner, 26 Nov 2026).
        plates = [c.choices for c in (self.keep, self.remember_window, self.fps, self.vsync, self.gpu,
                                      self.motion, self.counter)
                  if isinstance(c, ArrowChoice) and c.left is not None]
        if plates:
            width = max(plate.sizeHint().width() for plate in plates)
            for plate in plates:
                plate.setFixedWidth(width)
        sections = QWidget()
        column = QVBoxLayout(sections)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(16)
        column.addWidget(label("General", 22, 700, INK, "heading"))
        column.addWidget(panel)
        column.addWidget(label("Video", 22, 700, INK, "heading"))
        column.addWidget(graphics_panel)
        outer.addWidget(transparent_scroll(sections), 1)

        buttons = QHBoxLayout()
        back = Button("Back", "back", icon="back")
        back.setMinimumWidth(150)
        back.clicked.connect(window.show_start)
        save = Button("Save", "primary", icon="a_button")
        save.setMinimumWidth(190)
        save.clicked.connect(self.save)
        self.buttons = (back, save)  # the toast stays between them
        buttons.addWidget(back)
        buttons.addStretch(1)
        buttons.addWidget(save)
        outer.addLayout(buttons)

    def load(self):
        settings = B.Settings()
        value = settings.get("keep_files")
        self.keep.set_index(self.KEEP_VALUES.index(value) if value in self.KEEP_VALUES else 0)
        self.remember_window.set_index(1 if settings.get("remember_window") == "yes" else 0)
        self.folder.setText(settings.get("work_folder") or "")
        self.ftp.setText(settings.get("ftp_address") or "")
        self.letter.setText(settings.get("last_sd_letter") or "")
        values = [value for _, value in FRAME_RATES]
        fps = graphics(settings, "graphics_fps")
        self.fps.set_index(values.index(fps) if fps in values else 0)
        self.vsync.set_index(0 if graphics(settings, "graphics_vsync") == "on" else 1)
        values = [value for _, value in self.gpu_choices]
        card = graphics(settings, "graphics_gpu")
        self.gpu.set_index(values.index(card) if card in values else 0)
        values = [value for _, value in MOTION_CHOICES]
        motion = graphics(settings, "graphics_motion")
        self.motion.set_index(values.index(motion) if motion in values else 0)
        self.counter.set_index(1 if graphics(settings, "graphics_counter") == "on" else 0)
        cards = graphics_cards()
        in_use = graphics_card_in_use()
        hint = ("On computers with two cards, which one draws the window. Changes when the builder starts again."
                if os.name == "nt" else
                "Draw on the second card of a computer with two (PRIME). Changes when the builder starts again.")
        # A sentence each, with the makers' plain names: the window never breaks a sentence (25
        # Nov 2026), and the cards in one sentence made a line wider than the narrow window, which
        # pushed the choices' arrows out of the panel (owner, 26 Nov 2026).
        for number, card in zip(("First", "Second", "Third", "Fourth"), cards):
            hint += f" {number} card: {short_card_name(card)}."
        if in_use:
            hint += " Drawing now on " + short_card_name(in_use) + "."
        self.gpu_hint.setText(hint)
        self.gpu_hint.setVisible(bool(hint))  # no empty line under the name
        even_rows(*self.grids)  # the card's hint can make its row the tallest

    def choose_folder(self):
        path = QFileDialog.getExistingDirectory(None, "Folder of the kept files", self.folder.text().strip() or str(Path.home()))
        if path:
            self.folder.setText(str(Path(path)))

    def save(self):
        address = self.ftp.text().strip()
        if address and B.parse_ftp_address(address) is None:
            self.main.toast.show_text("Type the address and the port, for example 192.168.1.10:5000", BAD,
                                      self.buttons)
            return
        letter = self.letter.text().strip().rstrip(":").upper()
        if letter and not letter.isalpha():
            self.main.toast.show_text("The drive letter is a single letter", BAD, self.buttons)
            return
        settings = B.Settings()
        wanted = {"keep_files": self.KEEP_VALUES[self.keep.index], "work_folder": self.folder.text().strip() or None,
                  "ftp_address": B.parse_ftp_address(address) and "%s:%d" % B.parse_ftp_address(address) or None,
                  "remember_window": "yes" if self.remember_window.index else None}
        if os.name == "nt":
            wanted["last_sd_letter"] = letter or None
        before = {name: graphics(settings, name) for name in ("graphics_vsync", "graphics_gpu")}
        graphics_wanted = {"graphics_fps": FRAME_RATES[self.fps.index][1],
                           "graphics_vsync": ("on", "off")[self.vsync.index],
                           "graphics_gpu": self.gpu_choices[self.gpu.index][1],
                           "graphics_motion": MOTION_CHOICES[self.motion.index][1],
                           "graphics_counter": ("off", "on")[self.counter.index]}
        defaults = {"graphics_fps": "60", "graphics_vsync": "on", "graphics_gpu": "auto",
                    "graphics_motion": "system", "graphics_counter": "off"}
        for name, value in graphics_wanted.items():
            wanted[name] = None if value == defaults[name] else value
        for name, value in wanted.items():
            if value is None:
                settings.unset(name)
            elif settings.get(name) != value:
                settings.set(name, value)
        if self.remember_window.index == 0:
            settings.unset("window_geometry")
        else:
            self.main.save_window_geometry()  # where it is now, without waiting for a move
        # The frame rate, the motion and the counter change at once; VSync and the card when the
        # builder starts again (the card is also given to Windows now, for that start).
        builder_game.set_frame_rate(frame_rate_of(graphics_wanted["graphics_fps"]))
        self.main.set_motion(graphics_wanted["graphics_motion"])
        self.main.counter.set_shown(graphics_wanted["graphics_counter"] == "on")
        if os.name == "nt":
            apply_graphics_card(graphics_wanted["graphics_gpu"])
        later = any(graphics_wanted[name] != value for name, value in before.items())
        self.main.toast.show_text("Settings saved. VSync and the graphics card change when the builder starts again"
                                  if later else "Settings saved", GOOD, self.buttons)


class BuildPage(QWidget):
    TITLES = ("Disc image", "Extracting the disc", "Converting the game files", "Building the CIA",
              "Copying to the 3DS", "Cleaning up")
    # The big card's words for each step: short, and saying what happens, where the step list
    # only names it. (step, a word of the builder's title or None, words); the first that fits.
    CARD_TITLES = ((1, None, "Looking at your disc"),
                   (2, "already done", "The game is already unpacked"),
                   (2, None, "Unpacking the game from the disc"),
                   (3, "only what changed", "Updating the game for the 3DS"),
                   (3, None, "Turning the game for the 3DS"),
                   (4, None, "Packing the game to install"),
                   (5, None, "Sending the game to your 3DS"),
                   (6, None, "Removing leftover files"))

    @classmethod
    def card_title(cls, number, title):
        for step, word, words in cls.CARD_TITLES:
            if step == number and (word is None or word in title):
                return words + "..."  # still at work
        return title + "..."

    def __init__(self, window):
        super().__init__()
        self.main = window
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(20)

        steps = Glass(radius=24)
        steps.setFixedWidth(340)
        column = QVBoxLayout(steps)
        column.setContentsMargins(26, 26, 22, 22)
        column.setSpacing(2)
        column.addWidget(label("Steps", 14, 600, CYAN))
        column.addSpacing(14)
        self.rows = [StepRow(n + 1, title, n == len(self.TITLES) - 1) for n, title in enumerate(self.TITLES)]
        for row in self.rows:
            column.addWidget(row)
        column.addStretch(1)
        self.elapsed = Chip("0:00:00", ICE)
        column.addWidget(self.elapsed, 0, Qt.AlignmentFlag.AlignLeft)
        layout.addWidget(steps)

        right = QVBoxLayout()
        right.setSpacing(20)
        current = Glass(radius=24)
        card = QVBoxLayout(current)
        card.setContentsMargins(30, 26, 30, 26)
        card.setSpacing(10)
        self.caption = label("Getting ready", 14, 600, CYAN)
        card.addWidget(self.caption)
        self.title = label("Starting the build...", 26, 700, INK, "heading", wrap=True)
        card.addWidget(self.title)
        card.addSpacing(8)
        self.bar = Progress()
        self.bar.set_value(-1)
        card.addWidget(self.bar)
        if SKIN is not None and SKIN.has("save"):
            # With the game's look the bar is the spinning save star, which the step list already
            # shows on the running step: here only the percentage.
            self.bar.hide()
        numbers = QHBoxLayout()
        self.percent = label("", 30, 700, INK, "heading")
        numbers.addWidget(self.percent)
        numbers.addSpacing(14)
        self.detail = label("", 13, 400, MUTED, wrap=True)
        numbers.addWidget(self.detail, 1)
        card.addLayout(numbers)
        self.status = label("", 14, 400, INK, wrap=True)
        card.addWidget(self.status)
        right.addWidget(current)

        log_panel = Glass(radius=24)
        log_column = QVBoxLayout(log_panel)
        log_column.setContentsMargins(24, 20, 24, 22)
        header = QHBoxLayout()
        header.addWidget(label("Activity", 14, 600, CYAN))
        header.addStretch(1)
        open_log = Button("Open the log")
        open_log.clicked.connect(lambda: open_path(B.LOG_PATH))
        header.addWidget(open_log)
        log_column.addLayout(header)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(4000)
        self.log.setFont(font(SMALL_TEXT, kind="mono"))
        log_column.addWidget(self.log, 1)
        right.addWidget(log_panel, 1)
        layout.addLayout(right, 1)
        self.reset()

    def reset(self):
        self.started = now()
        self.current = 0
        self.work_step = 0
        self.copy_failed = False
        self.after_cleanup = []
        self.errors = []
        for row, title in zip(self.rows, self.TITLES):
            row.title = title
            row.set_state("wait")
        self.caption.setText("Getting ready")
        self.title.setText("Starting the build...")
        self.percent.setText("")
        self.detail.setText("")
        self.status.setText("")
        self.bar.set_value(-1)
        self.log.clear()

    def tick(self):
        seconds = int(now() - self.started)
        if self.main.building:
            self.elapsed.set_text(f"Elapsed {seconds // 3600:d}:{seconds // 60 % 60:02d}:{seconds % 60:02d}")

    def on_said(self, text):
        self.log.appendPlainText(B.log_format(text))  # the same look as the log file
        line = text.strip()
        if not line:
            return
        if self.current >= 6:
            self.after_cleanup.append(line)
        if line.startswith("ERROR:"):
            self.errors.append(line[6:].strip())
            if self.current == 5:
                self.copy_failed = True
            set_color(self.status, mix(BAD, INK, 0.25))
        elif not line.startswith("Log file"):
            set_color(self.status, INK)
        if not line.startswith("Log file"):
            self.status.setText(line)

    def on_step(self, number, total, title):
        if number < 6:
            self.work_step = number
        for row in self.rows[:number - 1]:
            if row.state in ("wait", "run"):
                row.set_state("done" if not (row.number == 5 and self.copy_failed) else "fail")
        if 0 < number <= len(self.rows):
            self.rows[number - 1].set_state("run")
        self.current = number
        self.caption.setText(f"Step {number} of {total}")
        self.title.setText(self.card_title(number, title))
        self.percent.setText("")
        self.detail.setText("")
        self.bar.set_value(-1)

    def on_progress(self, name, percent, detail):
        self.bar.set_value(percent)
        self.percent.setText(f"{percent:.0f}%" if percent >= 0 else "")
        self.detail.setText(", ".join(filter(None, (name, detail))))

    def finish(self, code):
        """Marks the steps as they ended: a failure on the step that was running."""
        for row in self.rows:
            if row.state == "run":
                row.set_state("done")
        if code != 0 and 0 < self.work_step:
            failed = self.rows[self.work_step - 1]
            failed.set_state("fail")
            for row in self.rows[self.work_step:5]:
                if row.state == "wait":
                    row.set_state("skip")
        if self.copy_failed:
            self.rows[4].set_state("fail")
        self.bar.set_value(100 if code == 0 else self.bar.target)


class DonePage(QWidget):
    def __init__(self, window):
        super().__init__()
        self.main = window
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.addStretch(1)
        row = QHBoxLayout()
        row.addStretch(0)  # the card takes the width first, up to its maximum
        # The install guide shows whole, never scrolled (owner, 26 Nov 2026), so the page gives
        # it the room: the card as wide as the window allows (the guide's lines wrap less), the
        # star beside the title instead of above it, and the three buttons on one row.
        self.card = Glass(radius=28, tint=120)
        self.card.setMinimumWidth(640)
        self.card.setMaximumWidth(900)
        column = QVBoxLayout(self.card)
        column.setContentsMargins(40, 18, 40, 20)
        column.setSpacing(10)
        top = QHBoxLayout()
        top.setSpacing(18)
        self.badge = Badge()
        top.addWidget(self.badge, 0, Qt.AlignmentFlag.AlignVCenter)
        words = QVBoxLayout()
        words.setSpacing(4)
        words.addStretch(1)
        self.title = label("", 30, 700, INK, "heading")
        words.addWidget(self.title)
        self.message = label("", 15, 400, MUTED, wrap=True)
        words.addWidget(self.message)
        words.addStretch(1)
        top.addLayout(words, 1)
        column.addLayout(top)
        self.guide_panel = GuidePanel()
        self.guide = self.guide_panel.text
        column.addWidget(self.guide_panel)
        self.open_log = Button("Open the log")
        self.open_log.clicked.connect(lambda: open_path(B.LOG_PATH))
        self.open_folder = Button("Open the files folder")
        self.open_folder.clicked.connect(lambda: open_path(B.RESULT.get("work")))
        again = Button("Main menu", "back", icon="back")
        again.clicked.connect(window.show_start)
        self.again = again
        column.addLayout(button_row([self.open_log, self.open_folder, again], "center"))
        row.addWidget(self.card, 1)
        row.addStretch(0)
        outer.addLayout(row)
        outer.addStretch(1)

    def show_result(self, code, build_page):
        result = B.RESULT
        copied = bool(result.get("copied"))
        good = code == 0 and not build_page.copy_failed
        self.badge.show_result(good)
        guide_text = ""
        if code != 0:
            reason = (build_page.errors[-1] if build_page.errors else "").strip()
            if not reason or reason.rstrip(".").lower() == "stopped":
                self.title.setText("The build stopped")
                self.message.setText("Your settings are kept, so you can start again whenever you like.")
            else:
                self.title.setText("The build could not finish")
                more = "" if "log" in reason.lower() else " The details are in the log."
                self.message.setText(f"{reason}{more}\n\nYour settings are kept: fix this and start again.")
        elif copied:
            self.title.setText("Your game is ready")
            sound = "" if result.get("sound") else ("\n\nNote: sdmc:/3ds/dspfirm.cdc is missing, so the game would "
                                                    "have no sound. Run the homebrew app DSP1 once on your 3DS.")
            self.message.setText(f"If the SD card is in this computer, put it back into your 3DS. Then install "
                                 f"cias/{B.CIA_NAME} with FBI or another CIA installer.{sound}")
        else:
            self.title.setText("Build complete!" if good else "Built, but not copied")
            problem = f"{build_page.errors[-1]}\n\n" if build_page.errors else ""
            self.message.setText(f"{problem}Copy the files when you are ready, with these steps. They are also saved "
                                 "in install_guide.txt next to the builder.")
            try:
                guide_text = Path(result.get("guide")).read_text(encoding="utf-8") if result.get("guide") else ""
            except OSError:
                guide_text = ""
        self.guide.setPlainText(guide_text.rstrip())
        self.guide_panel.setVisible(bool(guide_text))
        self.open_folder.setVisible(bool(result.get("work")) and Path(str(result.get("work"))).exists())


GUIDE_MARGIN = 31  # above and below the guide's text, inside its panel


class GuideText(QTextEdit):
    """The guide's text, asking for the height of all its lines (GuidePanel.fit)."""

    def __init__(self):
        super().__init__()
        self.needed = 0

    def sizeHint(self):
        hint = super().sizeHint()
        return QSize(hint.width(), self.needed or hint.height())


class GuidePanel(QWidget):
    """The install guide on the result page, on the panel the co-star's manual writes its pages
    on (the game window "page": P2Manual's WinTitle), inside the page's own window."""

    def __init__(self):
        super().__init__()
        layout = QVBoxLayout(self)
        # The same space above the first line as below the last (owner, 26 Nov 2026): the guide's
        # closing line break is left out (show_result), so no empty line widens the bottom.
        layout.setContentsMargins(22, GUIDE_MARGIN, 14, GUIDE_MARGIN + 4)  # +4: the last line's ink ends lower in its line
        # Whole and fixed, never scrolled (owner, 26 Nov 2026): the text asks to be exactly as
        # high as its wrapped lines at the panel's width (fit), and the window's smallest height
        # gives it that (Window.fit_minimum_size). Only on a screen too low for that window
        # does it get less, and then it scrolls rather than being cut.
        self.text = GuideText()
        self.text.setAcceptRichText(False)
        self.text.setReadOnly(True)
        self.text.setFont(font(SMALL_TEXT, kind="mono"))
        self.text.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.text.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.text.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        self.text.setMinimumHeight(64)
        self.text.document().documentLayout().documentSizeChanged.connect(self.fit)
        layout.addWidget(self.text)

    def fit(self, size=None):
        height = int(math.ceil(self.text.document().size().height())) + 2 * self.text.frameWidth()
        if self.text.needed != height:
            self.text.needed = height
            self.text.setMaximumHeight(height)
            self.text.updateGeometry()

    def paintEvent(self, event):
        if SKIN is not None and SKIN.has("page"):
            p = QPainter(self)
            p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            SKIN.window(p, QRectF(self.rect()), "page")


# ##############################################################################################
# # The window                                                                                 #
# ##############################################################################################
class FrameCounter(QWidget):
    """The frames the window drew in the last second (Settings > Video > Frame rate counter),
    small in the window's bottom right corner, in the game's font with its sky shadow."""

    def __init__(self, window):
        super().__init__(window)
        self.main = window
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.text, self.counted, self.since = "", 0, now()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.measure)
        self.set_shown(False)

    def set_shown(self, shown):
        self.setVisible(shown)
        if shown:
            self.counted, self.since = self.main.frames_drawn, now()
            self.timer.start(500)
            self.place()
            self.raise_()
        else:
            self.timer.stop()

    def place(self):
        self.setGeometry(self.main.width() - 130, self.main.height() - 34, 120, 26)

    def measure(self):
        t = now()
        drawn = self.main.frames_drawn - self.counted
        self.text = f"{drawn / max(0.001, t - self.since):.0f} fps"
        self.counted, self.since = self.main.frames_drawn, t
        self.raise_()
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setFont(font(13, 600))
        box = QRectF(self.rect())
        sky_shadow(p, self, INK, lambda d: p.drawText(box.translated(d), Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, self.text))
        p.setPen(INK)
        p.drawText(box, Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, self.text)


def fitted_on_screen(rect, screen_name, smallest):
    """rect (a window's inside) as it was left, on the screen named screen_name, or the screen
    nearest to it when that one is gone. It is moved only as much as needed for its title bar to
    be on the screen (a part of the window may stay past an edge, as the user left it), and made
    smaller only when it is larger than the screen's free area."""
    screens = QApplication.screens()
    screen = next((s for s in screens if s.name() == screen_name), None)
    moved = screen is None or not screen.availableGeometry().intersects(rect)  # it was left elsewhere
    if moved:
        screen = QApplication.screenAt(rect.center()) or screen or QApplication.primaryScreen()
    free = screen.availableGeometry()
    frame = 32  # Windows' title bar above the inside
    grip = free.width() if moved else 120  # how much of the window must be on the screen across
    width = min(max(rect.width(), smallest.width()), free.width())
    height = min(max(rect.height(), smallest.height()), free.height() - frame)
    x = min(max(rect.x(), free.left() - width + min(grip, width)), free.right() + 1 - min(grip, width))
    y = min(max(rect.y(), free.top() + frame), free.bottom() + 1 - (height if moved else frame))
    return QRect(x, y, width, height)


class Window(Space):
    def __init__(self):
        super().__init__()
        self.setObjectName("Space")
        self.setWindowTitle("GalaxyCTR Builder")
        self.fit_minimum_size()
        self.resize(1120, 740)
        self.watch_geometry()
        self.setStyleSheet(style_sheet())
        self.setFont(font(14))
        self.building = False
        self.worker = None  # the build's process (BuildProcess)
        self.runs = 0
        self.last_code = 0

        self.bridge = Bridge()
        self.bridge.said.connect(self.on_said)
        self.bridge.stepped.connect(self.on_step)
        self.bridge.progressed.connect(self.on_progress)
        self.bridge.asked.connect(self.on_asked)
        self.bridge.finished.connect(self.on_finished)

        root = QVBoxLayout(self)
        root.setContentsMargins(30, 20, 30, 26)
        root.setSpacing(18)
        header = QHBoxLayout()
        # The name of the page that shows (the logo on the main menu already says GalaxyCTR).
        # On the plate of the main menu's three notes (Chip), with Mario's head.
        self.page_title = Chip("Main menu", INK, icon="mario", height=40, size=16, weight=700)
        header.addWidget(self.page_title)
        header.addStretch(1)
        self.mode = Chip("Ready", GOOD)
        header.addWidget(self.mode)
        # With the game's music, its switch takes the place of the Ready chip (start_music).
        self.header = header
        root.addLayout(header)

        self.halo_layer = HaloLayer(self)
        holder = QWidget()
        self.stack = QStackedLayout(holder)
        self.sky = None
        self.title_sky = None
        self.title_sky_broken = False
        self.music = self.music_button = None
        self.job = "build"
        self.make_pages()
        root.addWidget(holder, 1)

        self.overlay = Overlay(self)
        self.toast = Toast(self)
        self.counter = FrameCounter(self)  # Settings > Video: the frame rate counter
        self.motion_notice = MotionNotice(self)  # Windows' animations are off (refresh_motion_notice)
        if os.name == "nt":  # Windows' animation setting can change while the builder is open
            self.motion_check = QTimer(self)
            self.motion_check.timeout.connect(self.check_system_motion)
            self.motion_check.start(1500)
        self.timer.timeout.connect(lambda: self.build_page.tick())
        self.start_music()
        self.page_change = None
        self.entrance = None  # the main menu coming in after the title screen (Entrance)
        self.after_page_change = []
        self.asking = 0  # questions of the build not answered yet
        self.answered_at = 0.0  # when the last one was answered (StepRow's save star)
        self.setup_card = None

    # ##########################################################################################
    # # Window position and screen (Settings > General, remember_window)                      #
    # #                                                                                        #
    # # window_geometry = x,y,width,height,state,screen: the window's inside (without Windows' #
    # # frame) when it is not maximized, in the screen coordinates Qt uses, whether it was     #
    # # maximized, and the screen's name. It is written half a second after each move or       #
    # # resize the user makes with the mouse (and when the window closes), so a builder ended  #
    # # any other way keeps it too. Back on a screen that is gone or smaller, the window is    #
    # # put on the nearest screen, no larger than it and with its title bar reachable.         #
    # ##########################################################################################
    GEOMETRY = re.compile(r"(-?\d+),(-?\d+),(\d+),(\d+),(normal|maximized)(?:,(.*))?")

    def watch_geometry(self):
        self.geometry_ready = False  # not before the saved place is applied and shown
        self.geometry_timer = QTimer(self)
        self.geometry_timer.setSingleShot(True)
        self.geometry_timer.setInterval(500)
        self.geometry_timer.timeout.connect(self.save_window_geometry)

    def geometry_changed(self):
        if getattr(self, "geometry_ready", False):
            self.geometry_timer.start()

    # ##################################################################################
    # # The window's smallest size                                                     #
    # #                                                                                #
    # # 960 wide: the result page's three game buttons on one row, inside its card.    #
    # # 750 high: the result page's install guide whole (owner, 26 Nov 2026: at 640  #
    # # it was cut at step 6 on Windows at 125 percent, the game's font a little       #
    # # taller there). Never taller than the screen it is on (a 1080p laptop at 150    #
    # # percent has about 680): there the guide scrolls (GuidePanel).                  #
    # ##################################################################################
    MIN_WIDTH, MIN_HEIGHT = 960, 750

    def fit_minimum_size(self):
        screen = self.screen()
        room = screen.availableGeometry().height() - 40 if screen is not None else self.MIN_HEIGHT  # 40: the title bar
        self.setMinimumSize(self.MIN_WIDTH, max(640, min(self.MIN_HEIGHT, room)))  # 640: the smallest before

    def moveEvent(self, event):
        super().moveEvent(event)
        self.fit_minimum_size()  # it may be on another screen now
        self.geometry_changed()

    def changeEvent(self, event):
        super().changeEvent(event)
        if event.type() == event.Type.WindowStateChange:
            self.geometry_changed()

    def show_where_saved(self):
        """Shows the window where it was left (when the user asked to remember it)."""
        maximized = False
        settings = B.Settings()
        saved = settings.get("window_geometry") if settings.get("remember_window") == "yes" else None
        found = self.GEOMETRY.fullmatch(saved or "")
        if found:
            x, y, width, height = (int(found.group(n)) for n in range(1, 5))
            maximized = found.group(5) == "maximized"
            self.setGeometry(fitted_on_screen(QRect(x, y, width, height), found.group(6) or "", self.minimumSize()))
        elif saved:  # what an earlier builder wrote (Qt's own saveGeometry)
            try:
                from PySide6.QtCore import QByteArray
                self.restoreGeometry(QByteArray.fromBase64(saved.encode("ascii")))
            except (ValueError, UnicodeError):
                pass
        if maximized:
            self.showMaximized()
        else:
            self.show()
        self.geometry_ready = True

    def save_window_geometry(self):
        if not getattr(self, "geometry_ready", False) or self.isMinimized():
            return
        settings = B.Settings()
        if settings.get("remember_window") != "yes":
            return
        maximized = self.isMaximized() or self.isFullScreen()
        inside = self.normalGeometry() if maximized else self.geometry()
        if not inside.isValid():
            inside = self.geometry()
        screen = self.screen()
        value = (f"{inside.x()},{inside.y()},{inside.width()},{inside.height()},"
                 f"{'maximized' if maximized else 'normal'},{screen.name() if screen else ''}")
        if settings.get("window_geometry") != value:
            settings.set("window_geometry", value)

    def make_pages(self):
        """The four pages, made again once the game's graphics are ready."""
        for page in [self.stack.widget(i) for i in range(self.stack.count())]:
            self.stack.removeWidget(page)
            page.deleteLater()
        self.title_page = None
        if title_available():
            try:
                self.title_page = TitlePage(self)
                self.title_page.finished.connect(self.leave_title)
            except Exception as error:  # noqa: BLE001 - straight to the main menu
                B.log(f"No title screen: {error!r}")
                self.title_page = None
        self.start_page = StartPage(self)
        self.settings_page = SettingsPage(self)
        self.build_page = BuildPage(self)
        self.done_page = DonePage(self)
        for page in (self.title_page, self.start_page, self.settings_page, self.build_page, self.done_page):
            if page is not None:
                self.stack.addWidget(page)
        self.stack.setCurrentWidget(self.title_page or self.start_page)
        self.title_page_shown(self.stack.currentWidget())
        self.make_sky()

    # ######################################################################################
    # # The game's sky behind the pages                                                   #
    # #                                                                                    #
    # # The title screen's sky (builder_sky.TitleSkyView) from the title on, behind every  #
    # # page, turning on as it did; GalaxySky (builder_sky.SkyView) when there is no title #
    # # screen or its sky cannot draw. Only when the files are there and OpenGL works (a   #
    # # test context first: a window with an OpenGL view that cannot draw would stay       #
    # # blank).                                                                            #
    # ######################################################################################
    def make_title_sky(self):
        """CometNearOrbitSky behind the title screen (builder_sky.TitleSkyView)."""
        if self.title_sky is not None or self.title_page is None or self.title_sky_broken or not opengl_works():
            return
        try:
            import builder_sky
            self.title_sky = builder_sky.TitleSkyView(SKIN.folder, self, moving=not REDUCED)
        except Exception as error:  # noqa: BLE001 - the title over the other sky
            B.log(f"No title sky: {error!r}")
            self.title_sky_broken = True
            return
        self.title_sky.failed.connect(self.title_sky_failed)
        self.title_sky.setGeometry(self.rect())
        self.title_sky.lower()

    def make_sky(self):
        self.make_title_sky()
        if self.title_sky is not None:
            # The title's sky stays behind every page, turning on from where it was (the
            # owner's choice, 22 Nov 2026); GalaxySky is only for a window with no title screen.
            self.update_sky()
            return
        if self.sky is not None or not (SKIN and SKIN.has("galaxysky")) or not opengl_works():
            self.update_sky()
            return
        try:
            import builder_sky
            self.sky = builder_sky.SkyView(SKIN.folder, "galaxysky", self, moving=not REDUCED)
        except Exception as error:  # noqa: BLE001 - the painted sky stays
            B.log(f"No game sky: {error!r}")
            return
        self.sky.failed.connect(self.sky_failed)
        self.sky.setGeometry(self.rect())
        self.sky.lower()
        self.update_sky()

    def title_sky_failed(self, reason):
        """The title's sky cannot draw: GalaxySky takes its place behind the pages."""
        B.log(f"No title sky: {reason}")
        if self.title_sky is not None:
            self.title_sky.hide()
            self.title_sky.deleteLater()
            self.title_sky = None
            self.title_sky_broken = True
            QTimer.singleShot(0, self.make_sky)

    def sky_failed(self, reason):
        B.log(f"No game sky: {reason}")
        if self.sky is not None:
            self.sky.hide()
            self.sky.deleteLater()
            self.sky = None

    def motion_back(self):
        """Windows animates again (MotionNotice): everything that stood still moves from now,
        the skies from the place they showed (frame 0), with no restart of the window."""
        global REDUCED
        REDUCED = False
        for sky in (self.title_sky, self.sky):
            if sky is not None:
                sky.moving = True
                sky.started = None  # set again on the next drawing: the turn starts from here
                if sky.renderer.ready:
                    sky.timer.start()
        self.refresh_motion_notice()

    def motion_stop(self):
        """The window stands still from now (the system asks for less motion, or Settings)."""
        global REDUCED
        REDUCED = True
        for sky in (self.title_sky, self.sky):
            if sky is not None:
                sky.moving = False
                sky.timer.stop()
                sky.update()
        self.refresh_motion_notice()

    def refresh_motion_notice(self):
        """The notice shows on the main menu while Windows' animations are what stills the window."""
        notice = getattr(self, "motion_notice", None)
        if notice is None or getattr(self, "start_page", None) is None:
            return
        wanted = (REDUCED and MOTION == "system" and windows_animations_off()
                  and self.stack.currentWidget() is self.start_page)
        if wanted:
            notice.slide_in()
        else:
            notice.slide_out()

    def check_system_motion(self):
        """Windows' animation setting, read again every moment: another program can turn it off
        and on while the builder is open (TeamViewer does for its remote sessions), and the
        window follows at once when Motion is "Follow Windows"."""
        if MOTION != "system":
            return
        still = motion_still("system")
        if still and not REDUCED:
            B.log("Windows animation effects went off: the window stands still.")
            self.motion_stop()
        elif not still and REDUCED:
            B.log("Windows animation effects came back on.")
            self.motion_back()

    def set_motion(self, value):
        """Settings > Video > Motion, at once: the window moves (motion_back) or stands still."""
        global MOTION
        MOTION = value
        still = motion_still(value)
        if not still and REDUCED:
            self.motion_back()
        elif still and not REDUCED:
            self.motion_stop()
        else:
            self.refresh_motion_notice()

    def update_sky(self):
        on_title = self.title_page is not None and self.stack.currentWidget() is self.title_page
        if self.title_sky is not None:
            self.title_sky.show()
            if self.title_sky.moving and not self.title_sky.timer.isActive() and self.title_sky.renderer.ready:
                self.title_sky.timer.start()
            return
        if self.sky is not None and on_title:
            self.sky.hide()
            self.sky.timer.stop()
            return
        if self.sky is not None:
            on_start = True  # the game's sky behind every page (the owner's choice, 22 Nov 2026)
            self.sky.setVisible(on_start)
            if on_start and self.sky.moving and not self.sky.timer.isActive() and self.sky.renderer.ready:
                self.sky.timer.start()
            elif not on_start:
                self.sky.timer.stop()

    # ######################################################################################
    # # The first start's setup                                                           #
    # #                                                                                    #
    # # galaxyctr_builder.run_setup in the build thread: the disc question, the extraction #
    # # and builder_assets.make, shown on a small card; then the pages are made again.     #
    # ######################################################################################
    def start_setup(self):
        if self.building:
            return
        self.building = True
        self.job = "setup"
        self.mode.set_text("Reading your disc", CYAN)
        self.setup_card = SetupCard(self)
        self.overlay.open(self.setup_card)
        self.worker = BuildProcess(self.bridge, "setup")

    # Pages fade and rise into place.
    def show_page_plain(self, page):
        """The old page fades out, then the new one fades in (the game's sky changes in between,
        when no page shows)."""
        if self.entrance is not None:
            self.entrance.finish()
        if self.page_change is not None:
            self.page_change.finish()
        old = self.stack.currentWidget()
        if old is page:
            return
        if REDUCED or old is None or not self.isVisible():
            self.stack.setCurrentWidget(page)
            self.title_page_shown(page)
            self.update_sky()
            return
        self.page_change = PageChange(self, old, page)

    def page_changed(self, change):
        if self.page_change is change:
            self.page_change = None
            waiting, self.after_page_change = self.after_page_change, []
            for call in waiting:
                call()

    def when_page_shows(self, call):
        """Runs call once the page change under way is over (at once when none is)."""
        if self.page_change is None:
            call()
        else:
            self.after_page_change.append(call)

    def title_page_shown(self, page):
        """The header for a page: its words, and none at all on the title screen."""
        names = {self.start_page: "Main menu", self.settings_page: "Settings", self.build_page: "Building your game",
                 self.done_page: "Result"}
        self.page_title.set_text(names.get(page, ""))
        on_title = page is not None and page is self.title_page
        # The title screen fills the whole window, as the game's screen does.
        self.layout().setContentsMargins(*((0, 0, 0, 0) if on_title else (30, 20, 30, 26)))
        self.layout().setSpacing(0 if on_title else 18)
        for widget in (self.page_title, self.music_button):
            if widget is not None:
                widget.setVisible(not on_title)
        self.refresh_motion_notice()

    def leave_title(self):
        """The title screen is over (its flash has faded): the main menu comes in, each part
        from its own side (Entrance), with the observatory's music."""
        if self.music is not None:
            self.music.play("observatory")
        if REDUCED or not self.isVisible():
            self.show_page(self.start_page)
            return
        if self.page_change is not None:
            self.page_change.finish()
        self.stack.setCurrentWidget(self.start_page)
        self.title_page_shown(self.start_page)
        self.update_sky()
        self.layout().activate()
        self.start_page.layout().activate()
        for row in self.start_page.findChildren(QWidget):
            if row.layout() is not None:
                row.layout().activate()
        self.entrance = Entrance(self, self.menu_parts())

    def menu_parts(self):
        """What comes in from where on the main menu (Entrance): the header, then the page."""
        parts = [(self.page_title, (-1, 0)), (self.music_button, (1, 0))]
        return parts + self.start_page.parts

    def back_to_title(self):
        """Back to Title Screen: the main menu's entrance played backwards, each part going back
        to its side while the observatory's music fades out over as long, then the title screen
        from its start (restart_title). The sky turns on from where it is."""
        if self.title_page is None or self.building:
            return
        if self.entrance is not None:
            self.entrance.finish()
        if self.page_change is not None:
            self.page_change.finish()
        if REDUCED or not self.isVisible():
            if self.music is not None:
                self.music.fade_out(1)
            self.restart_title()
            return
        self.start_page.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)  # no clicks while it goes
        self.entrance = Entrance(self, self.menu_parts(), reverse=True, then=self.restart_title)
        if self.music is not None:
            self.music.fade_out(int(self.entrance.total * 60))

    def restart_title(self):
        """A new title screen in place of the one that was left: its sequence from the start,
        with its music (TitlePage plays it on its first frame). The title's sky is not made
        again, so it keeps turning from where it was."""
        self.start_page.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, False)
        old = self.title_page
        try:
            page = TitlePage(self)
        except Exception as error:  # noqa: BLE001 - the main menu stays
            B.log(f"No title screen: {error!r}")
            return
        page.finished.connect(self.leave_title)
        self.title_page = page
        self.stack.insertWidget(0, page)
        self.stack.setCurrentWidget(page)
        self.title_page_shown(page)
        self.update_sky()
        page.setFocus()
        if old is not None:
            old.timer.stop()
            self.stack.removeWidget(old)
            old.deleteLater()

    def show_start(self):
        self.show_page(self.start_page)

    def show_settings(self):
        self.settings_page.load()
        self.show_page(self.settings_page)

    def start_build(self):
        if self.building:
            return
        self.building = True
        self.job = "build"
        self.asking = 0
        self.mode.set_text("Building", CYAN)
        self.answered_at = now()  # the first question comes as the build starts
        self.build_page.reset()
        self.show_page(self.build_page)
        if self.runs:
            B.log(f"\n---- Build {self.runs + 1}, {time.strftime('%Y-%m-%d %H:%M:%S')} ----")
        self.runs += 1
        self.worker = BuildProcess(self.bridge, "build")

    def on_said(self, text):
        if self.job == "setup" and self.setup_card is not None:
            self.setup_card.on_said(text)
        else:
            self.build_page.on_said(text)

    def on_step(self, number, total, title):
        self.build_page.on_step(number, total, title)

    def on_progress(self, name, percent, detail):
        if self.job == "setup" and self.setup_card is not None:
            self.setup_card.on_progress(name, percent, detail)
        else:
            self.build_page.on_progress(name, percent, detail)

    def on_asked(self, request):
        if not self.isActiveWindow():
            QApplication.alert(self)  # the taskbar button flashes: the build waits for an answer
        self.asking += 1
        # A question asked as the build starts waits for the build page to be in place.
        self.when_page_shows(lambda: self.overlay.open(QuestionCard(request, self)))

    def on_finished(self, code):
        self.building = False
        QTimer.singleShot(1500, self.update_program_icon)  # the disc image may be another one now
        if self.job == "setup":
            self.setup_finished(code)
            return
        self.last_code = code
        if SKIN is None:
            QTimer.singleShot(1200, self.try_skin)  # a build that read the disc made the graphics too
        good = code == 0 and not self.build_page.copy_failed
        self.mode.set_text("Done" if good else "Stopped", GOOD if good else BAD)
        self.build_page.finish(code)
        self.done_page.show_result(code, self.build_page)
        QTimer.singleShot(900, lambda: self.show_page(self.done_page))
        QApplication.alert(self)

    def update_program_icon(self):
        """The builder program's own icon (builder_exeicon): the game's Power Star, as the window
        shows it, while the disc image it was read from is there; the program's first icon when
        the disc image is gone. Drawn here (OpenGL wants this thread), written on a thread."""
        import builder_exeicon
        follow_disc_icon()  # Linux, at once (the disc image may be another one, or gone, now)
        if builder_exeicon.program() is None or self.building:
            return
        disc, pictures = None, None
        if SKIN is not None and SKIN.has("star"):
            disc = builder_exeicon.disc_identity(B.Settings().get("disc_image"))
        if disc:
            from PySide6.QtCore import QBuffer, QIODevice
            big = SKIN._icon_image("star", 256, True)
            if big is None or big.isNull():
                return
            pictures = {}
            for side in (16, 20, 24, 32, 40, 48, 64, 128, 256):
                image = big if side == 256 else big.scaled(side, side, Qt.AspectRatioMode.KeepAspectRatio,
                                                           Qt.TransformationMode.SmoothTransformation)
                buffer = QBuffer()
                buffer.open(QIODevice.OpenModeFlag.WriteOnly)
                image.save(buffer, "PNG")
                pictures[side] = bytes(buffer.data())
        # Not a daemon: closing the window waits for the new program to be written.
        threading.Thread(target=builder_exeicon.apply, args=(B.settings_folder(), disc, pictures, B.log)).start()

    def setup_finished(self, code):
        card, self.setup_card = self.setup_card, None
        if card is not None:
            self.overlay.close_card(card)
        if code == 0 and self.try_skin():
            self.mode.set_text("Ready", GOOD)
            self.toast.show_text("The game's look is ready", GOOD)
        else:
            self.mode.set_text("Ready", GOOD)
            reason = card.errors[-1] if card is not None and card.errors else "The disc was not read."
            self.toast.show_text(reason[:110], BAD)

    def try_skin(self):
        """Loads the game's graphics when they are there now; True when the window changed."""
        global SKIN
        if SKIN is not None:
            return False
        skin = load_skin()
        if skin is None:
            return False
        SKIN = skin
        use_game_font()
        use_game_cursor()
        SKIN.preload_sounds()
        if SKIN.has("star"):
            QApplication.instance().setWindowIcon(game_icon())
            follow_disc_icon()
        self.setStyleSheet(style_sheet())
        current = self.stack.currentWidget()
        on_done = current is self.done_page
        if on_done:  # keep showing how the build ended; the new look comes with the next page
            return True
        self.make_pages()
        self.start_music()
        self.update()
        return True

    def show_page(self, page):
        self.show_page_plain(page)

    # ######################################################################################
    # # The game's music (builder_music.Music)                                             #
    # #                                                                                    #
    # # The Comet Observatory's music behind every page. The switch pauses it where it is #
    # # and goes on from that very place.                                                 #
    # ######################################################################################
    def start_music(self):
        if SKIN is None or self.music is not None or not SKIN.manifest.get("music"):
            return
        try:
            import builder_music
            self.music = builder_music.Music(SKIN.folder, SKIN.manifest, B.log)
        except Exception as error:  # noqa: BLE001 - no music is no harm
            B.log(f"No music: {error!r}")
            return
        if not self.music.has("observatory"):
            self.music = None
            return
        self.mode.hide()
        self.music_button = IconButton("note", "Mute music")
        self.music_button.clicked.connect(self.toggle_music)
        self.header.addWidget(self.music_button)
        if self.stack.currentWidget() is self.title_page and self.title_page is not None:
            self.music_button.hide()  # the title screen plays its own music, with no header
        else:
            self.music.play("observatory")

    def toggle_music(self):
        if self.music is None:
            return
        paused = not self.music.paused
        self.music.set_paused(paused)
        self.music_button.set_icon("note", "forbid" if paused else None, "Play music" if paused else "Mute music")

    def resizeEvent(self, event):
        if self.entrance is not None:
            self.entrance.finish()  # the pages are laid out again
        self.counter.place()
        self.motion_notice.place()
        self.overlay.setGeometry(self.rect())
        self.halo_layer.setGeometry(self.rect())
        if self.sky is not None:
            self.sky.setGeometry(self.rect())
        if self.title_sky is not None:
            self.title_sky.setGeometry(self.rect())
        super().resizeEvent(event)
        self.geometry_changed()

    def closeEvent(self, event):
        if not self.building:
            self.save_window_geometry()
            event.accept()
            return
        event.ignore()

        def stop_now():
            self.save_window_geometry()
            B.log("The window was closed during the build.")
            if self.worker is not None:
                self.worker.kill()
            if B.LOG:
                B.LOG.flush()
            os._exit(1)
        self.overlay.open(ConfirmCard(self, "Stop the build and close?",
                                      "The build is still running. Closing now stops it halfway: the files made so "
                                      "far stay where they are, and the next build starts that step again.",
                                      "Stop and close", stop_now))


def run(builder):
    global B
    B = builder
    settings = apply_graphics_at_start()  # VSync and the card before any OpenGL drawing
    app = QApplication.instance() or QApplication(sys.argv[:1])
    app.setApplicationName("GalaxyCTR Builder")
    app.setDesktopFileName(DESKTOP_ID)  # Linux: the app id the desktop finds the icon by
    # No icon of the builder's own until the game's star is there (below): the system's (the
    # program's file on Windows, the desktop's generic one on Linux). The blue star is never
    # shown (owner, 26 Nov 2026).
    pick_fonts()
    global REDUCED, MOTION
    MOTION = graphics(settings, "graphics_motion")
    REDUCED = motion_still(MOTION)
    global SKIN
    SKIN = load_skin()
    follow_disc_icon()  # before any dialog asks for a missing disc image
    if (SKIN is None and builder_firststart.needed(B) and builder_game.L.Renderer.get() is not None
            and not os.environ.get("GALAXYCTR_DISC_LATER")):
        # The first start (or a builder that changed the game files it keeps), as the launcher's:
        # the disc in the system's own file dialog (no window before the game's graphics are
        # out), then the small window gets ready from it (builder_firststart.prepare).
        if builder_firststart.prepare(app, B):
            SKIN = load_skin()
    use_game_font()
    use_game_cursor()
    if SKIN is not None:
        SKIN.preload_sounds()
    if SKIN is not None and SKIN.has("star"):
        app.setWindowIcon(game_icon())
    follow_disc_icon()
    window = Window()
    window.show_where_saved()
    look = ", ".join(filter(None, ("the game's look" if SKIN else "the plain look",
                                   "the title screen's sky" if window.title_sky is not None else
                                   "the game's sky" if window.sky is not None else "",
                                   "reduced motion" if REDUCED else "")))
    B.log(f"The window opened with Qt {__import__('PySide6').__version__} ({app.platformName()}): {look}.")
    B.log(f"Graphics: {graphics(settings, 'graphics_fps')} frames a second (the screen shows "
          f"{builder_game.frame_rate():.0f}), VSync {graphics(settings, 'graphics_vsync')}, graphics card "
          f"setting '{graphics(settings, 'graphics_gpu')}', motion '{graphics(settings, 'graphics_motion')}'."
          f"\n{B.DETAIL}Drawing on {graphics_card_in_use() or 'an unknown graphics card'}.")
    window.counter.set_shown(graphics(settings, "graphics_counter") == "on")
    window.raise_()
    window.activateWindow()
    QTimer.singleShot(2000, window.update_program_icon)
    app.exec()
    return window.last_code
