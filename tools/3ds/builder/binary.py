"""Helpers for converting Wii (big-endian) data to the 3DS's little-endian layout."""

import struct


def be_u16(buf, offset):
    return struct.unpack_from(">H", buf, offset)[0]


def be_u32(buf, offset):
    return struct.unpack_from(">I", buf, offset)[0]


def be_s32(buf, offset):
    return struct.unpack_from(">i", buf, offset)[0]


def swap16(buf, offset):
    buf[offset], buf[offset + 1] = buf[offset + 1], buf[offset]


def swap32(buf, offset):
    buf[offset : offset + 4] = buf[offset : offset + 4][::-1]


def swap16_array(buf, offset, count):
    end = offset + count * 2
    data = buf[offset:end]
    data[0::2], data[1::2] = data[1::2], data[0::2]
    buf[offset:end] = data


def swap32_array(buf, offset, count):
    end = offset + count * 4
    data = buf[offset:end]
    b0, b1, b2, b3 = data[0::4], data[1::4], data[2::4], data[3::4]
    data[0::4], data[1::4], data[2::4], data[3::4] = b3, b2, b1, b0
    buf[offset:end] = data


def align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


class ConversionError(Exception):
    pass
