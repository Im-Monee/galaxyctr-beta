"""Native 3DS model files (.n3d) from the game's J3D models (.bdl, .bmd).

The game draws its models through J3D (libs/JSystem/J3DGraphBase, J3DGraphLoader): every shape
(SHP1) is a list of matrix groups, each a GX display list whose vertices index the model's
vertex arrays (VTX1) and pick their matrix from the group's table of draw matrices (DRW1
indices). The 3DS port emulated that list every frame; an .n3d holds it already decoded, so
the runtime only uploads matrices and draws.

Format (little-endian, offsets from the start of the file):
  header   "N3D1", u32 version, u32 shape count, u32 shape table offset, u32 material count,
           u32 material table offset (materials in J3D order, mat.MATERIAL_SIZE each: mat.py),
           u32 record count (the materials, then the records of their later passes)
  shape    u8 matrix type (J3DShape mode: 0 normal, 1 billboard, 2 Y billboard, 3 multi),
           u8 pad, u16 group count, u32 attribute mask (ATTR_*), u16 vertex stride, u16 pad,
           f32 radius, f32 min[3], f32 max[3], u32 group table offset,
           f32 position scale, f32 texture coordinate scale                      (0x34 bytes)
  group    u16 matrix count, u16 pad, u16 matrices[10] (DRW1 index, slot = vertex matrix),
           u32 vertex count, u32 vertex offset, u32 index count, u32 index offset (0x2C bytes)
  vertex   s16 position[3] (times the position scale), u8 matrix slot, u8 pad, u32 source
           VTX1 position index (ATTR_POS_F32:
           f32 position[3], u8 matrix slot, u8 pad[3]);
           s8 normal[3] (/ 127; ATTR_NRM_X2: / 63.5), s8 pad (ATTR_NRM); u8 colour0[4] (ATTR_CLR0); u8 colour1[4]
           (ATTR_CLR1); s16 tex[n][2] (times the texture coordinate scale; ATTR_TEX0 << n);
           u8 texture-matrix indices[8] (ATTR_TEXMTX; values from GX_TEX0MTXIDX..GX_TEX7MTXIDX)
  indices  u16 triangle list (GX winding: strips alternate, fans and quads from vertex 0)
"""

import os
import struct

import mat

# ##############################################################################################
# # The format's constants                                                                     #
# #                                                                                            #
# # The .n3d version (raised whenever the format or the material compiler changes, so the      #
# # converter writes the files again) and the bits of a shape's vertex format; they must match #
# # platform/3ds/gx/native.cpp.                                                                #
# ##############################################################################################
MAGIC = b"J3D2"
VERSION = 16  # 16: mat.compare_stage keeps a stage's alpha in the buffer

ATTR_NRM = 1 << 0
ATTR_CLR0 = 1 << 1
ATTR_CLR1 = 1 << 2
ATTR_TEX0 = 1 << 3  # ATTR_TEX0 << n for texture coordinate set n (0..7)
ATTR_POS_F32 = 1 << 11  # positions as f32[3] + u8 slot + 3 pad (large shapes: s16 would leave gaps)
ATTR_TEXMTX = 1 << 12  # per-vertex GX texture matrix indices, kept for direct J3D rendering
ATTR_SOURCE_POS = 1 << 13  # source VTX1 position index for J3D CPU skinning
# Normals stored at half scale (s8 / 63.5): the shape has normals longer than 1. The Wii reads s16
# normals with 14 fraction bits whatever their length (about 350 models have them near 2) and
# lights with them as they are; clamped to 1 per component they changed direction by up to 15
# degrees (model audit, 2026-10-29).
ATTR_NRM_X2 = 1 << 14
POS_F32_SCALE = 1.0 / 64  # s16 steps coarser than this use f32 positions

# GX vertex attributes (GXAttr).
GX_PNMTXIDX = 0
GX_TEX0MTXIDX = 1
GX_POS = 9
GX_NRM = 10
GX_CLR0 = 11
GX_CLR1 = 12
GX_TEX0 = 13
GX_END = 0xFF

