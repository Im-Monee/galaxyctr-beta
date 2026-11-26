"""The builder's first start: the disc, asked once, before the window opens.

The launcher calls prepare() before it checks for updates: while the builder has no game UI
folder yet (builder_assets.load), Windows' own file dialog asks for the disc image (ask_disc), then
a small window gets the builder ready from it and closes. The update check and everything after
it are then in the game's own look. The small window is plain only when no game graphics are
there at all (the very first start); when an older builder's are there (an update changed them),
it wears them (builder_window.paint_card). The disc chosen is kept (settings.txt, disc_image).
On the very first start the window waits unseen until the disc's layouts are out (about a fifth
of the read, the owner's choice: no plain look first), then shows in the game's look:
builder_assets.make_card writes the little it needs. A question or a problem shows it at once.

  disc known   when settings.txt names a disc image that still exists, or keeps an extracted
               disc (keep_files), nothing is asked: the window goes straight to getting ready
  later        "Later" opens the builder's window as it is; the start page offers the same
               step (galaxyctr_builder.run_setup) and the next start asks again

Getting ready runs in a process of its own (multiprocessing, as the converter's workers): the disc
is extracted until the files the window takes are out (galaxyctr_builder.extract_for_look), then
builder_assets.make writes them. That work holds Python's lock for whole seconds at a time; in its
own process it cannot stall this one, so the progress bar moves smoothly. The bar eases towards
the last value the process reported and keeps creeping a little between reports.
"""

import math
import multiprocessing
import queue
import shutil
import tempfile
import time
from pathlib import Path

from PySide6.QtCore import QEventLoop, QPointF, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QFont, QLinearGradient, QPainter, QPen
from PySide6.QtWidgets import QFileDialog, QWidget

import builder_game  # sentence_lines: the builder's words, a line per sentence

EXTRACT_SHARE = 0.86  # of the bar: extracting the disc; the rest: writing the window's files

INK = QColor(236, 243, 255)
MUTED = QColor(150, 166, 206)
CYAN = QColor(63, 216, 255)
BLUE = QColor(47, 123, 255)
BAD = QColor(255, 93, 115)


def needed(builder):
    """True when the window's game files are missing (or made by an older builder)."""
    try:
        import builder_assets
        missing = builder_assets.load(builder.game_ui_folder()) is None
        for line in builder_assets.PROBLEMS:
            builder.log(line)
        builder_assets.PROBLEMS.clear()
        return missing
    except Exception:  # noqa: BLE001
        return False


# ##############################################################################################
# # The work, in its own process                                                               #
# #                                                                                            #
# # Messages to the window: ("extract", bytes), ("card", folder), ("make", fraction),        #
# # ("log", text), ("done", None) or ("error", text).                                         #
# ##############################################################################################
def work(image, kept, messages, card=None):
    """card: a folder for the small window's game look (builder_assets.make_card), made as soon
    as the disc's layouts are out; None when the window has it already."""
    import builder_assets
    import galaxyctr_builder as B
    B.log = lambda text: messages.put(("log", text))  # this process has no log file of its own
    temp = None
    try:
        out = B.game_ui_folder()
        def try_card(files):
            nonlocal card
            if card and builder_assets.card_ready(files):
                folder, card = card, None
                try:
                    builder_assets.make_card(files, Path(folder))
                    messages.put(("card", folder))
                except Exception as error:  # noqa: BLE001 - the plain window shows
                    messages.put(("log", f"First start: no game look for the small window ({error!r})."))
                    messages.put(("card", None))

        if kept:
            files = Path(kept) / "original" / "files"
            try_card(files)
        else:
            image = Path(image)
            parent = image.parent if B.os.access(image.parent, B.os.W_OK) else None
            temp = Path(tempfile.mkdtemp(prefix="galaxyctr-ready-", dir=parent))
            if B.free_space(temp) < (5 << 30):
                raise B.BuildError(f"This needs about 5 GB free in {temp.parent}. {B.free_space(temp) >> 30} GB are free.")
            def extracted(done):
                messages.put(("extract", done))
                try_card(temp / "disc" / "files")

            B.extract_for_look(image, temp / "disc", extracted)
            files = temp / "disc" / "files"
        builder_assets.make(files, out, lambda step, done, total: messages.put(("make", done / max(1, total))))
        if builder_assets.load(out) is None:
            raise B.BuildError("The builder could not get ready. The details are in the log.")
        messages.put(("done", None))
    except B.BuildError as error:
        messages.put(("error", str(error)))
    except Exception as error:  # noqa: BLE001
        import traceback
        messages.put(("log", traceback.format_exc()))
        messages.put(("error", f"Something unexpected went wrong ({error})."))
    finally:
        if temp is not None:  # after "done": the window opens while the extracted disc is deleted
            shutil.rmtree(temp, ignore_errors=True)


