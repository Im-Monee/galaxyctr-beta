"""Whether the GalaxyCTR builder can show its window, and starting it.

galaxyctr_builder.main() asks available() first. The window itself is builder_window.py; this file
imports Qt only to find out, so that a builder program without Qt (the first ones were terminal
programs, and they still download each new release) falls back to the terminal cleanly.

  QT           True when PySide6 (Qt) is in the builder program, None when it is not
  UNAVAILABLE  why there is no window, for the log
"""

import os
import subprocess
import sys
from pathlib import Path

UNAVAILABLE = ""
try:
    from PySide6 import QtWidgets
    QT = True
except Exception as error:  # noqa: BLE001 - ImportError, or a Qt library that does not load
    QtWidgets = None
    QT = None
    UNAVAILABLE = f"Qt is not in this builder program ({error})"


# ##############################################################################################
# # Can a window open?                                                                         #
# #                                                                                            #
# # Windows and macOS always can. On Linux Qt needs a display and the system's X11 or Wayland  #
# # libraries, and when one is missing it ends the whole program instead of raising an error.  #
# # So on Linux a second copy of the program opens a window first (--gui-probe, answered by    #
# # the launcher, or by this file when run from the repository), and only its success counts.  #
# ##############################################################################################
def available():
    global UNAVAILABLE
    if QT is None:
        return False
    if os.environ.get("GALAXYCTR_GUI") == "0":
        UNAVAILABLE = "the launcher found no working display"
        return False
    if QtWidgets.QApplication.instance() is not None:
        return True  # the launcher already showed its window
    if sys.platform.startswith("linux"):
        if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
            UNAVAILABLE = "no display"
            return False
        if not probe():
            UNAVAILABLE = "Qt could not open a window on this display"
            return False
    return True


def probe():
    if getattr(sys, "frozen", False):
        command = [sys.executable, "--gui-probe"]
    else:
        command = [sys.executable, str(Path(__file__).resolve()), "--gui-probe"]
    try:
        return subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, timeout=60).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def gui_probe():
    """Opens Qt and closes it: exit code 0 when this system can show the window."""
    app = QtWidgets.QApplication(sys.argv[:1])
    window = QtWidgets.QWidget()
    window.show()
    app.processEvents()
    window.close()
    return 0


def run(builder):
    """Shows the window and runs builds from it; returns the last build's exit code."""
    import builder_window
    return builder_window.run(builder)


if __name__ == "__main__" and "--gui-probe" in sys.argv:
    sys.exit(gui_probe())
