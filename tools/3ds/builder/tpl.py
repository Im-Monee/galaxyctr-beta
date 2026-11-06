"""Texture palette libraries (.tpl), read through TPLBind/TPLGet (revolution/tpl.h).

  header:       u32 version (0x0020AF30), u32 count, u32 descriptor table offset
  descriptors:  u32 image header offset, u32 palette header offset
  image header: u16 height, u16 width, u32 format, u32 data offset, u32 wrapS, u32 wrapT,
                u32 minFilter, u32 magFilter, f32 LOD bias, u8 edgeLOD, minLOD, maxLOD, unpacked
  palette:      u16 count, u8 unpacked, u8 pad, u32 format, u32 data offset

Pixel and palette data stay in the Wii texture formats for the graphics layer to handle.
"""

from binary import ConversionError, be_u32, swap16_array, swap32, swap32_array

MAGIC = 0x0020AF30


def is_tpl(data):
    return len(data) >= 12 and be_u32(data, 0) == MAGIC


def convert(data):
    buf = bytearray(data)
    if not is_tpl(buf):
        raise ConversionError("not a TPL")

    count = be_u32(buf, 4)
    table = be_u32(buf, 8)
    swap32_array(buf, 0, 3)

    done = set()
    for i in range(count):
        descriptor = table + i * 8
        image = be_u32(buf, descriptor)
        palette = be_u32(buf, descriptor + 4)
        swap32_array(buf, descriptor, 2)

        # Several descriptors may share a header: convert each header once.
        if image and image not in done:
            done.add(image)
            swap16_array(buf, image, 2)
            swap32_array(buf, image + 4, 7)
        if palette and palette not in done:
            done.add(palette)
            swap16_array(buf, palette, 1)
            swap32_array(buf, palette + 4, 2)

    return bytes(buf)
