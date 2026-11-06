"""Model textures (TEX1 of .bdl, .bmd, .bmt) reduced to what the 3DS shows.

The port uploads a model texture's base level only, halved until neither side is above 128
(platform/3ds/rvl/jut_texture.cpp, kModelTextureLimit and fitImage). The game keeps the whole
archive in memory, so the larger levels and the mipmaps only take room. Here each texture keeps
the level the port would upload, in its own format (NormalMapBase reads and writes RGBA8 texels
in place): the base level decoded, halved as fitImage does (2x2 average) and encoded again.
Palette textures (C4, C8, C14X2) keep their base level. Mipmaps are dropped from every texture.

The rest leave as the 3DS's own compressed formats, in the GPU's memory layout, for the console to
copy with no decoding or encoding: ETC1 (4 bits a texel) when opaque, ETC1A4 (8) with alpha, LA4 (8)
for the I4 and IA4 that fit it exactly. Not the textures read as indirect ones, which need their
values, nor the models of NormalMapBase, whose code reads the texels (_compressible).

Works on the Wii (big-endian) model, before j3d.convert_model.
"""

import os
import struct
import sys

# ##############################################################################################
# # Sizes and formats                                                                          #
# #                                                                                            #
# # How large a model texture may stay (the top screen is 400x240; skies keep more), and the   #
# # GX formats with their block sizes. The 3DS's own formats (ETC1, ETC1A4, LA4) are numbered  #
# # past the GX ones, as rvl/jut_texture.cpp reads them.                                       #
# ##############################################################################################
LIMIT = 128
# Sky archives: their CMPR textures keep up to this size, unencoded again. CMPR at 128 re-encoded shows
# its 4x4 blocks as steps once a sky magnifies it (the title's Earth); jut_texture.cpp's fitImage
# halves the original decoded texels, without blocks.
SKY_CMPR_LIMIT = 256

I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4, C8, C14X2, CMPR = 0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 14
# (block width, block height, bytes per block)
BLOCKS = {I4: (8, 8, 32), I8: (8, 4, 32), IA4: (8, 4, 32), IA8: (4, 4, 32), RGB565: (4, 4, 32), RGB5A3: (4, 4, 32),
          RGBA8: (4, 4, 64), C4: (8, 8, 32), C8: (8, 4, 32), C14X2: (4, 4, 32), CMPR: (8, 8, 32)}
# The 3DS's formats, past the GX ones (jut_texture.cpp kNativeFormatFirst): 8x8 tiles.
ETC1, ETC1A4, LA4 = 0x20, 0x21, 0x22
BLOCKS.update({ETC1: (8, 8, 32), ETC1A4: (8, 8, 64), LA4: (8, 8, 64)})
CODEC = {I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CMPR}


