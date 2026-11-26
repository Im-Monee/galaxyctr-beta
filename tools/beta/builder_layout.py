"""The game's own layouts in the builder window: read, animated and drawn as the game does.

builder_assets.make() copies a few NintendoWare layouts of the user's disc (LayoutData/*.arc:
a .brlyt, its .brlan animations, its textures and fonts) into the game UI folder. This module
reads them and draws them the way nw4r::lyt does, so a button is the game's button: the same
panes stacked in the same order (the shadow, the frame, the face, the text, the shine), every
picture through its own material, and every state change played by the game's own animation.

  read_layout   .brlyt (libs/nw4r/include/nw4r/lyt/resources.h): panes (pan1, pic1, txt1,
                wnd1, bnd1), materials (mat1), textures (txl1), fonts (fnl1), groups (grp1)
  read_anim     .brlan: per pane or material, curves for the pane's SRT (RLPA), visibility
                (RLVI), vertex colours and alpha (RLVC), material colours (RLMC), texture SRT
                (RLTS) and texture patterns (RLTP); evaluated as src/nw4r/lyt/lyt_animation.cpp
  read_font     .brfnt (fontResourceFormat.h): glyph sheets, widths and the character map
  Scene         one layout with its current animation frames applied (an nw4r::lyt::Layout)
  Renderer      draws a Scene with OpenGL into an image: every pane's matrix as
                Pane::CalculateMtx makes it, alpha passed down as the game does
                (influenced alpha), materials as GLSL from their TEV stages
                (Material::SetupGX), windows as lyt_window.cpp, text as lyt_textBox.cpp

Everything here only reads the files builder_assets copied; nothing comes with the builder.
"""

import math
import os
import struct
import sys
from pathlib import Path


def _u16(d, p):
    return struct.unpack_from(">H", d, p)[0]


def _s16(d, p):
    return struct.unpack_from(">h", d, p)[0]


def _u32(d, p):
    return struct.unpack_from(">I", d, p)[0]


def _f32(d, p):
    return struct.unpack_from(">f", d, p)[0]


def _cstr(d, p, size=None):
    end = d.index(b"\0", p) if size is None else p + size
    return d[p:end].split(b"\0")[0].decode("ascii", "replace")


# ##############################################################################################
# # Layouts (.brlyt)                                                                           #
# ##############################################################################################
class Material:
    """res::Material: the colours the TEV reads and how it is set up (Material::SetupGX)."""

    def __init__(self, name):
        self.name = name
        self.colour = [255, 255, 255, 255]  # material colour (when the channel takes it)
        self.tev = [[0, 0, 0, 0], [255, 255, 255, 255], [0, 0, 0, 0]]  # black, white, colour 2 (s10)
        self.konst = [[255, 255, 255, 255] for _ in range(4)]
        self.textures = []  # [texture index, wrap s, wrap t]
        self.srts = []  # [translate x, y, rotate (degrees), scale x, y]
        self.coord_gens = []  # (type, source, matrix)
        self.vertex_colour = True  # channel control: rasterized colour from the vertices
        self.alpha_vertex = True
        self.swap = None  # four (r, g, b, a) channel picks, or None
        self.stages = []  # (tex coord, tex map, colour in-op, alpha in-op, ras swap, tex swap)
        self.alpha_compare = None  # (comp0, ref0, op, comp1, ref1)
        self.blend = None  # (type, src, dst, op)


class Pane:
    def __init__(self, kind, name):
        self.kind, self.name = kind, name
        self.children = []
        self.parent = None
        self.flag = 0
        self.visible = True
        self.influenced = False  # bit 1: children take this pane's alpha
        self.base = 4
        self.alpha = 255
        self.translate = [0.0, 0.0, 0.0]
        self.rotate = [0.0, 0.0, 0.0]
        self.scale = [1.0, 1.0]
        self.size = [0.0, 0.0]
        self.colours = [[255, 255, 255, 255] for _ in range(4)]  # TL, TR, BL, BR (text: top, bottom)
        self.material = None
        self.uvs = []  # per texture coordinate set: TL, TR, BL, BR (u, v)
        # windows
        self.inflation = (0.0, 0.0, 0.0, 0.0)
        self.frames = []  # (material, flip)
        # text
        self.text = ""
        self.font = 0
        self.font_size = (0.0, 0.0)
        self.char_space = 0.0
        self.line_space = 0.0
        self.position = 4
        self.alignment = 0

    def find(self, name):
        if self.name == name:
            return self
        for child in self.children:
            found = child.find(name)
            if found:
                return found
        return None

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()


class Layout:
    def __init__(self):
        self.root = None
        self.size = (608.0, 456.0)
        self.textures = []  # file names (lower case)
        self.fonts = []
        self.materials = []
        self.groups = {}  # name -> [pane names]
        self.panes = {}

    def find(self, name):
        return self.panes.get(name)


def _s10(d, p):
    return [max(-1024, min(1023, v)) for v in struct.unpack_from(">4h", d, p)]


def _material(d, p, textures):
    """res::Material and the bits of its MaterialResourceNum (resources.h)."""
    m = Material(_cstr(d, p, 20))
    m.tev = [_s10(d, p + 0x14), _s10(d, p + 0x1C), _s10(d, p + 0x24)]
    m.konst = [list(d[p + 0x2C + 4 * k:p + 0x30 + 4 * k]) for k in range(4)]
    bits = _u32(d, p + 0x3C)
    q = p + 0x40
    for _ in range(bits & 15):
        index, wrap_s, wrap_t = struct.unpack_from(">HBB", d, q)
        m.textures.append([index, wrap_s, wrap_t])
        q += 4
    for _ in range((bits >> 4) & 15):
        m.srts.append(list(struct.unpack_from(">5f", d, q)))
        q += 20
    for _ in range((bits >> 8) & 15):
        m.coord_gens.append(tuple(d[q:q + 3]))
        q += 4
    if bits >> 25 & 1:  # channel control: colour and alpha source (0 register, 1 vertex)
        m.vertex_colour, m.alpha_vertex = d[q] == 1, d[q + 1] == 1
        q += 4
    if bits >> 27 & 1:  # material colour
        m.colour = list(d[q:q + 4])
        q += 4
    if bits >> 12 & 1:  # TEV swap tables
        m.swap = [tuple((d[q + k] >> (2 * c)) & 3 for c in range(4)) for k in range(4)]
        q += 4
    q += 20 * ((bits >> 13) & 3)  # indirect SRTs
    q += 4 * ((bits >> 15) & 7)  # indirect stages
    for _ in range((bits >> 18) & 31):
        coord, chan, tex_map, swap = d[q:q + 4]
        colour_in, alpha_in = d[q + 4:q + 8], d[q + 8:q + 12]
        m.stages.append((coord, tex_map | ((swap & 1) << 8), _inop(colour_in), _inop(alpha_in), (swap >> 1) & 3,
                         (swap >> 3) & 3))
        q += 16
    if bits >> 23 & 1:  # alpha compare
        comp, op, ref0, ref1 = d[q:q + 4]
        m.alpha_compare = (comp & 7, ref0, op, comp >> 4 & 7, ref1)
        q += 4
    if bits >> 24 & 1:  # blend mode
        m.blend = tuple(d[q:q + 4])
        q += 4
    if not m.stages:
        # Material::SetupGX with no stages of its own: SetDefaultTev, lerp(black, white, tex) times
        # the rasterized colour (one texture), or the rasterized colour times the white one (none).
        if m.textures:
            m.stages = [(0, 0, (2, 4, 8, 15, 0, 0, 0, 1, 0, 0x0C), (1, 2, 4, 7, 0, 0, 0, 1, 0, 0x1C), 0, 0),
                        (0xFF, 0xFF, (15, 0, 10, 15, 0, 0, 0, 1, 0, 0x0C), (7, 0, 5, 7, 0, 0, 0, 1, 0, 0x1C), 0, 0)]
        else:
            m.stages = [(0xFF, 0xFF, (15, 4, 10, 15, 0, 0, 0, 1, 0, 0x0C), (7, 2, 5, 7, 0, 0, 0, 1, 0, 0x1C), 0, 0)]
    if not m.coord_gens:
        m.coord_gens = [(1, 4 + k, 30 + 3 * k) for k in range(len(m.textures))]
    for k, t in enumerate(m.textures):
        t.append(textures[t[0]] if t[0] < len(textures) else None)
    return m


