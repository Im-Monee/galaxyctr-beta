"""The builder program's own icon on Windows (the .exe File Explorer and the taskbar show).

The program is published with PyInstaller's icon: nothing of the game travels in it. Once the
builder has read the user's disc, it gives its own .exe the game's collected Power Star (the
pause menu's, drawn by builder_window from the game UI folder). When the disc image is gone the
program's first icon comes back; when another disc image is chosen, the star is drawn again from
the look made of it (galaxyctr_builder.refresh_look_disc).

A running program's file cannot be written. The program with the new icon is written to the
temporary folder, and a small hidden PowerShell waits for the builder to close and then moves it
into the program's place, so nothing but the program stays in its folder. The one-file program
is a Windows program (the PE part, with its resources) and PyInstaller's archive after it: the
icon is changed in the PE part alone (UpdateResource), and the archive is put back after it as
it was (the program finds it from the end of the file).

What the icon was is kept in the user's settings folder (galaxyctr_builder.settings_folder,
builder_exe_icon.json): the program's first icon, to put it back, and the icons this builder
made, to tell them from a program downloaded again.

  apply(...)        makes the program's file show the star, or its first icon
"""

import base64
import ctypes
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
from ctypes import wintypes
from pathlib import Path

RT_ICON, RT_GROUP_ICON = 3, 14
STATE_NAME = "builder_exe_icon.json"
OLD_ENDS = (".old", ".new")  # what older builders left next to the program
OLD_NAMES = ("builder_first_icon.ico", "builder_exe_icon.json")
WAITING = {}  # icon key -> the program file written this run, waiting for the builder to close


def program():
    """The running program's file, or None (not the Windows one-file program)."""
    if os.name != "nt" or not getattr(sys, "frozen", False):
        return None
    return Path(sys.executable).resolve()


# ##############################################################################################
# # Reading and writing a program's icon resources                                              #
# ##############################################################################################
def _kernel32():
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.LoadLibraryExW.restype = wintypes.HMODULE
    k.LoadLibraryExW.argtypes = (wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD)
    k.FindResourceExW.restype = wintypes.HANDLE
    k.FindResourceExW.argtypes = (wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD)
    k.LoadResource.restype = wintypes.HANDLE
    k.LoadResource.argtypes = (wintypes.HMODULE, wintypes.HANDLE)
    k.LockResource.restype = ctypes.c_void_p
    k.LockResource.argtypes = (wintypes.HANDLE,)
    k.SizeofResource.restype = wintypes.DWORD
    k.SizeofResource.argtypes = (wintypes.HMODULE, wintypes.HANDLE)
    k.FreeLibrary.argtypes = (wintypes.HMODULE,)
    k.EnumResourceNamesW.argtypes = (wintypes.HMODULE, ctypes.c_void_p, NAMES_PROC, ctypes.c_void_p)
    k.EnumResourceLanguagesW.argtypes = (wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p, LANGS_PROC,
                                         ctypes.c_void_p)
    k.BeginUpdateResourceW.restype = wintypes.HANDLE
    k.BeginUpdateResourceW.argtypes = (wintypes.LPCWSTR, wintypes.BOOL)
    k.UpdateResourceW.restype = wintypes.BOOL
    k.UpdateResourceW.argtypes = (wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD,
                                  ctypes.c_void_p, wintypes.DWORD)
    k.EndUpdateResourceW.restype = wintypes.BOOL
    k.EndUpdateResourceW.argtypes = (wintypes.HANDLE, wintypes.BOOL)
    return k


# Windows' resource enumeration callbacks. ctypes has WINFUNCTYPE only on Windows: elsewhere the
# module must still load (galaxyctr_builder.refresh_look_disc uses disc_identity during a build, and
# the window asks program(), which is None there), so they exist only on Windows.
if os.name == "nt":
    NAMES_PROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
    LANGS_PROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD,
                                    ctypes.c_void_p)


def _name(pointer):
    """A resource name as the enumeration gives it: a number, or a string (copied now)."""
    value = pointer or 0
    return value if value >> 16 == 0 else ctypes.wstring_at(value)


def _pointer(name):
    return ctypes.c_void_p(name) if isinstance(name, int) else ctypes.c_wchar_p(name)


