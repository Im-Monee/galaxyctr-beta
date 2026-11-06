"""Camera animations (.canm), read by CameraAnim::loadBin (src/Game/Camera/CameraAnim.cpp).

Header: "ANDO" and "CANM" (per-frame values) or "CKAN" (key frames), compared as bytes, then
six u32. The rest is made of 32-bit words only: the per-component table (count, offset and,
for key frames, interpolation type), the byte size of the value block and the f32 values.
"""

from binary import ConversionError, swap32_array


def convert(data):
    buf = bytearray(data)
    if buf[:4] != b"ANDO" or buf[4:8] not in (b"CANM", b"CKAN"):
        raise ConversionError("not a camera animation")
    words = (len(buf) - 8) // 4
    swap32_array(buf, 8, words)
    return bytes(buf)
