"""NintendoWare layouts: screens (.brlyt) and their animations (.brlan).

Structures are in libs/nw4r/include/nw4r/lyt/resources.h and types.h. Block kinds and file
signatures are read as u32 (GetSignatureInt), so they are swapped like numbers. Vertex and
text colours are declared u32 and assigned to ut::Color as 0xRRGGBBAA numbers, so they are
swapped too; colours declared GXColor are bytes and stay as they are. Text is UTF-16.
"""

import struct

from binary import ConversionError, be_u16, be_u32, swap16, swap16_array, swap32, swap32_array

# Pane: header(8), 4 x u8, name[16], userData[8], translate(3f), rotate(3f), scale(2f), size(2f)
PANE_SIZE = 0x4C
PANE_FLOATS = 0x24


# ##############################################################################################
# # Layouts (.brlyt)                                                                           #
# #                                                                                            #
# # A layout is a header and a list of blocks (panes, pictures, text boxes, materials...);     #
# # each block's fields the game reads are swapped.                                            #
# ##############################################################################################
def convert_file_header(buf):
    """BinaryFileHeader: signature, u16 byteOrder, u16 version, u32 fileSize, u16 headerSize, u16 dataBlocks."""
    swap32(buf, 0)
    swap16(buf, 4)
    swap16(buf, 6)
    swap32(buf, 8)
    swap16(buf, 12)
    swap16(buf, 14)


def blocks(buf):
    """Yields (offset, kind, size) for each data block, reading the big-endian header."""
    header_size = be_u16(buf, 12)
    count = be_u16(buf, 14)
    offset = header_size
    for _ in range(count):
        kind = bytes(buf[offset : offset + 4]).decode("ascii", "replace")
        size = be_u32(buf, offset + 4)
        if size < 8 or offset + size > len(buf):
            raise ConversionError(f"bad block {kind!r} at 0x{offset:X}")
        yield offset, kind, size
        offset += size


def convert_pane(buf, block):
    swap32_array(buf, block + PANE_FLOATS, 10)


def convert_tex_coords(buf, offset, count):
    # Each texture coordinate set is a quad of four VEC2.
    swap32_array(buf, offset, count * 8)


def convert_material(buf, material):
    """res::Material followed by the variable parts listed in lyt_material.cpp."""
    swap16_array(buf, material + 20, 12)  # GXColorS10 tevCols[3]
    # name[20], GXColorS10 tevCols[3] (24 bytes), GXColor tevKCols[4] (16 bytes), then the bits.
    bits = be_u32(buf, material + 0x3C)
    swap32(buf, material + 0x3C)

    def field(start, length):
        return (bits >> start) & ((1 << length) - 1)

    offset = material + 0x40
    for _ in range(field(0, 4)):  # TexMap: u16 texIdx, u8 wrapS, u8 wrapT
        swap16(buf, offset)
        offset += 4
    swap32_array(buf, offset, field(4, 4) * 5)  # TexSRT: 5 floats
    offset += field(4, 4) * 20
    offset += field(8, 4) * 4  # TexCoordGen
    offset += field(25, 1) * 4  # ChanCtrl
    offset += field(27, 1) * 4  # material colour
    offset += field(12, 1) * 4  # TevSwapMode table
    swap32_array(buf, offset, field(13, 2) * 5)  # indirect TexSRT
    offset += field(13, 2) * 20
    offset += field(15, 3) * 4  # IndirectStage
    offset += field(18, 5) * 16  # TevStage
    offset += field(23, 1) * 4  # AlphaCompare
    offset += field(24, 1) * 4  # BlendMode
    return offset