# ##############################################################################################
# # The small window                                                                           #
# #                                                                                            #
# # Drawn like the launcher's (launcher.with_splash): a dark card with a cyan edge, the name,  #
# # a spinning ring while something runs, a line of text and a bar.                            #
# ##############################################################################################
class SmallButton(QWidget):
    clicked = Signal()

    def __init__(self, text, primary, parent):
        super().__init__(parent)
        self.text, self.primary, self.hover = text, primary, False
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        font = QFont()
        font.setPixelSize(13)
        font.setWeight(QFont.Weight.DemiBold)
        self.setFont(font)
        self.setFixedSize(self.fontMetrics().horizontalAdvance(text) + 40, 34)

    def enterEvent(self, event):
        self.hover = True
        self.update()

    def leaveEvent(self, event):
        self.hover = False
        self.update()

    def mouseReleaseEvent(self, event):
        if self.rect().contains(event.position().toPoint()):
            self.clicked.emit()

    def keyPressEvent(self, event):
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter, Qt.Key.Key_Space):
            self.clicked.emit()
            return
        super().keyPressEvent(event)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        box = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        if self.primary:
            fill = QLinearGradient(box.topLeft(), box.topRight())
            fill.setColorAt(0, BLUE.lighter(120 if self.hover else 100))
            fill.setColorAt(1, CYAN)
            p.setBrush(fill)
            p.setPen(Qt.PenStyle.NoPen)
        else:
            p.setBrush(QColor(255, 255, 255, 34 if self.hover else 16))
            p.setPen(QPen(QColor(63, 216, 255, 200 if self.hover else 110), 1.2))
        p.drawRoundedRect(box, box.height() / 2, box.height() / 2)
        if self.hasFocus():
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(QPen(QColor(160, 225, 255), 1.5))
            p.drawRoundedRect(box.adjusted(-0.5, -0.5, 0.5, 0.5), box.height() / 2, box.height() / 2)
        p.setPen(QColor(4, 14, 40) if self.primary else INK)
        p.drawText(box, Qt.AlignmentFlag.AlignCenter, self.text)


