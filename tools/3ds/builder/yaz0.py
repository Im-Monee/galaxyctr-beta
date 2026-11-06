"""Yaz0 (SZS) compression and decompression."""

import struct

from binary import ConversionError


def is_compressed(data):
    return len(data) >= 16 and data[:4] == b"Yaz0"


def decompress(data):
    if not is_compressed(data):
        raise ConversionError("not Yaz0 data")

    size = struct.unpack_from(">I", data, 4)[0]
    out = bytearray(size)
    src = 16
    dst = 0
    src_end = len(data)

    while dst < size and src < src_end:
        header = data[src]
        src += 1

        for bit in range(8):
            if dst >= size:
                break

            if header & (0x80 >> bit):
                out[dst] = data[src]
                src += 1
                dst += 1
            else:
                b1 = data[src]
                b2 = data[src + 1]
                src += 2
                distance = ((b1 & 0x0F) << 8 | b2) + 1
                length = b1 >> 4
                if length == 0:
                    length = data[src] + 0x12
                    src += 1
                else:
                    length += 2

                copy_from = dst - distance
                if distance >= length:
                    out[dst : dst + length] = out[copy_from : copy_from + length]
                else:
                    # Overlapping copy: repeats the last `distance` bytes.
                    for i in range(length):
                        out[dst + i] = out[copy_from + i]
                dst += length

    if dst != size:
        raise ConversionError("truncated Yaz0 data")

    return bytes(out)


def compress(data):
    """Yaz0 compression with a greedy longest-match search over the last 4 KB."""
    size = len(data)
    out = bytearray(b"Yaz0" + struct.pack(">I", size) + bytes(8))
    # Recent positions of each 3-byte sequence, newest last.
    chains = {}
    pos = 0

    def find_match(pos):
        if pos + 3 > size:
            return 0, 0
        candidates = chains.get(data[pos : pos + 3])
        if not candidates:
            return 0, 0
        best_len, best_dist = 0, 0
        limit = min(0x111, size - pos)
        for start in reversed(candidates):
            dist = pos - start
            if dist > 0x1000:
                break
            length = 3
            while length < limit and data[start + length] == data[pos + length]:
                length += 1
            if length > best_len:
                best_len, best_dist = length, dist
                if length == limit:
                    break
        return best_len, best_dist

    def remember(pos):
        key = data[pos : pos + 3]
        chain = chains.setdefault(key, [])
        chain.append(pos)
        if len(chain) > 32:
            del chain[0]

    while pos < size:
        header_index = len(out)
        out.append(0)
        header = 0
        for bit in range(8):
            if pos >= size:
                break
            length, dist = find_match(pos)
            if length >= 3:
                dist -= 1
                if length >= 0x12:
                    out += bytes(((dist >> 8) & 0x0F, dist & 0xFF, length - 0x12))
                else:
                    out += bytes((((length - 2) << 4) | ((dist >> 8) & 0x0F), dist & 0xFF))
                for i in range(length):
                    if pos + i + 3 <= size:
                        remember(pos + i)
                pos += length
            else:
                header |= 0x80 >> bit
                out.append(data[pos])
                if pos + 3 <= size:
                    remember(pos)
                pos += 1
        out[header_index] = header

    return bytes(out)
