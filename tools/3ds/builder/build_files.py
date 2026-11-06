"""Converts the Wii game files into the form the 3DS port reads.

Reads the extracted disc (orig/<version>/files) and writes build/3ds/files, which is copied
to sdmc:/smg/files. Archives are decompressed and every known format is converted to
little-endian. Files in formats without a converter yet are copied unchanged and listed in
build/3ds/files_report.txt, which is the to-do list for the builder.

Usage: python tools/3ds/builder/build_files.py [--version RMGE01] [--force] [--only PATH]
"""

import argparse
import hashlib
import json
import os
import sys
import traceback
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import audio
import baa
import bas
import bmg
import canm
import font
import j3d
import j3d_anim
import jmap
import jpa
import kcl
import lyt
import movie
import rarc
import texreduce
import tpl
import yaz0
from binary import ConversionError

# ##############################################################################################
# # What is converted, and how                                                                 #
# #                                                                                            #
# # Each file is converted by its extension (FORMATS); text is the same on both platforms;     #
# # editor leftovers are dropped; some files are used as they are (KEPT_AS_IS, with the        #
# # reason). The movies are their own step, since they need ffmpeg.                            #
# ##############################################################################################
# Folders the file conversion leaves to their own step: the movies (movie.py, with ffmpeg).
SKIPPED_DIRS = {"MovieData"}

BUILDER_DIR = Path(__file__).parent

# Converters chosen by file extension.
FORMATS = {
    ".brlyt": lyt.convert_layout,
    ".brlan": lyt.convert_animation,
    ".bmg": bmg.convert,
    ".brfnt": font.convert,
    ".tpl": tpl.convert,
    ".bme": audio.convert_me_sequences,
    ".brs": audio.convert_remix_sequences,
    ".cit": audio.convert_chord_table,
    ".bct": audio.convert_speaker_table,
    ".csw": audio.convert_speaker_waves,
    ".ast": audio.convert_stream,
    ".jpc": jpa.convert,
    ".kcl": kcl.convert,
    ".canm": canm.convert,
    ".bas": bas.convert,
}

# Plain text, identical on both platforms.
TEXT_EXTENSIONS = {".txt"}
TEXT_NAMES = {"collisionversion"}

# Leftovers of Nintendo's editors that the game never reads.
UNUSED_EXTENSIONS = {".ini", ".prj", ".xnim", ".dtp", ".dck", ".dvmat", ".dvw", ".csv#", ".backup"}

# Files used unchanged, and why.
KEPT_AS_IS = {
    ".gst": "ghost race data (read big-endian by GhostPacket)",
    ".bin": "Message Board picture (sent as is)",
    ".dat": "Mii resources (unused: no Miis on 3DS)",
    ".bnr": "Wii menu banner (unused)",
    ".rso": "HOME Menu module (HOME Menu not ported)",
    ".sel": "HOME Menu module (HOME Menu not ported)",
    ".csv": "HOME Menu settings (HOME Menu not ported)",
    ".aw": "wave samples (DSP-ADPCM bytes; the few PCM16 waves are swapped by the mixer)",
}


def builder_version():
    digest = hashlib.sha1()
    for source in sorted(BUILDER_DIR.glob("*.py")):
        digest.update(source.read_bytes())
    return digest.hexdigest()[:12]


# ##############################################################################################
# # The converter                                                                              #
# #                                                                                            #
# # Converts one file: an archive file by file inside it, a model with its textures reduced,   #
# # anything else by its format. Counts what it did for the log.                               #
# ##############################################################################################
class Converter:
    """Converts one file, recording which formats it could not handle."""

    def __init__(self):
        self.unknown = Counter()
        self.converted = Counter()
        self.sky = False  # inside a sky archive (texreduce.SKY_CMPR_LIMIT)

    def convert(self, name, data):
        if yaz0.is_compressed(data):
            data = yaz0.decompress(data)

        ext = os.path.splitext(name)[1].lower()

        if data[:4] == b"AA_<":
            self.converted["audio archive"] += 1
            return baa.convert(data)

        if rarc.is_rarc(data):
            self.converted[".arc"] += 1
            self.sky = name.lower().endswith("sky.arc")
            try:
                return rarc.convert(data, self.convert)
            finally:
                self.sky = False

        converter = FORMATS.get(ext)
        if converter is not None:
            self.converted[ext] += 1
            return converter(data)

        if j3d.is_j3d_model(data):
            self.converted[ext] += 1
            return j3d.convert_model(texreduce.reduce_model(data, sky=self.sky))

        if j3d_anim.is_j3d_animation(data):
            self.converted[ext] += 1
            return j3d_anim.convert(data)

        if ext == ".bti":
            self.converted[ext] += 1
            return j3d.convert_bti(data)

        # .bmt is both the ME table (audio) and a J3D material table, which has a signature.
        if ext == ".bmt" and data[:4] != b"J3D2":
            self.converted[ext] += 1
            return audio.convert_me_table(data)

        if tpl.is_tpl(data):
            self.converted[".tpl"] += 1
            return tpl.convert(data)

        if ext in jmap.EXTENSIONS or jmap.looks_like_jmap(data):
            self.converted[ext if ext in jmap.EXTENSIONS else "jmap"] += 1
            return jmap.convert(data)

        # Music sequences (AudioRes/Seqs/JaiSeq.arc) are bytecode the game reads big-endian.
        if ext == ".szs":
            self.converted["music sequence (kept big-endian)"] += 1
            return data

        if ext in UNUSED_EXTENSIONS:
            self.converted["unused editor file"] += 1
            return data

        if ext in KEPT_AS_IS:
            self.converted[KEPT_AS_IS[ext]] += 1
            return data

        if ext in TEXT_EXTENSIONS or name.lower() in TEXT_NAMES:
            self.converted["text"] += 1
            return data

        self.unknown[ext or name] += 1
        return data