def read_icon(path):
    """The first icon group of a program: (group name, language, [(number, entry, data)]) where
    entry is the 12 bytes an icon file and a group share (sizes, colours, planes, bits, bytes)."""
    k = _kernel32()
    module = k.LoadLibraryExW(str(path), None, 0x00000002 | 0x00000020)  # as data, as image resource
    if not module:
        raise OSError(f"cannot read {path}")
    try:
        groups = []
        k.EnumResourceNamesW(module, ctypes.c_void_p(RT_GROUP_ICON),
                             NAMES_PROC(lambda m, t, n, p: groups.append(_name(n)) or False), None)
        if not groups:
            return None
        group = groups[0]

        def language(kind, name):
            found = []
            k.EnumResourceLanguagesW(module, ctypes.c_void_p(kind), _pointer(name),
                                     LANGS_PROC(lambda m, t, n, lang, p: found.append(lang) or False), None)
            return found[0] if found else 0

        def load(kind, name, lang):
            found = k.FindResourceExW(module, ctypes.c_void_p(kind), _pointer(name), lang)
            if not found:
                raise OSError(f"no resource {kind}/{name}")
            size = k.SizeofResource(module, found)
            return ctypes.string_at(k.LockResource(k.LoadResource(module, found)), size)

        lang = language(RT_GROUP_ICON, group)
        table = load(RT_GROUP_ICON, group, lang)
        count = struct.unpack_from("<H", table, 4)[0]
        icons = []
        for i in range(count):
            entry = table[6 + 14 * i:6 + 14 * i + 12]
            number = struct.unpack_from("<H", table, 6 + 14 * i + 12)[0]
            icons.append((number, entry, load(RT_ICON, number, language(RT_ICON, number))))
        return group, lang, icons
    finally:
        k.FreeLibrary(module)


def program_icons(exe):
    """[(entry, data)] of the program's first icon group."""
    group = read_icon(exe)
    return [(entry, data) for _n, entry, data in group[2]] if group else []


def ico_bytes(icons):
    """An .ico file of [(entry, data)]."""
    head = struct.pack("<HHH", 0, 1, len(icons))
    offset = len(head) + 16 * len(icons)
    body = []
    for entry, data in icons:
        head += entry[:8] + struct.pack("<II", len(data), offset)
        offset += len(data)
        body.append(data)
    return head + b"".join(body)


def ico_icons(data):
    """[(entry, data)] of an .ico file."""
    count = struct.unpack_from("<H", data, 4)[0]
    icons = []
    for i in range(count):
        entry = data[6 + 16 * i:6 + 16 * i + 16]
        size, offset = struct.unpack_from("<II", entry, 8)
        icons.append((entry[:8] + struct.pack("<I", size), data[offset:offset + size]))
    return icons


def pngs_icons(pictures):
    """[(entry, data)] of PNG pictures {side: bytes} (Windows reads PNG icons of any size)."""
    return [(struct.pack("<BBBBHHI", side % 256, side % 256, 0, 0, 1, 32, len(png)), png)
            for side, png in sorted(pictures.items())]


def icons_key(icons):
    """The same for the same pictures, wherever they come from ([(entry, data)])."""
    h = hashlib.sha256()
    for entry, data in icons:
        h.update(entry[:12])
        h.update(data)
    return h.hexdigest()


