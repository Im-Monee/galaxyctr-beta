"""The PowerStarHalo in the builder window: the light the game shows where a Power Star is while
the camera is still far from it (Game/Map/Halo.cpp), from the user's own disc.

builder_assets.make_sky writes its model (ObjectData/PowerStarHalo.arc: a round glow, NovaMat on
joint Glow, and a long flat streak, GlowMat on joint LineGlow, both added to the picture) with
its materials, its joints and its animations. Halo plays them as the game does: MapObjActor
starts the animation named as the model (powerstarhalo.btk: the glow's second texture scrolls,
round and round each second, so the glow flickers) and Halo::appear starts Appear (.brk), which
fades the halo in; the halo then stays as Appear ends. The joints place the shapes (LineGlow
flattens the streak to 0.68 of its height; the shapes are billboards, which keep that scale).

The model is drawn facing the window with OpenGL, its materials as builder_sky's TEV shaders,
into a premultiplied picture the window lays over its page.
"""

import array
import json
import struct
from pathlib import Path

import builder_particles as P
import builder_sky


# ##############################################################################################
# # Colour animations (J3DAnmTevRegKey, TRK1)                                                  #
# #                                                                                            #
# # Header: loop mode, frame count, how many colour and konst register entries, the sizes of  #
# # the value tables, then offsets: the entries, the material of each entry, their names and  #
# # the red, green, blue and alpha values (s16). An entry is four keys (count, first value,   #
# # tangent type) and the register it sets. One key is a value; more are (frame, value,       #
# # tangent) or (frame, value, in, out) and run on Hermite curves.                            #
# ##############################################################################################
class ColourAnim:
    def __init__(self, data):
        at = data.index(b"TRK1")
        d = data[at:]
        self.frames = struct.unpack_from(">H", d, 0x0A)[0]
        count = struct.unpack_from(">H", d, 0x0C)[0]
        table, names_at = struct.unpack_from(">I", d, 0x20)[0], struct.unpack_from(">I", d, 0x30)[0]
        values = [struct.unpack_from(">I", d, 0x38 + 4 * i)[0] for i in range(4)]
        names = _names(d, names_at)
        self.entries = []  # (material name, register, [keys of r, g, b, a])
        for i in range(count):
            e = table + 28 * i
            channels = []
            for c in range(4):
                n, first, tangent = struct.unpack_from(">HHH", d, e + 6 * c)
                channels.append(_keys(d, values[c], n, first, tangent))
            self.entries.append((names[i] if i < len(names) else "", d[e + 24], channels))

    def colours(self, frame):
        """{material name: {register: (r, g, b, a)}} at frame."""
        out = {}
        for name, register, channels in self.entries:
            colour = tuple(max(0, min(255, int(_value(keys, frame)))) for keys in channels)
            out.setdefault(name, {})[register] = colour
        return out


def _names(d, at):
    count = struct.unpack_from(">H", d, at)[0]
    out = []
    for i in range(count):
        offset = struct.unpack_from(">H", d, at + 6 + 4 * i)[0]
        out.append(d[at + offset:d.index(b"\0", at + offset)].decode("ascii", "replace"))
    return out


def _keys(d, table, count, first, tangent):
    if count == 1:
        return [(0.0, float(struct.unpack_from(">h", d, table + 2 * first)[0]), 0.0, 0.0)]
    size = 3 if tangent == 0 else 4
    raw = struct.unpack_from(f">{count * size}h", d, table + 2 * first)
    keys = []
    for k in range(count):
        v = raw[k * size:(k + 1) * size]
        keys.append((float(v[0]), float(v[1]), float(v[2]), float(v[2] if size == 3 else v[3])))
    return keys


def _value(keys, frame):  # J3DGetKeyFrameInterpolationS
    if len(keys) == 1 or frame <= keys[0][0]:
        return keys[0][1]
    if frame >= keys[-1][0]:
        return keys[-1][1]
    for k0, k1 in zip(keys, keys[1:]):
        if k0[0] <= frame < k1[0]:
            return P.hermite(frame, k0[0], k0[1], k0[3], k1[0], k1[1], k1[2])
    return keys[-1][1]


