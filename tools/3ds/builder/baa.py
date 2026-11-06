"""JAudio2 audio archives ("AA_<", AudioRes/SMR.szs), read by JAUAudioArcInterpreter.

The archive is a list of commands, each a u32 code followed by its parameters, ending with
'>_AA'. Parameters are offsets into the same file, where the data lives.

Supported: the sound table (bst), the sound name table (bstn), the SE category limits (sect),
the dynamic sequence blocks (dsqb), the sound effect sequences (bsc), the stream file table
(bsft), the wave systems (ws) and the instrument banks (bnk, version 1). Other commands
(bank lists, sequences in the archive; SMG's SMR.szs has none) are dropped. The file keeps
its size and layout, so the offsets of the kept data stay valid.
"""

import struct

from binary import ConversionError, be_u32, swap16, swap16_array, swap32, swap32_array

# Number of u32 parameters of each command ('sect' has four u8 instead).
PARAMS = {
    "ws  ": 3,
    "bnk ": 2,
    "bl_<": 2,
    ">_bl": 0,
    "bsc ": 2,
    "bst ": 2,
    "bstn": 2,
    "bms ": 3,
    "bmsa": 1,
    "vbnk": 2,
    "dsqb": 1,
    "bsft": 1,
    "sect": 1,
}

# Wave systems and banks ("ws  ", "bnk "). They were dropped for a while because the stage load
# hung waiting for a wave archive: a disc read completion never came (the port's interrupt state
# was not kept per thread, fixed in platform/3ds/rvl/os_thread.cpp). False drops them again.
WAVE_BANKS = True
KEPT = {"bst ", "bstn", "sect", "dsqb", "bsc ", "bsft"} | ({"ws  ", "bnk "} if WAVE_BANKS else set())

TYPE_SE = 0x50
TYPE_BGM = 0x60
TYPE_STREAM = 0x70


# ##############################################################################################
# # Reading an audio archive                                                                   #
# #                                                                                            #
# # An audio archive is a list of commands, each naming a section and where it lies; the       #
# # sections the game reads are converted, the rest are left as they are.                      #
# ##############################################################################################
def read_commands(data):
    if data[:4] != b"AA_<":
        raise ConversionError("not an audio archive")

    commands = []
    pos = 4
    while True:
        code = data[pos : pos + 4].decode("ascii", "replace")
        pos += 4
        if code == ">_AA":
            return commands, pos
        if code not in PARAMS:
            raise ConversionError(f"unknown audio archive command {code!r}")
        count = PARAMS[code]
        params = [be_u32(data, pos + i * 4) for i in range(count)]
        commands.append((code, params, data[pos : pos + count * 4]))
        pos += count * 4


def convert_sound_table(buf, start):
    """BST: magic, 2 x u32, u32 root offset; root -> sections -> groups -> items.
    Offsets are relative to the table. An item offset word holds the type in its top byte."""
    root = start + be_u32(buf, start + 12)
    sections = [be_u32(buf, root + 4 + i * 4) for i in range(be_u32(buf, root))]
    swap32_array(buf, start, 4)
    swap32_array(buf, root, 1 + len(sections))

    for section_offset in sections:
        if section_offset == 0:
            continue
        section = start + section_offset
        groups = [be_u32(buf, section + 4 + i * 4) for i in range(be_u32(buf, section))]
        swap32_array(buf, section, 1 + len(groups))

        for group_offset in groups:
            if group_offset == 0:
                continue
            group = start + group_offset
            items = [be_u32(buf, group + 8 + i * 4) for i in range(be_u32(buf, group))]
            swap32_array(buf, group, 2 + len(items))

            for word in items:
                offset = word & 0xFFFFFF
                if offset == 0:
                    continue
                item = start + offset
                kind = (word >> 24) & 0xF0
                swap16(buf, item + 2)  # resource ID, audible switch or channel control
                if kind in (TYPE_SE, TYPE_BGM):
                    swap16(buf, item + 4)
                if kind == TYPE_BGM:
                    swap16(buf, item + 6)
                if kind == TYPE_STREAM:
                    swap32(buf, item + 4)


def convert_name_table(buf, start):
    """BSTN: like BST, but sections and groups have one more u32 before their offsets and
    items are strings."""
    root = start + be_u32(buf, start + 12)
    sections = [be_u32(buf, root + 4 + i * 4) for i in range(be_u32(buf, root))]
    swap32_array(buf, start, 4)
    swap32_array(buf, root, 1 + len(sections))

    for section_offset in sections:
        if section_offset == 0:
            continue
        section = start + section_offset
        groups = [be_u32(buf, section + 8 + i * 4) for i in range(be_u32(buf, section))]
        swap32_array(buf, section, 2 + len(groups))

        for group_offset in groups:
            if group_offset == 0:
                continue
            group = start + group_offset
            swap32_array(buf, group, 2 + be_u32(buf, group))


