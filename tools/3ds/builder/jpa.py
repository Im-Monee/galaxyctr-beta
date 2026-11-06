"""JParticle resources (.jpc, "JPAC2-10"), read by JPAResourceLoader.cpp.

Header: magic, version, u16 resource count, u16 texture count, u32 offset of the textures.
Each resource is an 8-byte header (u16 user index, u16 block count, three u8 counts) followed
by its blocks; each block starts with a u32 tag and u32 size, both compared as words, and its
fields are laid out as in the JPA*Data structs. Textures are TEX1 blocks holding a ResTIMG
whose pixels stay big-endian.
"""

from binary import ConversionError, be_u16, be_u32, swap16, swap16_array, swap32, swap32_array
from j3d import convert_texture_header


def convert_field(buf, b, size):
    # JPAFieldBlockData: flags, position, direction, then magnitudes and timings (f32), u8 cycle.
    swap32(buf, b + 0x08)
    swap32_array(buf, b + 0x0C, 13)


def convert_key(buf, b, size):
    # u8 id at 8, u8 key count at 9, u8 loop at 0xB, then count x 4 f32 (frame, value, tangents).
    count = buf[b + 9]
    swap32_array(buf, b + 0x0C, count * 4)


def convert_dynamics(buf, b, size):
    # JPADynamicsBlockData: flags, user work, 22 f32, 3 s16 rotation, 5 s16/u16, u8 rate step.
    swap32_array(buf, b + 0x08, 2)
    swap32_array(buf, b + 0x10, 22)
    swap16_array(buf, b + 0x68, 8)


def convert_base_shape(buf, b, size):
    # JPABaseShapeData (0x34), then optional tables: texture coordinate animation (10 f32),
    # texture index animation (u8) and colour keys (s16 frame + GXColor) at given offsets.
    flags = be_u32(buf, b + 0x08)
    prm_offset = be_u16(buf, b + 0x0C)
    env_offset = be_u16(buf, b + 0x0E)
    clr_flags = buf[b + 0x21]
    prm_keys = buf[b + 0x22]
    env_keys = buf[b + 0x23]

    swap32(buf, b + 0x08)
    swap16_array(buf, b + 0x0C, 2)
    swap32_array(buf, b + 0x10, 2)
    swap16(buf, b + 0x18)
    swap16(buf, b + 0x24)

    if flags & 0x01000000:
        swap32_array(buf, b + 0x34, 10)
    if clr_flags & 0x02:
        for i in range(prm_keys):
            swap16(buf, b + prm_offset + i * 6)
    if clr_flags & 0x08 and env_offset != prm_offset:
        for i in range(env_keys):
            swap16(buf, b + env_offset + i * 6)


def convert_extra_shape(buf, b, size):
    # JPAExtraShapeData: flags, 7 f32, 2 s16, 13 f32.
    swap32(buf, b + 0x08)
    swap32_array(buf, b + 0x0C, 7)
    swap16_array(buf, b + 0x28, 2)
    swap32_array(buf, b + 0x2C, 13)


def convert_child_shape(buf, b, size):
    # JPAChildShapeData: flags, 10 f32, 2 GXColor, f32, 2 s16, 2 u8, s16.
    swap32(buf, b + 0x08)
    swap32_array(buf, b + 0x0C, 10)
    swap32(buf, b + 0x3C)
    swap16_array(buf, b + 0x40, 2)
    swap16(buf, b + 0x46)


def convert_ex_tex_shape(buf, b, size):
    # JPAExTexShapeData: flags, 2x3 f32 indirect matrix, three s8.
    swap32(buf, b + 0x08)
    swap32_array(buf, b + 0x0C, 6)


def convert_texture_db(buf, b, size):
    # Texture indices of the resource (u16).
    swap16_array(buf, b + 0x08, (size - 8) // 2)


BLOCKS = {
    b"FLD1": convert_field,
    b"KFA1": convert_key,
    b"BEM1": convert_dynamics,
    b"BSP1": convert_base_shape,
    b"ESP1": convert_extra_shape,
    b"SSP1": convert_child_shape,
    b"ETX1": convert_ex_tex_shape,
    b"TDB1": convert_texture_db,
}


def convert(data):
    buf = bytearray(data)
    if buf[:8] != b"JPAC2-10":
        raise ConversionError("unsupported particle resource version")

    res_count = be_u16(buf, 0x08)
    tex_count = be_u16(buf, 0x0A)
    tex_offset = be_u32(buf, 0x0C)

    pos = 0x10
    for _ in range(res_count):
        block_count = be_u16(buf, pos + 2)
        swap16_array(buf, pos, 2)
        pos += 8
        for _ in range(block_count):
            tag = bytes(buf[pos : pos + 4])
            size = be_u32(buf, pos + 4)
            if tag not in BLOCKS or size < 8:
                raise ConversionError(f"unknown particle block {tag!r}")
            BLOCKS[tag](buf, pos, size)
            swap32_array(buf, pos, 2)
            pos += size

    pos = tex_offset
    for _ in range(tex_count):
        if bytes(buf[pos : pos + 4]) != b"TEX1":
            raise ConversionError("bad particle texture")
        size = be_u32(buf, pos + 4)
        convert_texture_header(buf, pos + 0x20)
        swap32_array(buf, pos, 2)
        pos += size

    # Version is compared as a word ('2-10'); counts and the texture offset are native.
    swap32(buf, 0x04)
    swap16_array(buf, 0x08, 2)
    swap32(buf, 0x0C)
    return bytes(buf)