class FirstStart(QWidget):
    finished = Signal()

    def __init__(self, builder, image=None, kept=None, galaxy=False, card=None):
        super().__init__(None, Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint)
        self.B = builder
        self.galaxy = galaxy  # drawn in the game's look (builder_window.paint_card)
        self.card = card  # where the work writes that look, when there is none yet
        self.card_skin = False  # the look came from that folder (run() lets it go)
        self.waiting = False  # unseen until that look is there (reveal)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setWindowTitle("GalaxyCTR Builder")
        self.setAcceptDrops(True)
        self.resize(500, 240)
        self.state, self.text, self.error = "ask", "", ""
        self.shown, self.target = 0.0, 0.0
        self.process = self.messages = None
        self.started = time.monotonic()
        self.ready = False
        self.choose = SmallButton("Choose the disc image...", True, self)
        self.later = SmallButton("Later", False, self)
        self.choose.clicked.connect(self.pick)
        self.later.clicked.connect(self.close_later)
        self.place_buttons()
        self.timer = QTimer(self)
        self.timer.setInterval(16)
        self.timer.timeout.connect(self.tick)
        self.timer.start()
        if kept or image:
            self.start(image, kept)
        else:
            self.ask()

    def place_buttons(self):
        y = self.height() - 56
        self.later.move(self.width() - 38 - self.later.width(), y)
        self.choose.move(self.later.x() - 10 - self.choose.width(), y)

    # Asking.
    def reveal(self):
        """Shows the window in the middle of the screen, once."""
        self.waiting = False
        if self.isVisible():
            return
        from PySide6.QtWidgets import QApplication
        screen = QApplication.primaryScreen()
        if screen is not None:
            self.move(screen.availableGeometry().center() - self.rect().center())
        self.show()
        self.raise_()
        self.activateWindow()

    def ask(self, error=""):
        self.reveal()
        self.state, self.error = "ask", error
        self.text = "Choose your Super Mario Galaxy disc image (USA), or drag it here."
        self.choose.show()
        self.later.show()
        self.choose.setFocus()
        self.update()

    def pick(self):
        if self.state != "ask":
            return
        path, _ = QFileDialog.getOpenFileName(self, "Your Super Mario Galaxy disc image", "",
                                              "Wii disc images (*.iso *.wbfs *.rvz *.wia *.ciso *.gcz *.nfs);;All files (*)")
        if path:
            self.check(Path(path))

    def dragEnterEvent(self, event):
        if self.state == "ask" and event.mimeData().hasUrls():
            event.acceptProposedAction()

    def dropEvent(self, event):
        urls = event.mimeData().urls()
        if urls and urls[0].isLocalFile():
            self.check(Path(urls[0].toLocalFile()))

    def check(self, path):
        """Checks the image (nodtool, a second or two) in a thread; the window keeps moving."""
        import threading
        self.state, self.error, self.text = "check", "", "Checking the disc image..."
        self.choose.hide()
        self.later.hide()
        self.update()
        result = {}

        def run():
            result["problem"] = self.B.disc_problem(path)

        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        while thread.is_alive():
            from PySide6.QtWidgets import QApplication
            QApplication.processEvents(QEventLoop.ProcessEventsFlag.AllEvents, 16)
            time.sleep(0.008)
        if result.get("problem"):
            self.ask(result["problem"])
            return
        settings = self.B.Settings()
        settings.set("disc_image", str(path))
        self.B.log(f"First start: disc image {path}")
        follow_disc_icon(self.B)  # Linux: the star back at once when the look is already there
        self.start(path, None)

    # Getting ready.
    def start(self, image, kept):
        self.state, self.error = "ready", ""
        self.text = "Getting everything ready..."
        self.choose.hide()
        self.later.hide()
        self.shown = self.target = 0.0
        context = multiprocessing.get_context("spawn")
        self.messages = context.Queue()
        card = None if self.galaxy or not self.card else str(self.card)
        self.waiting = card is not None
        self.wait_since = time.monotonic()
        self.process = context.Process(target=work, args=(str(image) if image else None, str(kept) if kept else None,
                                                          self.messages, card), daemon=False)
        self.process.start()
        self.B.log(f"First start: getting ready from {'the kept files' if kept else image}")
        self.update()

    def tick(self):
        if self.messages is not None:
            while True:
                try:
                    kind, value = self.messages.get_nowait()
                except queue.Empty:
                    break
                except (EOFError, OSError):
                    break
                if kind == "extract":
                    self.target = max(self.target, EXTRACT_SHARE * min(1.0, value / self.B.LOOK_BYTES))
                elif kind == "make":
                    self.target = max(self.target, EXTRACT_SHARE + (1 - EXTRACT_SHARE) * value)
                elif kind == "log":
                    self.B.log(value)
                elif kind == "card":
                    self.wear(value)
                elif kind == "done":
                    self.target = 1.0
                    self.ready = True
                    self.messages = None
                    QTimer.singleShot(450, self.finish)
                    break
                elif kind == "error":
                    self.messages = None
                    self.B.log(f"First start failed: {value}")
                    self.ask(value)
                    break
            if self.messages is not None and self.process is not None and not self.process.is_alive() \
                    and self.messages.empty():
                self.messages = None
                self.ask("Getting ready stopped unexpectedly. Try again, or choose Later.")
        if self.waiting and time.monotonic() - self.wait_since > 90:
            self.reveal()  # a very slow disc: the plain window rather than nothing
        if self.state == "ready":
            # Ease towards what the work reported; between reports, creep on a little, so the bar
            # never stands still while the disc is read.
            step = (self.target - self.shown) * 0.08
            creep = 0.00035 if self.shown < min(0.985, self.target + 0.05) else 0.0
            self.shown = min(1.0, self.shown + max(step, creep))
        self.update()

    def wear(self, folder):
        """The game's look, from the folder the work wrote (builder_assets.make_card)."""
        import builder_game
        import builder_window
        try:
            if folder is None:
                return
            folder = Path(folder)
            manifest = __import__("json").loads((folder / "manifest.json").read_text(encoding="utf-8"))
            skin = builder_game.load(folder, manifest)
            if skin is not None and builder_window.SKIN is None:
                builder_window.SKIN = skin
                builder_window.pick_fonts()
                builder_window.use_game_font()
                self.galaxy = self.card_skin = True
        except Exception as error:  # noqa: BLE001 - the plain window
            self.B.log(f"First start: the small window keeps its plain look ({error!r}).")
        finally:
            self.reveal()

    def finish(self):
        self.timer.stop()
        self.close()
        self.finished.emit()

    def close_later(self):
        self.B.log("First start: the disc was left for later.")
        self.finish()

    # Drawing.
    def paintEvent(self, event):
        p = QPainter(self)
        if self.galaxy and self.state != "ask":
            import builder_window
            if builder_window.paint_card(p, self, self.text, self.shown if self.state == "ready" else None, self.error):
                return
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        box = QRectF(self.rect()).adjusted(2, 2, -2, -2)
        fill = QLinearGradient(box.topLeft(), box.bottomRight())
        fill.setColorAt(0, QColor(24, 34, 90, 245))
        fill.setColorAt(1, QColor(6, 8, 28, 245))
        edge = QLinearGradient(box.topLeft(), box.bottomRight())
        edge.setColorAt(0, QColor(63, 216, 255, 200))
        edge.setColorAt(1, QColor(140, 92, 255, 160))
        p.setBrush(fill)
        p.setPen(QPen(edge, 1.5))
        p.drawRoundedRect(box, 22, 22)
        t = time.monotonic()
        if self.state in ("check", "ready"):
            p.setPen(QPen(CYAN, 3, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            p.drawArc(QRectF(34, 38, 34, 34), int(-(t * 320) % 360 * 16), 110 * 16)
            name_x = 86
        else:
            name_x = 36
        heading = QFont()
        heading.setPixelSize(22)
        heading.setWeight(QFont.Weight.Bold)
        heading.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, 4)
        p.setFont(heading)
        p.setPen(INK)
        p.drawText(QPointF(name_x, 63), "GALAXYCTR")
        small = QFont()
        small.setPixelSize(13)
        p.setFont(small)
        p.setPen(MUTED)
        width = int(box.width()) - 70
        # A line per sentence, never broken inside one (builder_game.fitted_lines: smaller words
        # when a sentence does not fit), as everywhere in the builder.
        used, lines = builder_game.fitted_lines(small, self.text, width)
        p.setFont(used)
        p.drawText(QRectF(36, 92, width, 40), 0, "\n".join(lines[:2]))
        if self.error and self.state == "ask":
            p.setPen(BAD)
            used, lines = builder_game.fitted_lines(small, self.error, width)
            p.setFont(used)
            p.drawText(QRectF(36, 126, width, 40), 0, "\n".join(lines[:2]))
        if self.state in ("check", "ready"):
            track = QRectF(36, 160, box.width() - 70, 6)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(255, 255, 255, 26))
            p.drawRoundedRect(track, 3, 3)
            if self.state == "ready":
                part = QRectF(track.left(), track.top(), track.width() * self.shown, track.height())
            else:
                k = (math.sin(t * 2.2) + 1) / 2
                part = QRectF(track.left() + k * track.width() * 0.72, track.top(), track.width() * 0.28, track.height())
            bar = QLinearGradient(part.topLeft(), part.topRight())
            bar.setColorAt(0, BLUE)
            bar.setColorAt(1, CYAN)
            p.setBrush(bar)
            p.drawRoundedRect(part, 3, 3)