def _inop(raw):
    """TevStageInOp (types.h): a, b, c, d, op, bias, scale, clamp, out register, konst selection."""
    ab, cd, op, cl = raw
    return (ab & 15, ab >> 4, cd & 15, cd >> 4, op & 15, (op >> 4) & 3, (op >> 6) & 3, cl & 1, (cl >> 1) & 3, cl >> 3)


def _colour32(v):
    return [(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255]


def read_layout(data):
    layout = Layout()
    count = _u16(data, 0x0E)
    pos = _u16(data, 0x0C)
    parents, last = [], None
    group_stack = []
    for _ in range(count):
        kind = data[pos:pos + 4].decode("ascii", "replace")
        size = _u32(data, pos + 4)
        if kind == "lyt1":
            layout.size = struct.unpack_from(">2f", data, pos + 0x0C)
        elif kind in ("txl1", "fnl1"):
            n = _u16(data, pos + 8)
            names = [_cstr(data, pos + 12 + _u32(data, pos + 12 + 8 * i)).lower() for i in range(n)]
            if kind == "txl1":
                layout.textures = names
            else:
                layout.fonts = names
        elif kind == "mat1":
            n = _u16(data, pos + 8)
            layout.materials = [_material(data, pos + _u32(data, pos + 12 + 4 * i), layout.textures) for i in range(n)]
        elif kind in ("pan1", "pic1", "txt1", "wnd1", "bnd1"):
            pane = _pane(data, pos, kind, layout)
            layout.panes[pane.name] = pane
            if layout.root is None:
                layout.root = pane
            else:
                parent = parents[-1] if parents else layout.root
                pane.parent = parent
                parent.children.append(pane)
            last = pane
        elif kind == "pas1":
            parents.append(last)
        elif kind == "pae1":
            parents.pop()
        elif kind == "grp1":
            name = _cstr(data, pos + 8, 16)
            n = _u16(data, pos + 0x18)
            layout.groups[name] = [_cstr(data, pos + 0x1C + 16 * i, 16) for i in range(n)]
            group_stack.append(name)
        pos += size
    return layout


def _pane(d, pos, kind, layout):
    pane = Pane(kind, _cstr(d, pos + 0x0C, 16))
    pane.flag, pane.base, pane.alpha = d[pos + 8], d[pos + 9], d[pos + 10]
    pane.visible = bool(pane.flag & 1)
    pane.influenced = bool(pane.flag & 2)
    v = struct.unpack_from(">10f", d, pos + 0x24)
    pane.translate, pane.rotate, pane.scale, pane.size = list(v[0:3]), list(v[3:6]), list(v[6:8]), list(v[8:10])
    body = pos + 0x4C
    mats = layout.materials
    if kind == "pic1":
        pane.colours = [_colour32(c) for c in struct.unpack_from(">4I", d, body)]
        material, coords = struct.unpack_from(">HB", d, body + 0x10)
        pane.material = mats[material] if material < len(mats) else None
        pane.uvs = [[struct.unpack_from(">2f", d, body + 0x14 + 32 * k + 8 * c) for c in range(4)] for k in range(coords)]
    elif kind == "txt1":
        buf_bytes, str_bytes, material, font = struct.unpack_from(">4H", d, body)
        pane.position, pane.alignment = d[body + 8], d[body + 9]
        offset = _u32(d, body + 0xC)
        pane.colours = [_colour32(_u32(d, body + 0x10)), _colour32(_u32(d, body + 0x14))]
        pane.font_size = struct.unpack_from(">2f", d, body + 0x18)
        pane.char_space, pane.line_space = struct.unpack_from(">2f", d, body + 0x20)
        pane.material = mats[material] if material < len(mats) else None
        pane.font = font
        if str_bytes:
            pane.text = d[pos + offset:pos + offset + str_bytes].decode("utf-16-be", "replace").rstrip("\0")
    elif kind == "wnd1":
        pane.inflation = struct.unpack_from(">4f", d, body)
        frames = d[body + 0x10]
        content, table = struct.unpack_from(">II", d, body + 0x14)
        c = pos + content
        pane.colours = [_colour32(v) for v in struct.unpack_from(">4I", d, c)]
        material, coords = struct.unpack_from(">HB", d, c + 0x10)
        pane.material = mats[material] if material < len(mats) else None
        pane.uvs = [[struct.unpack_from(">2f", d, c + 0x14 + 32 * k + 8 * cc) for cc in range(4)] for k in range(coords)]
        for k in range(frames):
            f = pos + _u32(d, pos + table + 4 * k)
            fm, flip = struct.unpack_from(">HB", d, f)
            pane.frames.append((mats[fm] if fm < len(mats) else None, flip))
    return pane


# ##############################################################################################
# # Animations (.brlan)                                                                        #
# ##############################################################################################
class Anim:
    def __init__(self):
        self.frames = 0
        self.loop = False
        self.files = []  # texture pattern files
        self.panes = {}  # name -> [(tag, id, target, curve, keys)]
        self.materials = {}


def read_anim(data):
    anim = Anim()
    count = _u16(data, 0x0E)
    pos = _u16(data, 0x0C)
    for _ in range(count):
        kind = data[pos:pos + 4]
        size = _u32(data, pos + 4)
        if kind == b"pai1":
            anim.frames, anim.loop = _u16(data, pos + 8), bool(data[pos + 10])
            files, contents, table = _u16(data, pos + 12), _u16(data, pos + 14), _u32(data, pos + 16)
            for i in range(files):
                anim.files.append(_cstr(data, pos + 20 + _u32(data, pos + 20 + 4 * i)).lower())
            for i in range(contents):
                c = pos + _u32(data, pos + table + 4 * i)
                name = _cstr(data, c, 20)
                tags, content_type = data[c + 20], data[c + 21]
                curves = []
                for t in range(tags):
                    info = c + _u32(data, c + 24 + 4 * t)
                    tag = data[info:info + 4].decode("ascii", "replace")
                    for k in range(data[info + 4]):
                        target = info + _u32(data, info + 8 + 4 * k)
                        ident, which, curve = data[target], data[target + 1], data[target + 2]
                        n = _u16(data, target + 4)
                        keys_at = target + _u32(data, target + 8)
                        if curve == 2:
                            keys = [struct.unpack_from(">3f", data, keys_at + 12 * j) for j in range(n)]
                        else:
                            keys = [(_f32(data, keys_at + 8 * j), _u16(data, keys_at + 8 * j + 4)) for j in range(n)]
                        curves.append((tag, ident, which, curve, keys))
                (anim.materials if content_type == 1 else anim.panes)[name] = curves
        pos += size
    return anim


def hermite(frame, keys):
    """GetHermiteCurveValue (lyt_animation.cpp)."""
    if len(keys) == 1 or frame <= keys[0][0]:
        return keys[0][1]
    if frame >= keys[-1][0]:
        return keys[-1][1]
    left, right = 0, len(keys) - 1
    while left != right - 1 and left != right:
        centre = (left + right) // 2
        if frame <= keys[centre][0]:
            right = centre
        else:
            left = centre
    f0, v0, s0 = keys[left]
    f1, v1, s1 = keys[right]
    if abs(frame - f1) < 0.001:
        if right < len(keys) - 1 and f1 == keys[right + 1][0]:
            return keys[right + 1][1]
        return v1
    t1 = frame - f0
    t2 = 1.0 / (f1 - f0)
    a = t1 * t1 * t2
    b = a * t2
    c = t1 * b
    e = c * t2
    return v0 * (2 * e - 3 * b + 1) + v1 * (-2 * e + 3 * b) + s0 * (c - 2 * a + t1) + s1 * (c - a)


def step(frame, keys):
    """GetStepCurveValue."""
    if len(keys) == 1 or frame <= keys[0][0]:
        return keys[0][1]
    if frame >= keys[-1][0]:
        return keys[-1][1]
    left, right = 0, len(keys) - 1
    while left != right - 1 and left != right:
        centre = (left + right) // 2
        if frame < keys[centre][0]:
            right = centre
        else:
            left = centre
    return keys[right][1] if abs(frame - keys[right][0]) < 0.001 else keys[left][1]


# ##############################################################################################
# # Fonts (.brfnt)                                                                             #
# ##############################################################################################
class Font:
    def __init__(self):
        self.height = self.width = self.ascent = 0
        self.line_feed = 0
        self.default_width = (0, 0, 0)
        self.alter = 0
        self.cell = (0, 0)
        self.sheet_rows = self.sheet_cols = 0
        self.sheets = []  # (format, width, height, bytes)
        self.widths = {}  # glyph index -> (left, glyph width, char width)
        self.map = {}  # character code -> glyph index


def read_font(data):
    """FontInformation, FontTextureGlyph, FontWidth and FontCodeMap blocks (fontResourceFormat.h)."""
    font = Font()
    pos = _u16(data, 0x0C)
    for _ in range(_u16(data, 0x0E)):
        kind = data[pos:pos + 4]
        size = _u32(data, pos + 4)
        body = pos + 8
        if kind == b"FINF":
            font.line_feed = struct.unpack_from(">b", data, body + 1)[0]
            font.alter = _u16(data, body + 2)
            font.default_width = struct.unpack_from(">bBb", data, body + 4)
            font.height, font.width, font.ascent = data[body + 20], data[body + 21], data[body + 22]
        elif kind == b"TGLP":
            cell_w, cell_h = data[body], data[body + 1]
            sheet_size = _u32(data, body + 4)
            sheets, fmt, rows, cols, width, height = struct.unpack_from(">6H", data, body + 8)
            image = _u32(data, body + 20)
            font.cell = (cell_w, cell_h)
            font.sheet_rows, font.sheet_cols = rows, cols  # rows: glyphs per row; cols: lines
            font.baseline = data[body + 2]
            for s in range(sheets):
                start = image + s * sheet_size  # offsets are from the file start (ResFont::Rebuild)
                font.sheets.append((fmt, width, height, data[start:start + sheet_size]))
        elif kind == b"CWDH":
            first, last = _u16(data, body), _u16(data, body + 2)
            for i in range(last - first + 1):
                font.widths[first + i] = struct.unpack_from(">bBb", data, body + 8 + 3 * i)
        elif kind == b"CMAP":
            first, last, method = _u16(data, body), _u16(data, body + 2), _u16(data, body + 4)
            if method == 0:  # direct
                offset = _u16(data, body + 12)
                for code in range(first, last + 1):
                    font.map[code] = offset + code - first
            elif method == 1:  # table
                for code in range(first, last + 1):
                    glyph = _u16(data, body + 12 + 2 * (code - first))
                    if glyph != 0xFFFF:
                        font.map[code] = glyph
            else:  # scan
                n = _u16(data, body + 12)
                for k in range(n):
                    code, glyph = _u16(data, body + 14 + 4 * k), _u16(data, body + 16 + 4 * k)
                    font.map[code] = glyph
        pos += size
    return font


# ##############################################################################################
# # A layout with its animations playing                                                       #
# #                                                                                            #
# # A Scene is one nw4r::lyt::Layout: its panes and materials carry their current values, and   #
# # every animation bound to a pane changes the values it has curves for, on that pane and the  #
# # panes under it (LayoutPaneCtrl::start, Pane::BindAnimation with children). Values no       #
# # animation touches keep what the last one set, as in the game. Frames run as J3DFrameCtrl:  #
# # one per 60th of a second, looping animations wrap, the others stop on their last frame.   #
# ##############################################################################################
class Player:
    """LayoutAnmPlayer: one animation on one pane, with its J3DFrameCtrl."""

    def __init__(self, anim, pane):
        self.anim, self.pane = anim, pane
        self.frame = 0.0
        self.rate = 1.0
        self.end = float(anim.frames)

    def advance(self, steps=1.0):
        if self.rate == 0.0:
            return
        self.frame += self.rate * steps
        if self.anim.loop:
            while self.frame >= self.end and self.end > 0:
                self.frame -= self.end
        elif self.frame >= self.end:
            self.frame = self.end - 0.001
            self.rate = 0.0

    @property
    def stopped(self):
        return self.rate == 0.0


# The four-letter language suffixes of panes (Game/System/Language.cpp, cLanguages), and the
# language the window shows: English, as the builder's own text.
LANGUAGES = ("JpJa", "UsEn", "UsSp", "UsFr", "EuEn", "EuSp", "EuFr", "EuGe", "EuIt", "EuDu")
LANGUAGE = "UsEn"


def remove_language_panes(pane, layout, current=LANGUAGE):
    """LayoutManager::removeUnnecessaryPanes: among a pane's children, those of the current
    language replace all the others (their suffix dropped); with none, the other languages'
    panes go and the plain ones stay. Children are looked into only when no child had a suffix."""
    tagged = [c for c in pane.children if len(c.name) >= 4 and c.name[-4:] in LANGUAGES]
    if not tagged:
        for child in pane.children:
            remove_language_panes(child, layout, current)
        return
    own = [c for c in tagged if c.name[-4:] == current]
    if own:
        for child in pane.children:
            if child not in own:
                layout.panes.pop(child.name, None)
        for child in own:
            layout.panes.pop(child.name, None)
            child.name = child.name[:-4]
            layout.panes[child.name] = child
        pane.children = own
    else:
        for child in tagged:
            layout.panes.pop(child.name, None)
        pane.children = [c for c in pane.children if c not in tagged]


class Scene:
    def __init__(self, folder):
        self.folder = Path(folder)
        files = sorted(self.folder.glob("*.brlyt"))
        if not files:
            raise FileNotFoundError(f"no layout in {self.folder}")
        self.layout = read_layout(files[0].read_bytes())
        remove_language_panes(self.layout.root, self.layout)
        self.anims = {}
        # pane name -> Player; an animation of another group (the game's second argument of
        # startAnim, run beside the first) is kept as "pane#group".
        self.players = {}

    def anim(self, name):
        name = name.lower()
        if name not in self.anims:
            self.anims[name] = read_anim((self.folder / f"{name}.brlan").read_bytes())
        return self.anims[name]

    def has_anim(self, name):
        return (self.folder / f"{name.lower()}.brlan").is_file()

    def pane(self, name):
        return self.layout.find(name)

    # The game's calls (Game/Util/LayoutUtil.cpp).
    @staticmethod
    def _key(pane, group):
        return pane if group == 0 else f"{pane}#{group}"

    def start_pane_anim(self, pane, anim, group=0):
        # The newest animation is applied last, over the others (nw4r binds a started
        # animation at the end of the pane's list): the title's Decide fades the blooms
        # even while ReactionA or ReactionB, in the other group, holds them lit.
        key = self._key(pane, group)
        self.players.pop(key, None)
        self.players[key] = Player(self.anim(anim), pane)
        self.apply()

    def set_pane_anim_frame(self, pane, frame):
        if pane in self.players:
            self.players[pane].frame = frame
            self.apply()

    def set_anim_frame_and_stop(self, pane, frame, group=0):
        """MR::setAnimFrameAndStop: holds the group's animation at frame."""
        player = self.players.get(self._key(pane, group))
        if player is not None:
            player.frame, player.rate = frame, 0.0
            self.apply()

    def set_pane_anim_rate(self, pane, rate):
        if pane in self.players:
            self.players[pane].rate = rate

    def pane_anim_frame(self, pane):
        return self.players[pane].frame if pane in self.players else 0.0

    def is_pane_anim_stopped(self, pane):
        player = self.players.get(pane)
        return player is None or player.stopped

    def frame_max(self, anim):
        return self.anim(anim).frames

    def show_pane(self, name, shown=True):
        pane = self.pane(name)
        if pane is not None:
            pane.visible = shown

    def movement(self, steps=1.0):
        for player in self.players.values():
            player.advance(steps)
        self.apply()

    def apply(self, ahead=0.0):
        """Sets every pane and material to its animations' frames; ahead (0 to 1) draws them that
        share of a frame further on, for a window that draws more often than the game's 60 frames
        a second (builder_game.GameClock). The frames themselves do not move."""
        for player in self.players.values():
            root = self.pane(player.pane)
            if root is None:
                continue
            frame = player.frame
            if ahead and player.rate:
                frame += player.rate * ahead
                if player.anim.loop and player.end > 0:
                    frame %= player.end
                else:
                    frame = min(frame, player.end - 0.001)
            for pane in root.walk():
                curves = player.anim.panes.get(pane.name)
                if curves:
                    _animate_pane(pane, curves, frame)
                for material in _materials(pane):
                    curves = player.anim.materials.get(material.name)
                    if curves:
                        _animate_material(material, curves, frame, player.anim.files)

    def state_key(self, root):
        """Everything a draw of root depends on, to reuse an image already drawn."""
        out = []
        for pane in self.pane(root).walk():
            out.append((pane.visible, pane.alpha, tuple(pane.translate), tuple(pane.rotate), tuple(pane.scale),
                        tuple(pane.size), tuple(map(tuple, pane.colours)), pane.text))
            for m in _materials(pane):
                out.append((tuple(map(tuple, m.tev)), tuple(m.colour), tuple(map(tuple, m.konst)),
                            tuple(map(tuple, m.srts)), tuple(t[3] for t in m.textures)))
        return hash(tuple(out))


def _materials(pane):
    out = [pane.material] if pane.material is not None else []
    out += [f for f, _ in pane.frames if f is not None]
    return out


def _animate_pane(pane, curves, frame):
    for tag, ident, target, curve, keys in curves:
        if tag == "RLPA":  # Pane::SetSRTElement: trans xyz, rot xyz, scale xy, size wh
            v = hermite(frame, keys)
            if target < 3:
                pane.translate[target] = v
            elif target < 6:
                pane.rotate[target - 3] = v
            elif target < 8:
                pane.scale[target - 6] = v
            else:
                pane.size[target - 8] = v
        elif tag == "RLVI":
            pane.visible = step(frame, keys) != 0
        elif tag == "RLVC":  # SetColorElement: 4 corners (text: 2 colours) x RGBA, then the alpha
            v = max(0, min(255, int(hermite(frame, keys) + 0.5)))
            if target == 16:
                pane.alpha = v
            elif pane.kind == "txt1":
                pane.colours[target // 8][target % 4] = v
            elif target < 16:
                pane.colours[target // 4][target % 4] = v


def _animate_material(material, curves, frame, files):
    for tag, ident, target, curve, keys in curves:
        if tag == "RLMC":  # Material::SetColorElement: material colour, TEV colours 0-2, konst 0-3
            v = max(-1024, min(1023, int(math.floor(hermite(frame, keys) + 0.5))))
            if target < 4:
                material.colour[target] = max(0, min(255, v))
            elif target < 16:
                material.tev[(target - 4) // 4][target % 4] = v
            elif target < 32:
                material.konst[(target - 16) // 4][target % 4] = max(0, min(255, v))
        elif tag == "RLTS" and ident < len(material.srts):
            material.srts[ident][target] = hermite(frame, keys)
        elif tag == "RLTP" and ident < len(material.textures):
            index = step(frame, keys)
            if index < len(files):
                material.textures[ident][3] = files[index]


# ##############################################################################################
# # Pane geometry                                                                              #
# #                                                                                            #
# # Pane::CalculateMtx: a pane's matrix is its parent's times translate, rotate z (then y, x) #
# # and scale; positions are y up. A pane draws its own quad in a y-down local space          #
# # (Pane::LoadMtx reverses y; GetVtxPos gives the top left from the base position).          #
# ##############################################################################################
def _local_matrix(pane):
    """Pane::CalculateMtx: Rz * Ry * Rx * scale, then the translation. The layout is drawn with
    an orthographic projection, so a pane turned in depth (the save icon's star, tilted about
    x) is the x and y rows and columns of that 3D matrix: x' = a x + b y + tx, y' = c x + d y + ty."""
    sx, sy = pane.scale
    rx, ry, rz = (math.radians(v) for v in pane.rotate)
    if rx == 0 and ry == 0:
        c, s = math.cos(rz), math.sin(rz)
        return (c * sx, -s * sy, pane.translate[0], s * sx, c * sy, pane.translate[1])
    cx, sx_ = math.cos(rx), math.sin(rx)
    cy, sy_ = math.cos(ry), math.sin(ry)
    cz, sz = math.cos(rz), math.sin(rz)
    # Ry * Rx, first two columns (the quad's z is 0).
    m00, m01 = cy, sy_ * sx_
    m10, m11 = 0.0, cx
    # Rz times that, rows x and y.
    a = cz * m00 - sz * m10
    b = cz * m01 - sz * m11
    c = sz * m00 + cz * m10
    d = sz * m01 + cz * m11
    return (a * sx, b * sy, pane.translate[0], c * sx, d * sy, pane.translate[1])


def _mul(m, n):
    a, b, tx, c, d, ty = m
    e, f, ux, g, h, uy = n
    return (a * e + b * g, a * f + b * h, a * ux + b * uy + tx, c * e + d * g, c * f + d * h, c * ux + d * uy + ty)


def _apply(m, x, y):
    return m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]


def vtx_pos(pane):
    """Pane::GetVtxPos: the local top left (y down, as DrawQuad takes it)."""
    w, h = pane.size
    return -w * (pane.base % 3) / 2, -h * (pane.base // 3) / 2


def walk_draw(pane, parent_mtx=(1, 0, 0, 0, 1, 0), alpha=1.0, influenced=False, root=True):
    """(pane, global matrix, global alpha) of every visible pane, in draw order (parents first).
    root's own translation is dropped: the caller places it. A root with an origin (x, y) keeps
    how far its animations move it from there (MiiSelect's arrows sway in ButtonWait by moving
    their pane)."""
    if not pane.visible:
        return
    local = _local_matrix(pane)
    if root:
        ox, oy = getattr(pane, "origin", (local[2], local[5]))
        local = (local[0], local[1], local[2] - ox, local[3], local[4], local[5] - oy)
    mtx = _mul(parent_mtx, local)
    glb = pane.alpha * alpha if influenced and not root else pane.alpha
    yield pane, mtx, glb / 255.0
    child_alpha, child_influenced = alpha, influenced
    if pane.influenced and pane.alpha != 255:
        child_alpha, child_influenced = alpha * pane.alpha / 255.0, True
    for child in pane.children:
        yield from walk_draw(child, mtx, child_alpha, child_influenced, False)


def extent(pane, skip_bounding=True):
    """(left, bottom, right, top) of what pane and its children cover, in its own space (y up)."""
    xs, ys = [], []
    for p, mtx, _alpha in walk_draw(pane):
        if p.kind in ("pan1",) or (skip_bounding and p.kind == "bnd1"):
            continue
        x0, y0 = vtx_pos(p)
        w, h = p.size
        for cx, cy in ((x0, y0), (x0 + w, y0), (x0, y0 + h), (x0 + w, y0 + h)):
            x, y = _apply(mtx, cx, -cy)
            xs.append(x)
            ys.append(y)
    if not xs:
        return 0.0, 0.0, 0.0, 0.0
    return min(xs), min(ys), max(xs), max(ys)


def pane_box(scene, root, name):
    """(left, bottom, right, top) of one pane (a bounding pane) in root's space."""
    for p, mtx, _alpha in walk_draw(scene.pane(root)):
        if p.name == name:
            x0, y0 = vtx_pos(p)
            w, h = p.size
            pts = [_apply(mtx, cx, -cy) for cx, cy in ((x0, y0), (x0 + w, y0), (x0, y0 + h), (x0 + w, y0 + h))]
            return min(x for x, _ in pts), min(y for _, y in pts), max(x for x, _ in pts), max(y for _, y in pts)
    return None


# ##############################################################################################
# # Text (lyt_textBox.cpp, ut::CharWriter and ut::TextWriterBase)                              #
# #                                                                                            #
# # The font's glyphs are scaled from the font's size to the text box's; each one advances by  #
# # its width plus the character spacing. The text block sits in the box by textPosition      #
# # (horizontal: left, centre, right; vertical: top, middle, bottom), each line placed by the  #
# # alignment. A glyph's colour is mapped from the material's black to its white colour by    #
# # the glyph texel (CharWriter::SetColorMapping), times the top or bottom text colour.         #
# ##############################################################################################
class FontData:
    """A font as builder_assets wrote it: metrics (font.json) and its sheets as PNG."""

    def __init__(self, folder, name):
        import json
        self.folder = Path(folder)
        info = json.loads((self.folder / f"{name}.json").read_text(encoding="utf-8"))
        self.name = name
        self.width, self.height, self.line_feed = info["width"], info["height"], info["line_feed"]
        self.cell = tuple(info["cell"])
        self.per_row, self.rows = info["per_row"], info["rows"]
        self.sheet_size = tuple(info["sheet_size"])
        self.sheets = info["sheets"]  # sheet number -> file
        self.default = tuple(info["default"])
        self.alter = info["alter"]
        self.glyphs = {int(k): v for k, v in info["glyphs"].items()}  # code -> [glyph index, left, width, advance]

    def glyph(self, ch):
        g = self.glyphs.get(ord(ch))
        if g is None:
            g = self.glyphs.get(ord("?")) or [self.alter, *self.default]
        return g


def layout_text(pane, font, text):
    """Glyph quads of a text box: (sheet, x, y, w, h, u0, v0, u1, v1) in the pane's local space
    (y down, from the top left of the pane)."""
    if not text:
        return []
    scale_x = pane.font_size[0] / font.width if font.width else 1.0
    scale_y = pane.font_size[1] / font.height if font.height else 1.0
    line_height = font.line_feed * scale_y + pane.line_space
    lines = text.split("\n")
    widths = []
    for line in lines:
        w = 0.0
        for i, ch in enumerate(line):
            w += font.glyph(ch)[3] * scale_x + (pane.char_space if i < len(line) - 1 else 0.0)
        widths.append(w)
    block_w = max(widths) if widths else 0.0
    block_h = len(lines) * line_height - pane.line_space
    box_w, box_h = pane.size
    x0, y0 = vtx_pos(pane)
    hpos, vpos = pane.position % 3, pane.position // 3
    left = x0 + (0.0, (box_w - block_w) / 2, box_w - block_w)[hpos]
    top = y0 + (0.0, (box_h - block_h) / 2, box_h - block_h)[vpos]
    align = pane.alignment if pane.alignment in (1, 2, 3) else hpos + 1
    quads = []
    cell_w, cell_h = font.cell
    sheet_w, sheet_h = font.sheet_size
    per_sheet = font.per_row * font.rows
    y = top
    for line, w in zip(lines, widths):
        x = left + (0.0, (block_w - w) / 2, block_w - w)[align - 1]
        for ch in line:
            index, gleft, gwidth, advance = font.glyph(ch)
            sheet, cell = divmod(index, per_sheet) if per_sheet else (0, index)
            col, row = cell % font.per_row, cell // font.per_row
            # The cell of the glyph, one texel of border between cells (TGLP).
            cx, cy = col * (cell_w + 1), row * (cell_h + 1)
            qx, qw = x + gleft * scale_x, gwidth * scale_x
            quads.append((sheet, qx, y, qw, cell_h * scale_y, cx / sheet_w, cy / sheet_h,
                          (cx + gwidth) / sheet_w, (cy + cell_h) / sheet_h))
            x += advance * scale_x + pane.char_space
        y += line_height
    return quads


# ##############################################################################################
# # Drawing with OpenGL                                                                        #
# #                                                                                            #
# # One offscreen OpenGL context draws every layout image of the window. Each material         #
# # becomes a GLSL program from its TEV stages (the same numbering as builder_sky.tev_shader:  #
# # registers 0 PREV, 1 the black colour, 2 the white one, 3 colour 2, as Material::SetupGX    #
# # loads them), texture coordinates go through the material's texture SRT matrices           #
# # (Material::SetupGX, CalcTextureMtx), and quads are blended as nw4r's default blend mode:   #
# # source alpha over the image. The image is drawn at a scale (pixels per layout unit) and   #
# # comes back premultiplied, ready for QPainter.                                              #
# ##############################################################################################
VERTEX = """#version 120
attribute vec2 position;
attribute vec4 colour;
attribute vec2 uv0;
attribute vec2 uv1;
attribute vec2 uv2;
uniform vec4 view;
varying vec4 vColour;
varying vec2 vUv0;
varying vec2 vUv1;
varying vec2 vUv2;
void main() {
  vColour = colour;
  vUv0 = uv0; vUv1 = uv1; vUv2 = uv2;
  gl_Position = vec4(position.x * view.x + view.z, position.y * view.y + view.w, 0.0, 1.0);
}
"""

GLYPH_FRAGMENT = """#version 120
varying vec4 vColour;
varying vec2 vUv0;
uniform sampler2D tex0;
uniform vec4 low;
uniform vec4 high;
void main() {
  vec4 t = texture2D(tex0, vUv0);
  gl_FragColor = mix(low, high, t) * vColour;
}
"""

REGS = ("prev", "r0", "r1", "r2")


def _konst_c(sel):
    if sel < 8:
        return f"vec3({(8 - sel) / 8:.4f})"
    if sel < 16:
        return f"k{(sel - 12) & 3}.rgb"
    return f"vec3(k{(sel - 16) % 4}.{'rgba'[(sel - 16) // 4]})"


def _konst_a(sel):
    if sel < 8:
        return f"{(8 - sel) / 8:.4f}"
    if sel < 16:
        return "1.0"
    return f"k{(sel - 16) % 4}.{'rgba'[(sel - 16) // 4]}"


def _cin(arg):
    if arg < 8:
        reg = REGS[arg // 2]
        return f"{reg}.rgb" if arg % 2 == 0 else f"vec3({reg}.a)"
    return {8: "t.rgb", 9: "vec3(t.a)", 10: "ras.rgb", 11: "vec3(ras.a)", 12: "vec3(1.0)", 13: "vec3(0.5)",
            14: "kc"}.get(arg, "vec3(0.0)")


def _ain(arg):
    if arg < 4:
        return f"{REGS[arg]}.a"
    return {4: "t.a", 5: "ras.a", 6: "ka"}.get(arg, "0.0")


def _op(a, b, c, d, op, bias, scale, clamp, size):
    if op <= 1:
        value = f"({d} {'-' if op == 1 else '+'} mix({a}, {b}, {c}){('', ' + 0.5', ' - 0.5', '')[bias]})"
        value = f"({value} * {(1.0, 2.0, 4.0, 0.5)[scale]:.1f})"
    else:
        # Comparisons (GX_TEV_COMP_*): d + (a > b or a == b ? c : 0), on the red channel or all.
        cmp = ">" if op % 2 == 0 else "=="
        if size == 1:
            value = f"({d} + (({a}) {cmp} ({b}) ? ({c}) : 0.0))"
        else:
            value = f"({d} + ((({a}).r) {cmp} (({b}).r) ? ({c}) : vec3(0.0)))"
    if clamp:
        value = f"clamp({value}, 0.0, 1.0)"
    return value


def _swizzle(table, which):
    return "".join("rgba"[c] for c in table[which]) if table else "rgba"


def material_shader(m, uncovered_light=False):
    """The fragment shader of a material, and the texture units it reads. uncovered_light: added
    light keeps no alpha (see draw_lit)."""
    lines = ["#version 120", "varying vec4 vColour;", "varying vec2 vUv0;", "varying vec2 vUv1;", "varying vec2 vUv2;"]
    lines += [f"uniform sampler2D tex{i};" for i in range(3)]
    lines += [f"uniform vec3 mtx{i}a;\nuniform vec3 mtx{i}b;" for i in range(3)]
    lines += ["uniform vec4 c0, c1, c2, k0, k1, k2, k3, mcol;", "uniform float alpha;", "void main() {",
              "  vec4 prev = vec4(0.0), r0 = c0, r1 = c1, r2 = c2;",
              "  vec4 ras = vColour;", "  vec4 t; vec3 kc; float ka; vec2 uv;"]
    # Texture coordinates (TexCoordGen): source TEXk, through SRT k (matrix 30 + 3k) or none.
    coords = []
    for k, (kind, src, mtx) in enumerate(m.coord_gens[:3]):
        source = f"vUv{min(2, max(0, src - 4))}"
        if 30 <= mtx < 60 and (mtx - 30) // 3 < 3:
            n = (mtx - 30) // 3
            coords.append(f"vec2(dot(mtx{n}a, vec3({source}, 1.0)), dot(mtx{n}b, vec3({source}, 1.0)))")
        else:
            coords.append(source)
    for coord, tex_map, cin, ain, ras_swap, tex_swap in m.stages:
        if tex_map != 0xFF and (tex_map & 0xFF) < len(m.textures) and coord < len(coords):
            lines.append(f"  t = texture2D(tex{tex_map & 0xFF}, {coords[coord]}).{_swizzle(m.swap, tex_swap)};")
        else:
            lines.append("  t = vec4(1.0);")
        if m.swap and ras_swap:
            lines.append(f"  ras = vColour.{_swizzle(m.swap, ras_swap)};")
        lines.append(f"  kc = {_konst_c(cin[9])};")
        lines.append(f"  ka = {_konst_a(ain[9])};")
        colour = _op(*(_cin(x) for x in cin[:4]), *cin[4:8], 3)
        alpha = _op(*(_ain(x) for x in ain[:4]), *ain[4:8], 1)
        lines.append(f"  {{ vec3 rgb = {colour}; float al = {alpha};")
        lines.append(f"    {REGS[cin[8]]}.rgb = rgb; {REGS[ain[8]]}.a = al; }}")
    if m.alpha_compare:
        comp0, ref0, op, comp1, ref1 = m.alpha_compare
        tests = []
        for comp, ref in ((comp0, ref0), (comp1, ref1)):
            r = f"{ref / 255.0:.4f}"
            tests.append(("false", f"(prev.a < {r})", f"(prev.a == {r})", f"(prev.a <= {r})", f"(prev.a > {r})",
                          f"(prev.a != {r})", f"(prev.a >= {r})", "true")[comp])
        joined = ("&&", "||", "!=", "==")[op & 3]
        lines.append(f"  if (!(({tests[0]}) {joined} ({tests[1]}))) discard;")
    if added_light(m):
        # Light the material adds (nw4r blend mode: source alpha, one), premultiplied here. Its
        # alpha is either its brightness, so a plain drawing of the picture keeps it over
        # anything, or none when the picture is drawn with draw_lit and adds to what is under it.
        cover = "0.0" if uncovered_light else "max(max(light.r, light.g), light.b)"
        lines += ["  vec3 light = clamp(prev.rgb, 0.0, 1.0) * clamp(prev.a, 0.0, 1.0);",
                  f"  gl_FragColor = vec4(light, {cover});", "}"]
    else:
        lines += ["  gl_FragColor = prev;", "}"]
    return "\n".join(lines)


def added_light(m):
    """A material whose blend mode adds its colour (GX_BM_BLEND, source alpha, one)."""
    return m.blend is not None and m.blend[0] == 1 and m.blend[1] == 4 and m.blend[2] == 1


def texture_file(folder, name):
    """The PNG of a material's texture: the layout's own (name.tpl as name.png in its folder),
    or a picture put in its place by its full path (builder_game.GameButton.set_icon)."""
    path = Path(name)
    if path.suffix.lower() == ".png" and path.is_absolute():
        return path
    return Path(folder) / f"{path.stem}.png"


def texture_matrix(srt):
    """Material::SetupGX's CalcTextureMtx: scale and rotation about the centre, then translate."""
    tx, ty, rot, sx, sy = srt
    r = math.radians(rot)
    c, s = math.cos(r), math.sin(r)
    a0, a1 = c * sx, -s * sy
    b0, b1 = s * sx, c * sy
    return (a0, a1, tx + 0.5 - 0.5 * a0 - 0.5 * a1), (b0, b1, ty + 0.5 - 0.5 * b0 - 0.5 * b1)


# ##############################################################################################
# # Laying a picture with added light over the window                                          #
# #                                                                                            #
# # In the game added light (blend: source alpha, one) adds to what is already on screen. A    #
# # picture of it holds, per pixel, the colour it brings and how much it covers what is under  #
# # it (its alpha); the light covers nothing, so its colour can be above its alpha. Qt's plain #
# # drawing (source over) is only right when the colour is not, so draw_lit draws such a      #
# # picture in two steps: DestinationOut keeps what is under it as much as it shows through,   #
# # then Plus adds the colour, clipped at white. On screen the window's OpenGL views (the     #
# # skies) are under the pages and get the same sum when Qt lays the pages over them.         #
# ##############################################################################################
def draw_lit(painter, target, image):
    """Draws image (premultiplied, from render(..., lights=True) or a light picture) at target
    (a QRectF or a QPointF) so its light adds to what is under it."""
    from PySide6.QtGui import QPainter
    mode = painter.compositionMode()
    for step in (QPainter.CompositionMode.CompositionMode_DestinationOut, QPainter.CompositionMode.CompositionMode_Plus):
        painter.setCompositionMode(step)
        painter.drawImage(target, image)
    painter.setCompositionMode(mode)


# ##############################################################################################
# # Reading a drawing back (Readback)                                                          #
# #                                                                                            #
# # The window draws its layouts, the halo and the particles with OpenGL, then paints the      #
# # pictures. Made each frame, a framebuffer and QOpenGLFramebufferObject.toImage (which flips #
# # the rows and converts every pixel) cost more than the drawing. A Readback keeps its two   #
# # framebuffers (the multisampled one drawn into, a plain one it is resolved into) and reads #
# # the plain one with glReadPixels as BGRA, the bytes of a premultiplied ARGB32 QImage, rows  #
# # in OpenGL's order: whoever draws into it draws upside down (flipped) so the image is       #
# # upright. Without glReadPixels from the system library, toImage as before.                  #
# ##############################################################################################
_read_pixels = None


def _gl_read_pixels():
    """glReadPixels of the system's OpenGL library, through ctypes (None when not found)."""
    global _read_pixels
    if _read_pixels is None:
        import ctypes
        import ctypes.util
        function = False
        try:
            if os.name == "nt":
                library = ctypes.WinDLL("opengl32")
            elif sys.platform == "darwin":
                library = ctypes.CDLL("/System/Library/Frameworks/OpenGL.framework/OpenGL")
            else:
                library = ctypes.CDLL(ctypes.util.find_library("GL") or "libGL.so.1")
            function = library.glReadPixels
            function.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint,
                                 ctypes.c_uint, ctypes.c_void_p]
            function.restype = None
        except (OSError, AttributeError):
            function = False
        _read_pixels = function
    return _read_pixels or None


class Readback:
    def __init__(self, width, height, samples=4):
        from PySide6.QtOpenGL import QOpenGLFramebufferObject, QOpenGLFramebufferObjectFormat
        self.size = (width, height)
        fmt = QOpenGLFramebufferObjectFormat()
        fmt.setSamples(samples)
        self.drawn = QOpenGLFramebufferObject(width, height, fmt)
        self.plain = QOpenGLFramebufferObject(width, height) if samples else None
        self.flipped = _gl_read_pixels() is not None

    def bind(self):
        self.drawn.bind()

    def image(self):
        """A new premultiplied ARGB32 QImage of what was drawn (its own copy: the caller keeps it)."""
        import ctypes
        from PySide6.QtGui import QImage
        from PySide6.QtOpenGL import QOpenGLFramebufferObject
        self.drawn.release()
        source = self.drawn
        if self.plain is not None:
            QOpenGLFramebufferObject.blitFramebuffer(self.plain, self.drawn)
            source = self.plain
        if not self.flipped:
            return source.toImage(True).convertToFormat(QImage.Format.Format_ARGB32_Premultiplied)
        width, height = self.size
        image = QImage(width, height, QImage.Format.Format_ARGB32_Premultiplied)
        source.bind()
        buffer = image.bits()
        address = ctypes.addressof(ctypes.c_char.from_buffer(buffer))
        _gl_read_pixels()(0, 0, width, height, 0x80E1, 0x1401, address)  # GL_BGRA, GL_UNSIGNED_BYTE
        source.release()
        return image


class Renderer:
    """The offscreen OpenGL context and what it keeps: programs, textures, fonts."""

    _instance = None

    @classmethod
    def get(cls):
        """The one renderer, or None when OpenGL cannot draw here."""
        if cls._instance is None:
            renderer = cls()
            cls._instance = renderer if renderer.start() else False
        return cls._instance or None

    def __init__(self):
        self.error = ""
        self.uncovered_light = False  # render(..., lights=True)
        self.programs = {}
        self.textures = {}
        self.fonts = {}

    def start(self):
        try:
            from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
            self.context = QOpenGLContext()
            if not self.context.create():
                self.error = "no OpenGL context"
                return False
            self.surface = QOffscreenSurface()
            self.surface.setFormat(self.context.format())
            self.surface.create()
            if not self.context.makeCurrent(self.surface):
                self.error = "the OpenGL context did not start"
                return False
            self.gl = self.context.functions()
            self.glyph_program = self._program(GLYPH_FRAGMENT)
            return self.glyph_program is not None
        except Exception as error:  # noqa: BLE001 - the window falls back to its plain look
            self.error = str(error)
            return False

    def _program(self, fragment):
        from PySide6.QtOpenGL import QOpenGLShader, QOpenGLShaderProgram
        if fragment in self.programs:
            return self.programs[fragment]
        program = QOpenGLShaderProgram()
        ok = program.addShaderFromSourceCode(QOpenGLShader.ShaderTypeBit.Vertex, VERTEX) and \
            program.addShaderFromSourceCode(QOpenGLShader.ShaderTypeBit.Fragment, fragment)
        for i, name in enumerate(("position", "colour", "uv0", "uv1", "uv2")):
            program.bindAttributeLocation(name, i)
        if not ok or not program.link():
            self.error = program.log()
            program = None
        self.programs[fragment] = program
        return program

    def _texture(self, path, wrap_s=0, wrap_t=0):
        from PySide6.QtGui import QImage
        from PySide6.QtOpenGL import QOpenGLTexture
        key = (str(path), wrap_s, wrap_t)
        if key not in self.textures:
            image = QImage(str(path))
            if image.isNull():
                self.textures[key] = None
                return None
            texture = QOpenGLTexture(image.convertToFormat(QImage.Format.Format_RGBA8888))
            texture.setMinMagFilters(QOpenGLTexture.Filter.Linear, QOpenGLTexture.Filter.Linear)
            modes = (QOpenGLTexture.WrapMode.ClampToEdge, QOpenGLTexture.WrapMode.Repeat,
                     QOpenGLTexture.WrapMode.MirroredRepeat)
            texture.setWrapMode(QOpenGLTexture.CoordinateDirection.DirectionS, modes[min(wrap_s, 2)])
            texture.setWrapMode(QOpenGLTexture.CoordinateDirection.DirectionT, modes[min(wrap_t, 2)])
            self.textures[key] = (texture, image.width(), image.height())
        return self.textures[key]

    def font(self, folder, name):
        key = (str(folder), name)
        if key not in self.fonts:
            try:
                self.fonts[key] = FontData(folder, Path(name).stem)
            except (OSError, ValueError, KeyError):
                self.fonts[key] = None
        return self.fonts[key]

    # Drawing.
    def target(self, key, width, height, samples=4):
        """A Readback for one kind of drawing (the window draws the same sizes frame after frame,
        so its framebuffers are made once)."""
        if not hasattr(self, "readbacks"):
            self.readbacks = {}
        readback = self.readbacks.get(key)
        if readback is None or readback.size != (width, height):
            if len(self.readbacks) > 96:  # pages made again leave theirs behind
                self.readbacks.clear()
            readback = self.readbacks[key] = Readback(width, height, samples)
        return readback

    def render(self, scene, root, scale, box, texts=None, fonts_folder=None, lights=False):
        """A QImage of root and its children: box is (left, bottom, right, top) in root's space,
        drawn at scale pixels per unit. texts: {text box name: string} put in place of the
        layout's own strings. lights: the added light keeps no alpha, for draw_lit."""
        self.uncovered_light = lights
        from PySide6.QtGui import QImage
        from PySide6.QtOpenGL import QOpenGLFramebufferObject, QOpenGLFramebufferObjectFormat
        left, bottom, right, top = box
        width = max(1, int(math.ceil((right - left) * scale)))
        height = max(1, int(math.ceil((top - bottom) * scale)))
        self.context.makeCurrent(self.surface)
        readback = self.target(("layout", id(scene), root), width, height)
        readback.bind()
        gl = self.gl
        gl.glViewport(0, 0, width, height)
        gl.glClearColor(0.0, 0.0, 0.0, 0.0)
        gl.glClear(0x4000)
        gl.glEnable(0x0BE2)  # blending
        gl.glBlendFuncSeparate(0x0302, 0x0303, 1, 0x0303)  # src alpha, 1 - src alpha; alpha: 1, 1 - src alpha
        gl.glDisable(0x0B71)
        gl.glDisable(0x0B44)
        # Layout units (y up) to clip space; the image's top is the box's top. Drawn upside down
        # when the Readback reads rows straight into the image (its first row is OpenGL's last).
        self.view = (2.0 / (right - left), 2.0 / (top - bottom), -1.0 - 2.0 * left / (right - left),
                     -1.0 - 2.0 * bottom / (top - bottom))
        if readback.flipped:
            self.view = (self.view[0], -self.view[1], self.view[2], -self.view[3])
        texture_folder = scene.folder
        for pane, mtx, alpha in walk_draw(scene.pane(root)):
            if pane.kind == "pic1":
                self._picture(pane, mtx, alpha, texture_folder)
            elif pane.kind == "wnd1":
                self._window(pane, mtx, alpha, texture_folder)
            elif pane.kind == "txt1":
                text = pane.text if texts is None or pane.name not in texts else texts[pane.name]
                self._text(pane, mtx, alpha, text, scene, fonts_folder)
        image = readback.image()
        self.context.doneCurrent()
        return image

    def _quad(self, program, mtx, x, y, w, h, colours, uv_sets):
        """DrawQuad: corners TL, TR, BL, BR of a y-down local rectangle, as two triangles."""
        import array
        corners = [(x, y), (x + w, y), (x, y + h), (x + w, y + h)]
        data = array.array("f")
        for c in (0, 1, 3, 0, 3, 2):
            gx, gy = _apply(mtx, corners[c][0], -corners[c][1])
            data.extend((gx, gy))
            data.extend(v / 255.0 for v in colours[c])
            for k in range(3):
                data.extend(uv_sets[k][c] if k < len(uv_sets) else (0.0, 0.0))
        self._draw(program, data)

    def _draw(self, program, data):
        from PySide6.QtOpenGL import QOpenGLBuffer
        buffer = QOpenGLBuffer(QOpenGLBuffer.Type.VertexBuffer)
        buffer.create()
        buffer.bind()
        raw = data.tobytes()
        buffer.allocate(raw, len(raw))
        stride = 12 * 4
        for location, (offset, size) in enumerate(((0, 2), (8, 4), (24, 2), (32, 2), (40, 2))):
            program.enableAttributeArray(location)
            program.setAttributeBuffer(location, 0x1406, offset, size, stride)
        self.gl.glDrawArrays(0x0004, 0, len(data) // 12)
        # The arrays are turned off again: the halo and the particles draw in this context with
        # fewer of them, and an array left on, reading a buffer already gone, crashed the
        # Windows drivers there (access violation in glDrawArrays).
        for location in range(5):
            program.disableAttributeArray(location)
        buffer.release()
        buffer.destroy()

    def _setup(self, m, alpha, folder):
        """Material::SetupGX: the program, its colours, its textures and matrices."""
        program = self._program(material_shader(m, self.uncovered_light))
        if program is None:
            return None, []
        if added_light(m):
            self.gl.glBlendFuncSeparate(1, 1, 1, 1)  # the light and its brightness add up
        else:
            self.gl.glBlendFuncSeparate(0x0302, 0x0303, 1, 0x0303)
        program.bind()
        program.setUniformValue("view", *self.view)
        for i, colour in enumerate(m.tev):
            program.setUniformValue(f"c{i}", *(c / 255.0 for c in colour))
        for i, colour in enumerate(m.konst):
            program.setUniformValue(f"k{i}", *(c / 255.0 for c in colour))
        sizes = []
        for unit, (index, wrap_s, wrap_t, name) in enumerate(m.textures[:3]):
            found = self._texture(texture_file(folder, name), wrap_s, wrap_t) if name else None
            if found:
                found[0].bind(unit)
                sizes.append((found[1], found[2]))
            else:
                sizes.append((1, 1))
            self.gl.glUniform1i(program.uniformLocation(f"tex{unit}"), unit)
        for i in range(3):
            a, b = texture_matrix(m.srts[i]) if i < len(m.srts) else ((1, 0, 0), (0, 1, 0))
            program.setUniformValue(f"mtx{i}a", float(a[0]), float(a[1]), float(a[2]))
            program.setUniformValue(f"mtx{i}b", float(b[0]), float(b[1]), float(b[2]))
        return program, sizes

    def _colours(self, m, colours, alpha):
        """The vertex colours DrawQuad sends: the pane's (or the material colour when the channel
        reads the register), alpha multiplied by the pane's global alpha."""
        out = []
        for c in colours:
            rgb = c[:3] if m.vertex_colour else m.colour[:3]
            a = c[3] if m.alpha_vertex else m.colour[3]
            out.append((*rgb, a * alpha))
        return out

    def _picture(self, pane, mtx, alpha, folder):
        m = pane.material
        if m is None:
            return
        program, _sizes = self._setup(m, alpha, folder)
        if program is None:
            return
        uvs = pane.uvs or [[(0, 0), (1, 0), (0, 1), (1, 1)]]
        x, y = vtx_pos(pane)
        self._quad(program, mtx, x, y, pane.size[0], pane.size[1], self._colours(m, pane.colours, alpha), uvs)
        program.release()

    def _window(self, pane, mtx, alpha, folder):
        """Window::DrawSelf: the content, then the frames (one texture flipped to four corners,
        or four or eight textures), each corner piece stretched along its edge."""
        x, y = vtx_pos(pane)
        w, h = pane.size
        frames = pane.frames
        sizes = []
        for fm, _flip in frames:
            name = fm.textures[0][3] if fm and fm.textures else None
            found = self._texture(texture_file(folder, name)) if name else None
            sizes.append((found[1], found[2]) if found else (0, 0))
        if len(frames) == 1:
            l = r = sizes[0][0]
            t = b = sizes[0][1]
        elif len(frames) >= 4:
            l, t = sizes[0]
            r, b = sizes[3]
        else:
            l = r = t = b = 0
        il, ir, it, ib = pane.inflation
        if pane.material is not None:
            program, _ = self._setup(pane.material, alpha, folder)
            if program is not None:
                uvs = pane.uvs or [[(0, 0), (1, 0), (0, 1), (1, 1)]]
                self._quad(program, mtx, x + l - il, y + t - it, w - l + il - r + ir, h - t + it - b + ib,
                           self._colours(pane.material, pane.colours, alpha), uvs)
                program.release()
        white = [(255, 255, 255, 255)] * 4
        if len(frames) == 1:
            parts = [(0, (x, y, w - r, t), "LT", 0), (0, (x + w - r, y, r, h - b), "RT", 1),
                     (0, (x + l, y + h - b, w - l, b), "RB", 4), (0, (x, y + t, l, h - t), "LB", 2)]
        elif len(frames) == 4:
            parts = [(0, (x, y, w - r, t), "LT", None), (1, (x + w - r, y, r, h - b), "RT", None),
                     (3, (x + l, y + h - b, w - l, b), "RB", None), (2, (x, y + t, l, h - t), "LB", None)]
        elif len(frames) == 8:
            parts = [(0, (x, y, l, t), "LT", None), (6, (x + l, y, w - l - r, t), "LT", None),
                     (1, (x + w - r, y, r, t), "RT", None), (5, (x + w - r, y + t, r, h - t - b), "RT", None),
                     (3, (x + w - r, y + h - b, r, b), "RB", None), (7, (x + l, y + h - b, w - l - r, b), "RB", None),
                     (2, (x, y + h - b, l, b), "LB", None), (4, (x, y + t, l, h - t - b), "LB", None)]
        else:
            parts = []
        for index, (qx, qy, qw, qh), corner, flip in parts:
            fm, own_flip = frames[index]
            if fm is None or not fm.textures:
                continue
            program, _ = self._setup(fm, alpha, folder)
            if program is None:
                continue
            uvs = [_frame_coords(corner, qw, qh, sizes[index], own_flip if flip is None else flip)]
            self._quad(program, mtx, qx, qy, qw, qh, self._colours(fm, white, alpha), uvs)
            program.release()

    def _text(self, pane, mtx, alpha, text, scene, fonts_folder):
        m = pane.material
        if m is None or fonts_folder is None or not scene.layout.fonts:
            return
        font = self.font(fonts_folder, scene.layout.fonts[min(pane.font, len(scene.layout.fonts) - 1)])
        if font is None:
            return
        program = self.glyph_program
        self.gl.glBlendFuncSeparate(0x0302, 0x0303, 1, 0x0303)
        program.bind()
        program.setUniformValue("view", *self.view)
        low = [max(0, min(255, c)) / 255.0 for c in m.tev[0]]
        high = [max(0, min(255, c)) / 255.0 for c in m.tev[1]]
        program.setUniformValue("low", *low)
        program.setUniformValue("high", *high)
        self.gl.glUniform1i(program.uniformLocation("tex0"), 0)
        top = tuple(pane.colours[0][:3]) + (pane.colours[0][3] * alpha,)
        bottom = tuple(pane.colours[1][:3]) + (pane.colours[1][3] * alpha,)
        by_sheet = {}
        for quad in layout_text(pane, font, text):
            by_sheet.setdefault(quad[0], []).append(quad)
        import array
        for sheet, quads in by_sheet.items():
            file = font.sheets.get(str(sheet))
            found = self._texture(Path(fonts_folder) / file) if file else None
            if not found:
                continue
            found[0].bind(0)
            data = array.array("f")
            for _s, qx, qy, qw, qh, u0, v0, u1, v1 in quads:
                corners = [(qx, qy, u0, v0, top), (qx + qw, qy, u1, v0, top), (qx, qy + qh, u0, v1, bottom),
                           (qx + qw, qy + qh, u1, v1, bottom)]
                for c in (0, 1, 3, 0, 3, 2):
                    cx, cy, u, v, colour = corners[c]
                    gx, gy = _apply(mtx, cx, -cy)
                    data.extend((gx, gy))
                    data.extend(k / 255.0 for k in colour)
                    data.extend((u, v, 0.0, 0.0, 0.0, 0.0))
            self._draw(program, data)
        program.release()


# lyt_window.cpp's flip table: per flip, the texture coordinates of the four corners (TL, TR,
# BL, BR) and which axis of the texture runs along x and y.
FLIP_INFO = (
    (((0, 0), (1, 0), (0, 1), (1, 1)), (0, 1)),  # none
    (((1, 0), (0, 0), (1, 1), (0, 1)), (0, 1)),  # flip horizontally
    (((0, 1), (1, 1), (0, 0), (1, 0)), (0, 1)),  # flip vertically
    (((0, 1), (0, 0), (1, 1), (1, 0)), (1, 0)),  # rotate 90
    (((1, 1), (0, 1), (1, 0), (0, 0)), (0, 1)),  # rotate 180
    (((1, 0), (1, 1), (0, 0), (0, 1)), (1, 0)),  # rotate 270
)
# Get*TexCoord: (the corner holding the flip coordinate, the corner across x, the corner
# across y) for each frame piece, as corner indices TL 0, TR 1, BL 2, BR 3.
FRAME_CORNERS = {"LT": (0, 1, 2), "RT": (1, 0, 3), "LB": (2, 3, 0), "RB": (3, 2, 1)}


def _frame_coords(corner, width, height, tex_size, flip):
    coords, (ix, iy) = FLIP_INFO[min(flip, 5)]
    anchor, across_x, across_y = FRAME_CORNERS[corner]
    tsz = tex_size if tex_size[0] else (1, 1)
    out = [[0.0, 0.0] for _ in range(4)]
    # The anchor corner and its neighbours on the same row and column hold the flip coordinate.
    far_x = width / ((coords[across_x][ix] - coords[anchor][ix]) * tsz[ix]) + coords[anchor][ix] \
        if coords[across_x][ix] != coords[anchor][ix] else coords[anchor][ix]
    far_y = height / ((coords[across_y][iy] - coords[anchor][iy]) * tsz[iy]) + coords[anchor][iy] \
        if coords[across_y][iy] != coords[anchor][iy] else coords[anchor][iy]
    same_col = {0: 2, 1: 3, 2: 0, 3: 1}[anchor]  # the corner below or above the anchor
    same_row = {0: 1, 1: 0, 2: 3, 3: 2}[anchor]
    opposite = 3 - anchor
    out[anchor][ix] = out[same_col][ix] = coords[anchor][ix]
    out[anchor][iy] = out[same_row][iy] = coords[anchor][iy]
    out[same_row][ix] = out[opposite][ix] = far_x
    out[same_col][iy] = out[opposite][iy] = far_y
    return [tuple(c) for c in out]
