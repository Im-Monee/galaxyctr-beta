"""JMap tables (BCSV): the game's spreadsheets for objects, stages, galaxies and more.

Layout (read by JMapInfo, include/Game/Util/JMapInfo.hpp):
  header: s32 entryCount, s32 fieldCount, s32 dataOffset, u32 entrySize
  fields: u32 hash, u32 mask, u16 offset, u8 shift, u8 type
  data:   entryCount rows of entrySize bytes, then a string table

Values are read with the width of their type and then masked, so several fields can share
one word. Each distinct (offset, width) is therefore swapped exactly once per row.
"""

from binary import ConversionError, be_s32, be_u16, be_u32, swap16, swap32

TYPE_LONG = 0
TYPE_STRING = 1
TYPE_FLOAT = 2
TYPE_LONG_2 = 3
TYPE_SHORT = 4
TYPE_BYTE = 5
TYPE_STRING_PTR = 6

WIDTHS = {TYPE_LONG: 4, TYPE_STRING: 0, TYPE_FLOAT: 4, TYPE_LONG_2: 4, TYPE_SHORT: 2, TYPE_BYTE: 1, TYPE_STRING_PTR: 4}

EXTENSIONS = {".bcsv", ".tbl", ".banmt", ".bcam", ".pa"}


def looks_like_jmap(data):
    """Placement files in stage archives have no extension: recognize JMap tables by their header."""
    if len(data) < 16:
        return False

    entry_count = be_s32(data, 0)
    field_count = be_s32(data, 4)
    data_offset = be_s32(data, 8)
    entry_size = be_u32(data, 12)
    return (
        0 <= entry_count < 0x100000
        and 0 < field_count < 0x1000
        and data_offset == 16 + field_count * 12
        and data_offset + entry_count * entry_size <= len(data)
        and all(data[16 + i * 12 + 11] in WIDTHS for i in range(field_count))
    )


def convert(data):
    buf = bytearray(data)
    if len(buf) < 16:
        raise ConversionError("JMap too small")

    entry_count = be_s32(buf, 0)
    field_count = be_s32(buf, 4)
    data_offset = be_s32(buf, 8)
    entry_size = be_u32(buf, 12)

    if entry_count < 0 or field_count < 0 or data_offset > len(buf) or 16 + field_count * 12 > len(buf):
        raise ConversionError("not a JMap table")

    swaps = {}
    for i in range(field_count):
        field = 16 + i * 12
        offset = be_u16(buf, field + 8)
        field_type = buf[field + 11]
        if field_type not in WIDTHS:
            raise ConversionError(f"unknown JMap field type {field_type}")

        width = WIDTHS[field_type]
        if width >= 2:
            previous = swaps.get(offset)
            if previous is not None and previous != width:
                raise ConversionError(f"JMap fields of different widths share offset {offset}")
            swaps[offset] = width

        swap32(buf, field)
        swap32(buf, field + 4)
        swap16(buf, field + 8)

    for row in range(entry_count):
        base = data_offset + row * entry_size
        for offset, width in swaps.items():
            if base + offset + width > len(buf):
                raise ConversionError("JMap row out of range")
            if width == 4:
                swap32(buf, base + offset)
            else:
                swap16(buf, base + offset)

    for offset in (0, 4, 8, 12):
        swap32(buf, offset)

    return bytes(buf)
