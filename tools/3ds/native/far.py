"""Far models (.far) of fixed J3D models: the whole model as one draw, for when it is small on screen.

The native renderer draws a model one draw per material and shape. Far away, a model takes a few
pixels and its draws cost as much as near: the far model is one triangle list in one space, with
no texture, its colours baked into the vertices from the materials (their textures averaged
around each vertex and their Wii combiners evaluated, tevcheck.gx_eval), lit as the model's main
material lights it (platform/3ds/gx/native.cpp, rvlNativeFarPacket).

Only models that look the same every frame get one: no joint, colour, pattern or visibility
animation in their archive (texture scrolling is allowed: averaged colours do not show it), no
billboard, no weighted vertices, every material opaque (no blend, no alpha test). The vertices are
merged on a grid (vertex clustering), by cell and by the side their normal faces, so that thin
walls keep both faces.

Format (little-endian):
  header   "N3DF", u32 version, u16 draw matrix (DRW1 index whose joint space the vertices are
           in), u16 main material (J3D index: its lighting, fog, depth mode), u32 vertex count,
           u32 index count, f32 position scale, f32 sphere[4] (centre, radius), u8 cull none,
           u8 pad[3], u32 source triangles                                       (0x30 bytes)
           A header with no vertices: the model gets no far model (the converter does not
           look at it again).
  vertex   s16 position[3] (times the position scale), s8 normal[3] (/ 127), u8 pad,
           u8 colour[4], u8 pad[2]                                               (16 bytes)
  indices  u16 triangle list
"""

import math
import struct

import mat as M
import n3d
from tevcheck import gx_eval

# ##############################################################################################
# # The far model file                                                                         #
# #                                                                                            #
# # The header and vertex layout (they must match platform/3ds/gx/native.cpp, FarHeader and    #
# # FarVertex), and which models are worth one: fixed ones (no animation that moves them or    #
# # changes their colours) with at least a few draws.                                          #
# ##############################################################################################
MAGIC = b"N3DF"
VERSION = 1
HEADER = struct.Struct("<4sIHHIIf4fB3xI")
VERTEX = struct.Struct("<3h3bx4B2x")

# Animation files that change a model's shape or colours from frame to frame.
_MOVING = (".bck", ".bca", ".brk", ".btp", ".bpk", ".bva")
# Fewer draws than this: nothing to save.
MIN_DRAWS = 2
# Texture average grid (cells per side) used to colour vertices.
_TEXTURE_CELLS = 8


def _be16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def _be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


# ##############################################################################################
# # Joints                                                                                     #
# #                                                                                            #
# # The model's joint matrices in its rest pose, to put every vertex in the model's space.     #
# ##############################################################################################

def _mul(a, b):
    """3x4 matrices (rows), as 4x4 with (0, 0, 0, 1) last."""
    return [[a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + (a[r][3] if c == 3 else 0.0)
             for c in range(4)] for r in range(3)]


def _inverse(m):
    """Inverse of an affine 3x4 matrix."""
    a = [row[:3] for row in m]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    if abs(det) < 1e-12:
        return None
    inv = [[(a[(c + 1) % 3][(r + 1) % 3] * a[(c + 2) % 3][(r + 2) % 3] - a[(c + 1) % 3][(r + 2) % 3] * a[(c + 2) % 3][(r + 1) % 3]) / det
            for c in range(3)] for r in range(3)]
    t = [m[0][3], m[1][3], m[2][3]]
    return [inv[r] + [-(inv[r][0] * t[0] + inv[r][1] * t[1] + inv[r][2] * t[2])] for r in range(3)]


def _apply(m, p):
    return [m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3] for r in range(3)]


def _rotate(m, v):
    return [m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2] for r in range(3)]


