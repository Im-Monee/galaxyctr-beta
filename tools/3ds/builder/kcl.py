"""Collision (.kcl), read by KCollisionServer (src/Game/Map/KCollision.cpp).

Header (KCLFile): offsets of positions, normals, prisms and the octree, thickness, minimum
corner, three masks and three shifts. Positions and normals are f32 vectors; prisms are
KC_PrismData (f32 height, six u16), numbered from 1. The octree mixes s32 node words with u16 prism lists,
so it is walked from its root cells: a word with the top bit set points (relative to its
node) to a list read with pre-increment, ending at 0; other words point to a child node of
eight words.
"""

from binary import ConversionError, be_u32, swap16, swap32, swap32_array


def _s32(value):
    return value - (1 << 32) if value & 0x80000000 else value


def convert(data):
    buf = bytearray(data)
    if len(buf) < 0x38:
        raise ConversionError("collision file too small")

    pos_off, nrm_off, prism_off, tree_off = (be_u32(buf, i * 4) for i in range(4))
    if not (0x38 <= pos_off <= nrm_off <= prism_off <= tree_off <= len(buf)):
        raise ConversionError("bad collision offsets")
    masks = [_s32(be_u32(buf, 0x20 + i * 4)) for i in range(3)]
    shift = _s32(be_u32(buf, 0x2C))

    swap32_array(buf, 0x00, 14)
    # Prism 0 is never used: its slot holds the end of the normals (f32).
    swap32_array(buf, pos_off, (prism_off + 0x10 - pos_off) // 4)  # positions and normals
    for p in range(prism_off + 0x10, tree_off - 0x0F, 0x10):
        swap32(buf, p)
        for i in range(6):
            swap16(buf, p + 4 + i * 2)

    # Root cells: one per block of 2^shift units inside the masked extent.
    cells = 1
    for mask in masks:
        cells *= ((~mask & 0xFFFFFFFF) >> shift) + 1

    words = set()
    lists = set()
    pending = [(tree_off, cells)]
    while pending:
        base, count = pending.pop()
        for i in range(count):
            at = base + i * 4
            if at in words:
                continue
            words.add(at)
            value = be_u32(buf, at)
            if value & 0x80000000:
                lists.add(base + (value & 0x7FFFFFFF))
            else:
                pending.append((base + value, 8))

    halves = set()
    for start in lists:
        at = start + 2
        while at + 2 <= len(buf) and at not in halves:
            halves.add(at)
            if buf[at] == 0 and buf[at + 1] == 0:
                break
            at += 2

    # Node words and list entries must not overlap, or the walk misread the tree.
    if any((h - h % 4) in words for h in halves):
        raise ConversionError("collision octree lists overlap its nodes")
    for at in words:
        swap32(buf, at)
    for at in halves:
        swap16(buf, at)
    return bytes(buf)
