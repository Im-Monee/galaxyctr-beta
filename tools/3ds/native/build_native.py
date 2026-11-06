"""Converts the game's files into native 3DS files, in a folder of their own.

    py tools/3ds/native/build_native.py [--game orig/RMGE01/files] [--out build/3ds/native] [--only NAME]
    py tools/3ds/native/build_native.py --discover --game smg/files --out smg/native

Walks the original (Wii) game files, opens every archive (Yaz0, RARC) and writes, for each J3D
model inside, an .n3d (n3d.py) at <out>/<archive path>/<model name>.n3d. The original files are
only read. A report of what was converted and what failed goes to <out>/report.txt.

Incremental: <out>/stamps.txt remembers, for each game file, its size and time and a hash of the
converter's own sources; a file whose stamp still matches is skipped (--all converts everything).
Already converted little-endian RARCs and J3D models are recognized; converted J3D tables are
restored before native extraction. Discovery writes <out>/missing-native.txt.
"""

import argparse
import hashlib
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "builder"))
import yaz0  # noqa: E402

import n3d  # noqa: E402
import j3d  # noqa: E402


# A RARC as the Wii stores it, or already converted to little-endian by the 3DS file converter.
RARC_MAGICS = (b"RARC", b"CRAR")


def rarc_files(data):
    """(name, content) of every file in a RARC archive, decompressed."""
    if data[:4] not in RARC_MAGICS:
        return
    byteorder = ">" if struct.unpack_from(">I", data, 0x08)[0] in (0x20, 0x24) else "<"
    info = struct.unpack_from(byteorder + "I", data, 0x08)[0]
    data_start = info + struct.unpack_from(byteorder + "I", data, 0x0C)[0]
    file_count = struct.unpack_from(byteorder + "I", data, info + 0x08)[0]
    file_offset = info + struct.unpack_from(byteorder + "I", data, info + 0x0C)[0]
    string_offset = info + struct.unpack_from(byteorder + "I", data, info + 0x14)[0]
    for i in range(file_count):
        _, _, flags_name, offset, size = struct.unpack_from(
            byteorder + "HHIII", data, file_offset + i * 0x14
        )
        if not (flags_name >> 24) & 0x01:
            continue
        start = string_offset + (flags_name & 0xFFFFFF)
        name = data[start : data.index(b"\0", start)].decode("cp932")
        content = data[data_start + offset : data_start + offset + size]
        if yaz0.is_compressed(content):
            content = yaz0.decompress(content)
        yield name, content