def _local(joint):
    """J3DGetTranslateRotateMtx with the scale: T * Rz * Ry * Rx * S."""
    sx, sy, sz, rx, ry, rz, tx, ty, tz = joint
    ax, ay, az = (v * math.pi / 32768.0 for v in (rx, ry, rz))
    cx, cy, cz, snx, sny, snz = math.cos(ax), math.cos(ay), math.cos(az), math.sin(ax), math.sin(ay), math.sin(az)
    r = [[cy * cz, snx * sny * cz - cx * snz, cx * sny * cz + snx * snz],
         [cy * snz, snx * sny * snz + cx * cz, cx * sny * snz - snx * cz],
         [-sny, snx * cy, cx * cy]]
    return [[r[i][0] * sx, r[i][1] * sy, r[i][2] * sz, (tx, ty, tz)[i]] for i in range(3)]


def _joint_matrices(data, found):
    """Each joint's bind matrix in model space, or None when a joint is a billboard."""
    jb, _ = found["JNT1"]
    count = _be16(data, jb + 0x08)
    init, index = _be32(data, jb + 0x0C), _be32(data, jb + 0x10)
    joints = []
    for i in range(count):
        e = jb + init + _be16(data, jb + index + i * 2) * 0x40
        kind = _be16(data, e)
        if (kind >> 4) & 0xF:
            return None  # billboard joint (J3DJoint::getMtxType)
        joints.append(struct.unpack_from(">3f3h2x3f", data, e + 4))
    parent = [-1] * count
    ib, isize = found["INF1"]
    pos, stack, last = ib + _be32(data, ib + 0x14), [], -1
    while pos + 4 <= ib + isize:
        kind, value = _be16(data, pos), _be16(data, pos + 2)
        pos += 4
        if kind == 0:
            break
        if kind == 1:
            stack.append(last)
        elif kind == 2:
            last = stack.pop() if stack else -1
        elif kind == 0x10 and value < count:
            parent[value] = stack[-1] if stack else -1
            last = value
    world = [None] * count
    for i in range(count):
        chain, j = [], i
        while j >= 0 and world[j] is None and j not in chain:
            chain.append(j)
            j = parent[j]
        base = world[j] if j >= 0 and world[j] is not None else [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]
        for k in reversed(chain):
            base = _mul(base, _local(joints[k]))
            world[k] = base
    return world


def _draw_matrices(data, found):
    """(weighted, index) of each DRW1 entry."""
    db, _ = found["DRW1"]
    count = _be16(data, db + 0x08)
    flags, indices = _be32(data, db + 0x0C), _be32(data, db + 0x10)
    return [(data[db + flags + i], _be16(data, db + indices + i * 2)) for i in range(count)]


# ##############################################################################################
# # Textures                                                                                   #
# #                                                                                            #
# # Each texture as a small grid of average colours: a far model has no textures, its vertices #
# # take the colours.                                                                          #
# ##############################################################################################