# ##############################################################################################
# # Converting a tree                                                                          #
# #                                                                                            #
# # For the command line: every file of a tree, then the movies. convert_smg_files.py drives   #
# # the same Converter for the builder and the GUI.                                            #
# ##############################################################################################
def convert_one(job):
    src, dst = job
    converter = Converter()
    try:
        data = Path(src).read_bytes()
        out = converter.convert(Path(src).name, data)
        Path(dst).parent.mkdir(parents=True, exist_ok=True)
        Path(dst).write_bytes(out)
        return src, None, converter.converted, converter.unknown
    except (ConversionError, Exception) as error:
        return src, f"{type(error).__name__}: {error}\n{traceback.format_exc()}", converter.converted, converter.unknown


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", default="RMGE01")
    parser.add_argument("--out", default="build/3ds/files")
    parser.add_argument("--force", action="store_true", help="convert every file again")
    parser.add_argument("--only", help="only convert files whose path contains this text")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[3]
    os.chdir(root)
    src_root = Path("orig") / args.version / "files"
    out_root = Path(args.out)
    manifest_path = out_root.parent / "files_manifest.json"

    if not src_root.is_dir():
        sys.exit(f"{src_root} not found: extract the game with Dolphin first.")

    version = builder_version()
    manifest = {}
    if manifest_path.exists() and not args.force:
        manifest = json.loads(manifest_path.read_text())
    if manifest.get("_version") != version:
        manifest = {"_version": version}

    jobs = []
    for src in sorted(src_root.rglob("*")):
        rel = src.relative_to(src_root)
        if src.is_dir() and rel.parts[0] not in SKIPPED_DIRS:
            # Keep empty folders too, so the 3DS sees the same tree as the disc.
            (out_root / rel).mkdir(parents=True, exist_ok=True)
            continue
        if not src.is_file() or rel.parts[0] in SKIPPED_DIRS:
            continue
        if args.only and args.only.lower() not in rel.as_posix().lower():
            continue
        dst = out_root / rel
        stamp = f"{src.stat().st_size}:{src.stat().st_mtime_ns}"
        if manifest.get(str(rel)) == stamp and dst.exists():
            continue
        jobs.append((str(src), str(dst)))

    print(f"Converting {len(jobs)} files (builder {version})...")
    converted = Counter()
    unknown = Counter()
    failures = []

    with ProcessPoolExecutor() as pool:
        for src, error, done, missing in pool.map(convert_one, jobs, chunksize=4):
            converted.update(done)
            unknown.update(missing)
            rel = str(Path(src).relative_to(src_root))
            if error:
                failures.append((rel, error))
            else:
                manifest[rel] = f"{Path(src).stat().st_size}:{Path(src).stat().st_mtime_ns}"

    convert_movies(src_root, out_root, manifest, args.only)

    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=0))

    report = ["Converted:"]
    report += [f"  {ext:12} {count}" for ext, count in converted.most_common()]
    report += ["", "Not converted yet (copied unchanged):"]
    report += [f"  {ext:12} {count}" for ext, count in unknown.most_common()]
    report += ["", f"Failures: {len(failures)}"]
    for rel, error in failures:
        report += [f"  {rel}: {error.splitlines()[0]}"]
    (out_root.parent / "files_report.txt").write_text("\n".join(report) + "\n")

    print("\n".join(report[: 60]))
    for rel, error in failures[:3]:
        print(f"\n{rel}:\n{error}")


def convert_movies(src_root, out_root, manifest, only=None):
    """MovieData/*.thp to the 3DS movie format (movie.py), one at a time (ffmpeg uses every core)."""
    ffmpeg = movie.find_ffmpeg()
    for src in sorted((src_root / "MovieData").glob("*.thp")):
        rel = src.relative_to(src_root)
        if only and only.lower() not in rel.as_posix().lower():
            continue
        dst = out_root / rel
        stamp = f"{src.stat().st_size}:{src.stat().st_mtime_ns}"
        if manifest.get(str(rel)) == stamp and dst.exists():
            continue
        if ffmpeg is None:
            print(f"{rel}: ffmpeg not found, movie not converted (the game skips it)")
            continue
        result = movie.convert(str(src), str(dst), ffmpeg)
        manifest[str(rel)] = stamp
        print(f"{rel}: {result['pictures']} pictures, {result['bytes'] / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
