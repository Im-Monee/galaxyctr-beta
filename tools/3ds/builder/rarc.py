"""RARC archives (.arc): converted to little-endian, with every file inside converted too.

Layout (read by JKRArchive, libs/JSystem/include/JSystem/JKernel/JKRArchive.hpp):
  0x00 header        8 x u32
  0x20 info block    6 x u32, 2 x u16, u32
       directories   u32 id, u32 nameOffset, u16 hash, u16 fileCount, u32 firstFile
       files         u16 id, u16 hash, u32 (flags << 24 | nameOffset), u32 dataOffset, u32 size, u32 pointer
       string table
  file data, in three groups: loaded to main RAM, to ARAM, or left on the disc

Files inside that are compressed stay compressed, as the game keeps them that way in memory
and its memory budgets depend on it: unchanged files keep their original data, converted ones
are compressed again. The flags/name word is a bitfield; the 3DS build declares it in
reverse order so that it converts as a plain u32.
"""

import struct

import yaz0
from binary import ConversionError, align, be_u16, be_u32

MAGIC = b"RARC"

FLAG_FILE = 0x01
FLAG_FOLDER = 0x02
FLAG_COMPRESSED = 0x04
FLAG_MRAM = 0x10
FLAG_ARAM = 0x20
FLAG_DVD = 0x40
FLAG_YAZ0 = 0x80


def is_rarc(data):
    return data[:4] == MAGIC


def convert(data, convert_file):
    """Returns the converted archive. convert_file(name, data) converts each file inside."""
    if not is_rarc(data):
        raise ConversionError("not a RARC archive")

    header_size = be_u32(data, 0x08)
    data_offset = be_u32(data, 0x0C)
    info = header_size

    dir_count = be_u32(data, info + 0x00)
    dir_offset = info + be_u32(data, info + 0x04)
    file_count = be_u32(data, info + 0x08)
    file_offset = info + be_u32(data, info + 0x0C)
    string_size = be_u32(data, info + 0x10)
    string_offset = info + be_u32(data, info + 0x14)
    next_file_id = be_u16(data, info + 0x18)
    file_id_is_index = be_u16(data, info + 0x1A)
    data_start = info + data_offset

    def name_at(offset):
        start = string_offset + offset
        return data[start : data.index(b"\0", start)].decode("cp932")

    # Read and convert the file contents.
    entries = []
    for i in range(file_count):
        entry = file_offset + i * 0x14
        file_id, name_hash, flags_name, offset, size = struct.unpack_from(">HHIII", data, entry)
        flags = flags_name >> 24
        name_offset = flags_name & 0xFFFFFF
        content = None

        if flags & FLAG_FILE:
            content = data[data_start + offset : data_start + offset + size]
            if yaz0.is_compressed(content):
                plain = yaz0.decompress(content)
                converted = convert_file(name_at(name_offset), plain)
                if converted != plain:
                    content = yaz0.compress(converted)
            else:
                content = convert_file(name_at(name_offset), content)

        entries.append([file_id, name_hash, flags, name_offset, offset, size, content])

    # Lay the data out again: main RAM files first, then ARAM, then disc-only files.
    def group(entry):
        flags = entry[2]
        return 0 if flags & FLAG_MRAM else 1 if flags & FLAG_ARAM else 2

    files = sorted((e for e in entries if e[2] & FLAG_FILE), key=lambda e: e[4])
    blob = bytearray()
    group_sizes = [0, 0, 0]
    for g in range(3):
        start = len(blob)
        for entry in files:
            if group(entry) != g:
                continue
            entry[4] = len(blob)
            entry[5] = len(entry[6])
            blob += entry[6]
            blob += bytes(align(len(blob), 32) - len(blob))
        group_sizes[g] = len(blob) - start

    # Write the converted archive. The tables keep their size, so offsets stay the same.
    out = bytearray(data[:data_start])
    struct.pack_into("<IIIIIIII", out, 0, struct.unpack_from(">I", data, 0)[0], data_start + len(blob), header_size, data_offset, len(blob), group_sizes[0], group_sizes[1], 0)
    struct.pack_into(
        "<IIIIIIHHI",
        out,
        info,
        dir_count,
        be_u32(data, info + 0x04),
        file_count,
        be_u32(data, info + 0x0C),
        string_size,
        be_u32(data, info + 0x14),
        next_file_id,
        file_id_is_index,
        be_u32(data, info + 0x1C),
    )

    for i in range(dir_count):
        directory = dir_offset + i * 0x10
        values = struct.unpack_from(">IIHHI", data, directory)
        struct.pack_into("<IIHHI", out, directory, *values)

    for i, (file_id, name_hash, flags, name_offset, offset, size, _) in enumerate(entries):
        entry = file_offset + i * 0x14
        struct.pack_into("<HHIIII", out, entry, file_id, name_hash, flags << 24 | name_offset, offset, size, 0)

    out += blob
    return bytes(out)