def convert_seq_collection(buf, start):
    """JAUSeqCollectionData: 'SC', u16 group count, u32 size, then u32 offsets of the groups.
    Each group is a u32 count and u32 offsets of sequences, whose bytecode stays big-endian."""
    if buf[start : start + 2] != b"SC":
        raise ConversionError("bad sequence collection")
    groups = struct.unpack_from(">H", buf, start + 2)[0]
    tables = struct.unpack_from(f">{groups}I", buf, start + 8)
    swap16(buf, start + 2)
    swap32(buf, start + 4)
    swap32_array(buf, start + 8, groups)
    for offset in tables:
        count = be_u32(buf, start + offset)
        swap32_array(buf, start + offset, count + 1)


def convert_stream_file_table(buf, start):
    """BSFT (JAUStreamFileTable): the characters 'bsft' (compared one by one, kept), s32 file
    count, then s32 offsets of the file paths, relative to the table."""
    if buf[start : start + 4] != b"bsft":
        raise ConversionError("bad stream file table")
    count = be_u32(buf, start + 4)
    swap32_array(buf, start + 4, 1 + count)


def convert_wave_system(buf, start):
    """WSYS (JASWSParser): header, wave archive bank 'WINF', wave archives (file name, waves),
    control group 'WBCT' -> scenes 'SCNE' -> 'CDF ' lists of control waves. Offsets are
    relative to the header. The .aw sample files they describe are kept as they are."""
    if buf[start : start + 4] != b"WSYS":
        raise ConversionError("bad wave system")
    done = set()

    def once(offset):
        if offset in done:
            return False
        done.add(offset)
        return True

    archive_bank = start + be_u32(buf, start + 0x10)
    ctrl_group = start + be_u32(buf, start + 0x14)
    swap32_array(buf, start + 4, 5)

    archives = [be_u32(buf, archive_bank + 8 + i * 4) for i in range(be_u32(buf, archive_bank + 4))]
    swap32_array(buf, archive_bank + 4, 1 + len(archives))
    for archive_offset in archives:
        archive = start + archive_offset
        if archive_offset == 0 or not once(archive):
            continue
        waves = [be_u32(buf, archive + 0x74 + i * 4) for i in range(be_u32(buf, archive + 0x70))]
        swap32_array(buf, archive + 0x70, 1 + len(waves))
        for wave_offset in waves:
            wave = start + wave_offset
            if wave_offset == 0 or not once(wave):
                continue
            # TWave: three u8 (kept), f32 rate, six u32, two s16.
            swap32_array(buf, wave + 4, 7)
            swap16_array(buf, wave + 0x20, 2)

    scenes = [be_u32(buf, ctrl_group + 0xC + i * 4) for i in range(be_u32(buf, ctrl_group + 8))]
    swap32_array(buf, ctrl_group + 8, 1 + len(scenes))
    for scene_offset in scenes:
        scene = start + scene_offset
        if scene_offset == 0 or not once(scene):
            continue
        ctrl = start + be_u32(buf, scene + 0xC)
        swap32(buf, scene + 0xC)
        if ctrl == start or not once(ctrl):
            continue
        ctrl_waves = [be_u32(buf, ctrl + 8 + i * 4) for i in range(be_u32(buf, ctrl + 4))]
        swap32_array(buf, ctrl + 4, 1 + len(ctrl_waves))
        for ctrl_wave in ctrl_waves:
            # Read whole (JSULoHalf of the u32): the wave id is its low half.
            if ctrl_wave != 0 and once(start + ctrl_wave):
                swap32(buf, start + ctrl_wave)