def follow_disc_icon(builder):
    """builder_window.follow_disc_icon, also before the window's own start (the launcher's first
    start)."""
    import builder_window
    if builder_window.B is None:
        builder_window.B = builder
    builder_window.follow_disc_icon()


def ask_disc(builder):
    """The disc image in Windows' own file dialog, checked (nodtool); asked again after a wrong
    one, with the reason. None when the dialog is closed (Later)."""
    from PySide6.QtWidgets import QApplication, QMessageBox
    follow_disc_icon(builder)  # Linux: no star while the disc image is asked for, or left for later
    folder = ""
    while True:
        path, _ = QFileDialog.getOpenFileName(None, "Choose your Super Mario Galaxy disc image (USA)", folder,
                                              "Wii disc images (*.iso *.wbfs *.rvz *.wia *.ciso *.gcz *.nfs);;All files (*)")
        if not path:
            builder.log("First start: the disc was left for later.")
            return None
        path = Path(path)
        folder = str(path.parent)
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        try:
            problem = builder.disc_problem(path)
        finally:
            QApplication.restoreOverrideCursor()
        if not problem:
            builder.Settings().set("disc_image", str(path))
            builder.log(f"First start: disc image {path}")
            follow_disc_icon(builder)
            return path
        QMessageBox.warning(None, "GalaxyCTR Builder", problem)