class _Textures:
    """Each TEX1 texture as a grid of average colours, decoded on first use."""

    def __init__(self, data, found):
        self.data, self.cache, self.colours = data, {}, {}
        self.block = found["TEX1"][0] if "TEX1" in found else None

    def grid(self, index):
        if index in self.cache:
            return self.cache[index]
        result = None
        if self.block is not None and index < _be16(self.data, self.block + 8):
            try:
                import texreduce
            except ImportError:
                texreduce = None
            h = self.block + _be32(self.data, self.block + 0x0C) + index * 0x20
            fmt, width, height = self.data[h], _be16(self.data, h + 2), _be16(self.data, h + 4)
            wrap = (self.data[h + 6], self.data[h + 7])
            if texreduce is not None and fmt in texreduce.BLOCKS and fmt not in (texreduce.C4, texreduce.C8, texreduce.C14X2):
                image = h + _be32(self.data, h + 0x1C)
                channels = texreduce.decode(fmt, self.data[image:image + texreduce.image_size(fmt, width, height)], width, height)
                cells = _TEXTURE_CELLS
                cw, ch = max(1, width // cells), max(1, height // cells)
                gw, gh = max(1, width // cw), max(1, height // ch)
                cells_rgba = []
                for gy in range(gh):
                    row = []
                    for gx in range(gw):
                        total = [0, 0, 0, 0]
                        n = 0
                        for y in range(gy * ch, min(height, (gy + 1) * ch)):
                            for x in range(gx * cw, min(width, (gx + 1) * cw)):
                                i = y * width + x
                                for c in range(4):
                                    total[c] += channels[c][i]
                                n += 1
                        row.append(tuple(t // max(1, n) for t in total))
                    cells_rgba.append(row)
                mean = tuple(sum(c[k] for row in cells_rgba for c in row) // (gw * gh) for k in range(4))
                result = (cells_rgba, gw, gh, wrap, mean)
        self.cache[index] = result
        return result

    def sample(self, index, uv):
        g = self.grid(index)
        if g is None:
            return (255, 255, 255, 255)
        cells, gw, gh, wrap, mean = g
        if uv is None:
            return mean

        def coord(v, n, mode):
            if mode == 0:  # clamp
                v = min(max(v, 0.0), 0.999999)
            elif mode == 2:  # mirror
                v = v % 2.0
                v = 2.0 - v if v > 1.0 else v
                v = min(v, 0.999999)
            else:
                v = v % 1.0
            return min(n - 1, int(v * n))

        return cells[coord(uv[1], gh, wrap[1])][coord(uv[0], gw, wrap[0])]


# ##############################################################################################
# # Materials                                                                                  #
# #                                                                                            #
# # Which materials are opaque (only those are kept), and a vertex's colour from its material  #
# # and texture.                                                                               #
# ##############################################################################################

def _opaque(m):
    """Drawn opaque, with depth writes, and not a texture effect the colours cannot keep."""
    blend = M.fragment_state(m)
    return (m["mode"] == 1 and m["blend"][0] == 0 and blend[3] == M.TEST_ALWAYS and m["zmode"][0] and m["zmode"][2]
            and not m["indirect"])


def _vertex_colour(m, v, textures):
    tex = {}
    for s in range(m["stage_count"]):
        coord, tmap, _ = m["orders"][s]
        if tmap == 0xFF or (tmap, coord) in tex:
            continue
        number = m["tex_numbers"][tmap] if tmap < len(m["tex_numbers"]) else 0xFFFF
        uv = None
        if coord < len(m["texgens"]):
            src = m["texgens"][coord][1]
            if 4 <= src <= 11:  # GX_TG_TEX0..7: the vertex's coordinates
                uv = v.get(n3d.GX_TEX0 + src - 4)
        tex[(tmap, coord)] = textures.sample(number, uv) if number != 0xFFFF else (255, 255, 255, 255)
    ras = []
    for c in range(2):
        colour = m["chans"][c * 2] if c * 2 < len(m["chans"]) else bytes(8)
        alpha = m["chans"][c * 2 + 1] if c * 2 + 1 < len(m["chans"]) else bytes(8)
        vertex = v.get(n3d.GX_CLR0 + c, [255, 255, 255, 255])
        register = m["mat_colors"][c]
        rgb = vertex if colour[1] == 1 else register
        a = vertex[3] if alpha[1] == 1 else register[3]
        ras.append([rgb[0], rgb[1], rgb[2], a])
    # Most vertices of a material fall on the same few texture cells and colours.
    key = (id(m), tuple(sorted(tex.items())), tuple(map(tuple, ras)))
    cache = textures.colours
    if key not in cache:
        cache[key] = gx_eval(m, m["tev_colors"], tex, ras)
    return cache[key]


# ##############################################################################################
# # Building the far model                                                                     #
# #                                                                                            #
# # The opaque triangles of the whole model in one list, their vertices merged where they fall #
# # together at the far model's precision.                                                     #
# ##############################################################################################

def none():
    """The .far of a model that gets no far model."""
    return HEADER.pack(MAGIC, VERSION, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0, 0, 0)


def is_current(header):
    """True for the first 8 bytes of a .far this version wrote (with or without a far model)."""
    return header[:4] == MAGIC and int.from_bytes(header[4:8], "little") == VERSION


def eligible_archive(members):
    """False when the archive animates its models' shape or colours."""
    return not any(name.lower().endswith(_MOVING) for name, _ in members)


def build(data, animations):
    """The .far of a Wii (big-endian) J3D model, or None when it gets none. animations: the
    (name, content) files of its archive (and of its main model's, for a Middle/Low model)."""
    if not eligible_archive(animations):
        return None
    found = n3d.blocks(data)
    if any(b not in found for b in ("SHP1", "VTX1", "MAT3", "JNT1", "INF1", "DRW1")):
        return None
    materials = M.parse(data, *found["MAT3"])
    if len(materials) < MIN_DRAWS or not all(_opaque(m) for m in materials):
        return None
    joints = _joint_matrices(data, found)
    if joints is None:
        return None
    draws = _draw_matrices(data, found)

    # The shapes, as n3d decodes them, with their material.
    stats = n3d.Stats()
    vtx = n3d.VertexArrays(data, *found["VTX1"])
    sb, _ = found["SHP1"]
    shape_count = _be16(data, sb + 0x08)
    shape_material = _shape_materials(data, found, shape_count)
    init, index, _, vtx_desc, mtx_table, dl_data, mtx_init, draw_init = [_be32(data, sb + 0x0C + 4 * i) for i in range(8)]
    corners = []  # (position in model space, normal, colour) per triangle corner
    used_draws = set()
    material_triangles = [0] * len(materials)
    textures = _Textures(data, found)
    source_triangles = 0
    for s in range(shape_count):
        si = sb + init + _be16(data, sb + index + s * 2) * 0x28
        if data[si] not in (0, 3):
            return None  # billboard shape
        mi = shape_material.get(s)
        if mi is None or mi >= len(materials):
            return None
        m = materials[mi]
        desc = []
        p = sb + vtx_desc + _be16(data, si + 0x04)
        while True:
            attr, kind = struct.unpack_from(">II", data, p)
            p += 8
            if attr == n3d.GX_END:
                break
            if kind != n3d.GX_NONE:
                desc.append((attr, kind))
        previous = [0xFFFF] * 10
        for g in range(_be16(data, si + 0x02)):
            gi = sb + mtx_init + (_be16(data, si + 0x06) + g) * 8
            use_mtx, mcount, mstart = struct.unpack_from(">HHI", data, gi)
            matrices = [_be16(data, sb + mtx_table + (mstart + k) * 2) for k in range(mcount)]
            if mcount == 0 or data[si] != 3:
                matrices = [use_mtx]
            matrices = [previous[k] if mm == 0xFFFF and k < len(previous) else mm for k, mm in enumerate(matrices)]
            previous = matrices + previous[len(matrices):]
            di = sb + draw_init + (_be16(data, si + 0x08) + g) * 8
            dl_size, dl_offset = struct.unpack_from(">II", data, di)
            verts, tris = n3d.decode_list(data, sb + dl_data + dl_offset, dl_size, desc, vtx, stats)
            placed = []
            for v in verts:
                slot = v["slot"] if v["slot"] < len(matrices) else 0
                drw = matrices[slot]
                if drw >= len(draws) or draws[drw][0] or draws[drw][1] >= len(joints):
                    return None  # weighted (skinned) or unknown matrix
                used_draws.add(drw)
                w = joints[draws[drw][1]]
                normal = _rotate(w, v.get(n3d.GX_NRM, [0.0, 0.0, 0.0]))
                colour = _vertex_colour(m, v, textures)
                if colour is None:
                    return None
                placed.append((_apply(w, v[n3d.GX_POS]), normal, colour))
            for k in range(0, len(tris) - 2, 3):
                corners.append((placed[tris[k]], placed[tris[k + 1]], placed[tris[k + 2]]))
            material_triangles[mi] += len(tris) // 3
            source_triangles += len(tris) // 3
    if not corners or not used_draws:
        return None

    # The space of the draw matrix of the lowest joint drawn.
    base = min(used_draws, key=lambda d: (draws[d][1], d))
    to_base = _inverse(joints[draws[base][1]])
    if to_base is None:
        return None
    positions, indices = _cluster(corners, to_base)
    if not indices or len(indices) // 3 >= source_triangles:
        return None
    main = max(range(len(materials)), key=lambda i: material_triangles[i])
    cull_none = any(m["cull"] == 0 for m in materials)
    return _write(base, main, positions, indices, cull_none, source_triangles)


def _shape_materials(data, found, shape_count):
    """{shape: material} from the INF1 hierarchy (a material node, then its shapes)."""
    ib, isize = found["INF1"]
    pos, material, out = ib + _be32(data, ib + 0x14), None, {}
    while pos + 4 <= ib + isize:
        kind, value = _be16(data, pos), _be16(data, pos + 2)
        pos += 4
        if kind == 0:
            break
        if kind == 0x11:
            material = value
        elif kind == 0x12 and value < shape_count and material is not None:
            out[value] = _material_index(data, found, material)
    return out


def _material_index(data, found, node):
    """The J3D material of an INF1 material node: its number, mat.parse's order."""
    mb, _ = found["MAT3"]
    return node if node < _be16(data, mb + 0x08) else None


def _cluster(corners, to_base):
    """Vertices merged on a grid by cell and by the axis their normal faces: positions (with
    normal and colour) and the triangle list, smaller each try until it has at most a quarter of
    the triangles (and at least a few)."""
    points = [[(_apply(to_base, p), _normalized(_rotate(to_base, n)), c) for p, n, c in tri] for tri in corners]
    low = [min(v[0][i] for tri in points for v in tri) for i in range(3)]
    high = [max(v[0][i] for tri in points for v in tri) for i in range(3)]
    extent = max(high[i] - low[i] for i in range(3)) or 1.0
    source = len(points)
    target = max(64, source // 4)
    cells = 32
    while True:
        size = extent / cells
        clusters, index_of, indices = [], {}, []
        seen = set()
        for tri in points:
            ids = []
            for p, n, c in tri:
                axis = max(range(3), key=lambda i: abs(n[i])) if any(n) else 0
                key = (int((p[0] - low[0]) / size), int((p[1] - low[1]) / size), int((p[2] - low[2]) / size),
                       axis * 2 + (n[axis] < 0))
                k = index_of.get(key)
                if k is None:
                    k = index_of[key] = len(clusters)
                    clusters.append([[0.0, 0.0, 0.0], [0.0, 0.0, 0.0], [0, 0, 0, 0], 0])
                acc = clusters[k]
                for i in range(3):
                    acc[0][i] += p[i]
                    acc[1][i] += n[i]
                for i in range(4):
                    acc[2][i] += c[i]
                acc[3] += 1
                ids.append(k)
            if len(set(ids)) < 3:
                continue
            rotated = min((ids[i], ids[(i + 1) % 3], ids[(i + 2) % 3]) for i in range(3))
            if rotated in seen:
                continue
            seen.add(rotated)
            indices += ids
        if len(indices) // 3 <= target or cells <= 4:
            break
        cells = max(4, int(cells * 0.75))
    used = sorted(set(indices))
    remap = {k: i for i, k in enumerate(used)}
    positions = []
    for k in used:
        p, n, c, count = clusters[k]
        positions.append(([v / count for v in p], _normalized(n), [min(255, round(v / count)) for v in c]))
    return positions, [remap[k] for k in indices]


def _normalized(v):
    length = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return [x / length for x in v] if length > 1e-9 else [0.0, 0.0, 0.0]


def _write(base, main, vertices, indices, cull_none, source_triangles):
    low = [min(v[0][i] for v in vertices) for i in range(3)]
    high = [max(v[0][i] for v in vertices) for i in range(3)]
    centre = [(low[i] + high[i]) * 0.5 for i in range(3)]
    radius = math.sqrt(sum(((high[i] - low[i]) * 0.5) ** 2 for i in range(3)))
    scale = (max(abs(c) for v in vertices for c in v[0]) or 1.0) / 32767
    out = bytearray(HEADER.pack(MAGIC, VERSION, base, main, len(vertices), len(indices), scale, *centre, radius,
                                1 if cull_none else 0, source_triangles))
    for p, n, c in vertices:
        out += VERTEX.pack(*[n3d.quantize(x, scale, 32767) for x in p], *[n3d.quantize(x, 1.0 / 127, 127) for x in n], *c)
    out += struct.pack("<%dH" % len(indices), *indices)
    return bytes(out)
