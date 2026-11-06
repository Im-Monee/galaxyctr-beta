"""Refreshes the converter modules embedded in tools/3ds/convert_smg_files.py (the testers'
one-file converter): _BUILDER_ZIP_B64 from tools/3ds/builder and _NATIVE_ZIP_B64 from
tools/3ds/native, the same files as before, zipped and base64-encoded in the script's layout.
Run it after changing any of those modules; --check only reports whether they are current.

    py tools/3ds/bundle_converter.py [--check]
"""
import base64
import io
import re
import sys
import zipfile
from pathlib import Path

# ##############################################################################################
# # What is bundled                                                                            #
# #                                                                                            #
# # convert_smg_files.py carries the converter's modules (tools/3ds/builder and                #
# # tools/3ds/native) as zip files in base64, so that one file is the whole converter. After a #
# # change to a module this script writes them in again; --check only says whether they are    #
# # current.                                                                                   #
# ##############################################################################################
TOOLS = Path(__file__).resolve().parent
SCRIPT = TOOLS / "convert_smg_files.py"
BUNDLES = {"_BUILDER_ZIP_B64": TOOLS / "builder", "_NATIVE_ZIP_B64": TOOLS / "native"}
# Modules the bundle must hold besides those it already has: far models (far.py evaluates the Wii
# combiners with tevcheck.gx_eval, which imports build_native).
REQUIRED = {"_NATIVE_ZIP_B64": ["far.py", "tevcheck.py", "build_native.py"]}


# ##############################################################################################
# # Rewriting the embedded literals                                                            #
# #                                                                                            #
# # Each bundle is a Python tuple of base64 lines in the script: found, rebuilt from the       #
# # module folder with the same file names, and put back in the same layout.                   #
# ##############################################################################################
def literal_span(text, var):
    start = text.index(var + " = (")
    end = text.index("\n)\n", start) + 3
    return start, end


def embedded_names(text, var):
    start, end = literal_span(text, var)
    data = base64.b64decode("".join(re.findall(r'"([A-Za-z0-9+/=]*)"', text[start:end])))
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        return [(i.filename, z.read(i.filename)) for i in z.infolist()]


def build_literal(var, folder, names, width):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name in names:
            z.writestr(name, (folder / name).read_bytes().replace(b"\r\n", b"\n"))
    text = base64.b64encode(buf.getvalue()).decode("ascii")
    lines = ['    "%s"' % text[i:i + width] for i in range(0, len(text), width)]
    return var + " = (\n" + "\n".join(lines) + "\n)\n"


def main():
    check = "--check" in sys.argv
    text = SCRIPT.read_text(encoding="utf-8")
    stale = []
    for var, folder in BUNDLES.items():
        files = embedded_names(text, var)
        names = [n for n, _ in files] + [n for n in REQUIRED.get(var, []) if n not in {m for m, _ in files}]
        # A module deleted from the folder leaves the bundle too.
        names = [n for n in names if (folder / n).is_file()]
        current = [(n, (folder / n).read_bytes().replace(b"\r\n", b"\n")) for n in names]
        if current != files:
            stale.append(var)
        if not check:
            start, end = literal_span(text, var)
            first = re.search(r'"([A-Za-z0-9+/=]*)"', text[start:end]).group(1)
            text = text[:start] + build_literal(var, folder, names, len(first)) + text[end:]
    if check:
        print("stale: " + ", ".join(stale) if stale else "current")
        return 1 if stale else 0
    with open(SCRIPT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("refreshed: " + (", ".join(stale) or "nothing changed"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
