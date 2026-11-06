"""J3D models (.bdl, .bmd), material tables (.bmt) and textures (.bti).

Read by J3DModelLoader and its factories (libs/JSystem/include/JSystem/J3DGraphLoader). Data
the CPU reads is converted to little-endian. Data that only the GPU reads stays big-endian,
because the 3DS GX layer reads it as the Wii GPU would: vertex arrays (VTX1), display lists
(SHP1, MDL3) and texture pixels and palettes (TEX1, .bti).

Tables inside a block have no element count: their size runs to the next table's offset.
"""

from binary import ConversionError, swap16, swap16_array, swap32, swap32_array

# ##############################################################################################
# # Recognising a J3D file                                                                     #
# #                                                                                            #
# # A converted file starts with the magic reversed (2D3J), so a second conversion is          #
# # detected; restore_model turns a converted model back to the Wii layout.                    #
# ##############################################################################################
J3D_MAGIC = b"J3D2"
J3D_CONVERTED_MAGIC = b"2D3J"
_source_endian = "big"


def be_u16(data, offset):
    return int.from_bytes(data[offset : offset + 2], _source_endian)


def be_u32(data, offset):
    return int.from_bytes(data[offset : offset + 4], _source_endian)


def is_j3d_model(data):
    return (
        data[:4] == J3D_MAGIC
        and data[4:8] in (b"bdl4", b"bmd3", b"bmd2", b"bmt3")
    ) or (
        data[:4] == J3D_CONVERTED_MAGIC
        and data[4:8] in (b"4ldb", b"3dmb", b"2dmb", b"3tmb")
    )


def table_extents(buf, block, size, offsets):
    """Maps each table offset to the offset where the next table (or the block) starts."""
    starts = sorted(set(o for o in offsets if o))
    ends = {}
    for i, start in enumerate(starts):
        ends[start] = starts[i + 1] if i + 1 < len(starts) else size
    return ends


def convert_name_table(buf, pos):
    """ResNTAB: u16 count, u16 pad, then count x (u16 hash, u16 offset), then strings."""
    count = be_u16(buf, pos)
    swap16_array(buf, pos, 2)
    swap16_array(buf, pos + 4, count * 2)


def convert_texture_header(buf, pos):
    """ResTIMG (JUTTexture.hpp); the pixels and palette it points to stay big-endian."""
    swap16(buf, pos + 0x02)  # width
    swap16(buf, pos + 0x04)  # height
    swap16(buf, pos + 0x0A)  # palette entries
    swap32(buf, pos + 0x0C)  # palette offset
    swap16(buf, pos + 0x1A)  # LOD bias
    swap32(buf, pos + 0x1C)  # image offset


def offsets(buf, block, first, count):
    return [be_u32(buf, block + first + i * 4) for i in range(count)]


# ##############################################################################################
# # The blocks of a model                                                                      #
# #                                                                                            #
# # One converter per block (INF1 scene graph, VTX1 vertices, EVP1 and DRW1 skinning, JNT1     #
# # joints, SHP1 shapes, MAT3 materials, MDL3 display lists, TEX1 textures): every multi-byte  #
# # field the game reads is swapped to little-endian; the material display lists stay          #
# # big-endian, as GX data.                                                                    #
# ##############################################################################################

def convert_inf1(buf, block, size):
    hierarchy = be_u32(buf, block + 0x14)
    swap16(buf, block + 0x08)
    swap32_array(buf, block + 0x0C, 3)
    # Hierarchy: (u16 type, u16 index) pairs until type 0.
    pos = block + hierarchy
    while pos + 4 <= block + size:
        node_type = be_u16(buf, pos)
        swap16_array(buf, pos, 2)
        pos += 4
        if node_type == 0:
            break


def convert_vtx1(buf, block, size):
    table = offsets(buf, block, 0x08, 13)
    swap32_array(buf, block + 0x08, 13)
    # Attribute formats (GXVtxAttrFmtList): u32 attr, u32 count, u32 type, u8 frac, pad.
    pos = block + table[0]
    while pos + 16 <= block + size:
        attr = be_u32(buf, pos)
        swap32_array(buf, pos, 3)
        pos += 16
        if attr == 0xFF:
            break
    # The vertex arrays themselves are read by the GPU and stay big-endian.