def is_converted_j3d(data):
    return (
        data[:4] == b"2D3J"
        and data[4:8] in (b"4ldb", b"3dmb", b"2dmb")
        and len(data) >= 0x20
        and int.from_bytes(data[8:12], "little") == len(data)
    )


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")
    parser = argparse.ArgumentParser()
    parser.add_argument("--game", default=os.path.join(root, "orig", "RMGE01", "files"))
    parser.add_argument("--out", default=os.path.join(root, "build", "3ds", "native"))
    parser.add_argument("--only", default="", help="convert only archives whose path contains this")
    parser.add_argument("--all", action="store_true", help="ignore the stamps: convert every file again")
    parser.add_argument("--discover", action="store_true", help="list native models missing from --out")
    args = parser.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    tool_hash = hashlib.sha1()
    for source in sorted(os.listdir(here)):
        if source.endswith(".py"):
            with open(os.path.join(here, source), "rb") as f:
                tool_hash.update(f.read())
    tool_hash = tool_hash.hexdigest()[:16]
    stamps_path = os.path.join(args.out, "stamps.txt")
    stamps = {}
    if os.path.exists(stamps_path) and not args.all:
        with open(stamps_path, encoding="utf-8") as f:
            for line in f:
                key, _, value = line.rstrip("\n").partition("\t")
                stamps[key] = value
    skipped = 0

    stats = n3d.Stats()
    failures = []
    written = 0
    out_bytes = 0
    in_bytes = 0
    start = time.time()
    if args.discover:
        models = 0
        missing = []
        for folder, _, names in os.walk(args.game):
            for name in sorted(names):
                path = os.path.join(folder, name)
                rel = os.path.relpath(path, args.game)
                if args.only and args.only.lower() not in rel.lower():
                    continue
                if not name.lower().endswith((".arc", ".bdl", ".bmd")):
                    continue
                with open(path, "rb") as f:
                    data = f.read()
                if yaz0.is_compressed(data):
                    data = yaz0.decompress(data)
                members = list(rarc_files(data)) if data[:4] in RARC_MAGICS else [(name, data)]
                for member, content in members:
                    if not n3d.is_j3d_model(content):
                        continue
                    models += 1
                    target = os.path.join(args.out, rel, os.path.splitext(member)[0] + ".n3d")
                    if not os.path.isfile(target):
                        missing.append(os.path.relpath(target, args.out))
        os.makedirs(args.out, exist_ok=True)
        report = os.path.join(args.out, "missing-native.txt")
        with open(report, "w", encoding="utf-8") as f:
            f.write("Missing native models: %d of %d discovered\n%s\n" %
                    (len(missing), models, "\n".join(missing)))
        print("Native models: %d missing of %d discovered" % (len(missing), models))
        print("Missing-model report: %s" % report)
        for target in missing:
            print(target)
        return

    for folder, _, names in os.walk(args.game):
        for name in sorted(names):
            path = os.path.join(folder, name)
            rel = os.path.relpath(path, args.game)
            if args.only and args.only.lower() not in rel.lower():
                continue
            if not name.lower().endswith((".arc", ".bdl", ".bmd")):
                continue
            info = os.stat(path)
            stamp = "%s %d %d" % (tool_hash, info.st_size, int(info.st_mtime))
            if stamps.get(rel) == stamp:
                skipped += 1
                continue
            failed_before = len(failures)
            with open(path, "rb") as f:
                data = f.read()
            if yaz0.is_compressed(data):
                data = yaz0.decompress(data)
            members = list(rarc_files(data)) if data[:4] in RARC_MAGICS else [(name, data)]
            animations = members
            parent = n3d.lod_parent_archive(name)
            if parent is not None and os.path.isfile(os.path.join(folder, parent)):
                with open(os.path.join(folder, parent), "rb") as f:
                    parent_data = f.read()
                if yaz0.is_compressed(parent_data):
                    parent_data = yaz0.decompress(parent_data)
                if parent_data[:4] in RARC_MAGICS:
                    animations = members + list(rarc_files(parent_data))
            for member, content in members:
                if not n3d.is_j3d_model(content):
                    continue
                target = os.path.join(args.out, rel, os.path.splitext(member)[0] + ".n3d")
                try:
                    if is_converted_j3d(content):
                        content = j3d.restore_model(content)
                    result = n3d.convert_model(content, stats, n3d.animated_registers(animations),
                                               n3d.patterned_materials(animations))
                except (n3d.N3DError, struct.error, KeyError, IndexError) as e:
                    failures.append("%s/%s: %s" % (rel, member, e))
                    continue
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as f:
                    f.write(result)
                written += 1
                in_bytes += len(content)
                out_bytes += len(result)
            if len(failures) == failed_before:
                stamps[rel] = stamp

    lines = [
        "models converted: %d (%.1f MB; the original models: %.1f MB), failed: %d, in %.0f s"
        % (written, out_bytes / 1048576, in_bytes / 1048576, len(failures), time.time() - start),
        "shapes %d, matrix groups %d, vertices %d, triangles %d, line/point draws skipped %d"
        % (stats.shapes, stats.groups, stats.vertices, stats.triangles, stats.skipped_prims),
        "materials %d, of which deliberately approximated by the native combiner: %d, drawn in several passes: %d" % (stats.materials, stats.inexact, stats.multipass),
        "",
    ] + failures
    lines[0] += ", %d game files unchanged (skipped)" % skipped
    os.makedirs(args.out, exist_ok=True)
    with open(stamps_path, "w", encoding="utf-8") as f:
        for key in sorted(stamps):
            f.write("%s\t%s\n" % (key, stamps[key]))
    with open(os.path.join(args.out, "report.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines[:3]))
    for line in failures[:10]:
        print(line)
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
