"""Audio data.

Sequence bytecode (JAudio2 BMS and the game's ME/rhythm sequences) stays big-endian: on the
3DS, JASSeqReader reads its operands byte by byte. Only the tables around it are converted.
"""

from binary import ConversionError, be_s32, be_u16, be_u32, swap16, swap16_array, swap32, swap32_array


# ##############################################################################################
# # One converter per audio file kind                                                          #
# #                                                                                            #
# # Each function converts one kind of the game's audio files (build_files.py picks it by      #
# # extension); its docstring gives the layout and the game code that reads it. Sample data    #
# # stays as it is: the DSP mixer (platform/3ds/rvl/dsp.cpp) reads it big-endian, as the Wii's #
# # DSP does.                                                                                  #
# ##############################################################################################
def convert_me_table(data):
    """metable.bmt (AudMePlayingParamsHolder::setResource):
    s32 count, s32 entries offset, s32 names offset, then count 8-byte entries
    (u8, u8, u8, u8, u16, u8, u8) and count s32 name offsets."""
    buf = bytearray(data)
    count = be_s32(buf, 0)
    entries = be_s32(buf, 4)
    names = be_s32(buf, 8)
    if count < 0 or entries + count * 8 > len(buf) or names + count * 4 > len(buf):
        raise ConversionError("bad ME table")

    swap32_array(buf, 0, 3)
    for i in range(count):
        swap16(buf, entries + i * 8 + 4)
    swap32_array(buf, names, count)
    return bytes(buf)


def convert_me_sequences(data):
    """*.bme (AudMeTable): s32 count, s32 start offsets[count], then sequence bytecode."""
    buf = bytearray(data)
    count = be_s32(buf, 0)
    if count < 0 or 4 + count * 4 > len(buf):
        raise ConversionError("bad ME sequence table")

    swap32_array(buf, 0, count + 1)
    return bytes(buf)


def convert_remix_sequences(data):
    """*.brs (AudRemixMgr::setRemixSeqResource): only 32-bit values."""
    buf = bytearray(data)
    if len(buf) % 4:
        raise ConversionError("remix sequence size is not a multiple of 4")

    swap32_array(buf, 0, len(buf) // 4)
    return bytes(buf)


def convert_chord_table(data):
    """*.cit (AudChordTable::setChordTableResource):
    u32 relocated flag, "CITS" (compared as characters), u32 size, u16 chord count,
    u16 scale count, u32 chord offsets, u32 scale offsets; chords are bytes and each scale is
    two u32 offsets (up and down note lists)."""
    buf = bytearray(data)
    if buf[4:8] != b"CITS":
        raise ConversionError("not a chord table")

    chords = be_u16(buf, 12)
    scales = be_u16(buf, 14)
    scale_table = 16 + chords * 4
    scale_offsets = [be_u32(buf, scale_table + i * 4) for i in range(scales)]

    swap32(buf, 0)
    swap32(buf, 8)
    swap16(buf, 12)
    swap16(buf, 14)
    swap32_array(buf, 16, chords + scales)
    for offset in sorted(set(scale_offsets)):
        swap32_array(buf, offset, 2)
    return bytes(buf)



def convert_speaker_table(data):
    """spktable.bct (SpkTable::setResource): s32 count, s32 entries offset, s32 names offset,
    s32 relocated flag, then 8-byte entries (u16 wave, u8, u8 volume, u16 release, u16) and
    count s32 name offsets."""
    buf = bytearray(data)
    count = be_s32(buf, 0)
    entries = be_s32(buf, 4)
    names = be_s32(buf, 8)
    if count < 0 or entries + count * 8 > len(buf) or names + count * 4 > len(buf):
        raise ConversionError("bad speaker table")

    swap32_array(buf, 0, 4)
    for i in range(count):
        entry = entries + i * 8
        swap16(buf, entry)
        swap16(buf, entry + 4)
        swap16(buf, entry + 6)
    swap32_array(buf, names, count)
    return bytes(buf)


def convert_speaker_waves(data):
    """spkwave.csw (SpkWave): u32 size, u32 count, u32 wave offsets[count]; each wave is
    u32 size, u32 loop start, u32 loop end, then 16-bit PCM samples up to the next wave."""
    buf = bytearray(data)
    count = be_u32(buf, 4)
    if 8 + count * 4 > len(buf):
        raise ConversionError("bad speaker wave file")

    offsets = sorted(set(be_u32(buf, 8 + i * 4) for i in range(count)))
    swap32_array(buf, 0, count + 2)
    for i, offset in enumerate(offsets):
        end = offsets[i + 1] if i + 1 < len(offsets) else len(buf)
        swap32_array(buf, offset, 3)
        swap16_array(buf, offset + 12, (end - offset - 12) // 2)
    return bytes(buf)


def convert_stream(data):
    """*.ast (JASAramStream): a 0x40-byte "STRM" header, then blocks of a 0x20-byte "BLCK"
    header followed by the channels' sample data. Only the fields the game reads as
    multi-byte values are converted; the u8 format/bits fields (0x09, 0x0A) and the sample
    data (moved to ARAM as is, like wave data) stay as they are."""
    buf = bytearray(data)
    if buf[:4] != b"STRM" or len(buf) < 0x40:
        raise ConversionError("not an AST stream")
    channels = be_u16(buf, 0x0C)
    if channels == 0:
        raise ConversionError("AST stream without channels")

    swap32_array(buf, 0x00, 2)  # tag, data size
    swap16_array(buf, 0x0C, 2)  # channels, loop flag
    swap32_array(buf, 0x10, 6)  # sample rate, sample count, loop start, loop end, block size, 0x24

    pos = 0x40
    while pos + 0x20 <= len(buf):
        if buf[pos:pos + 4] != b"BLCK":
            raise ConversionError(f"AST block expected at {pos:#x}")
        size = be_u32(buf, pos + 4)
        swap32_array(buf, pos, 2)  # tag, block size per channel
        swap16_array(buf, pos + 8, 12)  # ADPCM history: 6 pairs of s16
        pos += 0x20 + size * channels
    # The last block's size can run a little past the end of the file (padding not stored).
    if pos < len(buf):
        raise ConversionError(f"AST stream ends at {pos:#x}, file is {len(buf):#x} bytes")
    return bytes(buf)
