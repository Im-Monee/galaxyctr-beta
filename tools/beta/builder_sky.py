"""The game's sky behind the builder window's start page, drawn with OpenGL (Qt).

builder_assets.make_sky writes a sky model taken from the user's disc as JSON and PNG files;
SkyRenderer draws it, and SkyView is the widget the window puts behind its start page. The
model's material becomes a GLSL shader (tev_shader): the TEV stages of the GameCube/Wii pixel
pipeline written as code, with the same inputs, operations, registers and konst colours, and
texture coordinates through the material's texture matrices. In the game a sky has no motion
of its own (Game/Map/Sky.cpp only centres it on the camera), so the camera turns slowly.

Anything that goes wrong here (no OpenGL, a driver that refuses the shader) only hides the view:
the window keeps its own painted sky.
"""

import array
import json
import math
from pathlib import Path

from PySide6.QtCore import QTimer, Signal
from PySide6.QtGui import QImage, QMatrix4x4, QOpenGLContext, QVector3D
from PySide6.QtOpenGL import (QOpenGLBuffer, QOpenGLShader, QOpenGLShaderProgram, QOpenGLTexture,
                              QOpenGLVertexArrayObject)
from PySide6.QtOpenGLWidgets import QOpenGLWidget

import builder_game  # the window's frame rate (frame_timer)

GL_FLOAT, GL_TRIANGLES = 0x1406, 0x0004
GL_COLOR_BUFFER_BIT, GL_DEPTH_TEST, GL_CULL_FACE, GL_BLEND = 0x4000, 0x0B71, 0x0B44, 0x0BE2
GL_DEPTH_BUFFER_BIT, GL_NEVER, GL_CW, GL_CCW = 0x0100, 0x0200, 0x0900, 0x0901
GL_FRONT_AND_BACK = 0x0408
GL_CULL = {1: 0x0404, 2: 0x0405}  # GX_CULL_FRONT, GX_CULL_BACK as GL_FRONT, GL_BACK
FLOATS = 9  # x y z, r g b a, u v
YAW, PITCH = 60.0, 18.0  # where the camera starts: the galaxy band across the view


# ##############################################################################################
# # TEV stages as GLSL                                                                         #
# #                                                                                            #
# # GX numbering (revolution/gx/GXEnum.h): colour inputs 0 CPREV, 1 APREV, 2 C0, 3 A0, 4 C1,    #
# # 5 A1, 6 C2, 7 A2, 8 TEXC, 9 TEXA, 10 RASC, 11 RASA, 12 ONE, 13 HALF, 14 KONST, 15 ZERO;     #
# # alpha inputs 0 APREV, 1 A0, 2 A1, 3 A2, 4 TEXA, 5 RASA, 6 KONST, 7 ZERO; outputs 0 PREV,   #
# # 1 REG0, 2 REG1, 3 REG2. A stage is (a, b, c, d, op, bias, scale, clamp, out):              #
# # out = (d +/- ((1 - c) a + c b) + bias) * scale. J3D loads tev colours 0 to 2 into REG0 to  #
# # REG2 and colour 3 into PREV.                                                               #
# ##############################################################################################
REGS = ("prev", "r0", "r1", "r2")


def _konst_colour(sel):
    if sel < 8:
        v = (8 - sel) / 8
        return f"vec3({v:.4f})"
    if sel < 16:
        return f"k{sel - 12}.rgb"
    return f"vec3(k{(sel - 16) % 4}.{'rgba'[(sel - 16) // 4]})"


def _konst_alpha(sel):
    if sel < 8:
        return f"{(8 - sel) / 8:.4f}"
    return f"k{(sel - 16) % 4}.{'rgba'[max(0, sel - 16) // 4]}"


