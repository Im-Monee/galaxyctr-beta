"""The game's message font as a TrueType font, for the builder window's own text.

The window's labels, fields and log are Qt widgets, and Qt draws text only from outline fonts.
The game's fonts (LayoutData/Font.arc, .brfnt) are pictures of their characters. So, like the
rest of the game's look, this font is made on the user's computer from their disc
(builder_assets.make) and never shipped: each character's picture is traced into outlines and
written as a small TrueType file the window loads (QFontDatabase.addApplicationFont).

  trace     marching squares on the glyph's 4-bit coverage, at half coverage, with the edges
            placed between pixels by linear interpolation (so the curves are smooth, not
            stair steps); loops are simplified (Douglas-Peucker) and turned the TrueType way:
            outer loops clockwise, holes counter-clockwise (found by how deep a loop lies)
  metrics   from the font's own: the baseline of its glyph cells, each character's left side,
            glyph width and advance (CWDH), the line feed (FINF)
  ttf       the tables Windows, macOS and Linux need: head, hhea, maxp, OS/2, name, cmap
            (format 4), post (format 3), hmtx, loca and glyf (points on the curve only)
"""

import struct

UNITS = 2048  # units per em
FAMILY = "GalaxyCTR Game"


# ##############################################################################################
# # Tracing a glyph                                                                            #
# ##############################################################################################
def trace(pixels, width, height, level=0.5):
    """Closed loops [(x, y), ...] (pixel units, y down) around where pixels (rows of 0..1) reach
    level; the picture is read with a border of empty pixels."""
    def value(x, y):
        if 0 <= x < width and 0 <= y < height:
            return pixels[y][x]
        return 0.0

    def point(x0, y0, x1, y1):
        a, b = value(x0, y0), value(x1, y1)
        t = 0.5 if a == b else (level - a) / (b - a)
        return (x0 + (x1 - x0) * t, y0 + (y1 - y0) * t)

    # Segments of every square of four pixel centres, directed with the inside on the right
    # (y down), so they join into loops.
    segments = {}
    for y in range(-1, height):
        for x in range(-1, width):
            tl, tr = value(x, y) >= level, value(x + 1, y) >= level
            br, bl = value(x + 1, y + 1) >= level, value(x, y + 1) >= level
            case = tl * 8 + tr * 4 + br * 2 + bl
            if case in (0, 15):
                continue
            top, right = ("t", x, y), ("r", x, y)
            bottom, left = ("b", x, y), ("l", x, y)
            edges = {"t": (x, y, x + 1, y), "r": (x + 1, y, x + 1, y + 1),
                     "b": (x, y + 1, x + 1, y + 1), "l": (x, y, x, y + 1)}
            centre = (value(x, y) + value(x + 1, y) + value(x + 1, y + 1) + value(x, y + 1)) / 4 >= level
            pairs = {
                1: [(left, bottom)], 2: [(bottom, right)], 3: [(left, right)], 4: [(right, top)],
                5: [(left, top), (right, bottom)] if centre else [(left, bottom), (right, top)],
                6: [(bottom, top)], 7: [(left, top)], 8: [(top, left)], 9: [(top, bottom)],
                10: [(top, right), (bottom, left)] if centre else [(top, left), (bottom, right)],
                11: [(top, right)], 12: [(right, left)], 13: [(right, bottom)], 14: [(bottom, left)],
            }[case]
            for a, b in pairs:
                key_a = _edge_key(a, edges)
                key_b = _edge_key(b, edges)
                segments[key_a] = (key_b, point(*edges[a[0]]), point(*edges[b[0]]))
    loops = []
    while segments:
        start, (nxt, pa, _pb) = next(iter(segments.items()))
        loop = [pa]
        segments.pop(start)
        key = nxt
        while key in segments:
            nxt, pa, _pb = segments.pop(key)
            loop.append(pa)
            key = nxt
        if len(loop) >= 3:
            loops.append(loop)
    return loops


def _edge_key(edge, edges):
    """An edge between two pixel centres, the same from both squares that share it."""
    x0, y0, x1, y1 = edges[edge[0]]
    return (x0, y0, x1, y1)


def simplify(loop, tolerance):
    """Douglas-Peucker on a closed loop."""
    if len(loop) < 5:
        return loop
    # Split at the point farthest from the first, simplify both halves.
    first = loop[0]
    far = max(range(len(loop)), key=lambda i: (loop[i][0] - first[0]) ** 2 + (loop[i][1] - first[1]) ** 2)
    a = _dp(loop[:far + 1], tolerance)
    b = _dp(loop[far:] + [first], tolerance)
    return a[:-1] + b[:-1]