# ##############################################################################################
# # Drawing                                                                                    #
# ##############################################################################################
VERTEX = """#version 120
attribute vec3 position;
attribute vec4 colour;
attribute vec2 uv;
uniform vec4 view;
varying vec4 vColour;
varying vec2 vUv;
void main() {
  vColour = colour;
  vUv = uv;
  gl_Position = vec4(position.x * view.x + view.z, position.y * view.y + view.w, 0.0, 1.0);
}
"""


def fragment(material, textures):
    """builder_sky's TEV shader, ending as the material's blend asks: light added to the picture
    (destination factor one) is premultiplied by its source factor and covers nothing (no alpha:
    builder_layout.draw_lit adds it to the window); other materials keep their colour and alpha."""
    source = builder_sky.tev_shader(material, textures)
    mode, src, dst = material["blend"][:3]
    if mode == 1 and dst == 1:
        k = {1: "1.0", 4: "prev.a", 5: "(1.0 - prev.a)"}.get(src, "1.0")
        end = (f"  vec3 light = clamp(prev.rgb, 0.0, 1.0) * clamp({k}, 0.0, 1.0);\n"
               "  gl_FragColor = vec4(light, 0.0);")
    else:
        end = "  gl_FragColor = vec4(clamp(prev.rgb, 0.0, 1.0), clamp(prev.a, 0.0, 1.0));"
    return source.replace("  gl_FragColor = vec4(prev.rgb, 1.0);", end)