def _colour_input(arg):
    if arg < 8:
        reg = REGS[arg // 2]
        return f"{reg}.rgb" if arg % 2 == 0 else f"vec3({reg}.a)"
    return {8: "t.rgb", 9: "vec3(t.a)", 10: "ras.rgb", 11: "vec3(ras.a)", 12: "vec3(1.0)",
            13: "vec3(0.5)", 14: "kc"}.get(arg, "vec3(0.0)")


def _alpha_input(arg):
    if arg < 4:
        return f"{REGS[arg]}.a"
    return {4: "t.a", 5: "ras.a", 6: "ka"}.get(arg, "0.0")


def _operation(a, b, c, d, op, bias, scale, clamp, kind):
    lerp = f"mix({a}, {b}, {c})"
    value = f"({d} {'-' if op == 1 else '+'} {lerp} {('', '+ 0.5', '- 0.5', '')[bias]})"
    value = f"({value} * {(1.0, 2.0, 4.0, 0.5)[scale]:.1f})"
    if clamp:
        value = f"clamp({value}, 0.0, 1.0)"
    return value


def _rasterised(material):
    """The rasterised colour: the vertices' or the material colour, for colour and alpha."""
    from_vertex_rgb, from_vertex_a, colour = material.get("ras", (True, True, (255, 255, 255, 255)))
    reg = [c / 255.0 for c in colour]
    rgb = "vColour.rgb" if from_vertex_rgb else f"vec3({reg[0]:.4f}, {reg[1]:.4f}, {reg[2]:.4f})"
    a = "vColour.a" if from_vertex_a else f"{reg[3]:.4f}"
    return f"vec4({rgb}, {a})"


def tev_shader(material, textures):
    """The fragment shader of a material (GLSL 1.20, which every OpenGL Qt opens can run)."""
    lines = ["#version 120", "varying vec4 vColour;", "varying vec2 vUv;"]
    lines += [f"uniform sampler2D tex{i};" for i in range(len(textures))]
    lines += [f"uniform vec3 mtx{i}a;\nuniform vec3 mtx{i}b;" for i in range(8)]
    lines += ["uniform vec4 c0, c1, c2, c3, k0, k1, k2, k3;", "void main() {",
              "  vec4 prev = c3, r0 = c0, r1 = c1, r2 = c2;", f"  vec4 ras = {_rasterised(material)};",
              "  vec4 t; vec3 kc; float ka; vec2 uv;"]
    for i, ((cin, ain), (coord, tex_map, _chan)) in enumerate(zip(material["stages"], material["orders"])):
        if tex_map != 255 and tex_map < len(material["textures"]):
            lines.append(f"  uv = vec2(dot(mtx{coord}a, vec3(vUv, 1.0)), dot(mtx{coord}b, vec3(vUv, 1.0)));")
            lines.append(f"  t = texture2D(tex{material['textures'][tex_map]}, uv);")
        else:
            lines.append("  t = vec4(1.0);")
        lines.append(f"  kc = {_konst_colour(material['kcsel'][i])};")
        lines.append(f"  ka = {_konst_alpha(material['kasel'][i])};")
        a, b, c, d, op, bias, scale, clamp, out = cin[:9]
        colour = _operation(*(_colour_input(x) for x in (a, b, c, d)), op, bias, scale, clamp, "rgb")
        a, b, c, d, op, bias, scale, clamp, out_a = ain[:9]
        alpha = _operation(*(_alpha_input(x) for x in (a, b, c, d)), op, bias, scale, clamp, "a")
        lines.append(f"  {{ vec3 rgb = {colour}; float al = {alpha};")
        lines.append(f"    {REGS[out]}.rgb = rgb; {REGS[out_a]}.a = al; }}")
    lines += ["  gl_FragColor = vec4(prev.rgb, 1.0);", "}"]
    return "\n".join(lines)


VERTEX_SHADER = """#version 120
attribute vec3 position;
attribute vec4 colour;
attribute vec2 uv;
uniform mat4 matrix;
varying vec4 vColour;
varying vec2 vUv;
void main() {
  vColour = colour;
  vUv = uv;
  gl_Position = matrix * vec4(position, 1.0);
}
"""


class SkyRenderer:
    """Draws one sky model; init() and draw() run with its OpenGL context current."""

    def __init__(self, folder, name):
        self.folder = Path(folder)
        self.data = json.loads((self.folder / f"{name}.json").read_text(encoding="utf-8"))
        self.ready = False
        self.error = ""

    def init(self):
        context = QOpenGLContext.currentContext()
        self.gl = context.functions()
        self.textures = []
        for info in self.data["textures"]:
            image = QImage(str(self.folder / info["file"])).convertToFormat(QImage.Format.Format_RGBA8888)
            texture = QOpenGLTexture(image, QOpenGLTexture.MipMapGeneration.GenerateMipMaps)
            texture.setMinMagFilters(QOpenGLTexture.Filter.LinearMipMapLinear, QOpenGLTexture.Filter.Linear)
            modes = (QOpenGLTexture.WrapMode.ClampToEdge, QOpenGLTexture.WrapMode.Repeat, QOpenGLTexture.WrapMode.MirroredRepeat)
            texture.setWrapMode(QOpenGLTexture.CoordinateDirection.DirectionS, modes[min(info["wrap"][0], 2)])
            texture.setWrapMode(QOpenGLTexture.CoordinateDirection.DirectionT, modes[min(info["wrap"][1], 2)])
            self.textures.append(texture)
        self.programs = []
        for material in self.data["materials"]:
            program = QOpenGLShaderProgram()
            if not program.addShaderFromSourceCode(QOpenGLShader.ShaderTypeBit.Vertex, VERTEX_SHADER) or \
                    not program.addShaderFromSourceCode(QOpenGLShader.ShaderTypeBit.Fragment, tev_shader(material, self.textures)) or \
                    not program.link():
                self.error = program.log()
                return False
            self.programs.append(program)
        self.vao = QOpenGLVertexArrayObject()
        self.vao.create()
        self.buffers = []
        for shape in self.data["shapes"]:
            values = array.array("f", shape["vertices"])
            buffer = QOpenGLBuffer(QOpenGLBuffer.Type.VertexBuffer)
            buffer.create()
            buffer.bind()
            buffer.allocate(values.tobytes(), len(values) * 4)
            buffer.release()
            self.buffers.append((buffer, len(values) // FLOATS, shape["material"]))
        self.ready = True
        return True

    def draw(self, width, height, t, yaw=None, pitch=None):
        gl = self.gl
        gl.glViewport(0, 0, width, height)
        gl.glClearColor(0.0, 0.0, 0.0, 1.0)
        gl.glClear(GL_COLOR_BUFFER_BIT)
        if not self.ready:
            return
        gl.glDisable(GL_DEPTH_TEST)
        gl.glDisable(GL_CULL_FACE)
        gl.glDisable(GL_BLEND)
        projection = QMatrix4x4()
        projection.perspective(55.0, width / max(1, height), 100.0, 400000.0)
        view = QMatrix4x4()
        view.rotate(PITCH + 6.0 * math.sin(t * 0.05) if pitch is None else pitch, QVector3D(1, 0, 0))  # a slow nod
        view.rotate(YAW + t * 1.2 if yaw is None else yaw, QVector3D(0, 1, 0))  # and a slow turn: 1.2 degrees a second
        matrix = projection * view
        self.vao.bind()
        for buffer, count, material_index in self.buffers:
            program = self.programs[material_index]
            material = self.data["materials"][material_index]
            program.bind()
            program.setUniformValue("matrix", matrix)
            for name, colour in zip(("c0", "c1", "c2", "c3"), material["tev_colours"]):
                program.setUniformValue(name, *(c / 255 for c in colour))
            for name, colour in zip(("k0", "k1", "k2", "k3"), material["konst"]):
                program.setUniformValue(name, *(c / 255 for c in colour))
            for i, m in enumerate(material["matrices"]):
                m = m or [1, 0, 0, 0, 1, 0]
                program.setUniformValue(f"mtx{i}a", float(m[0]), float(m[1]), float(m[2]))
                program.setUniformValue(f"mtx{i}b", float(m[3]), float(m[4]), float(m[5]))
            for unit, texture in enumerate(self.textures):
                texture.bind(unit)
                # A sampler takes an integer: glUniform1i (PySide picks the float overload otherwise).
                gl.glUniform1i(program.uniformLocation(f"tex{unit}"), unit)
            buffer.bind()
            stride = FLOATS * 4
            for location, (name, offset, size) in enumerate((("position", 0, 3), ("colour", 12, 4), ("uv", 28, 2))):
                where = program.attributeLocation(name)
                if where >= 0:
                    program.enableAttributeArray(where)
                    program.setAttributeBuffer(where, GL_FLOAT, offset, size, stride)
            gl.glDrawArrays(GL_TRIANGLES, 0, count)
            buffer.release()
            program.release()
        self.vao.release()


class SkyView(QOpenGLWidget):
    """The sky as a widget; failed is emitted (and the view hidden) when OpenGL cannot draw it."""

    failed = Signal(str)

    def __init__(self, folder, name, parent=None, moving=True):
        super().__init__(parent)
        self.renderer = SkyRenderer(folder, name)
        self.moving = moving
        self.started = None
        self.timer = builder_game.frame_timer(self)  # Settings > Graphics: the frame rate
        self.timer.timeout.connect(self.update)

    def initializeGL(self):
        try:
            if not self.renderer.init():
                raise RuntimeError(self.renderer.error or "the shader did not compile")
        except Exception as error:  # noqa: BLE001 - the painted sky stays
            self.failed.emit(str(error))
            return
        if self.moving:
            self.timer.start()

    def paintGL(self):
        import time
        if self.started is None:
            self.started = time.monotonic()
        t = time.monotonic() - self.started if self.moving else 0.0
        ratio = self.devicePixelRatioF()
        self.renderer.draw(int(self.width() * ratio), int(self.height() * ratio), t)


# ##############################################################################################
# # Joint and texture animations (J3DAnmTransformKey, ANK1; J3DAnmTextureSRTKey, TTK1)         #
# #                                                                                            #
# # Both are tables of three entries per joint or texture matrix (x, y, z), each three keys    #
# # (scale, rotation, translation: count, first value, tangent type), over a scale table      #
# # (f32), a rotation table (s16, shifted left by the header's shift) and a translation table #
# # (f32). One key is a value; more are (frame, value, tangent) or (frame, value, in, out),   #
# # on Hermite curves (J3DGetKeyFrameInterpolation).                                          #
# ##############################################################################################
class TransformAnim:
    def __init__(self, data):
        import struct
        tag = b"ANK1" if b"ANK1" in data else b"TTK1"
        d = data[data.index(tag):]
        self.texture = tag == b"TTK1"
        self.loop, self.shift, self.frames, count = struct.unpack_from(">BBHH", d, 8)
        if self.texture:
            table, remap, names, ids, centres, scales, rots, trans = struct.unpack_from(">8I", d, 0x14)
            count //= 3
        else:
            table, scales, rots, trans = struct.unpack_from(">4I", d, 0x14)
        self.d = d
        self.tables = (scales, rots, trans)
        self.entries = []
        for i in range(count):
            e = table + 54 * i
            self.entries.append([struct.unpack_from(">HHH", d, e + 6 * k) for k in range(9)])
        self.targets = {}
        if self.texture:
            mat_names = _names(d, names)
            for i in range(count):
                cx, cy, _cz = struct.unpack_from(">3f", d, centres + 12 * i)
                self.targets[(mat_names[i], d[ids + i])] = (i, cx, cy)

    def _key(self, info, kind, frame):
        import struct
        count, first, tangent = info
        if count == 0:
            return 1.0 if kind == 0 else 0.0
        fmt, size = ("h", 2) if kind == 1 else ("f", 4)
        base = self.tables[kind]
        if count == 1:
            return struct.unpack_from(">" + fmt, self.d, base + size * first)[0]
        n = 3 if tangent == 0 else 4
        raw = struct.unpack_from(f">{count * n}{fmt}", self.d, base + size * first)
        keys = [raw[k * n:(k + 1) * n] for k in range(count)]
        if frame <= keys[0][0]:
            return keys[0][1]
        if frame >= keys[-1][0]:
            return keys[-1][1]
        import builder_particles
        for k0, k1 in zip(keys, keys[1:]):
            if k0[0] <= frame < k1[0]:
                return builder_particles.hermite(frame, k0[0], k0[1], k0[n - 1], k1[0], k1[1], k1[2])
        return keys[-1][1]

    def frame_of(self, frame):
        if self.loop == 2 and self.frames:
            return frame % self.frames
        return min(frame, self.frames)

    def joint(self, index, frame):
        """(scale xyz, rotation xyz in 65536ths, translation xyz) of a joint at frame."""
        e = self.entries[index]
        f = self.frame_of(frame)
        scale = [self._key(e[3 * a], 0, f) for a in range(3)]
        rot = [self._key(e[3 * a + 1], 1, f) * (1 << self.shift) for a in range(3)]
        trans = [self._key(e[3 * a + 2], 2, f) for a in range(3)]
        return scale, rot, trans

    def texture_srt(self, material, slot, frame):
        """(cx, cy, sx, sy, angle, tx, ty) of a material's texture matrix at frame, or None."""
        found = self.targets.get((material, slot))
        if found is None:
            return None
        i, cx, cy = found
        e = self.entries[i]
        f = self.frame_of(frame)
        return [cx, cy, self._key(e[0], 0, f), self._key(e[3], 0, f), self._key(e[7], 1, f) * (1 << self.shift),
                self._key(e[2], 2, f), self._key(e[5], 2, f)]


def _names(d, at):
    import struct
    count = struct.unpack_from(">H", d, at)[0]
    out = []
    for i in range(count):
        offset = struct.unpack_from(">H", d, at + 6 + 4 * i)[0]
        out.append(d[at + offset:d.index(b"\0", at + offset)].decode("ascii", "replace"))
    return out


def joint_matrix(scale, rot, trans):
    """J3DGetTranslateRotateMtx with scale: T * Rz * Ry * Rx * S, as a QMatrix4x4."""
    m = QMatrix4x4()
    m.translate(*trans)
    m.rotate(rot[2] * 180.0 / 32768.0, QVector3D(0, 0, 1))
    m.rotate(rot[1] * 180.0 / 32768.0, QVector3D(0, 1, 0))
    m.rotate(rot[0] * 180.0 / 32768.0, QVector3D(1, 0, 0))
    m.scale(*scale)
    return m


def joint_worlds(joints, anim=None, frame=0.0):
    """Every joint's matrix in the model, its parents' first; anim (ANK1) moves the joints it has."""
    out = []
    for i, j in enumerate(joints):
        if anim is not None and i < len(anim.entries):
            local = joint_matrix(*anim.joint(i, frame))
        else:
            local = joint_matrix(j["scale"], j["rotate"], j["translate"])
        parent = j.get("parent")
        out.append(out[parent] * local if parent is not None and parent < len(out) else local)
    return out


# ##############################################################################################
# # The title screen's sky (Game/Map/FileSelectSky.cpp)                                       #
# #                                                                                            #
# # CometNearOrbitSky at 0.8 of its size, around the title camera (FileSelectCameraController:  #
# # looking straight ahead, 60 degrees high), turning as FileSelectSky::exeWait turns it: 0.001 #
# # of a radian a frame about y, and about x half a cosine over 3000 frames up to three        #
# # quarters of a half turn. Its joints and texture matrices play their animations; each       #
# # material blends as its blend mode says, the opaque ones first.                            #
# ##############################################################################################
GX_SRC = {0: 0, 1: 1, 2: 0x0306, 3: 0x0307, 4: 0x0302, 5: 0x0303, 6: 0x0304, 7: 0x0305}
GX_DST = {0: 0, 1: 1, 2: 0x0300, 3: 0x0301, 4: 0x0302, 5: 0x0303, 6: 0x0304, 7: 0x0305}


TITLE_VERTEX = """#version 120
attribute vec3 position;
attribute vec4 colour;
attribute vec2 uv;
attribute vec3 normal;
uniform mat4 matrix;
uniform mat4 model;
uniform mat4 view;
varying vec4 vColour;
varying vec2 vUv;
varying vec3 vPos;
varying vec3 vViewPos;
varying vec3 vNormal;
void main() {
  vColour = colour;
  vUv = uv;
  vPos = (model * vec4(position, 1.0)).xyz;
  vViewPos = (view * vec4(position, 1.0)).xyz;
  vNormal = mat3(view) * normal;
  gl_Position = matrix * vec4(position, 1.0);
}
"""


# ##############################################################################################
# # Lit colour channels (GX lighting, GXSetChanCtrl)                                           #
# #                                                                                            #
# # A lit channel is its material colour times the ambient colour plus each light of its mask: #
# # the light's colour times the diffuse term (none, the sign or the clamp of N.L, L towards   #
# # the light). The scene's lights are loaded with no attenuation (loadLightDiffuse: 1, 0, 0), #
# # and the sum is clamped. Positions and normals are in view space: the title camera looks    #
# # straight ahead from the origin, so the lights' places stay as they are.                    #
# ##############################################################################################
def _channel(chan, material, lights, part):
    """The GLSL of one channel (part "rgb" or "a") as the TEV reads it."""
    enable, mat_src, mask, diffuse, _attn, amb_src = (list(chan) + [0] * 6)[:6]
    reg = [c / 255.0 for c in material.get("ras", (True, True, (255, 255, 255, 255)))[2]]
    amb = [c / 255.0 for c in material.get("amb", (0, 0, 0, 0))]

    def constant(values):
        return f"vec3({values[0]:.4f}, {values[1]:.4f}, {values[2]:.4f})" if part == "rgb" else f"{values[3]:.4f}"

    base = f"vColour.{part}" if mat_src else constant(reg)
    if not enable or not lights:
        return base
    total = [f"vColour.{part}" if amb_src else constant(amb)]
    for i, light in enumerate(lights["lights"][:8]):
        if not mask & (1 << i):
            continue
        colour = constant([c / 255.0 for c in light["colour"]])
        x, y, z = light["pos"]
        towards = f"normalize(vec3({x:.2f}, {y:.2f}, {z:.2f}) - vViewPos)"
        dot = f"dot(normalize(vNormal), {towards})"
        term = {0: "1.0", 1: dot, 2: f"max({dot}, 0.0)"}.get(diffuse, "1.0")
        total.append(f"{colour} * {term}")
    return f"{base} * clamp({' + '.join(total)}, 0.0, 1.0)"


def lit_rasterised(material, lights):
    """The rasterised colour of a material whose channels are lit, or None when neither is."""
    chans = material.get("chans") or []
    if len(chans) < 2 or not any(c[0] & 1 for c in chans[:2]):
        return None
    return f"vec4({_channel(chans[0], material, lights, 'rgb')}, {_channel(chans[1], material, lights, 'a')})"


def title_shader(material, textures, lights=None):
    """tev_shader, with the projection-mapped coordinates (J3DTexMtxMode_Projmap: a 3x4 matrix
    from the vertex position, GX_TG_MTX3x4 from GX_TG_POS) and the alpha kept for the blend.
    The position is the model's, before the sky's own placement (ProjmapEffectMtxSetter undoes
    the base matrix); the effect matrix, then the texture matrix (its translation on q), then
    the divide by q."""
    source = tev_shader(material, textures).replace("varying vec2 vUv;", "varying vec2 vUv;\nvarying vec3 vPos;")
    lit = lit_rasterised(material, lights)
    if lit is not None:
        source = source.replace(f"  vec4 ras = {_rasterised(material)};", f"  vec4 ras = {lit};")
        source = source.replace("varying vec3 vPos;", "varying vec3 vPos;\nvarying vec3 vViewPos;\nvarying vec3 vNormal;")
    source = source.replace("  gl_FragColor = vec4(prev.rgb, 1.0);", "  gl_FragColor = clamp(prev, 0.0, 1.0);")
    modes = material.get("tex_modes") or []
    for k, gen in enumerate(material["texgens"]):
        kind, src, mtx = gen[:3]
        slot = (mtx - 30) // 3 if 30 <= mtx < 60 else None
        if kind == 0 and src == 0 and slot is not None and slot < len(modes) and modes[slot] and modes[slot][0] == 8:
            plain = f"  uv = vec2(dot(mtx{k}a, vec3(vUv, 1.0)), dot(mtx{k}b, vec3(vUv, 1.0)));"
            e = modes[slot][1]
            rows = [f"vec4({e[4 * r]:.8f}, {e[4 * r + 1]:.8f}, {e[4 * r + 2]:.8f}, {e[4 * r + 3]:.8f})" for r in range(3)]
            projected = (f"  {{ vec4 hp = vec4(vPos, 1.0); vec3 q = vec3(dot({rows[0]}, hp), dot({rows[1]}, hp), dot({rows[2]}, hp));\n"
                         f"    uv = vec2(dot(mtx{k}a, q), dot(mtx{k}b, q)) / q.z; }}")
            source = source.replace(plain, projected)
    return source


def draw_order(material, shape):
    """Where a shape's material comes in the game's drawing (Game/System/DrawBuffer.cpp,
    DrawBuffer::initTable): the opaque list first (material mode 1 or 2), then the translucent
    one (mode 4, J3DMaterial::isDrawModeOpaTexEdge), each sorted by material name without case
    (sortShapeDrawer: strcasecmp), the shapes of a material in the model's order. The comet's
    yellow halo (CometHalo_v, opaque, no z writes) so comes first, and the space behind it
    covers it. Without the mode (older files), a blended material counts as translucent."""
    mode = material.get("mode")
    translucent = (mode & 3) == 0 if mode is not None else material["blend"][0] != 0
    return translucent, material.get("name", "").lower(), shape


class TitleSkyRenderer(SkyRenderer):
    SCALE = 0.8  # FileSelectSky::init
    FOVY = 60.0  # FileSelectCameraController: cTitleFovy

    def init(self):
        anims = self.data.get("anims", {})
        self.joint_anim = self.texture_anim = None
        for member, file in anims.items():
            body = (self.folder / file).read_bytes()
            if member.endswith(".bck"):
                self.joint_anim = TransformAnim(body)
            elif member.endswith(".btk"):
                self.texture_anim = TransformAnim(body)
        if not super().init():
            return False
        # The same TEV stages, ending with their alpha for the blend.
        self.programs = []
        for material in self.data["materials"]:
            program = QOpenGLShaderProgram()
            source = title_shader(material, self.textures, self.data.get("lights"))
            if not program.addShaderFromSourceCode(QOpenGLShader.ShaderTypeBit.Vertex, TITLE_VERTEX) or \
                    not program.addShaderFromSourceCode(QOpenGLShader.ShaderTypeBit.Fragment, source) or \
                    not program.link():
                self.error = program.log()
                self.ready = False
                return False
            self.programs.append(program)
        # The normals of the lit shapes, a buffer of their own.
        self.normals = {}
        for i, shape in enumerate(self.data["shapes"]):
            if shape.get("normals"):
                values = array.array("f", shape["normals"])
                buffer = QOpenGLBuffer(QOpenGLBuffer.Type.VertexBuffer)
                buffer.create()
                buffer.bind()
                buffer.allocate(values.tobytes(), len(values) * 4)
                buffer.release()
                self.normals[i] = buffer
        return True

    def draw(self, width, height, frame, yaw=None, pitch=None):
        gl = self.gl
        gl.glViewport(0, 0, width, height)
        gl.glClearColor(0.0, 0.0, 0.0, 1.0)
        gl.glDepthMask(True)
        gl.glClearDepthf(1.0)
        gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
        if not self.ready:
            return
        gl.glFrontFace(GL_CW)
        # The materials' alpha only feeds their blend (none reads the alpha already drawn). The
        # picture itself stays opaque, as the game's: an alpha written here would make Qt take
        # the colours as premultiplied and blow them up (the horizon band went white).
        gl.glColorMask(True, True, True, False)
        projection = QMatrix4x4()
        projection.perspective(self.FOVY, width / max(1, height), 100.0, 4000000.0)
        # FileSelectSky::exeWait: the base matrix is (rotate y * rotate x) inverted.
        angle_y = 0.001 * frame
        angle_x = 0.5 * (1.0 - math.cos(frame * math.pi / 3000.0)) * math.pi * 3.0 / 4.0
        turn = QMatrix4x4()
        turn.rotate(math.degrees(angle_y), QVector3D(0, 1, 0))
        turn.rotate(math.degrees(angle_x), QVector3D(1, 0, 0))
        base, _ok = turn.inverted()
        # The camera's frame for the sky's effects (TitleShootingStars): the turn alone, as the
        # effect follows world_root's place and turn, not its scale.
        self.effect_view = tuple((base.row(r).x(), base.row(r).y(), base.row(r).z()) for r in range(3))
        self.effect_lens = (1.0 / math.tan(math.radians(self.FOVY) / 2.0), width / max(1, height))
        base.scale(self.SCALE)
        worlds = joint_worlds(self.data.get("joints") or [{"scale": [1, 1, 1], "rotate": [0, 0, 0],
                                                          "translate": [0, 0, 0], "parent": None}],
                              self.joint_anim, frame)
        order = sorted(range(len(self.buffers)), key=lambda i: draw_order(self.data["materials"][self.buffers[i][2]], i))
        self.vao.bind()
        for i in order:
            buffer, count, material_index = self.buffers[i]
            shape = self.data["shapes"][i]
            program = self.programs[material_index]
            material = self.data["materials"][material_index]
            mode, src, dst = material["blend"][:3]
            if mode == 1:
                gl.glEnable(GL_BLEND)
                gl.glBlendFunc(GX_SRC.get(src, 1), GX_DST.get(dst, 0))
            else:
                gl.glDisable(GL_BLEND)
            # GX's cull mode (1 front, 2 back, 3 both), its front faces clockwise on the screen.
            cull = material.get("cull", 0)
            if cull:
                gl.glEnable(GL_CULL_FACE)
                gl.glCullFace(GL_CULL.get(cull, GL_FRONT_AND_BACK))
            else:
                gl.glDisable(GL_CULL_FACE)
            # GX's z mode: the comet's halos test against the planet and the sky without
            # writing, the opaque parts write. GX numbers the compare functions as GL does.
            test, compare, update = material.get("zmode") or (1, 3, int(mode == 0))
            if test:
                gl.glEnable(GL_DEPTH_TEST)
                gl.glDepthFunc(GL_NEVER + (compare & 7))
            else:
                gl.glDisable(GL_DEPTH_TEST)
            gl.glDepthMask(bool(update))
            program.bind()
            joint = worlds[min(shape.get("joint", 0), len(worlds) - 1)]
            program.setUniformValue("matrix", projection * base * joint)
            model = QMatrix4x4()
            model.scale(self.SCALE)
            program.setUniformValue("model", model * joint)
            program.setUniformValue("view", base * joint)
            for name, colour in zip(("c0", "c1", "c2", "c3"), material["tev_colours"]):
                program.setUniformValue(name, *(c / 255 for c in colour))
            for name, colour in zip(("k0", "k1", "k2", "k3"), material["konst"]):
                program.setUniformValue(name, *(c / 255 for c in colour))
            for slot, m in enumerate(material["matrices"]):
                srt = self.texture_anim.texture_srt(material.get("name", ""), slot, frame) if self.texture_anim else None
                if srt is not None:
                    import builder_assets
                    m = builder_assets.tex_matrix(*srt)
                m = m or [1, 0, 0, 0, 1, 0]
                program.setUniformValue(f"mtx{slot}a", float(m[0]), float(m[1]), float(m[2]))
                program.setUniformValue(f"mtx{slot}b", float(m[3]), float(m[4]), float(m[5]))
            for unit, texture in enumerate(self.textures):
                texture.bind(unit)
                gl.glUniform1i(program.uniformLocation(f"tex{unit}"), unit)
            used = []
            buffer.bind()
            stride = FLOATS * 4
            for name, offset, size in (("position", 0, 3), ("colour", 12, 4), ("uv", 28, 2)):
                where = program.attributeLocation(name)
                if where >= 0:
                    program.enableAttributeArray(where)
                    program.setAttributeBuffer(where, GL_FLOAT, offset, size, stride)
                    used.append(where)
            buffer.release()
            where = program.attributeLocation("normal")
            if where >= 0 and i in self.normals:
                self.normals[i].bind()
                program.enableAttributeArray(where)
                program.setAttributeBuffer(where, GL_FLOAT, 0, 3, 12)
                used.append(where)
                self.normals[i].release()
            gl.glDrawArrays(GL_TRIANGLES, 0, count)
            for where in used:  # none left on for the next shape's buffers
                program.disableAttributeArray(where)
            program.release()
        gl.glDisable(GL_BLEND)
        gl.glDisable(GL_CULL_FACE)
        gl.glDisable(GL_DEPTH_TEST)
        gl.glDepthMask(True)
        gl.glFrontFace(GL_CCW)
        gl.glColorMask(True, True, True, True)
        self.vao.release()


class TitleSkyView(QOpenGLWidget):
    """The title screen's sky as a widget, at the game's 60 frames a second, with its effect:
    the small shooting stars that fall along the comet (autoeffectlist.bcsv: CometNearOrbitSky's
    TitleShootingStar00 at world_root, scale 10, started with the sky's joint animation)."""

    failed = Signal(str)
    EFFECT, EFFECT_SCALE = "TitleShootingStar", 10.0

    def __init__(self, folder, parent=None, moving=True):
        super().__init__(parent)
        self.renderer = TitleSkyRenderer(folder, "cometnearorbitsky")
        self.moving = moving
        self.started = None
        self.effects = None
        try:
            import builder_particles
            manifest = json.loads((Path(folder) / "manifest.json").read_text(encoding="utf-8"))
            library = builder_particles.Library(folder, manifest["particles"])
            if library.has(self.EFFECT):
                self.effects = builder_particles.Player(library, builder_particles.SceneRenderer(library))
                self.effects.emit(self.EFFECT, scale=self.EFFECT_SCALE)
        except Exception:  # noqa: BLE001 - the sky shows without its shooting stars
            self.effects = None
        self.timer = builder_game.frame_timer(self)  # Settings > Graphics: the frame rate
        self.timer.timeout.connect(self.update)

    def initializeGL(self):
        try:
            if not self.renderer.init():
                raise RuntimeError(self.renderer.error or "the shader did not compile")
        except Exception as error:  # noqa: BLE001
            self.failed.emit(str(error))
            return
        if self.moving:
            self.timer.start()

    def paintGL(self):
        import time
        if self.started is None:
            self.started = time.monotonic()
        frame = (time.monotonic() - self.started) * 60.0 if self.moving else 0.0
        ratio = self.devicePixelRatioF()
        self.renderer.draw(int(self.width() * ratio), int(self.height() * ratio), frame)
        if self.effects is not None and self.renderer.ready and self.moving:
            self.effects.advance(time.monotonic())
            self.draw_effects()

    def draw_effects(self):
        rows = self.renderer.effect_view
        lens, aspect = self.renderer.effect_lens

        def to_view(p):
            x, y, z = p
            return (rows[0][0] * x + rows[0][1] * y + rows[0][2] * z, rows[1][0] * x + rows[1][1] * y + rows[1][2] * z,
                    rows[2][0] * x + rows[2][1] * y + rows[2][2] * z)

        def project(q):
            depth = -q[2]
            if depth < 100.0:  # the near plane of the sky's lens
                return None
            return lens / aspect * q[0] / depth, lens * q[1] / depth

        try:
            self.effects.renderer.draw(self.effects.emitters, to_view, project)
        except Exception:  # noqa: BLE001 - a driver that refuses them leaves the sky alone
            self.effects = None