def convert_layout(data):
    buf = bytearray(data)
    if buf[:4] != b"RLYT":
        raise ConversionError("not a layout")

    for block, kind, size in list(blocks(buf)):
        body = block + 8

        if kind == "lyt1":
            swap32_array(buf, body + 4, 2)  # layout size
        elif kind in ("txl1", "fnl1"):
            count = be_u16(buf, body)
            swap16(buf, body)
            for i in range(count):
                swap32(buf, body + 4 + i * 8)  # name offset
        elif kind == "mat1":
            count = be_u16(buf, body)
            swap16(buf, body)
            for i in range(count):
                material = block + be_u32(buf, body + 4 + i * 4)
                swap32(buf, body + 4 + i * 4)
                convert_material(buf, material)
        elif kind in ("pan1", "bnd1"):
            convert_pane(buf, block)
        elif kind == "pic1":
            convert_pane(buf, block)
            swap32_array(buf, block + PANE_SIZE, 4)  # vertex colours
            swap16(buf, block + PANE_SIZE + 16)  # materialIdx
            convert_tex_coords(buf, block + PANE_SIZE + 20, buf[block + PANE_SIZE + 18])
        elif kind == "txt1":
            convert_pane(buf, block)
            pos = block + PANE_SIZE
            text_bytes = be_u16(buf, pos + 2)
            text_offset = be_u32(buf, pos + 12)
            swap16_array(buf, pos, 4)  # textBufBytes, textStrBytes, materialIdx, fontIdx
            swap32(buf, pos + 12)
            swap32_array(buf, pos + 16, 2)  # text colours
            swap32_array(buf, pos + 24, 4)  # fontSize, charSpace, lineSpace
            swap16_array(buf, block + text_offset, text_bytes // 2)
        elif kind == "wnd1":
            convert_pane(buf, block)
            pos = block + PANE_SIZE
            frame_count = buf[pos + 16]
            content = block + be_u32(buf, pos + 20)
            frame_table = block + be_u32(buf, pos + 24)
            swap32_array(buf, pos, 4)  # inflation
            swap32(buf, pos + 20)
            swap32(buf, pos + 24)
            swap32_array(buf, content, 4)  # content vertex colours
            swap16(buf, content + 16)  # content materialIdx
            convert_tex_coords(buf, content + 20, buf[content + 18])
            for i in range(frame_count):
                frame = block + be_u32(buf, frame_table + i * 4)
                swap32(buf, frame_table + i * 4)
                swap16(buf, frame)  # frame materialIdx
        elif kind == "grp1":
            swap16(buf, body + 16)  # paneNum
        elif kind in ("pas1", "pae1", "grs1", "gre1"):
            pass
        else:
            raise ConversionError(f"unknown layout block {kind!r}")

        swap32(buf, block)
        swap32(buf, block + 4)

    convert_file_header(buf)
    return bytes(buf)


# ##############################################################################################
# # Layout animations (.brlan)                                                                 #
# #                                                                                            #
# # Animation blocks hold curves of keys; visibility and texture pattern keys are steps, the   #
# # others float keys with slopes.                                                             #
# ##############################################################################################
STEP_KEY_KINDS = {"RLVI", "RLTP"}


def convert_animation_block(buf, block):
    """pai1: per-pane and per-material animation curves."""
    file_count = be_u16(buf, block + 12)
    content_count = be_u16(buf, block + 14)
    contents_offset = be_u32(buf, block + 16)
    swap16(buf, block + 8)  # frameSize
    swap16_array(buf, block + 12, 2)
    swap32(buf, block + 16)

    swap32_array(buf, block + 20, file_count)  # file name offsets

    contents = block + contents_offset
    for i in range(content_count):
        content = block + be_u32(buf, contents + i * 4)
        swap32(buf, contents + i * 4)

        info_count = buf[content + 20]
        for j in range(info_count):
            info = content + be_u32(buf, content + 24 + j * 4)
            swap32(buf, content + 24 + j * 4)

            kind = bytes(buf[info : info + 4]).decode("ascii", "replace")
            target_count = buf[info + 4]
            swap32(buf, info)

            for k in range(target_count):
                target = info + be_u32(buf, info + 8 + k * 4)
                swap32(buf, info + 8 + k * 4)

                key_count = be_u16(buf, target + 4)
                keys = target + be_u32(buf, target + 8)
                swap16(buf, target + 4)
                swap32(buf, target + 8)

                if kind in STEP_KEY_KINDS:
                    for key in range(key_count):
                        swap32(buf, keys + key * 8)  # frame
                        swap16(buf, keys + key * 8 + 4)  # value
                else:
                    swap32_array(buf, keys, key_count * 3)  # frame, value, slope


def convert_animation(data):
    buf = bytearray(data)
    if buf[:4] != b"RLAN":
        raise ConversionError("not a layout animation")

    for block, kind, size in list(blocks(buf)):
        if kind == "pai1":
            convert_animation_block(buf, block)
        elif kind == "pat1":
            # tagOrder, groupNum, nameOffset, groupsOffset, startFrame, endFrame
            swap16_array(buf, block + 8, 2)
            swap32_array(buf, block + 12, 2)
            swap16_array(buf, block + 20, 2)
        elif kind == "pah1":
            swap32(buf, block + 8)
            swap16(buf, block + 12)
        else:
            raise ConversionError(f"unknown animation block {kind!r}")

        swap32(buf, block)
        swap32(buf, block + 4)

    convert_file_header(buf)
    return bytes(buf)