class Halo:
    """The PowerStarHalo model and its Appear animation, drawn on demand."""

    FRAME = 1.0 / 60.0

    def __init__(self, folder, name="powerstarhalo"):
        import builder_layout
        self.folder = Path(folder)
        self.data = json.loads((self.folder / f"{name}.json").read_text(encoding="utf-8"))
        anims = self.data.get("colour_anims", {})
        self.appear = ColourAnim((self.folder / anims["appear"]).read_bytes()) if "appear" in anims else None
        btk = next((file for member, file in self.data.get("anims", {}).items() if member.endswith(".btk")), None)
        self.texture_anim = builder_sky.TransformAnim((self.folder / btk).read_bytes()) if btk else None
        # Each shape where its joint puts it (no joint animation: once, here).
        from PySide6.QtGui import QVector3D
        worlds = builder_sky.joint_worlds(self.data.get("joints") or [])
        for shape in self.data["shapes"]:
            joint = shape.get("joint", 0)
            if not worlds or joint >= len(worlds):
                continue
            v = shape["vertices"]
            for at in range(0, len(v), builder_sky.FLOATS):
                moved = worlds[joint].map(QVector3D(v[at], v[at + 1], v[at + 2]))
                v[at], v[at + 1], v[at + 2] = moved.x(), moved.y(), moved.z()
        self.base = builder_layout.Renderer.get()
        self.programs = None
        self.vertex_buffers = None
        self.started = None
        # The model's size: the glow and the streak, flat in x and y.
        xs = [v for shape in self.data["shapes"] for v in shape["vertices"][0::builder_sky.FLOATS]]
        ys = [v for shape in self.data["shapes"] for v in shape["vertices"][1::builder_sky.FLOATS]]
        self.box = (min(xs), min(ys), max(xs), max(ys))

    def start(self, now):
        """Halo::appear: Appear from its first frame."""
        self.started = now

    def _ready(self):
        if self.base is None:
            return False
        if self.programs is None:
            from PySide6.QtOpenGL import QOpenGLShader, QOpenGLShaderProgram
            self.base.context.makeCurrent(self.base.surface)
            self.textures = []
            for info in self.data["textures"]:
                self.textures.append(self.base._texture(self.folder / info["file"], *info["wrap"]))
            self.programs = []
            for material in self.data["materials"]:
                program = QOpenGLShaderProgram()
                ok = program.addShaderFromSourceCode(QOpenGLShader.ShaderTypeBit.Vertex, VERTEX) and \
                    program.addShaderFromSourceCode(QOpenGLShader.ShaderTypeBit.Fragment,
                                                    fragment(material, self.textures))
                for i, attribute in enumerate(("position", "colour", "uv")):
                    program.bindAttributeLocation(attribute, i)
                if not ok or not program.link():
                    self.base = None
                    return False
                self.programs.append(program)
        return True

    def image(self, width, height, view, now):
        """A width x height premultiplied QImage of the halo; view is (left, bottom, right, top)
        in the model's units."""
        from PySide6.QtOpenGL import QOpenGLBuffer
        if not self._ready():
            return None
        frame = 0.0 if self.started is None else (now - self.started) / self.FRAME
        colours = self.appear.colours(min(frame, self.appear.frames)) if self.appear else {}
        base = self.base
        base.context.makeCurrent(base.surface)
        readback = base.target(("halo", id(self)), width, height)  # kept framebuffers (builder_layout.Readback)
        readback.bind()
        gl = base.gl
        gl.glViewport(0, 0, width, height)
        gl.glClearColor(0.0, 0.0, 0.0, 0.0)
        gl.glClear(0x4000)
        gl.glEnable(0x0BE2)
        gl.glDisable(0x0B71)
        gl.glDisable(0x0B44)
        left, bottom, right, top = view
        flip = -1.0 if readback.flipped else 1.0  # drawn upside down for the Readback's rows
        if self.vertex_buffers is None:  # the shapes never change: their buffers are made once
            self.vertex_buffers = []
            for shape in self.data["shapes"]:
                values = array.array("f", shape["vertices"])
                buffer = QOpenGLBuffer(QOpenGLBuffer.Type.VertexBuffer)
                buffer.create()
                buffer.bind()
                raw = values.tobytes()
                buffer.allocate(raw, len(raw))
                buffer.release()
                self.vertex_buffers.append((buffer, len(values) // builder_sky.FLOATS))
        for shape, (buffer, count) in zip(self.data["shapes"], self.vertex_buffers):
            material = self.data["materials"][shape["material"]]
            program = self.programs[shape["material"]]
            program.bind()
            program.setUniformValue("view", 2.0 / (right - left), flip * 2.0 / (top - bottom),
                                    -1.0 - 2.0 * left / (right - left), flip * (-1.0 - 2.0 * bottom / (top - bottom)))
            registers = list(material["tev_colours"])
            for register, colour in colours.get(material.get("name", ""), {}).items():
                if register < 4:
                    registers[register] = colour
            for name, colour in zip(("c0", "c1", "c2", "c3"), registers):
                program.setUniformValue(name, *(c / 255 for c in colour))
            for name, colour in zip(("k0", "k1", "k2", "k3"), material["konst"]):
                program.setUniformValue(name, *(c / 255 for c in colour))
            for i, m in enumerate(material["matrices"]):
                srt = self.texture_anim.texture_srt(material.get("name", ""), i, frame) if self.texture_anim else None
                if srt is not None:
                    import builder_assets
                    m = builder_assets.tex_matrix(*srt)
                m = m or [1, 0, 0, 0, 1, 0]
                program.setUniformValue(f"mtx{i}a", float(m[0]), float(m[1]), float(m[2]))
                program.setUniformValue(f"mtx{i}b", float(m[3]), float(m[4]), float(m[5]))
            for unit, texture in enumerate(self.textures):
                if texture:
                    texture[0].bind(unit)
                gl.glUniform1i(program.uniformLocation(f"tex{unit}"), unit)
            mode, src, dst = material["blend"][:3]
            if mode == 1 and dst == 1:
                # Added light (the shader's end, with no alpha).
                gl.glBlendEquationSeparate(0x8006, 0x8006)
                gl.glBlendFuncSeparate(1, 1, 1, 1)
            else:  # premultiplied, over what is there
                gl.glBlendEquationSeparate(0x8006, 0x8006)
                gl.glBlendFuncSeparate(0x0302, 0x0303, 1, 0x0303)
            buffer.bind()
            stride = builder_sky.FLOATS * 4
            for location, (offset, size) in enumerate(((0, 3), (12, 4), (28, 2))):
                program.enableAttributeArray(location)
                program.setAttributeBuffer(location, 0x1406, offset, size, stride)
            gl.glDrawArrays(0x0004, 0, count)
            for location in range(3):  # off again, for the next drawing in this context (builder_layout._draw)
                program.disableAttributeArray(location)
            buffer.release()
            program.release()
        gl.glBlendEquationSeparate(0x8006, 0x8006)
        image = readback.image()
        base.context.doneCurrent()
        return image

    def settled(self, now):
        """True once Appear has played to its end and nothing else moves (no texture animation)."""
        return self.started is not None and self.appear is not None and self.texture_anim is None and \
            (now - self.started) / self.FRAME > self.appear.frames