def prepare(app, builder):
    """The first start, before the launcher's update check: when the game's graphics are missing,
    the disc image (asked in Windows' dialog when settings.txt names none that still exists, and
    no kept files) and getting ready. True when the graphics are there afterwards. Closing the
    dialog leaves it for later: the builder's window then does not ask again this time."""
    import os
    if not needed(builder):
        return True
    settings = builder.Settings()
    kept = builder.kept_folder(settings)
    image = settings.get("disc_image")
    if not kept and not (image and Path(image).is_file()):
        if ask_disc(builder) is None:
            os.environ["GALAXYCTR_DISC_LATER"] = "1"
            return False
    return run(app, builder)


def run(app, builder):
    """Shows the small window until the builder is ready or the user chose Later. With an older
    builder's game graphics there, the window wears them."""
    import builder_window
    settings = builder.Settings()
    kept = builder.kept_folder(settings)
    image = settings.get("disc_image")
    image = image if image and Path(image).is_file() else None
    galaxy = False
    card = None
    if builder_window.SKIN is None:
        builder_window.B = builder
        old = builder_window.load_old_skin()
        if old is not None:
            builder_window.SKIN = old
            builder_window.pick_fonts()
            galaxy = builder_window.use_game_font() or True
        else:
            card = Path(tempfile.mkdtemp(prefix="galaxyctr-card-"))
    window = FirstStart(builder, image, kept, galaxy, card)
    loop = QEventLoop()
    window.finished.connect(loop.quit)
    if not window.waiting:
        window.reveal()
    loop.exec()
    if galaxy or window.card_skin:
        builder_window.SKIN = None  # the builder loads the new graphics itself
    if card is not None:
        shutil.rmtree(card, ignore_errors=True)
    return window.ready
