"""Sound animations (.bas): a JAUSoundAnimation used in place (JAUSoundAnimator.hpp).

u16 sound count, pad, u32 control pointer (0 in files), then one 0x20-byte
JAUSoundAnimationSound per sound: u32 sound ID, f32 note-on, f32 note-off, f32 pitch,
u32 flags, then bytes (volume, pitch delta, play time, pan, volume delta, interval, ...)
and a final u32.
"""

from binary import ConversionError, be_u16, swap16, swap32, swap32_array


def convert(data):
    buf = bytearray(data)
    if len(buf) < 8:
        raise ConversionError("sound animation too small")
    count = be_u16(buf, 0)
    if 8 + count * 0x20 > len(buf):
        raise ConversionError("sound animation shorter than its sound count")

    swap16(buf, 0)
    swap32(buf, 4)
    for i in range(count):
        s = 8 + i * 0x20
        swap32_array(buf, s, 5)
        swap32(buf, s + 0x1C)
    return bytes(buf)
