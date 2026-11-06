"""J3D animations: .bck, .btk, .bpk, .brk, .btp, .bva (J3DAnmLoader.cpp, J3DAnimation.hpp).

Each block is a header of counts and offsets followed by tables. Tables have no element
count of their own: like in j3d.py, a table runs to the next table's offset (or the block end),
and every table holds a single element type.
"""

from binary import ConversionError, be_u16, be_u32, swap16, swap16_array, swap32, swap32_array
from j3d import convert_name_table, table_extents

J3D_ANIM_MAGIC = b"J3D1"

U8, U16, F32, NAMES, TEX_PATTERN, TEV_REG = range(6)


def is_j3d_animation(data):
    return data[:4] == J3D_ANIM_MAGIC


def convert_tables(buf, block, size, tables, counts):
    """tables: (header offset, table kind) pairs. counts: header offset -> element count for
    tables whose elements mix sizes."""
    starts = {slot: be_u32(buf, block + slot) for slot, _ in tables}
    ends = table_extents(buf, block, size, list(starts.values()))
    done = set()
    for slot, kind in tables:
        start = starts[slot]
        if not start or start in done:
            continue
        done.add(start)
        pos = block + start
        length = ends[start] - start
        if kind == U16:
            swap16_array(buf, pos, length // 2)
        elif kind == F32:
            swap32_array(buf, pos, length // 4)
        elif kind == NAMES:
            convert_name_table(buf, pos)
        elif kind == TEX_PATTERN:
            # J3DAnmTexPatternFullTable (8): u16 max frame, u16 offset, u8 texture, pad, u16.
            for i in range(counts[slot]):
                swap16_array(buf, pos + i * 8, 2)
                swap16(buf, pos + i * 8 + 6)
        elif kind == TEV_REG:
            # J3DAnmCRegKeyTable (0x1C): 4 x J3DAnmKeyTableBase (3 x u16), u8 register, pad.
            for i in range(counts[slot]):
                swap16_array(buf, pos + i * 0x1C, 12)
    for slot, _ in tables:
        swap32(buf, block + slot)


def convert_ank1(buf, block, size):
    # u8 attribute, u8 rotation shift, s16 frames, 4 x u16 counts; tables of key ranges
    # (J3DAnmKeyTableBase, u16) and values: scale f32, rotation s16, translation f32.
    swap16_array(buf, block + 0x0A, 5)
    convert_tables(buf, block, size, [(0x14, U16), (0x18, F32), (0x1C, U16), (0x20, F32)], {})


def convert_ttk1(buf, block, size):
    swap16_array(buf, block + 0x0A, 5)
    swap16_array(buf, block + 0x34, 4)
    swap32(buf, block + 0x5C)  # texture matrix calculation type
    tables = [
        (0x14, U16), (0x18, U16), (0x1C, NAMES), (0x20, U8), (0x24, F32), (0x28, F32), (0x2C, U16), (0x30, F32),
        (0x3C, U16), (0x40, U16), (0x44, NAMES), (0x48, U8), (0x4C, F32), (0x50, F32), (0x54, U16), (0x58, F32),
    ]
    convert_tables(buf, block, size, tables, {})


def convert_pak1(buf, block, size):
    swap16_array(buf, block + 0x0C, 6)
    tables = [(0x18, U16), (0x1C, U16), (0x20, NAMES), (0x24, U16), (0x28, U16), (0x2C, U16), (0x30, U16)]
    convert_tables(buf, block, size, tables, {})


def convert_trk1(buf, block, size):
    counts = {0x20: be_u16(buf, block + 0x0C), 0x24: be_u16(buf, block + 0x0E)}
    swap16_array(buf, block + 0x0A, 11)
    tables = [(0x20, TEV_REG), (0x24, TEV_REG), (0x28, U16), (0x2C, U16), (0x30, NAMES), (0x34, NAMES)]
    tables += [(slot, U16) for slot in range(0x38, 0x58, 4)]
    convert_tables(buf, block, size, tables, counts)


def convert_tpt1(buf, block, size):
    counts = {0x10: be_u16(buf, block + 0x0C)}
    swap16_array(buf, block + 0x0A, 3)
    convert_tables(buf, block, size, [(0x10, TEX_PATTERN), (0x14, U16), (0x18, U16), (0x1C, NAMES)], counts)


def convert_vaf1(buf, block, size):
    # J3DAnmVisibilityFullTable: u16 max frame, u16 offset; the values are u8.
    swap16_array(buf, block + 0x0A, 3)
    convert_tables(buf, block, size, [(0x10, U16), (0x14, U8)], {})


BLOCKS = {
    b"ANK1": convert_ank1,
    b"TTK1": convert_ttk1,
    b"PAK1": convert_pak1,
    b"TRK1": convert_trk1,
    b"TPT1": convert_tpt1,
    b"VAF1": convert_vaf1,
}


def convert(data):
    buf = bytearray(data)
    if not is_j3d_animation(buf):
        raise ConversionError("not a J3D animation")

    block_count = be_u32(buf, 0x0C)
    pos = 0x20
    for _ in range(block_count):
        kind = bytes(buf[pos : pos + 4])
        size = be_u32(buf, pos + 4)
        if size < 8 or pos + 8 > len(buf):
            raise ConversionError(f"bad J3D animation block {kind!r}")
        if kind not in BLOCKS:
            raise ConversionError(f"unsupported J3D animation block {kind!r}")

        # Some blocks claim a few bytes more than the file holds; only their tables matter.
        BLOCKS[kind](buf, pos, min(size, len(buf) - pos))
        swap32_array(buf, pos, 2)
        pos += size

    # File header: magic, type, size, block count, and the sound animation offset of some .bck.
    swap32_array(buf, 0, 4)
    swap32(buf, 0x1C)
    return bytes(buf)