def image_size(fmt, width, height):
    bw, bh, bb = BLOCKS[fmt]
    return ((width + bw - 1) // bw) * ((height + bh - 1) // bh) * bb


def target_size(width, height, limit=LIMIT):
    """(width, height, halvings) after fitImage: halved while a side is above LIMIT and both
    power-of-two sizes are above 8."""
    pot_w = pot_h = 8
    while pot_w < width:
        pot_w <<= 1
    while pot_h < height:
        pot_h <<= 1
    steps = 0
    while (width > limit or height > limit) and pot_w > 8 and pot_h > 8:
        width, height, pot_w, pot_h, steps = (width + 1) // 2, (height + 1) // 2, pot_w >> 1, pot_h >> 1, steps + 1
    return width, height, steps


# ##############################################################################################
# # The GX texel codecs                                                                        #
# #                                                                                            #
# # Decoding and encoding every GX format the game's models use, exactly as the console's      #
# # decoder (rvl/jut_texture.cpp) reads them back, and halving an image.                       #
# ##############################################################################################
# Texel codecs, as jut_texture.cpp decodes. Images are four flat lists (r, g, b, a) of width x height.

def _565(v):
    r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def _5a3(v):
    if v & 0x8000:
        r, g, b = (v >> 10) & 0x1F, (v >> 5) & 0x1F, v & 0x1F
        return (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255
    a, r, g, b = (v >> 12) & 7, (v >> 8) & 0xF, (v >> 4) & 0xF, v & 0xF
    return r * 17, g * 17, b * 17, (a << 5) | (a << 2) | (a >> 1)


def _cmpr_palette(c0, c1):
    # As the Wii's texture unit decodes CMPR (Dolphin's DecodeDXTBlock, from hardware tests): the
    # two middle colours are 3/8 blends, not thirds, and the transparent texel keeps the average
    # colour (alpha 0), not black, so filtering does not darken the edges of cut-out textures.
    p0, p1 = _565(c0) + (255,), _565(c1) + (255,)
    if c0 > c1:
        p2 = tuple((p1[i] * 3 + p0[i] * 5) >> 3 for i in range(3)) + (255,)
        p3 = tuple((p0[i] * 3 + p1[i] * 5) >> 3 for i in range(3)) + (255,)
    else:
        avg = tuple((p0[i] + p1[i]) // 2 for i in range(3))
        p2 = avg + (255,)
        p3 = avg + (0,)
    return p0, p1, p2, p3


def decode(fmt, data, width, height):
    if fmt in (ETC1, ETC1A4, LA4):
        return _decode_native(fmt, data, width, height)
    n = width * height
    r, g, b, a = [0] * n, [0] * n, [0] * n, [0] * n

    def put(x, y, c):
        if x < width and y < height:
            i = y * width + x
            r[i], g[i], b[i], a[i] = c

    bw, bh, bb = BLOCKS[fmt]
    pos = 0
    for by in range(0, height, bh):
        for bx in range(0, width, bw):
            block = data[pos:pos + bb]
            pos += bb
            if fmt == CMPR:
                for sub in range(4):
                    s = block[sub * 8:sub * 8 + 8]
                    pal = _cmpr_palette((s[0] << 8) | s[1], (s[2] << 8) | s[3])
                    sx, sy = bx + (sub & 1) * 4, by + (sub >> 1) * 4
                    for y in range(4):
                        bits = s[4 + y]
                        for x in range(4):
                            put(sx + x, sy + y, pal[(bits >> (6 - x * 2)) & 3])
                continue
            if fmt == RGBA8:
                for i in range(16):
                    put(bx + (i & 3), by + (i >> 2), (block[i * 2 + 1], block[32 + i * 2], block[33 + i * 2], block[i * 2]))
                continue
            for y in range(bh):
                for x in range(bw):
                    k = y * bw + x
                    if fmt == I4:
                        v = block[k // 2]
                        i = ((v & 0xF) if x & 1 else (v >> 4)) * 17
                        c = (i, i, i, i)
                    elif fmt == I8:
                        i = block[k]
                        c = (i, i, i, i)
                    elif fmt == IA4:
                        v = block[k]
                        i = (v & 0xF) * 17
                        c = (i, i, i, (v >> 4) * 17)
                    elif fmt == IA8:
                        c = (block[k * 2 + 1],) * 3 + (block[k * 2],)
                    elif fmt == RGB565:
                        c = _565((block[k * 2] << 8) | block[k * 2 + 1]) + (255,)
                    else:  # RGB5A3
                        c = _5a3((block[k * 2] << 8) | block[k * 2 + 1])
                    put(bx + x, by + y, c)
    return r, g, b, a


def halve(image, width, height):
    """fitImage's step: each texel the rounded average of a 2x2 square (edge texels repeated)."""
    nw, nh = (width + 1) // 2, (height + 1) // 2
    out = []
    for ch in image:
        dst = [0] * (nw * nh)
        for y in range(nh):
            row0 = y * 2 * width
            row1 = (y * 2 + 1 if y * 2 + 1 < height else y * 2) * width
            for x in range(nw):
                x0 = x * 2
                x1 = x0 + 1 if x0 + 1 < width else x0
                dst[y * nw + x] = (ch[row0 + x0] + ch[row0 + x1] + ch[row1 + x0] + ch[row1 + x1] + 2) // 4
        out.append(dst)
    return out, nw, nh


def _q(v, levels):
    """0..255 to the nearest of levels + 1 steps."""
    return (v * levels + 127) // 255


def _to565(r, g, b):
    return (_q(r, 31) << 11) | (_q(g, 63) << 5) | _q(b, 31)


def _to5a3(r, g, b, a):
    if a >= 0xF0:
        return 0x8000 | (_q(r, 31) << 10) | (_q(g, 31) << 5) | _q(b, 31)
    return (_q(a, 7) << 12) | (_q(r, 15) << 8) | (_q(g, 15) << 4) | _q(b, 15)


def _cmpr_block(texels):
    """One 4x4 CMPR block (8 bytes) from 16 (r, g, b, a) texels."""
    transparent = [t[3] < 128 for t in texels]
    solid = [t for t, clear in zip(texels, transparent) if not clear]
    if not solid:
        return b"\0\0\0\0\xff\xff\xff\xff"
    # Endpoints: the extremes along the channel range's axis.
    lo = [min(t[i] for t in solid) for i in range(3)]
    hi = [max(t[i] for t in solid) for i in range(3)]
    axis = [hi[i] - lo[i] for i in range(3)]
    proj = [sum(t[i] * axis[i] for i in range(3)) for t in solid]
    e0, e1 = solid[proj.index(max(proj))], solid[proj.index(min(proj))]
    c0, c1 = _to565(*e0[:3]), _to565(*e1[:3])
    if any(transparent):
        if c0 > c1:
            c0, c1 = c1, c0  # three colours and transparent: c0 <= c1
    else:
        if c0 < c1:
            c0, c1 = c1, c0
        elif c0 == c1:
            # One colour: c0 > c1 is needed for four colours; index 0 is c0 either way.
            pass
    pal = _cmpr_palette(c0, c1)
    choices = range(4) if c0 > c1 else range(3)
    rows = []
    for y in range(4):
        bits = 0
        for x in range(4):
            t = texels[y * 4 + x]
            if transparent[y * 4 + x] and c0 <= c1:
                index = 3
            else:
                index = min(choices, key=lambda k: (pal[k][0] - t[0]) ** 2 + (pal[k][1] - t[1]) ** 2 + (pal[k][2] - t[2]) ** 2)
            bits |= index << (6 - x * 2)
        rows.append(bits)
    return struct.pack(">HH4B", c0, c1, *rows)


def encode(fmt, image, width, height):
    r, g, b, a = image

    def texel(x, y):
        # Outside the image: the nearest edge texel (block padding).
        i = min(y, height - 1) * width + min(x, width - 1)
        return r[i], g[i], b[i], a[i]

    bw, bh, _ = BLOCKS[fmt]
    out = bytearray()
    for by in range(0, height, bh):
        for bx in range(0, width, bw):
            if fmt == CMPR:
                for sub in range(4):
                    sx, sy = bx + (sub & 1) * 4, by + (sub >> 1) * 4
                    out += _cmpr_block([texel(sx + x, sy + y) for y in range(4) for x in range(4)])
                continue
            if fmt == RGBA8:
                ar, gb = bytearray(), bytearray()
                for i in range(16):
                    t = texel(bx + (i & 3), by + (i >> 2))
                    ar += bytes((t[3], t[0]))
                    gb += bytes((t[1], t[2]))
                out += ar + gb
                continue
            if fmt == I4:
                for y in range(bh):
                    for x in range(0, bw, 2):
                        out.append((_q(texel(bx + x, by + y)[0], 15) << 4) | _q(texel(bx + x + 1, by + y)[0], 15))
                continue
            for y in range(bh):
                for x in range(bw):
                    t = texel(bx + x, by + y)
                    if fmt == I8:
                        out.append(t[0])
                    elif fmt == IA4:
                        out.append((_q(t[3], 15) << 4) | _q(t[0], 15))
                    elif fmt == IA8:
                        out += bytes((t[3], t[0]))
                    elif fmt == RGB565:
                        out += struct.pack(">H", _to565(*t[:3]))
                    else:  # RGB5A3
                        out += struct.pack(">H", _to5a3(*t))
    return bytes(out)


# ##############################################################################################
# # The 3DS's formats                                                                          #
# #                                                                                            #
# # ETC1 (4 bits a texel, colour), ETC1A4 (with 4-bit alpha) and LA4, written in the GPU's own #
# # layout (8x8 tiles of 4x4 blocks in Morton order), so the console only copies them: an      #
# # encoder on the console took tens of milliseconds a texture.                                #
# ##############################################################################################
_ETC1_TABLES = ((2, 8), (5, 17), (9, 29), (13, 42), (18, 60), (24, 80), (33, 106), (47, 183))
# Each table's modifiers in rising order and their index (0 +a, 1 +b, 2 -a, 3 -b).
_ETC1_SORTED = tuple(((-b, 3), (-a, 2), (a, 0), (b, 1)) for a, b in _ETC1_TABLES)


def _clamp(v):
    return 0 if v < 0 else 255 if v > 255 else v


def _etc1_half(px, base):
    """The closest table for a half's texels around base: (error, table, indices). The channels
    share the modifier, green weighing most (2 r + 4 g + b), so the texel's best one is the nearest to
    its weighted distance from base; the error is exact for the table that wins."""
    br, bg, bb = base
    offsets = [(2 * (p[0] - br) + 4 * (p[1] - bg) + (p[2] - bb)) / 7 for p in px]
    best_table, best_cost = 0, None
    for t, ((m0, _), (m1, _), (m2, _), (m3, _)) in enumerate(_ETC1_SORTED):
        cost = 0.0
        low, mid, high = (m0 + m1) / 2, (m1 + m2) / 2, (m2 + m3) / 2
        for v in offsets:
            d = v - (m2 if v > mid else m1) if low <= v <= high else v - (m0 if v < low else m3)
            cost += d * d
        if best_cost is None or cost < best_cost:
            best_table, best_cost = t, cost
    mods = _ETC1_SORTED[best_table]
    low, mid, high = (mods[0][0] + mods[1][0]) / 2, (mods[1][0] + mods[2][0]) / 2, (mods[2][0] + mods[3][0]) / 2
    error = 0
    indices = []
    for p, v in zip(px, offsets):
        m, index = mods[0 if v < low else 1 if v <= mid else 2 if v <= high else 3]
        indices.append(index)
        dr, dg, db = _clamp(br + m) - p[0], _clamp(bg + m) - p[1], _clamp(bb + m) - p[2]
        error += 2 * dr * dr + 4 * dg * dg + db * db
    return error, best_table, indices


def _etc1_block(block, memo={}):
    """The 64-bit ETC1 word of a block: 16 (r, g, b, visible) texels, as texel number x * 4 + y.
    Both orientations and both kinds of base are tried. Texels with no alpha cost nothing."""
    key = tuple(block)
    word = memo.get(key)
    if word is not None:
        return word
    best_word, best_error = 0, None
    for flip in (0, 1):
        halves = ([], [])
        positions = ([], [])
        for p, texel in enumerate(block):
            h = (p & 3) >= 2 if flip else (p >> 2) >= 2
            positions[h].append(p)
            if texel[3]:
                halves[h].append(texel)
        averages = []
        for px in halves:
            n = len(px)
            averages.append([(sum(t[c] for t in px) + n // 2) // n for c in range(3)] if n else [0, 0, 0])
        for diff in (1, 0):
            levels = 31 if diff else 15
            q = [[(avg * levels + 127) // 255 for avg in half] for half in averages]
            if diff:
                if any(not -4 <= q[1][c] - q[0][c] <= 3 for c in range(3)):
                    continue
                base = [[(v << 3) | (v >> 2) for v in half] for half in q]
            else:
                base = [[v * 17 for v in half] for half in q]
            error0, table0, indices0 = _etc1_half(halves[0], base[0])
            error1, table1, indices1 = _etc1_half(halves[1], base[1])
            error = error0 + error1
            if best_error is not None and error >= best_error:
                continue
            if diff:
                hi = q[0][0] << 27 | ((q[1][0] - q[0][0]) & 7) << 24 | q[0][1] << 19 | ((q[1][1] - q[0][1]) & 7) << 16 | \
                    q[0][2] << 11 | ((q[1][2] - q[0][2]) & 7) << 8
            else:
                hi = q[0][0] << 28 | q[1][0] << 24 | q[0][1] << 20 | q[1][1] << 16 | q[0][2] << 12 | q[1][2] << 8
            hi |= table0 << 5 | table1 << 2 | diff << 1 | flip
            lo = 0
            for h, indices in enumerate((indices0, indices1)):
                visible = iter(indices)
                for p in positions[h]:
                    # Index m (0 +a, 1 +b, 2 -a, 3 -b) as its MSB (sign) and LSB (large).
                    m = next(visible) if block[p][3] else 0
                    lo |= (m & 1) << p | (m >> 1) << (p + 16)
            best_word, best_error = hi << 32 | lo, error
    memo[key] = best_word
    return best_word


def _morton(x, y):
    """A texel's place in its 8x8 tile."""
    return sum(((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1) for i in range(3))


def _encode_native(fmt, image, size):
    """The square-or-not power-of-two image in the GPU's layout: 8x8 tiles in rows, memory rows
    bottom-up (gfx.c hostGfxTextureCreate). ETC1: four 4x4 blocks per tile in Z order, each a little-
    endian word; ETC1A4 puts before each one a word of 4-bit alphas, at the same texel numbers."""
    width, height = size
    r, g, b, a = image
    out = bytearray()
    for tile_y in range(0, height, 8):
        for tile_x in range(0, width, 8):
            if fmt == LA4:
                tile = bytearray(64)
                for y in range(8):
                    for x in range(8):
                        i = (height - 1 - (tile_y + y)) * width + tile_x + x
                        tile[_morton(x, y)] = _q(r[i], 15) << 4 | _q(a[i], 15)
                out += tile
                continue
            for sub in range(4):
                bx, by = tile_x + (sub & 1) * 4, tile_y + (sub >> 1) * 4
                texels = []
                alpha = 0
                for x in range(4):
                    for y in range(4):
                        i = (height - 1 - (by + y)) * width + bx + x
                        alpha |= _q(a[i], 15) << ((x * 4 + y) * 4)
                        texels.append((r[i], g[i], b[i], a[i] != 0))
                if fmt == ETC1A4:
                    out += struct.pack("<Q", alpha)
                out += struct.pack("<Q", _etc1_block(texels))
    return bytes(out)


def _decode_native(fmt, data, width, height):
    n = width * height
    r, g, b, a = [0] * n, [0] * n, [0] * n, [255] * n
    pos = 0
    for tile_y in range(0, height, 8):
        for tile_x in range(0, width, 8):
            if fmt == LA4:
                for y in range(8):
                    for x in range(8):
                        v = data[pos + _morton(x, y)]
                        i = (height - 1 - (tile_y + y)) * width + tile_x + x
                        r[i] = g[i] = b[i] = (v >> 4) * 17
                        a[i] = (v & 0xF) * 17
                pos += 64
                continue
            for sub in range(4):
                bx, by = tile_x + (sub & 1) * 4, tile_y + (sub >> 1) * 4
                alpha = None
                if fmt == ETC1A4:
                    alpha = struct.unpack_from("<Q", data, pos)[0]
                    pos += 8
                word = struct.unpack_from("<Q", data, pos)[0]
                pos += 8
                hi, lo = word >> 32, word & 0xFFFFFFFF
                flip, diff = hi & 1, (hi >> 1) & 1
                if diff:
                    q0 = ((hi >> 27) & 31, (hi >> 19) & 31, (hi >> 11) & 31)
                    d = ((hi >> 24) & 7, (hi >> 16) & 7, (hi >> 8) & 7)
                    q1 = tuple(q0[c] + (d[c] - 8 if d[c] > 3 else d[c]) for c in range(3))
                    bases = [[(v << 3) | (v >> 2) for v in q] for q in (q0, q1)]
                else:
                    bases = [[((hi >> s) & 15) * 17 for s in (28, 20, 12)], [((hi >> s) & 15) * 17 for s in (24, 16, 8)]]
                tables = (hi >> 5) & 7, (hi >> 2) & 7
                for x in range(4):
                    for y in range(4):
                        p = x * 4 + y
                        half = int((y if flip else x) >= 2)
                        low_a, low_b = _ETC1_TABLES[tables[half]]
                        m = ((lo >> p) & 1) | ((lo >> (p + 16)) & 1) << 1
                        mod = (low_a, low_b, -low_a, -low_b)[m]
                        i = (height - 1 - (by + y)) * width + bx + x
                        r[i], g[i], b[i] = (_clamp(bases[half][c] + mod) for c in range(3))
                        if alpha is not None:
                            a[i] = ((alpha >> (p * 4)) & 0xF) * 17
    return r, g, b, a


def _compressed(fmt, data, width, height):
    """(bytes, format, width, height, largest r, g, b, a) of the texture as the 3DS keeps it: halved
    as _reduced, from the original texels (no CMPR in between), stretched to its power-of-two size as
    fitImage does. The largest values are the uncompressed ones: materials are compiled from them
    (n3d.texture_maxima), and ETC1's modifiers overshoot."""
    image, w, h = decode(fmt, data[:image_size(fmt, width, height)], width, height), width, height
    pot_w, pot_h, steps = 8, 8, target_size(width, height)[2]
    while pot_w < width:
        pot_w <<= 1
    while pot_h < height:
        pot_h <<= 1
    for _ in range(steps):
        image, w, h = halve(image, w, h)
        pot_w, pot_h = pot_w >> 1, pot_h >> 1
    largest = tuple(max(channel) for channel in image)
    if (w, h) != (pot_w, pot_h):
        stretched = []
        for channel in image:
            stretched.append([channel[(y * h // pot_h) * w + x * w // pot_w] for y in range(pot_h) for x in range(pot_w)])
        image = stretched
    if min(image[3]) == 255:
        target = ETC1
    else:
        target = LA4 if fmt in (I4, IA4) else ETC1A4
    return _encode_native(target, image, (pot_w, pot_h)), target, pot_w, pot_h, largest


# ##############################################################################################
# # Reducing a texture                                                                         #
# #                                                                                            #
# # A texture over the limit is halved (its own mipmaps are not used: they differ from the     #
# # halved base level), and compressed into a 3DS format, except the textures NormalMapBase    #
# # works on, which keep their GX format. An indirect texture (I4 to IA8) with a narrow range  #
# # of values is stretched to the whole 8 bits, since the 3DS blends its samples by those      #
# # values (native.cpp, rvlDistortionPlan).                                                    #
# ##############################################################################################
def _reduced(fmt, data, width, height, mips, sky=False):
    """(bytes, width, height) of the level the port uploads, or None to keep the base level."""
    _, _, steps = target_size(width, height, SKY_CMPR_LIMIT if sky and fmt == CMPR else LIMIT)
    if steps == 0:
        return data[:image_size(fmt, width, height)], width, height
    # Not the texture's own mipmaps: they differ from the halved base level the port shows (by
    # 19 to 27 of 255 on average, measured on ObjectData).
    if fmt not in CODEC:
        return None
    image, w, h = decode(fmt, data, width, height), width, height
    for _ in range(steps):
        image, w, h = halve(image, w, h)
    return encode(fmt, image, w, h), w, h


def _min_filter(value):
    """GX min filter without mipmaps: NEAR_MIP_* to NEAR, LIN_MIP_* to LINEAR."""
    return {2: 0, 3: 0, 4: 1, 5: 1}.get(value, value)


def indirect_only(data):
    """TEX1 indices of the textures read only as indirect textures (mat.indirect_only_textures)."""
    native = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "native")
    if native not in sys.path:
        sys.path.insert(0, native)
    import mat
    import n3d
    found = n3d.blocks(data)
    return mat.indirect_only_textures(data, *found["MAT3"]) if "MAT3" in found else set()


def _texture_names(block, names):
    if not names:
        return []
    count = struct.unpack_from(">H", block, names)[0]
    out = []
    for i in range(count):
        start = names + struct.unpack_from(">H", block, names + 4 + i * 4 + 2)[0]
        out.append(block[start:block.index(b"\0", start)])
    return out


def _compressible(data, block, names):
    """TEX1 indices that may leave the GX formats, or an empty set: NormalMapBase takes its textures
    by these names and works on the texels (setupTexture)."""
    all_names = _texture_names(block, names)
    if any(b"ormal" in n or b"Nswap_" in n for n in all_names):
        return set()
    native = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "native")
    if native not in sys.path:
        sys.path.insert(0, native)
    import mat
    import n3d
    found = n3d.blocks(data)
    indirect = mat.indirect_textures(data, *found["MAT3"]) if "MAT3" in found else set()
    return set(range(len(all_names))) - indirect


def stretches(fmt, value_range):
    """Whether an indirect texture of this format and range of values is stretched to the whole 8 bits."""
    return fmt in (I4, I8, IA4, IA8) and value_range is not None and value_range[1] - value_range[0] >= 2


def _stretched(fmt, image, value_range):
    """The texture's values spread from low..high to 0..255: the 3DS blends between samples by values
    multiplied by 1, 2 or 4 at most (native.cpp rvlDistortionPlan), and the rest of a narrow range is lost."""
    low, high = value_range
    full = [max(0, min(255, round((v - low) * 255 / (high - low)))) for v in range(256)]
    if fmt == I8:
        return bytes(full[b] for b in image)
    if fmt == IA8:  # alpha, intensity
        return bytes(b if i & 1 == 0 else full[b] for i, b in enumerate(image))
    four = [round(full[v * 17] / 17) for v in range(16)]
    if fmt == IA4:  # alpha in the high half
        return bytes((b & 0xF0) | four[b & 0xF] for b in image)
    return bytes(four[b >> 4] << 4 | four[b & 0xF] for b in image)


# ##############################################################################################
# # A model's textures                                                                         #
# #                                                                                            #
# # The TEX1 block of a J3D model, rewritten with the reduced textures; the counts go to the   #
# # converter's log.                                                                           #
# ##############################################################################################
def reduce_model(data, stats=None, sky=False, stretch=True, compress=True):
    """The model with its TEX1 textures reduced (unchanged when there is nothing to do or the
    block is not laid out as expected). stats, a dict, gets "before" and "after" byte counts.
    stretch: textures read only as indirect ones get the whole range of values (_stretched).
    compress: the textures that may in the 3DS's formats (_compressed)."""
    if data[:4] != b"J3D2":
        return data
    count = struct.unpack_from(">I", data, 0x0C)[0]
    pos = 0x20
    for _ in range(count):
        if pos + 8 > len(data):
            return data
        kind, size = data[pos:pos + 4], struct.unpack_from(">I", data, pos + 4)[0]
        if size < 8:
            return data
        if kind == b"TEX1":
            names = struct.unpack_from(">I", data, pos + 0x10)[0]
            block = _reduce_tex1(data[pos:pos + size], stats, sky, indirect_only(data) if stretch else (),
                                 _compressible(data, data[pos:pos + size], names) if compress else ())
            if block is None:
                return data
            out = bytearray(data[:pos] + block + data[pos + size:])
            struct.pack_into(">I", out, 0x08, len(out))
            return bytes(out)
        pos += size
    return data


def _align(buf, n=32):
    buf += b"\0" * (-len(buf) % n)


def _reduce_tex1(block, stats, sky=False, stretched=(), compressible=()):
    count = struct.unpack_from(">H", block, 0x08)[0]
    headers, names = struct.unpack_from(">II", block, 0x0C)
    if count == 0 or headers < 0x14 or headers + count * 0x20 > len(block):
        return None
    images, palettes = {}, {}
    fields = []
    kept = set()  # images with a header that is not only an indirect texture's
    for i in range(count):
        h = headers + i * 0x20
        fmt, width, height = block[h], *struct.unpack_from(">HH", block, h + 2)
        palette_count, palette_offset = struct.unpack_from(">HI", block, h + 0x0A)
        mips = max(block[h + 0x18], 1)
        image = h + struct.unpack_from(">I", block, h + 0x1C)[0]
        if fmt not in BLOCKS or width == 0 or height == 0:
            return None
        images.setdefault(image, (fmt, width, height, mips))
        if images[image] != (fmt, width, height, mips):
            return None  # one image read two ways: leave the model alone
        palette = h + palette_offset if fmt in (C4, C8, C14X2) and palette_count else None
        if palette is not None:
            palettes[palette] = max(palettes.get(palette, 0), palette_count * 2)
        fields.append((h, image, palette))
        if i not in stretched:
            kept.add(image)
    fixed = {image for i, (_, image, _) in enumerate(fields) if i not in compressible or images[image][0] in (C4, C8, C14X2)}
    starts = sorted(set(images) | set(palettes))
    name_size = _name_table_size(block, names) if names else 0
    # The name table comes before the pixels or after them (then it is moved after the new ones).
    names_after = names >= starts[0]
    if headers + count * 0x20 > starts[0] or (not names_after and names + name_size > starts[0]):
        return None
    out = bytearray(block[:starts[0]])
    new_image, new_palette, new_size, value_range, new_format, largest = {}, {}, {}, {}, {}, {}
    spread = set()
    before = after = 0
    for start in starts:
        _align(out)
        if start in images:
            fmt, width, height, mips = images[start]
            value_range[start] = _value_range(fmt, block[start:start + image_size(fmt, width, height)], width, height)
            total, w, h = 0, width, height
            for _ in range(mips):
                total += image_size(fmt, w, h)
                w, h = max(w // 2, 1), max(h // 2, 1)
            original = block[start:start + total]
            if start not in kept and stretches(fmt, value_range[start]):
                original = _stretched(fmt, original, value_range[start])
                spread.add(start)
            if start not in fixed:
                pixels, new_format[start], w, h, largest[start] = _compressed(fmt, original, width, height)
                result = (pixels, w, h)
            else:
                result = _reduced(fmt, original, width, height, mips, sky)
                if result is None:
                    result = (original[:image_size(fmt, width, height)], width, height)
            new_image[start] = len(out)
            new_size[start] = result[1:]
            out += result[0]
            before += total
            after += len(result[0])
        if start in palettes:
            new_palette[start] = len(out)
            out += block[start:start + palettes[start]]
    _align(out)
    if names_after:
        struct.pack_into(">I", out, 0x10, len(out))
        out += block[names:names + name_size]
        _align(out)
    for h, image, palette in fields:
        width, height = new_size[image]
        before_width = struct.unpack_from(">H", block, h + 2)[0]
        out[h] = new_format.get(image, out[h])
        struct.pack_into(">HH", out, h + 2, width, height)
        out[h + 0x19] = _halvings(before_width, width)  # ResTIMG::_19: the indirect displacement is in the file's texels
        if image in largest:  # no palette: in its unused offset
            out[h + 0x0C:h + 0x10] = bytes(largest[image])
        elif value_range[image] is not None and palette is None:
            # As an indirect texture, the values it holds: bit 7 of _19, then in the unused palette offset.
            out[h + 0x19] |= 0xC0 if image in spread else 0x80
            struct.pack_into(">I", out, h + 0x0C, value_range[image][0] | value_range[image][1] << 8)
        out[h + 0x10] = 0                      # mipmaps off
        out[h + 0x14] = _min_filter(out[h + 0x14])
        out[h + 0x17] = 0                      # max LOD
        out[h + 0x18] = 1                      # one level
        struct.pack_into(">I", out, h + 0x1C, new_image[image] - h)
        if palette is not None:
            struct.pack_into(">I", out, h + 0x0C, new_palette[palette] - h)
    struct.pack_into(">I", out, 4, len(out))
    if stats is not None:
        stats["before"] = stats.get("before", 0) + before
        stats["after"] = stats.get("after", 0) + after
    return bytes(out)


def _value_range(fmt, image, width, height):
    """(low, high) of the red channel without the rarest 1% at each end, None for a palette format."""
    if fmt in (C4, C8, C14X2):
        return None
    values = sorted(decode(fmt, image, width, height)[0])
    return values[len(values) // 100], values[-1 - len(values) // 100]


def _halvings(before, after):
    steps = 0
    while before > after:
        before, steps = (before + 1) // 2, steps + 1
    return steps


def _name_table_size(block, pos):
    """ResNTAB: u16 count, u16 pad, count x (u16 hash, u16 offset), then the strings."""
    count = struct.unpack_from(">H", block, pos)[0]
    end = pos + 4 + count * 4
    for i in range(count):
        offset = struct.unpack_from(">H", block, pos + 4 + i * 4 + 2)[0]
        end = max(end, block.index(b"\0", pos + offset) + 1)
    return end - pos