GX_NONE, GX_DIRECT, GX_INDEX8, GX_INDEX16 = 0, 1, 2, 3

PRIM_QUADS, PRIM_QUADS2, PRIM_TRIANGLES, PRIM_TRISTRIP, PRIM_TRIFAN = 0x80, 0x88, 0x90, 0x98, 0xA0


# ##############################################################################################
# # Reading the J3D model                                                                      #
# #                                                                                            #
# # Little helpers and the model's blocks, read from the Wii (big-endian) model.               #
# ##############################################################################################
class N3DError(Exception):
    pass


def u8(b, o):
    return b[o]


def u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def is_j3d_model(data):
    return (
        data[:4] == MAGIC
        and data[4:8] in (b"bdl4", b"bmd3", b"bmd2")
    ) or (
        data[:4] == b"2D3J"
        and data[4:8] in (b"4ldb", b"3dmb", b"2dmb")
    )


def blocks(data):
    count = u32(data, 0x0C)
    pos = 0x20
    found = {}
    for _ in range(count):
        tag = data[pos : pos + 4].decode("ascii", "replace")
        size = u32(data, pos + 4)
        found[tag] = (pos, size)
        pos += size
    return found


# ##############################################################################################
# # Vertex arrays (VTX1)                                                                       #
# #                                                                                            #
# # The model's position, normal, colour and texture coordinate arrays, decoded from their GX  #
# # component types into numbers.                                                              #
# ##############################################################################################
COMP_SIZE = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4}
COMP_FMT = {0: "B", 1: "b", 2: "H", 3: "h", 4: "f"}