def convert_evp1(buf, block, size):
    count = be_u16(buf, block + 0x08)
    mix_num, mix_index, mix_weight, inv_mtx = offsets(buf, block, 0x0C, 4)
    swap16(buf, block + 0x08)
    swap32_array(buf, block + 0x0C, 4)
    if count == 0:
        return

    total = sum(buf[block + mix_num + i] for i in range(count))
    swap16_array(buf, block + mix_index, total)
    swap32_array(buf, block + mix_weight, total)
    # Inverse bind matrices (3x4 floats) run to the end of the block.
    end = table_extents(buf, block, size, [mix_num, mix_index, mix_weight, inv_mtx])[inv_mtx]
    swap32_array(buf, block + inv_mtx, (end - inv_mtx) // 4)


def convert_drw1(buf, block, size):
    count = be_u16(buf, block + 0x08)
    flags, indices = offsets(buf, block, 0x0C, 2)
    swap16(buf, block + 0x08)
    swap32_array(buf, block + 0x0C, 2)
    swap16_array(buf, block + indices, count)


def convert_jnt1(buf, block, size):
    count = be_u16(buf, block + 0x08)
    init, index, names = offsets(buf, block, 0x0C, 3)
    swap16(buf, block + 0x08)
    swap32_array(buf, block + 0x0C, 3)

    # J3DJointInitData (0x40): u16 kind, bool, pad, scale (3 f32), rotation (3 s16), pad,
    # translation (3 f32), radius, min and max (7 f32).
    joints = (table_extents(buf, block, size, [init, index, names])[init] - init) // 0x40
    for i in range(joints):
        j = block + init + i * 0x40
        swap16(buf, j)
        swap32_array(buf, j + 0x04, 3)
        swap16_array(buf, j + 0x10, 3)
        swap32_array(buf, j + 0x18, 10)
    swap16_array(buf, block + index, count)
    if names:
        convert_name_table(buf, block + names)


def convert_shp1(buf, block, size):
    count = be_u16(buf, block + 0x08)
    table = offsets(buf, block, 0x0C, 8)
    init, index, names, vtx_desc, mtx_table, dl_data, mtx_init, draw_init = table
    swap16(buf, block + 0x08)
    swap32_array(buf, block + 0x0C, 8)
    ends = table_extents(buf, block, size, table)

    # J3DShapeInitData (0x28): u8, pad, 4 x u16, pad, radius, min, max (7 f32).
    for i in range((ends[init] - init) // 0x28):
        s = block + init + i * 0x28
        swap16_array(buf, s + 0x02, 4)
        swap32_array(buf, s + 0x0C, 7)
    swap16_array(buf, block + index, count)
    if names:
        convert_name_table(buf, block + names)

    # Vertex descriptions (GXVtxDescList): u32 attr, u32 type, until attr 0xFF.
    pos = block + vtx_desc
    while pos + 8 <= block + ends[vtx_desc]:
        swap32_array(buf, pos, 2)
        pos += 8

    swap16_array(buf, block + mtx_table, (ends[mtx_table] - mtx_table) // 2)
    # Shape matrix init: u16 index, u16 count, u32 first index.
    for i in range((ends[mtx_init] - mtx_init) // 8):
        m = block + mtx_init + i * 8
        swap16_array(buf, m, 2)
        swap32(buf, m + 4)
    # Shape draw init: u32 display list size, u32 offset. The display lists stay big-endian.
    swap32_array(buf, block + draw_init, (ends[draw_init] - draw_init) // 4)


# ##############################################################################################
# # Materials (MAT3)                                                                           #
# #                                                                                            #
# # MAT3 is a set of tables shared by the materials; each table's elements are converted by    #
# # their own layout, found from the materials that index them.                                #
# ##############################################################################################
# MAT3 table element sizes and how to convert one element, by offset slot in the block.
def _bytes(buf, pos):
    pass


def _u16x(n):
    return lambda buf, pos: swap16_array(buf, pos, n)


def _u32x(n):
    return lambda buf, pos: swap32_array(buf, pos, n)


def _material_init(buf, pos):
    # J3DMaterialInitData (0x14C): every field after the first 8 bytes is u16, except the
    # konst colour and alpha selections (0x9C-0xBC), which are bytes.
    swap16_array(buf, pos + 0x08, (0x9C - 0x08) // 2)
    swap16_array(buf, pos + 0xBC, (0x14C - 0xBC) // 2)


def _ind_init(buf, pos):
    # J3DIndInitData (0x138): only the three indirect matrices (6 f32 at 0x14 + i * 0x1C).
    for i in range(3):
        swap32_array(buf, pos + 0x14 + i * 0x1C, 6)


def _light(buf, pos):
    # Position and direction (6 f32), colour (bytes), attenuation (6 f32).
    swap32_array(buf, pos, 6)
    swap32_array(buf, pos + 0x1C, 6)


def _tex_mtx(buf, pos):
    # J3DTexMtxInfo (0x64): centre (3 f32), scale (2 f32), rotation (s16), translation
    # (2 f32), effect matrix (16 f32).
    swap32_array(buf, pos + 0x04, 5)
    swap16(buf, pos + 0x18)
    swap32_array(buf, pos + 0x1C, 2)
    swap32_array(buf, pos + 0x24, 16)


def _fog(buf, pos):
    # J3DFogInfo (0x2C): u16 centre, 4 f32, colour, 10 u16 adjustments.
    swap16(buf, pos + 0x02)
    swap32_array(buf, pos + 0x04, 4)
    swap16_array(buf, pos + 0x18, 10)


def _nbt_scale(buf, pos):
    swap32_array(buf, pos + 0x04, 3)


# Slot offset in the MAT3 block -> (element size, converter).
MAT3_TABLES = {
    0x0C: (0x14C, _material_init),
    0x10: (2, _u16x(1)),  # material IDs
    0x18: (0x138, _ind_init),
    0x1C: (4, _u32x(1)),  # cull modes
    0x20: (4, _bytes),  # material colours
    0x24: (1, _bytes),
    0x28: (8, _bytes),  # colour channels
    0x2C: (4, _bytes),  # ambient colours
    0x30: (0x34, _light),
    0x34: (1, _bytes),
    0x38: (4, _bytes),  # tex coord gens
    0x3C: (4, _bytes),
    0x40: (0x64, _tex_mtx),
    0x44: (0x64, _tex_mtx),  # post-transform texture matrices
    0x48: (2, _u16x(1)),  # texture numbers
    0x4C: (4, _bytes),  # TEV orders
    0x50: (8, _u16x(4)),  # TEV colours (GXColorS10)
    0x54: (4, _bytes),  # konst colours
    0x58: (1, _bytes),
    0x5C: (0x14, _bytes),  # TEV stages
    0x60: (4, _bytes),
    0x64: (4, _bytes),
    0x68: (0x2C, _fog),
    0x6C: (8, _bytes),  # alpha compare
    0x70: (4, _bytes),  # blend
    0x74: (4, _bytes),  # z mode
    0x78: (1, _bytes),
    0x7C: (1, _bytes),
    0x80: (0x10, _nbt_scale),
}


# J3DMaterialInitData index fields (offset, count, byte-sized) -> the MAT3 slot they index.
_MATERIAL_INDEXES = [
    (0x01, 1, True, 0x1C), (0x02, 1, True, 0x24), (0x03, 1, True, 0x34), (0x04, 1, True, 0x58),
    (0x05, 1, True, 0x78), (0x06, 1, True, 0x74), (0x07, 1, True, 0x7C), (0x08, 2, False, 0x20),
    (0x0C, 4, False, 0x28), (0x14, 2, False, 0x2C), (0x28, 8, False, 0x38), (0x48, 8, False, 0x40),
    (0x84, 8, False, 0x48), (0x94, 4, False, 0x54), (0xBC, 16, False, 0x4C), (0xDC, 4, False, 0x50),
    (0xE4, 16, False, 0x5C), (0x104, 16, False, 0x60), (0x124, 4, False, 0x64), (0x144, 1, False, 0x68),
    (0x146, 1, False, 0x6C), (0x148, 1, False, 0x70), (0x14A, 1, False, 0x80),
]


def _mat3_readers(buf, block, count, table):
    """MAT3 slots some material indexes (the Wii file, before any swap)."""
    init, ids = table.get(0x0C), table.get(0x10)
    if not init or not ids or count == 0:
        return set()
    n = max(be_u16(buf, block + ids + 2 * i) for i in range(count)) + 1
    out = set()
    for i in range(n):
        m = block + init + i * 0x14C
        for off, cnt, is_byte, slot in _MATERIAL_INDEXES:
            for k in range(cnt):
                v = buf[m + off + k] if is_byte else be_u16(buf, m + off + 2 * k)
                if v != (0xFF if is_byte else 0xFFFF):
                    out.add(slot)
    return out


def convert_mat3(buf, block, size):
    count = be_u16(buf, block + 0x08)
    slots = list(range(0x0C, 0x84, 4))
    table = [be_u32(buf, block + slot) for slot in slots]
    swap16(buf, block + 0x08)
    swap32_array(buf, block + 0x0C, len(slots))
    ends = table_extents(buf, block, size, table)
    readers = _mat3_readers(buf, block, count, dict(zip(slots, table)))

    done = set()
    for slot, start in zip(slots, table):
        if not start or start in done:
            continue
        # Empty tables may share one offset (OceanRotateStep: tex coords, texture numbers and TEV
        # orders): convert the bytes once, as the table the materials index (each converted in
        # turn swapped them as texture numbers, or twice, back to big-endian).
        sharing = [s for s, t in zip(slots, table) if t == start]
        if len(sharing) > 1:
            used = [s for s in sharing if s in readers]
            if not used:
                done.add(start)
                continue
            slot = used[0]
        done.add(start)
        if slot == 0x14:
            convert_name_table(buf, block + start)
            continue
        elem_size, convert = MAT3_TABLES[slot]
        if convert is _bytes:
            continue
        for i in range((ends[start] - start) // elem_size):
            convert(buf, block + start + i * elem_size)


def convert_mdl3(buf, block, size):
    """Material display lists (J3DMaterialDLBlock). The lists themselves stay big-endian."""
    count = be_u16(buf, block + 0x08)
    dl_init, patching, current_mtx, mode, unknown, names = offsets(buf, block, 0x0C, 6)
    swap16(buf, block + 0x08)
    swap32_array(buf, block + 0x0C, 6)
    swap32_array(buf, block + dl_init, count * 2)  # u32 offset, u32 size
    for i in range(count):
        swap16_array(buf, block + patching + i * 0x10, 6)
    swap32_array(buf, block + current_mtx, count * 2)
    if names:
        convert_name_table(buf, block + names)


def convert_tex1(buf, block, size):
    count = be_u16(buf, block + 0x08)
    textures, names = offsets(buf, block, 0x0C, 2)
    swap16(buf, block + 0x08)
    swap32_array(buf, block + 0x0C, 2)
    for i in range(count):
        convert_texture_header(buf, block + textures + i * 0x20)
    if names:
        convert_name_table(buf, block + names)


BLOCKS = {
    "INF1": convert_inf1,
    "VTX1": convert_vtx1,
    "EVP1": convert_evp1,
    "DRW1": convert_drw1,
    "JNT1": convert_jnt1,
    "SHP1": convert_shp1,
    "MAT3": convert_mat3,
    "MDL3": convert_mdl3,
    "TEX1": convert_tex1,
}


# ##############################################################################################
# # Converting a whole model                                                                   #
# #                                                                                            #
# # Every block in order, then the header; and the .bti files, one texture each.               #
# ##############################################################################################
def convert_model(data, source_endian="big"):
    global _source_endian
    previous_endian = _source_endian
    _source_endian = source_endian
    buf = bytearray(data)
    try:
        if not is_j3d_model(buf):
            raise ConversionError("not a J3D model")

        block_count = be_u32(buf, 0x0C)
        pos = 0x20
        for _ in range(block_count):
            kind_bytes = bytes(buf[pos : pos + 4])
            kind = (kind_bytes if source_endian == "big" else kind_bytes[::-1]).decode(
                "ascii", "replace"
            )
            size = be_u32(buf, pos + 4)
            if size < 8 or pos + size > len(buf):
                raise ConversionError(f"bad J3D block {kind!r}")
            if kind not in BLOCKS:
                raise ConversionError(f"unsupported J3D block {kind!r}")

            BLOCKS[kind](buf, pos, size)
            swap32_array(buf, pos, 2)
            pos += size

        # File header: magic, type, size, block count. The SVR3 section after it is not read.
        swap32_array(buf, 0, 4)
        return bytes(buf)
    finally:
        _source_endian = previous_endian


def restore_model(data):
    """Restore a project-converted little-endian J3D model to Wii layout."""
    return convert_model(data, source_endian="little")


def convert_bti(data):
    buf = bytearray(data)
    if len(buf) < 0x20:
        raise ConversionError("texture too small")
    convert_texture_header(buf, 0)
    return bytes(buf)
