"""NintendoWare fonts (.brfnt), read by nw4r::ut::ResFont (src/nw4r/ut/ut_ResFont.cpp).

Structures are in libs/nw4r/include/nw4r/ut/fontResourceFormat.h. Pointers are stored as
offsets and relocated at load time. Glyph sheet pixels stay in the Wii texture format for the
graphics layer to handle.
"""

from binary import ConversionError, be_u16, be_u32, swap16, swap16_array, swap32
from lyt import blocks, convert_file_header


def convert(data):
    buf = bytearray(data)
    if buf[:4] != b"RFNT":
        raise ConversionError("not a font")

    for block, kind, size in list(blocks(buf)):
        body = block + 8

        if kind == "FINF":
            swap16(buf, body + 2)  # alterCharIndex
            swap32(buf, body + 8)  # glyph
            swap32(buf, body + 12)  # widths
            swap32(buf, body + 16)  # code map
        elif kind == "TGLP":
            swap32(buf, body + 4)  # sheetSize
            swap16_array(buf, body + 8, 6)  # sheetNum, format, row, line, width, height
            swap32(buf, body + 20)  # sheet image
        elif kind == "CWDH":
            swap16_array(buf, body, 2)  # first and last index
            swap32(buf, body + 4)  # next; the width table is bytes
        elif kind == "CMAP":
            swap16_array(buf, body, 4)  # first code, last code, method, reserved
            swap32(buf, body + 8)  # next
            # Every mapping method stores only u16 values after the header.
            swap16_array(buf, body + 12, (size - 20) // 2)
        elif kind == "GLGR":
            raise ConversionError("font glyph groups are not supported yet")
        else:
            raise ConversionError(f"unknown font block {kind!r}")

        swap32(buf, block)
        swap32(buf, block + 4)

    convert_file_header(buf)
    return bytes(buf)