def _dp(points, tolerance):
    if len(points) < 3:
        return points
    (x0, y0), (x1, y1) = points[0], points[-1]
    dx, dy = x1 - x0, y1 - y0
    length = (dx * dx + dy * dy) ** 0.5 or 1e-9
    worst, index = 0.0, 0
    for i in range(1, len(points) - 1):
        px, py = points[i]
        d = abs(dy * (px - x0) - dx * (py - y0)) / length
        if d > worst:
            worst, index = d, i
    if worst <= tolerance:
        return [points[0], points[-1]]
    return _dp(points[:index + 1], tolerance)[:-1] + _dp(points[index:], tolerance)


def _area(loop):
    return sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(loop, loop[1:] + loop[:1])) / 2


def _inside(point, loop):
    x, y = point
    inside = False
    for (x0, y0), (x1, y1) in zip(loop, loop[1:] + loop[:1]):
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            inside = not inside
    return inside


def orient(loops):
    """TrueType directions (y up): outer loops clockwise, holes counter-clockwise, by depth."""
    out = []
    for i, loop in enumerate(loops):
        depth = sum(1 for j, other in enumerate(loops) if j != i and _inside(loop[0], other))
        clockwise = depth % 2 == 0
        if (_area(loop) < 0) != clockwise:
            loop = loop[::-1]
        out.append(loop)
    return out


# ##############################################################################################
# # Writing the TrueType file                                                                  #
# ##############################################################################################
def _checksum(data):
    data += b"\0" * (-len(data) % 4)
    return sum(struct.unpack(f">{len(data) // 4}I", data)) & 0xFFFFFFFF


def _glyph(contours):
    if not contours:
        return b""
    xs = [x for c in contours for x, _ in c]
    ys = [y for c in contours for _, y in c]
    out = struct.pack(">hhhhh", len(contours), min(xs), min(ys), max(xs), max(ys))
    ends, total = [], 0
    for c in contours:
        total += len(c)
        ends.append(total - 1)
    out += struct.pack(f">{len(ends)}H", *ends) + struct.pack(">H", 0)
    points = [p for c in contours for p in c]
    out += bytes([1] * len(points))  # on the curve, x and y as 16-bit deltas
    last = 0
    for x, _ in points:
        out += struct.pack(">h", x - last)
        last = x
    last = 0
    for _, y in points:
        out += struct.pack(">h", y - last)
        last = y
    return out + b"\0" * (-len(out) % 4)


