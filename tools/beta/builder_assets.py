"""The builder window's game graphics, made from the user's own disc.

The window (builder_window.py) can dress itself in Super Mario Galaxy's own buttons, window frames
and icons, and show the game's GalaxySky behind its start page. None of that is in the builder or
the repository: like the HOME Menu icon (galaxyctr_builder.make_icon_and_banner), it is read from
the disc the user extracts, and kept on the user's computer only, in the folder GAME_UI_NAME in
the user's settings folder (galaxyctr_builder.data_home). make() writes it from an extracted disc;
load() reads it back.

The set is sealed (seal): the manifest lists every file with its SHA-256, signed (SEAL_KEY). load()
checks them all, so a texture drawn over, a sound swapped or a file deleted makes the set
unusable, and the builder makes it again from the disc (builder_firststart). Changed graphics are
not accepted.

  layouts  LayoutData/*.arc: NintendoWare layouts (.brlyt), their animations (.brlan) and their
           textures (.tpl, written as PNG). builder_layout.py draws them at run time with
           OpenGL, pane by pane and animation by animation, as the game does.
  fonts    LayoutData/Font.arc: the message font's glyph sheets (PNG) and metrics (JSON), and the
           same font traced into a TrueType file for the window's own text (builder_font.py)
  sounds   AudioRes: the menu sound effects, rendered to WAV by builder_sounds.py
  sky      ObjectData/GalaxySky.arc: a J3D model (.bdl). Its triangles, texture coordinates and
           vertex colours, its two textures and its material (TEV stages, texture matrices) are
           written as sky.json and PNG files; builder_sky.py draws them with OpenGL.

Every format detail follows the decomp: libs/nw4r/include/nw4r/lyt/resources.h for the layouts,
tools/3ds/native/n3d.py and mat.py (the converter's own readers) for the model, src/JSystem/JAudio2
for the sounds.
"""

import hashlib
import hmac
import json
import math
import struct
import sys
from pathlib import Path

ASSET_VERSION = 26
SEAL_KEY = b"GalaxyCTR game look: only the files made from the user's disc"
PROBLEMS = []  # why a set was refused (seal_problem), for the builder's log
_CHECKED = {}  # folder -> (manifest's time, the problem or None): a set is read once per run
GAME_UI_NAME = "GalaxyCTR-game-ui"


def _paths():
    """The converter's modules, found from this file's place in a source tree."""
    root = Path(__file__).resolve().parents[2]
    for folder in (root / "tools" / "3ds", root / "tools" / "3ds" / "builder", root / "tools" / "3ds" / "native"):
        if str(folder) not in sys.path:
            sys.path.insert(0, str(folder))


_paths()


# ##############################################################################################
# # Archives and textures                                                                      #
# ##############################################################################################
def read_archive(path):
    """{member name (lower case): bytes} of a RARC archive, Yaz0 or not."""
    import yaz0
    data = Path(path).read_bytes()
    if yaz0.is_compressed(data):
        data = yaz0.decompress(data)
    be = lambda offset: int.from_bytes(data[offset:offset + 4], "big")  # noqa: E731
    info = be(8)
    data_start = info + be(12)
    count, entries, strings = be(info + 8), info + be(info + 12), info + be(info + 20)
    members = {}
    for index in range(count):
        entry = entries + index * 0x14
        flags_name = be(entry + 4)
        if not (flags_name >> 24) & 1:
            continue
        name_start = strings + (flags_name & 0xFFFFFF)
        name = data[name_start:data.index(b"\0", name_start)].decode("ascii").lower()
        members[name] = data[data_start + be(entry + 8):data_start + be(entry + 8) + be(entry + 12)]
    return members


class Image:
    """RGBA pixels, rows top to bottom."""

    def __init__(self, width, height, pixels=None):
        self.width, self.height = width, height
        self.pixels = pixels or [(0, 0, 0, 0)] * (width * height)

    def get(self, x, y):
        return self.pixels[y * self.width + x]

    def sample(self, u, v, wrap_s=0, wrap_t=0):
        """Bilinear sample at (u, v) in 0..1 (GX wrap: 0 clamp, 1 repeat, 2 mirror)."""
        x, y = u * self.width - 0.5, v * self.height - 0.5
        x0, y0 = math.floor(x), math.floor(y)
        fx, fy = x - x0, y - y0

        def fetch(px, py):
            return self.get(_wrap(px, self.width, wrap_s), _wrap(py, self.height, wrap_t))
        a, b, c, d = fetch(x0, y0), fetch(x0 + 1, y0), fetch(x0, y0 + 1), fetch(x0 + 1, y0 + 1)
        return tuple(a[k] * (1 - fx) * (1 - fy) + b[k] * fx * (1 - fy) + c[k] * (1 - fx) * fy + d[k] * fx * fy
                     for k in range(4))

    def save(self, path):
        import zlib
        rows = b"".join(b"\0" + bytes(int(max(0, min(255, round(c)))) for p in self.pixels[y * self.width:(y + 1) * self.width]
                                      for c in p) for y in range(self.height))

        def chunk(kind, body):
            return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
        Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.width, self.height, 8, 6, 0, 0, 0))
                               + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def _wrap(c, size, mode):
    if mode == 1:
        return c % size
    if mode == 2:
        period = c % (2 * size)
        return period if period < size else 2 * size - 1 - period
    return max(0, min(size - 1, c))