def convert_bank(buf, start):
    """IBNK version 1 (JASBNKParser::Ver1): header, then chunks ('ENVT' oscillator points,
    'OSCT' oscillators, 'LIST' offsets of 'Inst'/'Perc' entries, which point at 'Pmap',
    'Rand' and 'Sens' entries). Offsets are relative to the header. Words compared with
    character constants ('ENVT', 'Inst'...) are read as u32, so they are swapped too."""
    if buf[start : start + 4] != b"IBNK" or be_u32(buf, start + 12) != 1:
        raise ConversionError("bad instrument bank (only version 1 is supported)")
    size = be_u32(buf, start + 4)
    done = set()

    def once(offset):
        if offset in done:
            return False
        done.add(offset)
        return True

    def effect(offset):
        # 'Rand' {u8 type, f32 base, f32 range} or 'Sens' {u8 type, u8, u8, f32, f32}.
        at = start + offset
        if offset == 0 or not once(at):
            return
        if buf[at : at + 4] not in (b"Rand", b"Sens"):
            raise ConversionError(f"unknown instrument effect {bytes(buf[at:at + 4])!r}")
        swap32(buf, at)
        swap32_array(buf, at + 8, 2)

    def velocity_regions(at, count):
        # TVeloData: u8 (kept), u32 wave, f32 volume, f32 pitch.
        for k in range(count):
            swap32_array(buf, at + k * 16 + 4, 3)
        return at + count * 16

    def instrument(at):
        swap32(buf, at)
        at += 4
        osc_count = be_u32(buf, at)
        swap32_array(buf, at, 1 + osc_count)
        at += 4 + osc_count * 4
        effects = [be_u32(buf, at + 4 + j * 4) for j in range(be_u32(buf, at))]
        swap32_array(buf, at, 1 + len(effects))
        at += 4 + len(effects) * 4
        for offset in effects:
            effect(offset)
        regions = be_u32(buf, at)
        swap32(buf, at)
        at += 4
        for _ in range(regions):
            # High key in the top byte of a u32, then the velocity region count.
            velocity = be_u32(buf, at + 4)
            swap32_array(buf, at, 2)
            at = velocity_regions(at + 8, velocity)
        swap32_array(buf, at, 2)  # volume, pitch

    def percussion_map(at):
        # 'Pmap', f32 volume, f32 pitch, u8 pan, u16 release, effect count + offsets, regions.
        swap32_array(buf, at, 3)
        swap16(buf, at + 0xE)
        at += 0x10
        effects = [be_u32(buf, at + 4 + j * 4) for j in range(be_u32(buf, at))]
        swap32_array(buf, at, 1 + len(effects))
        at += 4 + len(effects) * 4
        for offset in effects:
            effect(offset)
        velocity = be_u32(buf, at)
        swap32(buf, at)
        velocity_regions(at + 4, velocity)

    def percussion(at):
        swap32(buf, at)
        maps = [be_u32(buf, at + 8 + j * 4) for j in range(be_u32(buf, at + 4))]
        swap32_array(buf, at + 4, 1 + len(maps))
        for offset in maps:
            if offset != 0 and once(start + offset):
                percussion_map(start + offset)

    # Chunks: u32 id, u32 size, data; the next chunk starts at (id + 0xB + size) & ~3.
    chunks = {}
    at = start + 0x20
    while at < start + size:
        chunk_id, chunk_size = bytes(buf[at : at + 4]), be_u32(buf, at + 4)
        chunks[chunk_id] = (at, chunk_size)
        at = (at + 0xB + chunk_size) & ~3
    for name in (b"ENVT", b"OSCT", b"LIST"):
        if name not in chunks:
            raise ConversionError(f"instrument bank without {name!r}")
    swap32_array(buf, start, 4)

    envt, envt_size = chunks[b"ENVT"]
    swap16_array(buf, envt + 8, envt_size // 2)  # oscillator points: s16 triples

    osct, _ = chunks[b"OSCT"]
    count = be_u32(buf, osct + 8)
    swap32(buf, osct + 8)
    for i in range(count):
        # TOsc: u32 id, u8 target (kept), f32, u32 table, u32 release table, f32 scale, f32.
        osc = osct + 12 + i * 0x1C
        swap32(buf, osc)
        swap32_array(buf, osc + 8, 5)

    lst, _ = chunks[b"LIST"]
    entries = [be_u32(buf, lst + 12 + i * 4) for i in range(be_u32(buf, lst + 8))]
    swap32_array(buf, lst + 8, 1 + len(entries))
    for offset in entries:
        at = start + offset
        if offset == 0 or not once(at):
            continue
        kind = bytes(buf[at : at + 4])
        if kind == b"Inst":
            instrument(at)
        elif kind == b"Perc":
            percussion(at)
        else:
            raise ConversionError(f"unknown bank entry {kind!r}")

    for chunk_id, (at, _) in chunks.items():
        swap32_array(buf, at, 2)


# ##############################################################################################
# # Converting an archive                                                                      #
# #                                                                                            #
# # Every kept section converted by its kind; describe lists them, for checks.                 #
# ##############################################################################################
def convert(data):
    commands, end = read_commands(data)
    buf = bytearray(data)

    # Convert the data of the kept commands while the file is still big-endian.
    converted = set()
    for code, params, _ in commands:
        if code in ("bst ", "bstn") and params[0] not in converted:
            converted.add(params[0])
            (convert_sound_table if code == "bst " else convert_name_table)(buf, params[0])
        if code == "bsft" and params[0] not in converted:
            converted.add(params[0])
            convert_stream_file_table(buf, params[0])
        if code == "bsc " and params[0] not in converted:
            converted.add(params[0])
            convert_seq_collection(buf, params[0])
        if code in ("ws  ", "bnk ") and params[1] not in converted:
            converted.add(params[1])
            (convert_wave_system if code == "ws  " else convert_bank)(buf, params[1])

    # Rewrite the command list in place with only the kept commands.
    out = bytearray(b"AA_<"[::-1])
    for code, params, raw in commands:
        if code not in KEPT:
            continue
        out += code.encode("ascii")[::-1]
        out += raw if code == "sect" else struct.pack(f"<{len(params)}I", *params)
    out += b">_AA"[::-1]
    if len(out) > end:
        raise ConversionError("audio archive command list grew")

    buf[: len(out)] = out
    buf[len(out) : end] = bytes(end - len(out))
    return bytes(buf)


def describe(data):
    commands, _ = read_commands(data)
    return [(code, params) for code, params, _ in commands]