def _cmap(codes):
    """Format 4 over the given (code, glyph) pairs, one segment per run of codes."""
    pairs = sorted((c, g) for c, g in codes if c < 0xFFFF)
    segments = []
    for code, glyph in pairs:
        if segments and code == segments[-1][1] + 1 and glyph == segments[-1][2] + (code - segments[-1][0]):
            segments[-1][1] = code
        else:
            segments.append([code, code, glyph])
    segments.append([0xFFFF, 0xFFFF, 0])
    n = len(segments)
    search = 2 * (1 << (n.bit_length() - 1))
    body = struct.pack(">HHHH", 2 * n, search, (search // 2).bit_length() - 1, 2 * n - search)
    body += struct.pack(f">{n}H", *(s[1] for s in segments)) + b"\0\0"
    body += struct.pack(f">{n}H", *(s[0] for s in segments))
    body += struct.pack(f">{n}h", *(((s[2] - s[0]) if s[0] != 0xFFFF else 1) for s in segments))
    body += struct.pack(f">{n}H", *([0] * n))
    sub = struct.pack(">HHH", 4, 6 + len(body), 0) + body
    return struct.pack(">HHHHIHHI", 0, 2, 0, 3, 20, 3, 1, 20) + sub


def _name(family):
    records = [(1, family), (2, "Regular"), (3, f"{family} Regular"), (4, family), (5, "Version 1.0"),
               (6, family.replace(" ", "") + "-Regular")]
    strings, entries = b"", b""
    for name_id, text in records:
        raw = text.encode("utf-16-be")
        entries += struct.pack(">HHHHHH", 3, 1, 0x409, name_id, len(raw), len(strings))
        strings += raw
    return struct.pack(">HHH", 0, len(records), 6 + 12 * len(records)) + entries + strings


def write_ttf(path, glyphs, ascent, descent, line_gap, family=FAMILY):
    """glyphs: [(code or None, advance, contours)], the first one .notdef; units UNITS per em."""
    glyf, loca = b"", [0]
    for _code, _advance, contours in glyphs:
        glyf += _glyph(contours)
        loca.append(len(glyf))
    all_x = [x for _, _, cs in glyphs for c in cs for x, _ in c] or [0]
    all_y = [y for _, _, cs in glyphs for c in cs for _, y in c] or [0]
    advances = [a for _, a, _ in glyphs]
    # Each glyph's left side bearing is its leftmost point (xMin), as the head flags say: Windows
    # places a glyph by this number, not by its outline, so a 0 here lost the blank columns
    # before a character (the game's 1 sits 4 pixels in, CWDH) and the letters spread unevenly.
    bearings = [min(x for c in cs for x, _ in c) if cs else 0 for _, _, cs in glyphs]
    inks = [max(x for c in cs for x, _ in c) - b if cs else 0 for (_, _, cs), b in zip(glyphs, bearings)]
    max_points = max([sum(len(c) for c in cs) for _, _, cs in glyphs] + [0])
    max_contours = max([len(cs) for _, _, cs in glyphs] + [0])
    tables = {
        b"head": struct.pack(">IIIIHHqqhhhhHHhhh", 0x00010000, 0x00010000, 0, 0x5F0F3CF5, 0x0003, UNITS, 0, 0,
                             min(all_x), min(all_y), max(all_x), max(all_y), 0, 8, 2, 1, 0),
        b"hhea": struct.pack(">IhhhHhhhhhhhhhhhH", 0x00010000, ascent, -descent, line_gap, max(advances), min(bearings),
                             min(a - b - w for a, b, w in zip(advances, bearings, inks)),
                             max(b + w for b, w in zip(bearings, inks)), 1, 0, 0, 0, 0, 0, 0, 0, len(glyphs)),
        b"maxp": struct.pack(">IHHHHHHHHHHHHHH", 0x00010000, len(glyphs), max_points, max_contours, 0, 0, 2, 0, 0, 0,
                             0, 0, 0, 0, 0),
        b"OS/2": struct.pack(">HhHHHhhhhhhhhhhh10sIIII4sHHHhhhHHIIhhHHH", 4, sum(advances) // len(advances), 400, 5,
                             0, UNITS // 5, UNITS // 5, 0, UNITS // 10, UNITS // 5, UNITS // 5, 0, UNITS // 3,
                             UNITS // 20, UNITS // 4, 0, b"\0" * 10, 1, 0, 0, 0, b"GCTR", 0x40, 0x20,
                             max(c for c, _, _ in glyphs if c is not None) if len(glyphs) > 1 else 0x20, ascent, -descent, line_gap, ascent, descent, 1, 0, UNITS // 2, ascent * 2 // 3,
                             0, 0x20, 0),
        b"hmtx": b"".join(struct.pack(">Hh", a, b) for a, b in zip(advances, bearings)),
        b"cmap": _cmap([(code, i) for i, (code, _, _) in enumerate(glyphs) if code is not None]),
        b"loca": struct.pack(f">{len(loca)}I", *loca),
        b"glyf": glyf or b"\0\0\0\0",
        b"name": _name(family),
        b"post": struct.pack(">IIhhIIIII", 0x00030000, 0, -UNITS // 10, UNITS // 20, 0, 0, 0, 0, 0),
    }
    tags = sorted(tables)
    count = len(tags)
    search = 16 * (1 << (count.bit_length() - 1))
    header = struct.pack(">IHHHH", 0x00010000, count, search, (search // 16).bit_length() - 1, count * 16 - search)
    offset = 12 + 16 * count
    directory, body = b"", b""
    for tag in tags:
        data = tables[tag]
        directory += struct.pack(">4sIII", tag, _checksum(data), offset + len(body), len(data))
        body += data + b"\0" * (-len(data) % 4)
    font = bytearray(header + directory + body)
    head_at = offset + sum(len(tables[t]) + (-len(tables[t]) % 4) for t in tags[:tags.index(b"head")])
    struct.pack_into(">I", font, head_at + 8, (0xB1B0AFBA - _checksum(bytes(font))) & 0xFFFFFFFF)
    with open(path, "wb") as file:
        file.write(font)


def repair(path):
    """A font written before the side bearings were kept (hmtx all 0) gets them now, from each
    glyph's xMin, in place, and the head flags lose 'integer ppem'. True when the file changed."""
    font = bytearray(open(path, "rb").read())
    count = struct.unpack_from(">H", font, 4)[0]
    tables = {}
    for i in range(count):
        tag, _sum, offset, length = struct.unpack_from(">4sIII", font, 12 + 16 * i)
        tables[tag] = (12 + 16 * i, offset, length)
    _, head, _ = tables[b"head"]
    _, hmtx, _ = tables[b"hmtx"]
    _, loca, _ = tables[b"loca"]
    _, glyf, _ = tables[b"glyf"]
    glyph_count = struct.unpack_from(">H", font, tables[b"maxp"][1] + 4)[0]
    long_loca = struct.unpack_from(">h", font, head + 50)[0] == 1
    changed = False
    for g in range(glyph_count):
        start = struct.unpack_from(">I" if long_loca else ">H", font, loca + (4 if long_loca else 2) * g)[0]
        end = struct.unpack_from(">I" if long_loca else ">H", font, loca + (4 if long_loca else 2) * (g + 1))[0]
        if not long_loca:
            start, end = start * 2, end * 2
        bearing = struct.unpack_from(">h", font, glyf + start + 2)[0] if end > start else 0
        if struct.unpack_from(">h", font, hmtx + 4 * g + 2)[0] != bearing:
            struct.pack_into(">h", font, hmtx + 4 * g + 2, bearing)
            changed = True
    if struct.unpack_from(">H", font, head + 16)[0] != 0x0003:
        struct.pack_into(">H", font, head + 16, 0x0003)
        changed = True
    if not changed:
        return False
    struct.pack_into(">I", font, head + 8, 0)
    for tag, (entry, offset, length) in tables.items():
        struct.pack_into(">I", font, entry + 4, _checksum(bytes(font[offset:offset + length])))
    struct.pack_into(">I", font, head + 8, (0xB1B0AFBA - _checksum(bytes(font))) & 0xFFFFFFFF)
    with open(path, "wb") as file:
        file.write(font)
    return True


# ##############################################################################################
# # From a .brfnt                                                                              #
# ##############################################################################################
def make(font, characters, path, family=FAMILY):
    """Writes the characters of font (builder_layout.Font) that it has as a TrueType file."""
    import texreduce
    cell_w, cell_h = font.cell
    per_sheet = font.sheet_rows * font.sheet_cols
    baseline = getattr(font, "baseline", font.ascent)
    # Units per pixel: a line of the font is 1.25 em, as in most text fonts, so the window's
    # pixel sizes give the same text size as the system's fonts did.
    k = UNITS * 1.25 / font.height
    sheets = {}

    def coverage(index, width):
        sheet, cell = divmod(index, per_sheet)
        if sheet not in sheets:
            fmt, w, h, data = font.sheets[sheet]
            r, g, b, a = texreduce.decode(fmt, data, w, h)
            sheets[sheet] = (w, a if fmt in (0, 1) else a)
        w, alpha = sheets[sheet]
        col, row = cell % font.sheet_rows, cell // font.sheet_rows
        x0, y0 = col * (cell_w + 1), row * (cell_h + 1)
        return [[alpha[(y0 + y) * w + x0 + x] / 255.0 for x in range(width)] for y in range(cell_h)]

    glyphs = [(None, int(font.width * k * 0.6), [])]  # .notdef: empty
    for code in characters:
        index = font.map.get(code)
        if index is None:
            continue
        left, glyph_width, advance = font.widths.get(index, font.default_width)
        contours = []
        if glyph_width:
            loops = trace(coverage(index, glyph_width), glyph_width, cell_h)
            loops = [simplify(loop, 0.08) for loop in loops]
            loops = [loop for loop in loops if len(loop) >= 3 and abs(_area(loop)) > 0.05]
            # A pixel's centre is half a pixel in from its corner.
            for loop in orient([[(x + 0.5, -(y + 0.5)) for x, y in loop] for loop in loops]):
                contours.append([(round((x + left) * k), round((y + baseline) * k)) for x, y in loop])
        glyphs.append((code, max(0, round(advance * k)), contours))
    ascent = round(baseline * k)
    descent = round((cell_h - baseline) * k)
    write_ttf(path, glyphs, ascent, descent, max(0, round((font.line_feed - cell_h) * k)), family)