def pe_end(data):
    """Where the Windows program ends in a one-file program (the archive follows), or None when
    the program is signed (a change would break the signature)."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    sections, optional = struct.unpack_from("<HxxxxxxxxxxxxH", data, pe + 6)
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    directories = pe + 24 + (96 if magic == 0x10B else 112)
    if struct.unpack_from("<II", data, directories + 8 * 4)[1]:  # IMAGE_DIRECTORY_ENTRY_SECURITY
        return None
    table = pe + 24 + optional
    end = 0
    for i in range(sections):
        size, offset = struct.unpack_from("<II", data, table + 40 * i + 16)
        end = max(end, offset + size)
    return end


def waiting_file(exe):
    """Where the program with its new icon waits for the builder to close."""
    tag = hashlib.sha256(str(exe).lower().encode("utf-8")).hexdigest()[:12]
    return Path(tempfile.gettempdir()) / "GalaxyCTR" / f"builder-{tag}.exe"


def write_program(exe, icons):
    """A copy of exe whose first icon group is made of icons ([(entry, data)]), written to the
    temporary folder. Returns its path."""
    data = exe.read_bytes()
    end = pe_end(data)
    if end is None:
        raise OSError("the program is signed or not a Windows program")
    group, lang, old = read_icon(exe)
    new = waiting_file(exe)
    new.parent.mkdir(parents=True, exist_ok=True)
    new.write_bytes(data[:end])
    k = _kernel32()
    handle = k.BeginUpdateResourceW(str(new), False)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    ok = False
    try:
        for number, _entry, _data in old:  # the old pictures go
            k.UpdateResourceW(handle, ctypes.c_void_p(RT_ICON), ctypes.c_void_p(number), lang, None, 0)
        table = struct.pack("<HHH", 0, 1, len(icons))
        for number, (entry, picture) in enumerate(icons, start=1):
            buffer = ctypes.create_string_buffer(picture, len(picture))
            if not k.UpdateResourceW(handle, ctypes.c_void_p(RT_ICON), ctypes.c_void_p(number), lang, buffer,
                                     len(picture)):
                raise ctypes.WinError(ctypes.get_last_error())
            table += entry[:12] + struct.pack("<H", number)
        buffer = ctypes.create_string_buffer(table, len(table))
        if not k.UpdateResourceW(handle, ctypes.c_void_p(RT_GROUP_ICON), _pointer(group), lang, buffer, len(table)):
            raise ctypes.WinError(ctypes.get_last_error())
        ok = True
    finally:
        k.EndUpdateResourceW(handle, not ok)
    if not ok:
        new.unlink()
        raise OSError("the icon could not be written")
    with open(new, "ab") as out:
        out.write(data[end:])  # PyInstaller's archive, as it was
    return new


# A hidden PowerShell waits for the builder (the one-file program and the Python it starts),
# moves the new program into its place, then has File Explorer read the icons again (it keeps
# the old icon for the same path otherwise).
SWAP_SCRIPT = r"""
$ErrorActionPreference = 'SilentlyContinue'
Wait-Process -Id {pids}
$done = $false
for ($i = 0; $i -lt 60 -and -not $done; $i++) {{
    try {{ Move-Item -LiteralPath '{new}' -Destination '{exe}' -Force -ErrorAction Stop; $done = $true }}
    catch {{ Start-Sleep -Milliseconds 500 }}
}}
if ($done) {{
    try {{
        Add-Type -Namespace GalaxyCTR -Name Shell -MemberDefinition '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, uint f, System.IntPtr a, System.IntPtr b);'
        [GalaxyCTR.Shell]::SHChangeNotify(0x08000000, 0x1000, [System.IntPtr]::Zero, [System.IntPtr]::Zero)
    }} catch {{}}
    & ie4uinit.exe -show
}}
"""


def swap_after_exit(exe, new):
    def quoted(path):
        return str(path).replace("'", "''")
    pids = ",".join(str(pid) for pid in sorted({os.getpid(), os.getppid()}))
    script = SWAP_SCRIPT.format(pids=pids, new=quoted(new), exe=quoted(exe))
    subprocess.Popen(["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                      "-WindowStyle", "Hidden", "-Command", script],
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     creationflags=0x08000000 | 0x00000200,  # CREATE_NO_WINDOW, CREATE_NEW_PROCESS_GROUP
                     close_fds=True)


# ##############################################################################################
# # What the icon should be                                                                    #
# ##############################################################################################
def disc_identity(image):
    """The disc image as the icon remembers it: its place, size and time; None when it is gone."""
    try:
        path = Path(image).resolve()
        st = path.stat()
    except (OSError, TypeError):
        return None
    return f"{path}|{st.st_size}|{int(st.st_mtime)}" if path.is_file() else None


def first_state(icons):
    return {"first": base64.b64encode(ico_bytes(icons)).decode("ascii"), "made": []}


def read_state(folder, exe):
    """builder_exe_icon.json, taking over (and deleting) what older builders kept next to the
    program."""
    try:
        state = json.loads((folder / STATE_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        state = {}
    old_first = exe.parent / OLD_NAMES[0]
    if not state.get("first") and old_first.is_file():
        try:
            first = ico_icons(old_first.read_bytes())
            state = first_state(first)
            current = icons_key(program_icons(exe))
            if current != icons_key(first):
                state["made"].append(current)  # the star an older builder gave it
        except (OSError, struct.error):
            pass
    for name in [exe.name + end for end in OLD_ENDS] + list(OLD_NAMES):
        try:
            (exe.parent / name).unlink()
        except OSError:
            pass
    return state


def apply(folder, disc, pictures, log=print):
    """Makes the program's icon the star of pictures ({side: PNG bytes}) while disc (the disc
    image's identity) is there, or its first icon when disc is None, from the moment the builder
    closes. Nothing happens when the program's file already shows that. folder: the user's
    settings folder, where the first icon is kept."""
    exe = program()
    if exe is None:
        return
    try:
        state = read_state(folder, exe)
        current = program_icons(exe)
        key = icons_key(current)
        first = ico_icons(base64.b64decode(state["first"])) if state.get("first") else None
        if first is None or key != icons_key(first) and key not in state.get("made", []):
            # A program this builder did not change (the first start, or a new download): its
            # icon is the first one, kept to be put back.
            state, first = first_state(current), current
        if disc is None:
            icons = first
        elif pictures:
            icons = pngs_icons(pictures)
        else:
            return
        wanted = icons_key(icons)
        if wanted == key and not WAITING or wanted in WAITING:
            return
        new = write_program(exe, icons)
        swap_after_exit(exe, new)
        WAITING.clear()
        WAITING[wanted] = new
        if wanted != icons_key(first) and wanted not in state["made"]:
            state["made"] = (state["made"] + [wanted])[-8:]
        folder.mkdir(parents=True, exist_ok=True)
        (folder / STATE_NAME).write_text(json.dumps(state), encoding="utf-8")
        log("The builder program's icon becomes " + ("the game's Power Star" if disc else "its first one")
            + " when the builder closes.")
    except Exception as error:  # noqa: BLE001 - only the icon: the builder works the same
        log(f"The builder program's icon could not be changed: {error!r}")
