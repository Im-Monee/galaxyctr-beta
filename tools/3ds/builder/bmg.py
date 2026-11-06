"""Game text (.bmg, "MESGbmg1"), read by MessageData in src/Game/System/MessageHolder.cpp.

Blocks: INF1 (message table), DAT1 (UTF-16 text), FLW1 (conversation flow), FLI1, MID1.

Text contains tags: the character 0x1A followed by u8 size (bytes including the 0x1A),
u8 group, u16 tag and parameters. The game reads the size and group as single bytes, the tag
as a u16 and the parameters with the width that the tag uses (MessageEditorMessageTag and
CustomTagProcessor), so each part is converted with its own width.
"""

from binary import ConversionError, be_u16, be_u32, swap16, swap16_array, swap32


def tag_params(buf, params, length, group, tag):
    """Swaps a tag's parameters (params = offset of the first parameter byte)."""
    if group == 1 and tag == 0 and length >= 2:  # wait: u16 frames
        swap16(buf, params)
    elif group in (2, 10):  # sound name, font name: UTF-16 string
        swap16_array(buf, params, length // 2)
    elif group in (6, 7):  # number, string: u32 slot (written at runtime) and u32 argument index
        for offset in range(0, length - 3, 4):
            swap32(buf, params + offset)
    elif group == 8 and tag == 0 and length >= 4:  # message reference
        swap32(buf, params)
    elif group == 255 and tag == 2 and length >= 1:  # ruby: u8 base length, then UTF-16 text
        swap16_array(buf, params + 1, (length - 1) // 2)
    # Everything else is read as bytes (colours, player name form) or has no parameters.


def convert_text(buf, start, end):
    pos = start
    while pos + 1 < end:
        char = be_u16(buf, pos)
        swap16(buf, pos)
        if char != 0x1A:
            pos += 2
            continue

        size = buf[pos + 2]
        group = buf[pos + 3]
        if size < 6 or pos + size > end:
            raise ConversionError(f"bad message tag at 0x{pos:X}")

        tag = be_u16(buf, pos + 4)
        swap16(buf, pos + 4)
        tag_params(buf, pos + 6, size - 6, group, tag)
        pos += size


def convert(data):
    buf = bytearray(data)
    if buf[:8] != b"MESGbmg1":
        raise ConversionError("not a message file")

    block_count = be_u32(buf, 0x0C)
    pos = 0x20
    for _ in range(block_count):
        kind = bytes(buf[pos : pos + 4]).decode("ascii", "replace")
        size = be_u32(buf, pos + 4)
        if size < 8 or pos + 8 > len(buf):
            raise ConversionError(f"bad message block {kind!r}")
        # The last block's size can count padding that is not in the file.
        end = min(pos + size, len(buf))

        if kind == "INF1":
            count = be_u16(buf, pos + 8)
            item_size = be_u16(buf, pos + 10)
            swap16_array(buf, pos + 8, 2)
            swap32(buf, pos + 12)
            for i in range(count):
                item = pos + 16 + i * item_size
                swap32(buf, item)  # text offset
                swap16(buf, item + 4)  # camera set ID; the rest are bytes
        elif kind == "DAT1":
            convert_text(buf, pos + 8, end)
        elif kind == "FLW1":
            node_count = be_u16(buf, pos + 8)
            branch_count = be_u16(buf, pos + 10)
            swap16_array(buf, pos + 8, 2)
            swap32(buf, pos + 12)
            nodes = pos + 16
            for i in range(node_count):
                node = nodes + i * 8
                swap16(buf, node + 2)
                if buf[node] == 3:  # event: u32 argument
                    swap32(buf, node + 4)
                else:  # message and branch: two u16
                    swap16_array(buf, node + 4, 2)
            swap16_array(buf, nodes + node_count * 8, branch_count)
        elif kind == "FLI1":
            # u16 count, u8 entry size; entries: u32 flow ID, u16, u16 node index
            count = be_u16(buf, pos + 8)
            entry_size = buf[pos + 10]
            swap16(buf, pos + 8)
            for i in range(count):
                entry = pos + 16 + i * entry_size
                if entry + 8 > end:
                    break
                swap32(buf, entry)
                swap16_array(buf, entry + 4, 2)
        # MID1 and other blocks are not read by the game.

        swap32(buf, pos)
        swap32(buf, pos + 4)
        pos += size

    swap32(buf, 0x08)
    swap32(buf, 0x0C)
    return bytes(buf)