class VertexArrays:
    def __init__(self, data, block, size):
        self.data = data
        end = block + size
        offsets = [u32(data, block + 0x08 + 4 * i) for i in range(14)]
        self.formats = {}
        pos = block + offsets[0]
        while pos + 16 <= end:
            attr, count, comp, frac = struct.unpack_from(">IIIB", data, pos)
            pos += 16
            if attr == GX_END:
                break
            self.formats[attr] = (count, comp, frac)

        # Array starts by attribute (slot 1 position, 2 normal, 4/5 colours, 6.. texture sets).
        slots = {GX_POS: 1, GX_NRM: 2, GX_CLR0: 4, GX_CLR1: 5}
        for t in range(8):
            slots[GX_TEX0 + t] = 6 + t
        self.arrays = {}
        for attr, slot in slots.items():
            if offsets[slot]:
                self.arrays[attr] = block + offsets[slot]

    def read(self, attr, index):
        count, comp, frac = self.formats[attr]
        base = self.arrays[attr]
        if attr in (GX_CLR0, GX_CLR1):
            return self._colour(base, comp, index)
        n = {GX_POS: 3 if count else 2, GX_NRM: 3}.get(attr, 2 if count else 1)
        if attr == GX_NRM:
            frac = 6 if comp == 1 else 14 if comp == 3 else frac
        size = COMP_SIZE[comp]
        values = struct.unpack_from(">" + COMP_FMT[comp] * n, self.data, base + index * n * size)
        scale = 1.0 if comp == 4 else 1.0 / (1 << frac)
        values = [v * scale for v in values]
        while len(values) < (3 if attr in (GX_POS, GX_NRM) else 2):
            values.append(0.0)
        return values

    def _colour(self, base, comp, index):
        d = self.data
        if comp == 0:  # RGB565
            v = u16(d, base + index * 2)
            return [((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255]
        if comp == 1:  # RGB8
            p = base + index * 3
            return [d[p], d[p + 1], d[p + 2], 255]
        if comp == 2:  # RGBX8
            p = base + index * 4
            return [d[p], d[p + 1], d[p + 2], 255]
        if comp == 3:  # RGBA4
            v = u16(d, base + index * 2)
            return [((v >> 12) & 15) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17]
        if comp == 4:  # RGBA6
            p = base + index * 3
            v = (d[p] << 16) | (d[p + 1] << 8) | d[p + 2]
            return [((v >> 18) & 63) * 255 // 63, ((v >> 12) & 63) * 255 // 63, ((v >> 6) & 63) * 255 // 63, (v & 63) * 255 // 63]
        p = base + index * 4  # RGBA8
        return [d[p], d[p + 1], d[p + 2], d[p + 3]]


# ##############################################################################################
# # Shapes (SHP1) and the file                                                                 #
# #                                                                                            #
# # Each shape's display lists are decoded into triangle lists (strips and fans split), its    #
# # vertices packed in the smallest format that keeps them (positions as s16 at a scale, or    #
# # f32 for large shapes), grouped by the matrix slots they use. The materials are compiled by #
# # mat.py; animated colour registers and texture patterns are found from the archive's        #
# # animation files, since a compiled material must leave those values to the run time.        #
# ##############################################################################################

def triangles(prim, first, count):
    out = []
    if prim in (PRIM_QUADS, PRIM_QUADS2):
        for i in range(0, count - 3, 4):
            out += [first + i, first + i + 1, first + i + 2, first + i, first + i + 2, first + i + 3]
    elif prim == PRIM_TRIANGLES:
        out = list(range(first, first + count - count % 3))
    elif prim == PRIM_TRISTRIP:
        for i in range(count - 2):
            odd = i & 1
            out += [first + i + odd, first + i + 1 - odd, first + i + 2]
    elif prim == PRIM_TRIFAN:
        for i in range(1, count - 1):
            out += [first, first + i, first + i + 1]
    return out


class Stats:
    def __init__(self):
        self.models = 0
        self.shapes = 0
        self.groups = 0
        self.vertices = 0
        self.triangles = 0
        self.skipped_prims = 0  # lines and points (not drawn natively)
        self.materials = 0
        self.inexact = 0  # materials whose TEV needed approximations (mat.FLAG_INEXACT)
        self.multipass = 0  # materials drawn in more than one pass


def lod_parent_archive(archive_name):
    """The archive of the model a Middle/Low LOD model belongs to ("TicoMiddle.arc" -> "Tico.arc"),
    or None. LodCtrl loads "/ObjectData/<res>Middle.arc" and "<res>Low.arc" and drives them with the
    main model's animations (syncMaterialAnimation): those archives hold only the model, so the
    colour and pattern animations that make registers and textures change are in the parent's."""
    stem, ext = os.path.splitext(archive_name)
    for suffix in ("Middle", "Low"):
        if stem.endswith(suffix) and len(stem) > len(suffix):
            return stem[: -len(suffix)] + ext
    return None


def animated_registers(members):
    """{material name: {TEV colour register 1-3: {"rgb": values, "alpha": values}}}: the values
    the colour animations (.brk, TRK1) among an archive's (name, content) files give the
    registers, original big-endian or converted little-endian."""
    registers = {}
    for name, c in members:
        if not name.lower().endswith(".brk") or len(c) < 0x60:
            continue
        if c[:8] == b"J3D1brk1" and c[0x20:0x24] == b"TRK1":
            bo = ">"
        elif c[:8] == b"1D3J1krb" and c[0x20:0x24] == b"1KRT":
            bo = "<"
        else:
            continue
        p = 0x20
        count = struct.unpack_from(bo + "H", c, p + 0x0C)[0]
        table, _, _, _, name_offset = struct.unpack_from(bo + "5I", c, p + 0x20)
        data_offsets = struct.unpack_from(bo + "4I", c, p + 0x38)  # R, G, B, A values (s16)
        if not table or not name_offset:
            continue
        names = mat.name_table(c, p + name_offset, bo)
        for i in range(min(count, len(names))):
            entry = p + table + i * 0x1C
            color_id = c[entry + 0x18]
            if color_id >= 3:
                continue
            values = registers.setdefault(names[i], {}).setdefault(color_id + 1, {"rgb": [], "alpha": []})
            for k in range(4):
                keys, first, tangent = struct.unpack_from(bo + "3H", c, entry + k * 6)
                start = p + data_offsets[k] + first * 2
                if keys == 1:
                    found = [struct.unpack_from(bo + "h", c, start)[0]]
                else:
                    stride = 4 if tangent else 3  # time, value, tangent(s)
                    found = [struct.unpack_from(bo + "h", c, start + (j * stride + 1) * 2)[0] for j in range(keys)]
                values["alpha" if k == 3 else "rgb"] += found
    return registers


def patterned_materials(members):
    """Names of the materials whose textures a texture pattern animation (.btp, TPT1) among an
    archive's files swaps: a bound from their own textures does not hold for them."""
    names = set()
    for name, c in members:
        if not name.lower().endswith(".btp") or len(c) < 0x40:
            continue
        if c[:8] == b"J3D1btp1" and c[0x20:0x24] == b"TPT1":
            bo = ">"
        elif c[:8] == b"1D3J1ptb" and c[0x20:0x24] == b"1TPT":
            bo = "<"
        else:
            continue
        p = 0x20
        name_offset = struct.unpack_from(bo + "I", c, p + 0x1C)[0]
        if name_offset:
            names.update(mat.name_table(c, p + name_offset, bo))
    return names


def texture_maxima(data, tex1):
    """A function giving the largest value of each channel (r, g, b, a) of TEX1 texture i, or None
    when it is unknown (palette formats, no TEX1); decoded once, on first use (mat.wrapped_last_alpha)."""
    cache = {}

    def get(i):
        if tex1 is None:
            return None
        if i not in cache:
            cache[i] = None
            block = tex1[0]
            count, headers = struct.unpack_from(">H", data, block + 8)[0], struct.unpack_from(">I", data, block + 0x0C)[0]
            if i < count:
                h = block + headers + i * 0x20
                fmt, width, height = data[h], *struct.unpack_from(">HH", data, h + 2)
                try:
                    import texreduce
                except ImportError:
                    return None
                if fmt in (texreduce.ETC1, texreduce.ETC1A4, texreduce.LA4):
                    cache[i] = tuple(data[h + 0x0C:h + 0x10])  # texreduce._compressed
                elif fmt in texreduce.BLOCKS and fmt not in (texreduce.C4, texreduce.C8, texreduce.C14X2):
                    image = h + struct.unpack_from(">I", data, h + 0x1C)[0]
                    channels = texreduce.decode(fmt, data[image:image + texreduce.image_size(fmt, width, height)], width, height)
                    cache[i] = tuple(max(c) for c in channels)
        return cache[i]

    return get


def convert_model(data, stats, animated=None, patterned=None):
    found = blocks(data)
    if "SHP1" not in found or "VTX1" not in found:
        raise N3DError("no SHP1/VTX1 block")
    vtx = VertexArrays(data, *found["VTX1"])
    block, size = found["SHP1"]
    count = u16(data, block + 0x08)
    init, index, names, vtx_desc, mtx_table, dl_data, mtx_init, draw_init = [u32(data, block + 0x0C + 4 * i) for i in range(8)]

    shapes = []
    for s in range(count):
        si = block + init + u16(data, block + index + s * 2) * 0x28
        mode = data[si]
        group_count = u16(data, si + 0x02)
        desc_offset = u16(data, si + 0x04)
        mtx_first = u16(data, si + 0x06)
        draw_first = u16(data, si + 0x08)
        radius, *box = struct.unpack_from(">7f", data, si + 0x0C)

        desc = []
        p = block + vtx_desc + desc_offset
        while True:
            attr, kind = struct.unpack_from(">II", data, p)
            p += 8
            if attr == GX_END:
                break
            if kind != GX_NONE:
                desc.append((attr, kind))

        mask = 0
        for attr, _ in desc:
            if attr == GX_NRM:
                mask |= ATTR_NRM
            elif attr == GX_CLR0:
                mask |= ATTR_CLR0
            elif attr == GX_CLR1:
                mask |= ATTR_CLR1
            elif GX_TEX0MTXIDX <= attr <= 8:
                mask |= ATTR_TEXMTX
            elif GX_TEX0 <= attr < GX_TEX0 + 8:
                mask |= ATTR_TEX0 << (attr - GX_TEX0)
        if any(attr == GX_POS for attr, _ in desc):
            mask |= ATTR_SOURCE_POS

        groups = []
        previous = [0xFFFF] * 10
        for g in range(group_count):
            mi = block + mtx_init + (mtx_first + g) * 8
            use_mtx, mcount, mstart = struct.unpack_from(">HHI", data, mi)
            matrices = [u16(data, block + mtx_table + (mstart + k) * 2) for k in range(mcount)]
            if mcount == 0 or mode != 3:
                # Only multi-matrix shapes read the table; the others use the group's own index
                # (J3DShapeFactory::newShapeMtx).
                matrices = [use_mtx]
            # 0xFFFF: the slot keeps the matrix the previous group loaded (J3DShapeMtxMulti).
            matrices = [previous[k] if m == 0xFFFF and k < len(previous) else m for k, m in enumerate(matrices)]
            if len(matrices) > 10:
                raise N3DError("shape group uses %d matrices; native limit is 10" % len(matrices))
            previous = matrices + previous[len(matrices) :]

            di = block + draw_init + (draw_first + g) * 8
            dl_size, dl_offset = struct.unpack_from(">II", data, di)
            groups.append((matrices, decode_list(data, block + dl_data + dl_offset, dl_size, desc, vtx, stats)))
        shapes.append((mode, mask, radius, box, groups))
        stats.shapes += 1
        stats.groups += len(groups)
    materials = []
    passes = []
    if "MAT3" in found:
        parsed = mat.parse(data, *found["MAT3"])
        texture_max = texture_maxima(data, found.get("TEX1"))
        for m in parsed:
            m["animated_registers"] = (animated or {}).get(m["name"], {})
            m["texture_max"] = texture_max if m["name"] not in (patterned or ()) else None
            packed, inexact, extra = mat.pack(m, len(parsed) + len(passes))
            materials.append(packed)
            passes += extra
            stats.materials += 1
            stats.inexact += inexact
            stats.multipass += bool(extra)
    stats.models += 1
    return write(shapes, materials, passes)


def decode_list(data, start, size, desc, vtx, stats):
    """The group's vertices (deduplicated) and triangle list."""
    p, end = start, start + size
    unique = {}
    verts = []
    tris = []
    while p < end:
        cmd = data[p]
        p += 1
        if cmd == 0:
            continue
        if cmd < 0x80:
            raise N3DError("display list command 0x%02x" % cmd)
        prim = cmd & 0xF8
        n = u16(data, p)
        p += 2
        ids = []
        for _ in range(n):
            key = []
            for attr, kind in desc:
                if kind == GX_DIRECT:
                    if attr <= 8:  # matrix indices
                        key.append(data[p])
                        p += 1
                    else:
                        raise N3DError("direct vertex attribute %d" % attr)
                elif kind == GX_INDEX8:
                    key.append(data[p])
                    p += 1
                else:
                    key.append(u16(data, p))
                    p += 2
            key = tuple(key)
            if key not in unique:
                unique[key] = len(verts)
                verts.append(key)
            ids.append(unique[key])
        if prim > PRIM_TRIFAN:
            stats.skipped_prims += 1
            continue
        local = triangles(prim, 0, n)
        tris += [ids[i] for i in local]

    out = []
    for key in verts:
        v = {"slot": 0}
        v["texMtx"] = [0] * 8
        for (attr, _), value in zip(desc, key):
            if attr == GX_PNMTXIDX:
                v["slot"] = value // 3
            elif attr == GX_POS:
                v["sourcePos"] = value
                v[attr] = vtx.read(attr, value)
            elif GX_TEX0MTXIDX <= attr <= 8:
                v["texMtx"][attr - GX_TEX0MTXIDX] = value
            elif attr <= 8:
                continue
            else:
                v[attr] = vtx.read(attr, value)
        out.append(v)
    stats.vertices += len(out)
    stats.triangles += len(tris) // 3
    return out, tris


def stride_of(mask):
    size = (16 if mask & ATTR_POS_F32 else 8) + 4  # position, matrix slot + source VTX1 index
    if mask & ATTR_NRM:
        size += 4
    if mask & ATTR_CLR0:
        size += 4
    if mask & ATTR_CLR1:
        size += 4
    for t in range(8):
        if mask & (ATTR_TEX0 << t):
            size += 4
    if mask & ATTR_TEXMTX:
        size += 8
    return size


def quantize(value, scale, limit):
    return max(-limit, min(limit, int(round(value / scale))))


def pack_vertex(v, mask, pos_scale, uv_scale):
    if mask & ATTR_POS_F32:
        b = struct.pack("<3fB3x", *v[GX_POS], v["slot"])
    else:
        b = struct.pack("<3hBB", *[quantize(c, pos_scale, 32767) for c in v[GX_POS]], v["slot"], 0)
    b += struct.pack("<I", v.get("sourcePos", 0))
    if mask & ATTR_NRM:
        step = 2.0 / 127 if mask & ATTR_NRM_X2 else 1.0 / 127
        b += struct.pack("<3bb", *[quantize(c, step, 127) for c in v[GX_NRM]], 0)
    if mask & ATTR_CLR0:
        b += bytes(v[GX_CLR0])
    if mask & ATTR_CLR1:
        b += bytes(v[GX_CLR1])
    for t in range(8):
        if mask & (ATTR_TEX0 << t):
            b += struct.pack("<2h", *[quantize(c, uv_scale, 32767) for c in v[GX_TEX0 + t]])
    if mask & ATTR_TEXMTX:
        b += bytes(v["texMtx"])
    return b


def scales(groups, mask):
    """Position and texture coordinate scales: the largest magnitude maps to 32767."""
    pos = uv = 0.0
    for _, (verts, _) in groups:
        for v in verts:
            pos = max(pos, max(abs(c) for c in v[GX_POS]))
            for t in range(8):
                if mask & (ATTR_TEX0 << t):
                    uv = max(uv, max(abs(c) for c in v[GX_TEX0 + t]))
    return (pos or 1.0) / 32767, (uv or 1.0) / 32767


def align4(b):
    b += bytes(-len(b) % 4)


def write(shapes, materials, passes):
    out = bytearray(struct.pack("<4sIIIIII", b"N3D1", VERSION, len(shapes), 0x1C, len(materials), 0, len(materials) + len(passes)))
    shape_table = len(out)
    out += bytes(0x34 * len(shapes))
    for s, (mode, mask, radius, box, groups) in enumerate(shapes):
        if mode > 3:
            raise N3DError("unsupported SHP1 shape mode %d" % mode)
        if any(len(matrices) > 10 for matrices, _ in groups):
            raise N3DError("shape group exceeds the native ten-matrix limit")
        group_table = len(out)
        out += bytes(0x2C * len(groups))
        pos_scale, uv_scale = scales(groups, mask)
        if mask & ATTR_NRM and any(abs(c) > 1.0 for _, (verts, _) in groups for v in verts for c in v[GX_NRM]):
            mask |= ATTR_NRM_X2
        if pos_scale > POS_F32_SCALE:
            mask |= ATTR_POS_F32
            pos_scale = 1.0
        for g, (matrices, (verts, tris)) in enumerate(groups):
            vertex_offset = len(out)
            for v in verts:
                out += pack_vertex(v, mask, pos_scale, uv_scale)
            align4(out)
            index_offset = len(out)
            out += struct.pack("<%dH" % len(tris), *tris)
            align4(out)
            slots = matrices + [0xFFFF] * (10 - len(matrices))
            struct.pack_into("<HH10HIIII", out, group_table + g * 0x2C, len(matrices), 0, *slots, len(verts), vertex_offset, len(tris), index_offset)
        struct.pack_into("<BBHIHH7fIff", out, shape_table + s * 0x34, mode, 0, len(groups), mask, stride_of(mask), 0, radius, *box, group_table, pos_scale, uv_scale)
    align4(out)
    struct.pack_into("<I", out, 0x14, len(out))
    for m in materials + passes:
        out += m
    return bytes(out)