def decode_texture(fmt, data, width, height, palette=None):
    """A GX texture to an Image (texreduce.decode for the direct formats, the palette ones here)."""
    import texreduce
    if fmt in (8, 9, 10):
        block_w, block_h = {8: (8, 8), 9: (8, 4), 10: (4, 4)}[fmt]
        pixels = [(0, 0, 0, 0)] * (width * height)
        pos = 0
        for by in range(0, height, block_h):
            for bx in range(0, width, block_w):
                for y in range(block_h):
                    for x in range(block_w):
                        if fmt == 8:
                            v = data[pos + (y * block_w + x) // 2]
                            v = v >> 4 if x % 2 == 0 else v & 15
                        elif fmt == 9:
                            v = data[pos + y * block_w + x]
                        else:
                            v = struct.unpack_from(">H", data, pos + 2 * (y * block_w + x))[0] & 0x3FFF
                        if by + y < height and bx + x < width and palette and v < len(palette):
                            pixels[(by + y) * width + bx + x] = palette[v]
                pos += 32
        return Image(width, height, pixels)
    r, g, b, a = texreduce.decode(fmt, data, width, height)
    return Image(width, height, list(zip(r, g, b, a)))


def palette_colours(fmt, data, count):
    import texreduce
    out = []
    for k in range(count):
        v = struct.unpack_from(">H", data, 2 * k)[0]
        if fmt == 0:  # IA8
            out.append((v & 255, v & 255, v & 255, v >> 8))
        elif fmt == 1:  # RGB565
            out.append((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255))
        else:
            out.append(texreduce._5a3(v))
    return out


def read_tpl(data):
    """The first image of a .tpl file."""
    table = struct.unpack_from(">I", data, 8)[0]
    image_offset, palette_offset = struct.unpack_from(">II", data, table)
    height, width, fmt, address = struct.unpack_from(">HHII", data, image_offset)
    wrap_s, wrap_t = struct.unpack_from(">II", data, image_offset + 12)
    palette = None
    if fmt in (8, 9, 10) and palette_offset:
        count, _, _, pfmt, paddress = struct.unpack_from(">HBBII", data, palette_offset)
        palette = palette_colours(pfmt, data[paddress:], count)
    image = decode_texture(fmt, data[address:], width, height, palette)
    image.wrap = (wrap_s, wrap_t)
    image.format = fmt
    return image


# ##############################################################################################
# # The sky (J3D model)                                                                        #
# #                                                                                            #
# # The converter's own readers (native/n3d.py: vertex arrays, display lists; native/mat.py:   #
# # materials) give the triangles and the material. Written for builder_window.SkyView:        #
# #   vertices  x, y, z, r, g, b, a (0..1), u, v per triangle corner, in draw order           #
# #   material  the TEV stages as mat.parse gives them, the TEV orders, colour registers,        #
# #             konst colours and selections, and per texture coordinate its 2x3 matrix        #
# #             (J3DTexMtxInfo: scale, rotation and translation about the centre)             #
# #   textures  PNG files with their wrap modes                                                #
# ##############################################################################################
def _tex_matrix(data, pos):
    """A J3DTexMtxInfo as a 2x3 matrix (u' = m0*u + m1*v + m2, v' = m3*u + m4*v + m5)."""
    cx, cy, _ = struct.unpack_from(">3f", data, pos + 0x04)
    sx, sy = struct.unpack_from(">2f", data, pos + 0x10)
    angle = struct.unpack_from(">h", data, pos + 0x18)[0] * math.pi / 32768
    tx, ty = struct.unpack_from(">2f", data, pos + 0x1C)
    c, s = math.cos(angle), math.sin(angle)
    m0, m1, m3, m4 = sx * c, -sx * s, sy * s, sy * c
    return [m0, m1, cx + tx - m0 * cx - m1 * cy, m3, m4, cy + ty - m3 * cx - m4 * cy]


def _tex_srt(data, pos):
    """A J3DTexMtxInfo's centre, scale, rotation and translation: what a texture animation sets."""
    cx, cy, _ = struct.unpack_from(">3f", data, pos + 0x04)
    sx, sy = struct.unpack_from(">2f", data, pos + 0x10)
    angle = struct.unpack_from(">h", data, pos + 0x18)[0]
    tx, ty = struct.unpack_from(">2f", data, pos + 0x1C)
    return [cx, cy, sx, sy, angle, tx, ty]


def tex_matrix(cx, cy, sx, sy, angle, tx, ty):
    """_tex_matrix's 2x3 matrix from those values (angle in 65536ths of a turn)."""
    a = angle * math.pi / 32768
    c, s = math.cos(a), math.sin(a)
    m0, m1, m3, m4 = sx * c, -sx * s, sy * s, sy * c
    return [m0, m1, cx + tx - m0 * cx - m1 * cy, m3, m4, cy + ty - m3 * cx - m4 * cy]


def make_sky(archive, out, name, lights=None):
    """Writes <name>.json and its textures into out from a sky archive; lights (scene_lights)
    light the materials whose colour channels are lit."""
    import mat
    import n3d
    members = read_archive(archive)
    model = next(body for member, body in members.items() if member.endswith((".bdl", ".bmd")))
    found = n3d.blocks(model)
    arrays = n3d.VertexArrays(model, *found["VTX1"])
    stats = n3d.Stats()

    # Shapes and their materials, in the order the scene graph (INF1) draws them.
    inf, _ = found["INF1"]
    pos = inf + n3d.u32(model, inf + 0x14)
    # Joints (0x10) nest by 0x01 and 0x02; a shape belongs to the joint above it.
    order, material = [], 0
    parents, stack, last_joint = {}, [], None
    while True:
        kind, value = struct.unpack_from(">HH", model, pos)
        pos += 4
        if kind == 0:
            break
        if kind == 0x01:
            stack.append(last_joint)
        elif kind == 0x02:
            if stack:
                stack.pop()
        elif kind == 0x10:
            parents[value] = next((j for j in reversed(stack) if j is not None), None)
            last_joint = value
        elif kind == 0x11:
            material = value
        elif kind == 0x12:
            order.append((value, material, next((j for j in reversed(stack + [last_joint]) if j is not None), 0)))

    # The joints' rest pose (JNT1: scale, rotation in 65536ths of a turn, translation).
    joints = []
    if "JNT1" in found:
        jnt, _ = found["JNT1"]
        count = n3d.u16(model, jnt + 8)
        entries, remap = n3d.u32(model, jnt + 0x0C), n3d.u32(model, jnt + 0x10)
        names = mat.name_table(model, jnt + n3d.u32(model, jnt + 0x14), ">")
        for j in range(count):
            e = jnt + entries + struct.unpack_from(">H", model, jnt + remap + 2 * j)[0] * 0x40
            joints.append({"name": names[j] if j < len(names) else "", "parent": parents.get(j),
                           "scale": list(struct.unpack_from(">3f", model, e + 4)),
                           "rotate": list(struct.unpack_from(">3h", model, e + 0x10)),
                           "translate": list(struct.unpack_from(">3f", model, e + 0x18))})

    block, _ = found["SHP1"]
    init, index, _, vtx_desc = [n3d.u32(model, block + 0x0C + 4 * i) for i in range(4)]
    dl_data, _, draw_init = [n3d.u32(model, block + 0x0C + 4 * i) for i in range(5, 8)]
    remap = struct.unpack_from(">%dH" % (n3d.u16(model, block + 8)), model, block + index)
    shapes = []
    for shape, material, joint in order:
        si = block + init + remap[shape] * 0x28
        groups, desc_offset, draw_first = n3d.u16(model, si + 2), n3d.u16(model, si + 4), n3d.u16(model, si + 8)
        desc, p = [], block + vtx_desc + desc_offset
        while True:
            attr, kind = struct.unpack_from(">II", model, p)
            p += 8
            if attr == n3d.GX_END:
                break
            if kind != n3d.GX_NONE:
                desc.append((attr, kind))
        flat, normals = [], []
        for g in range(groups):
            size, offset = struct.unpack_from(">II", model, block + draw_init + (draw_first + g) * 8)
            verts, tris = n3d.decode_list(model, block + dl_data + offset, size, desc, arrays, stats)
            for i in tris:
                v = verts[i]
                x, y, z = v[n3d.GX_POS]
                r, g_, b, a = v.get(n3d.GX_CLR0, (255, 255, 255, 255))
                u, t = v.get(n3d.GX_TEX0, (0.0, 0.0))
                flat += [round(x, 3), round(y, 3), round(z, 3), round(r / 255, 4), round(g_ / 255, 4), round(b / 255, 4),
                         round(a / 255, 4), round(u, 5), round(t, 5)]
                normals += [round(c, 4) for c in v.get(n3d.GX_NRM, (0.0, 0.0, 0.0))]
        shapes.append({"material": material, "vertices": flat, "joint": joint, "matrix_type": model[si],
                       "normals": normals})

    m3, size = found["MAT3"]
    parsed = mat.parse(model, m3, size)
    tables = {o: n3d.u32(model, m3 + o) for o in range(0x0C, 0x84, 4)}
    ids = struct.unpack_from(">%dH" % len(parsed), model, m3 + tables[0x10])
    names = mat.name_table(model, m3 + tables[0x14], ">") if tables.get(0x14) else []
    materials = []
    for number, m in enumerate(parsed):
        entry = m3 + tables[0x0C] + ids[number] * 0x14C
        matrices, infos, modes = [], [], []
        for k in range(8):
            slot = n3d.u16(model, entry + 0x48 + 2 * k)
            matrices.append(_tex_matrix(model, m3 + tables[0x40] + slot * 0x64) if slot != 0xFFFF else None)
            infos.append(_tex_srt(model, m3 + tables[0x40] + slot * 0x64) if slot != 0xFFFF else None)
            q = m3 + tables[0x40] + slot * 0x64
            # J3DTexMtxInfo's mapping mode (8: projection mapping) and effect matrix (4x4).
            modes.append([model[q + 1] & 0x3F, list(struct.unpack_from(">16f", model, q + 0x24))] if slot != 0xFFFF else None)
        materials.append({
            "name": names[number] if number < len(names) else "",
            "stages": [list(map(list, s)) for s in m["stages"][:m["stage_count"]]],
            "orders": [list(o) for o in m["orders"][:m["stage_count"]]],
            "texgens": [list(g[:3]) for g in m["texgens"][:m["texgen_count"]]],
            "matrices": matrices,
            "srt": infos,
            "tex_modes": modes,
            # The colour channel (COLOR0, ALPHA0: the material register or the vertices, GX's
            # matSrc) and the material colour: what the TEV stages read as the rasterised colour.
            "ras": [bool(m["chans"][0][1]) if m.get("chans") else True, bool(m["chans"][1][1]) if len(m.get("chans") or []) > 1 else True,
                    list(m["mat_colors"][0]) if m.get("mat_colors") else [255, 255, 255, 255]],
            "textures": [t for t in m["tex_numbers"] if t != 0xFFFF],
            "tev_colours": [list(c) for c in m["tev_colors"]],
            "konst": [list(c) for c in m["konst_colors"]],
            "kcsel": list(m["kcsel"][:m["stage_count"]]),
            "kasel": list(m["kasel"][:m["stage_count"]]),
            "blend": list(m["blend"]),
            "cull": m["cull"],
            # GX's z mode (J3DZMode): test on or off, its compare function, and whether it writes.
            "zmode": list(m["zmode"]),
            "mode": m["mode"],  # J3D's draw mode: 1 opaque, 2 texture edge, 4 translucent
            # COLOR0 and ALPHA0 (J3DColorChanInfo: lighting on, material source, light mask,
            # diffuse function, attenuation function, ambient source) and the ambient colour.
            "chans": [list(c[:6]) for c in (m.get("chans") or [])[:2]],
            "amb": list(m["amb_colors"][0]) if m.get("amb_colors") else [0, 0, 0, 0],
        })
    # Only lit shapes keep their normals.
    for shape in shapes:
        chans = materials[shape["material"]]["chans"] if shape["material"] < len(materials) else []
        if not any(c and c[0] & 1 for c in chans):
            del shape["normals"]

    tex1, _ = found["TEX1"]
    count = n3d.u16(model, tex1 + 8)
    headers = tex1 + n3d.u32(model, tex1 + 0x0C)
    textures = []
    for i in range(count):
        h = headers + i * 0x20
        fmt, width, height, wrap_s, wrap_t = model[h], n3d.u16(model, h + 2), n3d.u16(model, h + 4), model[h + 6], model[h + 7]
        palette = None
        if fmt in (8, 9, 10):
            pfmt, pcount, poffset = model[h + 9], n3d.u16(model, h + 0x0A), n3d.u32(model, h + 0x0C)
            palette = palette_colours(pfmt, model[h + poffset:], pcount)
        image = decode_texture(fmt, model[h + n3d.u32(model, h + 0x1C):], width, height, palette)
        file = f"{name}_tex{i}.png"
        image.save(Path(out) / file)
        textures.append({"file": file, "wrap": [wrap_s, wrap_t]})

    # Its animations as they are: colours (.brk, builder_halo.py), texture matrices (.btk) and
    # joints (.bck, builder_sky.py).
    anims = {}
    for member, body in members.items():
        if member.endswith((".brk", ".btk", ".bck")):
            file = f"{name}_{member}"
            Path(out, file).write_bytes(body)
            anims[member] = file
    colour_anims = {Path(m).stem: f for m, f in anims.items() if m.endswith(".brk")}
    Path(out, f"{name}.json").write_text(json.dumps({"shapes": shapes, "materials": materials, "textures": textures,
                                                     "joints": joints, "anims": anims, "colour_anims": colour_anims,
                                                     "lights": lights}),
                                         encoding="utf-8")


# ##############################################################################################
# # A scene's lights (Game/Map/LightFunction.cpp, ObjectData/LightData.arc)                   #
# #                                                                                            #
# # light<scene>.bcsv names the scene's default area light, lightdata.bcsv holds it. A sky is  #
# # drawn with the planet lights (DrawBufferType_Sky: LightType_Planet): LIGHT0 and LIGHT1 at  #
# # their places, and LIGHT2 at the camera with only an alpha, Alpha2 (loadActorLightInfo).    #
# ##############################################################################################
def _jmap_hash(name):
    h = 0
    for c in name.encode("ascii"):
        h = (h * 31 + c) & 0xFFFFFFFF
    return h


def _jmap_rows(data):
    """Rows of a JMap table as {field hash: value}."""
    count, fields, offset, size = struct.unpack_from(">IIII", data, 0)
    columns = [struct.unpack_from(">IIHBB", data, 16 + 12 * i) for i in range(fields)]
    strings = offset + count * size
    rows = []
    for r in range(count):
        row = {}
        for field, mask, at, shift, kind in columns:
            p = offset + r * size + at
            if kind == 6:
                s = strings + struct.unpack_from(">I", data, p)[0]
                row[field] = data[s:data.index(b"\0", s)].decode("shift_jis", "replace")
            elif kind == 2:
                row[field] = struct.unpack_from(">f", data, p)[0]
            elif kind in (0, 3):
                row[field] = (struct.unpack_from(">I", data, p)[0] & mask) >> shift
            elif kind == 4:
                row[field] = (struct.unpack_from(">H", data, p)[0] & mask) >> shift
            elif kind == 5:
                row[field] = (data[p] & mask) >> shift
        rows.append(row)
    return rows


def scene_lights(light_arc, scene):
    """The planet lights of a scene's default area light, as make_sky stores them, or None."""
    tables = read_archive(light_arc)
    own, common = tables.get(f"light{scene}.bcsv"), tables.get("lightdata.bcsv")
    if own is None or common is None:
        return None
    name = _jmap_hash("AreaLightName")
    wanted = {row.get(name) for row in _jmap_rows(own)}
    for row in _jmap_rows(common):
        if row.get(name) in wanted:
            def get(field, default=0):
                return row.get(_jmap_hash(field), default)

            def colour(field):
                return [get(field + c) for c in "RGBA"]

            lights = [{"pos": [get(f"PlanetLight{i}Pos{a}", 0.0) for a in "XYZ"], "colour": colour(f"PlanetLight{i}Color"),
                       "camera": bool(get(f"PlanetLight{i}FollowCamera"))} for i in (0, 1)]
            lights.append({"pos": [0.0, 0.0, 0.0], "colour": [0, 0, 0, get("PlanetAlpha2")], "camera": True})
            return {"lights": lights, "ambient": colour("PlanetAmbient")}
    return None


# ##############################################################################################
# # What the window uses                                                                       #
# #                                                                                            #
# # Layouts are copied as they are (.brlyt and .brlan) with their textures as PNG files, and  #
# # drawn at run time by builder_layout.py (OpenGL), pane by pane and animation by animation,  #
# # as the game draws them. Fonts become their glyph sheets (PNG) and metrics (JSON), sounds   #
# # are rendered by builder_sounds.py, the sky by make_sky.                                    #
# ##############################################################################################
LAYOUTS = ("SelectButton", "GalaxySelect", "PauseMenu", "BackButton", "SysInfoWindow", "MiiSelect",
           "IconAButton", "IconComet", "ScenarioSelect", "MapButton", "IconSave", "IconMario",
           "TiltGuidanceReplace", "DPDPointer", "TitleLogo", "PressStart", "CameraInfo", "P2Manual")
FONTS = ("messagefont26.brfnt",)
UI_FONT = "messagefont26.brfnt"  # also the window's text font
# Characters the window's text uses (Latin-1 and a few marks); others show as '?'.
CHARACTERS = list(range(0x20, 0x100)) + [0x2026, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022]
RAW = {"mario": ("IconMario", "mymarios.tpl"), "letter": ("PauseMenu", "imluigiletter.tpl")}
# Pictures of the game's picture font (picturefont.brfnt, the icons its messages show), by their
# character: the bell, the white cross, the open hand and the envelope. Each is its cell of the sheet.
PICTURE_ICONS = {"bell": "=", "cross": "@", "hand": "V", "envelope": "Y"}
# Pictures of the message font (messagefont26.brfnt): the music note. Its glyph is only coverage
# (I4), which the game colours with the text's colours: it is written as ink (black) with that
# coverage as alpha, so a picture pane's material colours it as it does the letter's black lines.
FONT_ICONS = {"note": "♪"}
SKIES = {"galaxysky": "GalaxySky", "powerstarhalo": "PowerStarHalo", "cometnearorbitsky": "CometNearOrbitSky"}  # models drawn as make_sky writes them
SKY_SCENES = {"cometnearorbitsky": "fileselect"}  # the scene whose lights a sky is drawn with (scene_lights)


def export_layout(archive, out):
    """A layout archive's .brlyt and .brlan files, and its textures as PNG, into out."""
    out.mkdir(parents=True, exist_ok=True)
    for name, body in read_archive(archive).items():
        if name.endswith((".brlyt", ".brlan")):
            (out / name).write_bytes(body)
        elif name.endswith(".tpl"):
            read_tpl(body).save(out / f"{Path(name).stem}.png")


def export_font(data, out, name):
    """A font's sheets that hold CHARACTERS, as PNG, and its metrics as <name>.json."""
    import builder_layout
    import texreduce
    font = builder_layout.read_font(data)
    per_sheet = font.sheet_rows * font.sheet_cols
    glyphs, sheets = {}, {}
    for code in CHARACTERS:
        index = font.map.get(code)
        if index is None:
            continue
        left, width, advance = font.widths.get(index, font.default_width)
        glyphs[code] = [index, left, width, advance]
        sheets[index // per_sheet] = None
    for number in sorted(sheets):
        fmt, width, height, pixels = font.sheets[number]
        r, g, b, a = texreduce.decode(fmt, pixels, width, height)
        file = f"{name}_sheet{number}.png"
        Image(width, height, list(zip(r, g, b, a))).save(out / file)
        sheets[number] = file
    info = {"width": font.width, "height": font.height, "line_feed": font.line_feed, "cell": list(font.cell),
            "per_row": font.sheet_rows, "rows": font.sheet_cols, "sheet_size": [font.sheets[0][1], font.sheets[0][2]],
            "sheets": {str(k): v for k, v in sheets.items()}, "alter": font.alter,
            "default": list(font.default_width), "glyphs": {str(k): v for k, v in glyphs.items()}}
    (out / f"{name}.json").write_text(json.dumps(info), encoding="utf-8")


def export_picture_icons(data, out, icons=None, ink=False):
    """icons (PICTURE_ICONS by default) as PNG files ({name: file}): each glyph's cell at the
    sheet's own resolution, one texel of border between cells (TGLP), as
    builder_layout.layout_text reads it. With ink, the colour is black and the glyph's
    coverage its alpha (FONT_ICONS)."""
    import builder_layout
    import texreduce
    font = builder_layout.read_font(data)
    per_sheet = font.sheet_rows * font.sheet_cols
    cell_w, cell_h = font.cell
    sheets, written = {}, {}
    for name, char in (PICTURE_ICONS if icons is None else icons).items():
        index = font.map.get(ord(char))
        if index is None:
            continue
        number, cell = divmod(index, per_sheet)
        if number not in sheets:
            fmt, width, height, pixels = font.sheets[number]
            r, g, b, a = texreduce.decode(fmt, pixels, width, height)
            sheets[number] = (width, [(0, 0, 0, v) for v in a] if ink else list(zip(r, g, b, a)))
        width, pixels = sheets[number]
        cx, cy = (cell % font.sheet_rows) * (cell_w + 1), (cell // font.sheet_rows) * (cell_h + 1)
        image = Image(cell_w, cell_h, [pixels[(cy + y) * width + cx + x] for y in range(cell_h) for x in range(cell_w)])
        image.save(Path(out) / f"{name}.png")
        written[name] = f"{name}.png"
    return written


def needed(files):
    """The disc's files make() reads, under an extracted disc's files/ folder."""
    import builder_music
    files = Path(files)
    paths = [files / "LayoutData" / f"{name}.arc" for name in LAYOUTS]
    paths += [files / "LayoutData" / "Font.arc", files / "AudioRes" / "SMR.szs"]
    paths += [files / "ObjectData" / f"{name}.arc" for name in SKIES.values()]
    paths += [files / "AudioRes" / "Stream" / name for name in builder_music.MUSIC.values()]
    paths += [files / "ParticleData" / "Effect.arc"]
    return paths


# The last of the disc's top folders (by name, as nodtool extracts them) that make() reads from.
LAST_FOLDER = "ParticleData"


def make(files, out, report=None):
    """Writes everything the window uses into out from an extracted disc's files/ folder;
    report(step, done, total) after each step."""
    import builder_sounds
    import shutil
    files, out = Path(files), Path(out)
    if out.name == GAME_UI_NAME:  # what an older builder wrote there goes first
        shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    import builder_music
    manifest = {"version": ASSET_VERSION, "layouts": [], "fonts": [], "raw": {}, "sounds": {}, "skies": {}, "music": {}}
    steps = [("layout", name) for name in LAYOUTS] + [("font", name) for name in FONTS] + \
            [("raw", name) for name in RAW] + [("picture", None)] + [("sounds", None)] + [("sky", name) for name in SKIES] + \
            [("music", name) for name in builder_music.MUSIC] + [("particles", None)]
    for done, (kind, name) in enumerate(steps):
        if report:
            report(kind, done, len(steps))
        if kind == "layout":
            archive = files / "LayoutData" / f"{name}.arc"
            if archive.is_file():
                export_layout(archive, out / "layouts" / name)
                manifest["layouts"].append(name)
        elif kind == "font":
            fonts = read_archive(files / "LayoutData" / "Font.arc")
            (out / "fonts").mkdir(exist_ok=True)
            export_font(fonts[name], out / "fonts", Path(name).stem)
            manifest["fonts"].append(name)
            if name == UI_FONT:  # the window's own text, as a TrueType font (builder_font.py)
                import builder_font
                import builder_layout
                builder_font.make(builder_layout.read_font(fonts[name]), CHARACTERS, out / "fonts" / "ui.ttf")
                manifest["ui_font"] = "fonts/ui.ttf"
        elif kind == "picture":
            fonts = read_archive(files / "LayoutData" / "Font.arc")
            if "picturefont.brfnt" in fonts:
                for icon, file in export_picture_icons(fonts["picturefont.brfnt"], out).items():
                    manifest["raw"][icon] = file
            if UI_FONT in fonts:
                for icon, file in export_picture_icons(fonts[UI_FONT], out, FONT_ICONS, ink=True).items():
                    manifest["raw"][icon] = file
        elif kind == "raw":
            arc, member = RAW[name]
            read_tpl(read_archive(files / "LayoutData" / f"{arc}.arc")[member]).save(out / f"{name}.png")
            manifest["raw"][name] = f"{name}.png"
        elif kind == "sounds":
            (out / "sounds").mkdir(exist_ok=True)
            written = builder_sounds.make(files / "AudioRes", out / "sounds")
            manifest["sounds"] = {key: f"sounds/{file}" for key, file in written.items()}
        elif kind == "sky":
            lights = scene_lights(files / "ObjectData" / "LightData.arc", SKY_SCENES[name]) if name in SKY_SCENES else None
            make_sky(files / "ObjectData" / f"{SKIES[name]}.arc", out, name, lights)
            manifest["skies"][name] = f"{name}.json"
        elif kind == "particles":  # the effects the window shows (builder_particles.py)
            import builder_particles
            manifest["particles"] = builder_particles.export(read_archive(files / "ParticleData" / "Effect.arc"), out)
        else:  # the music's streams, as WAV files (builder_music.py)
            source = files / "AudioRes" / "Stream" / builder_music.MUSIC[name]
            if source.is_file():
                (out / "music").mkdir(exist_ok=True)
                info = builder_music.convert_ast(source, out / "music" / f"{name}.wav")
                manifest["music"][name] = {"file": f"music/{name}.wav", **info}
    seal(out, manifest)
    if report:
        report("done", len(steps), len(steps))
    return manifest


CARD_LAYOUTS = ("SysInfoWindow", "IconSave")  # the small window's system window and its star


def card_ready(files):
    """True once the disc being extracted has its layout folder complete: the files are written
    in order of name, so a later folder means LayoutData is all out."""
    files = Path(files)
    if not all((files / "LayoutData" / name).is_file() for name in [f"{n}.arc" for n in CARD_LAYOUTS] + ["Font.arc"]):
        return False
    try:
        return any(entry.name.lower() > "layoutdata" for entry in files.iterdir())
    except OSError:
        return False


def make_card(files, out):
    """The little the first start's small window wears (builder_window.paint_card) while the rest
    of the disc is still read: the system window, the save star and the message font, from
    LayoutData alone. Its manifest is not a finished set (load says None)."""
    import builder_font
    import builder_layout
    files, out = Path(files), Path(out)
    manifest = {"version": -1, "layouts": [], "fonts": [], "raw": {}, "sounds": {}, "skies": {}, "music": {}}
    for name in CARD_LAYOUTS:
        export_layout(files / "LayoutData" / f"{name}.arc", out / "layouts" / name)
        manifest["layouts"].append(name)
    fonts = read_archive(files / "LayoutData" / "Font.arc")
    (out / "fonts").mkdir(parents=True, exist_ok=True)
    export_font(fonts[UI_FONT], out / "fonts", Path(UI_FONT).stem)
    manifest["fonts"].append(UI_FONT)
    builder_font.make(builder_layout.read_font(fonts[UI_FONT]), CHARACTERS, out / "fonts" / "ui.ttf")
    manifest["ui_font"] = "fonts/ui.ttf"
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    return manifest


# ##############################################################################################
# # The seal                                                                                   #
# ##############################################################################################
def _file_hashes(folder):
    hashes = {}
    for path in sorted(folder.rglob("*")):
        if path.is_file() and path.name != "manifest.json":
            hashes[path.relative_to(folder).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return hashes


def _signature(hashes):
    return hmac.new(SEAL_KEY, json.dumps(hashes, sort_keys=True).encode("utf-8"), hashlib.sha256).hexdigest()


def seal(folder, manifest=None):
    """Writes the manifest with every file of folder and its SHA-256, signed. Also after a file
    of the set is written again from a disc (galaxyctr_builder.refresh_look_disc)."""
    folder = Path(folder)
    if manifest is None:
        manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
    manifest["files"] = _file_hashes(folder)
    manifest["seal"] = _signature(manifest["files"])
    (folder / "manifest.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    _CHECKED.pop(str(folder.resolve()), None)
    return manifest


def seal_problem(folder, manifest):
    """None when every sealed file of folder is there and unchanged; else what is wrong."""
    files = manifest.get("files")
    if not isinstance(files, dict) or not files:
        return "the set has no seal"
    if not hmac.compare_digest(str(manifest.get("seal", "")), _signature(files)):
        return "the seal does not match the list of files"
    for name, wanted in files.items():
        path = folder / name
        try:
            found = hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError:
            return f"{name} is missing"
        if found != wanted:
            return f"{name} was changed"
    return None


def checked(folder, manifest):
    """seal_problem, once per run for the same manifest; the reason goes to PROBLEMS."""
    folder = Path(folder)
    key = str(folder.resolve())
    try:
        stamp = (folder / "manifest.json").stat().st_mtime_ns
    except OSError:
        stamp = None
    if key in _CHECKED and _CHECKED[key][0] == stamp:
        return _CHECKED[key][1]
    found = seal_problem(folder, manifest)
    _CHECKED[key] = (stamp, found)
    if found:
        PROBLEMS.append(f"The game look in {folder} is made again: {found}.")
    return found


def load(folder):
    """The manifest of a finished set of graphics in folder, or None (missing, unfinished, made
    by an older version of this file, or not as it was made: seal)."""
    folder = Path(folder)
    try:
        manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if manifest.get("version") != ASSET_VERSION:
        return None
    if checked(folder, manifest):
        return None
    files = [*manifest["raw"].values(), *manifest["sounds"].values(), *manifest["skies"].values()]
    files += [piece["file"] for piece in manifest.get("music", {}).values()]
    files += [manifest["particles"]] if "particles" in manifest else []
    files += [f"layouts/{name}" for name in manifest["layouts"]]
    files += [f"fonts/{Path(name).stem}.json" for name in manifest["fonts"]]
    files += [manifest["ui_font"]] if "ui_font" in manifest else []
    if not all((folder / file).exists() for file in files):
        return None
    return manifest


def dump(pane, depth=0):
    """Prints a layout's panes (builder_layout.Pane), for checks: python builder_assets.py --dump X.arc"""
    m = pane.material
    extra = ""
    if m:
        extra = f" mat={m.name} tex={[t[3] for t in m.textures]} black={m.tev[0]} white={m.tev[1]} stages={len(m.stages)}"
    if pane.frames:
        extra += f" frames={[(f[0].name if f[0] else None, f[1]) for f in pane.frames]} infl={pane.inflation}"
    if pane.kind == "txt1":
        extra += f" text={pane.text!r} size={pane.font_size}"
    colours = "" if all(c == [255, 255, 255, 255] for c in pane.colours) else f" vtx={pane.colours}"
    print("  " * depth + f"{pane.kind} {pane.name} vis={int(pane.visible)} base={pane.base} a={pane.alpha} "
          f"t=({pane.translate[0]:.0f},{pane.translate[1]:.0f}) s=({pane.size[0]:.0f}x{pane.size[1]:.0f}) "
          f"sc=({pane.scale[0]:.2f},{pane.scale[1]:.2f}) r={pane.rotate[2]:.0f}{colours}{extra}")
    for child in pane.children:
        dump(child, depth + 1)


if __name__ == "__main__" and len(sys.argv) > 2 and sys.argv[1] == "--dump":
    import builder_layout
    for arc in sys.argv[2:]:
        members = read_archive(arc)
        for name, body in members.items():
            if name.endswith(".brlyt"):
                print("==", arc, name)
                dump(builder_layout.read_layout(body).root)
