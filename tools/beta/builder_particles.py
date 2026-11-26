"""The game's particle effects in the builder window, from the user's own disc.

The game's sparkles and glows are JParticle effects (ParticleData/Effect.arc: particles.jpc and
the names in particlenames.bcsv). export() copies the effects the window shows, block by block,
with their textures as PNG files. Emitter runs them as JParticle does (src/JSystem/JParticle and
the game's Game/System/Overwrite.cpp), frame by frame at 60 frames a second, and Renderer draws
their particles with OpenGL:

  emitting     JPADynamicsBlock::create and the volume shapes, JPABaseEmitter::init
  particles    JPABaseParticle::init_p, calc_p, init_c and calc_c: velocity, air, children
  animation    JPAExtraShape (scale and alpha over a particle's life, rotation), the colour
               tables of JPABaseShape, key blocks (JPAKeyBlock), texture coordinate animation
  drawing      billboards and directional quads (jpa_pos, jpa_dl), the first TEV stage of
               JPABaseShape::setGX (st_ca, st_aa), the second texture (JPAExTexShape) and the
               blend modes (st_bm, st_bf)

Left out, as none of the window's effects uses them: fields (FLD1), stripes, points and lines,
texture index animation and indirect textures.

Every name below is the decomp's, so each step can be checked against it.
"""

import array
import base64
import json
import math
import struct
from pathlib import Path

# The effects the window shows (their names in particlenames.bcsv, without the number).
EFFECTS = ("TitleLogoLightA", "TitleLogoLightB", "TitleLogoLightC", "TitleLogoLightD",
           "TitleLogoLightE", "TitleLogoLightF", "TitleLogoLightG",
           # The small shooting stars of the title screen's sky: the AutoEffect of CometNearOrbitSky
           # (autoeffectlist.bcsv) at its world_root, started with its joint animation, scale 10.
           "TitleShootingStar")


def _bcsv(data):
    """Rows of a JMap table (bcsv) as lists, strings decoded."""
    count, fields, offset, size = struct.unpack_from(">IIII", data, 0)
    columns = [struct.unpack_from(">IIHBB", data, 16 + 12 * i) for i in range(fields)]
    strings = offset + count * size
    rows = []
    for r in range(count):
        row = []
        for _hash, mask, at, shift, kind in columns:
            p = offset + r * size + at
            if kind == 6:
                s = strings + struct.unpack_from(">I", data, p)[0]
                row.append(data[s:data.index(b"\0", s)].decode("shift_jis", "replace"))
            elif kind == 0:
                row.append(struct.unpack_from(">i", data, p)[0])
            elif kind == 2:
                row.append(struct.unpack_from(">f", data, p)[0])
            elif kind == 3:
                row.append((struct.unpack_from(">I", data, p)[0] & mask) >> shift)
            elif kind == 4:
                row.append((struct.unpack_from(">H", data, p)[0] & mask) >> shift)
            elif kind == 5:
                row.append((data[p] & mask) >> shift)
            else:
                row.append(None)
        rows.append(row)
    return rows


# ##############################################################################################
# # Copying the effects from the disc                                                          #
# #                                                                                            #
# # particles.jpc ("JPAC2-10", JPAResourceLoader::load_jpc): resources, each a header (user    #
# # index, block count) and its blocks (tag, size, data), then the textures (TEX1 blocks: a   #
# # name at 0x0C and a texture header, ResTIMG, at 0x20).                                     #
# ##############################################################################################
def export(effect_arc, out):
    """Writes particles/particles.json and the effects' textures into out from Effect.arc's
    members ({name: bytes}); the manifest's entry."""
    import builder_assets
    jpc = effect_arc["particles.jpc"]
    if jpc[:8] != b"JPAC2-10":
        raise ValueError("unknown particle file")
    ids = {}
    for name, number in (row[:2] for row in _bcsv(effect_arc["particlenames.bcsv"])):
        ids[name] = number
    wanted = {}
    for effect in EFFECTS:
        wanted[effect] = sorted(number for name, number in ids.items()
                                if name.startswith(effect) and name[len(effect):].isdigit() and len(name) == len(effect) + 2)
    needed = {n for numbers in wanted.values() for n in numbers}
    resources, textures_used = {}, set()
    count, tex_count, tex_offset = struct.unpack_from(">HHI", jpc, 8)
    at = 0x10
    for _ in range(count):
        user, blocks = struct.unpack_from(">HH", jpc, at)
        at += 8
        found = []
        for _ in range(blocks):
            tag, size = jpc[at:at + 4].decode("ascii"), struct.unpack_from(">I", jpc, at + 4)[0]
            found.append((tag, jpc[at:at + size]))
            at += size
        if user in needed:
            resources[str(user)] = [[tag, base64.b64encode(body).decode("ascii")] for tag, body in found]
            for tag, body in found:
                if tag == "TDB1":
                    textures_used.update(struct.unpack_from(f">{(len(body) - 8) // 2}H", body, 8))
    folder = Path(out) / "particles"
    folder.mkdir(parents=True, exist_ok=True)
    textures = {}
    at = tex_offset
    for index in range(tex_count):
        size = struct.unpack_from(">I", jpc, at + 4)[0]
        if index in textures_used:
            h = at + 0x20
            fmt, width, height, wrap_s, wrap_t = jpc[h], *struct.unpack_from(">HH", jpc, h + 2), jpc[h + 6], jpc[h + 7]
            palette = None
            if fmt in (8, 9, 10):
                pfmt, pcount, poffset = jpc[h + 9], *struct.unpack_from(">HI", jpc, h + 0x0A)
                palette = builder_assets.palette_colours(pfmt, jpc[h + poffset:], pcount)
            image = builder_assets.decode_texture(fmt, jpc[h + struct.unpack_from(">I", jpc, h + 0x1C)[0]:], width, height,
                                                  palette)
            file = f"tex{index}.png"
            image.save(folder / file)
            textures[str(index)] = {"file": file, "wrap": [wrap_s, wrap_t]}
        at += size
    (folder / "particles.json").write_text(json.dumps({"effects": wanted, "resources": resources, "textures": textures}),
                                           encoding="utf-8")
    return "particles/particles.json"


# ##############################################################################################
# # Numbers as the game has them                                                               #
# ##############################################################################################
def s16(v):
    v = int(v) & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def sin_s(a):  # JMASSin: an angle of 65536 to the turn
    return math.sin(s16(a) * math.pi / 32768.0)


def cos_s(a):
    return math.cos(s16(a) * math.pi / 32768.0)


class Random:
    """JPARandom: the game's own sequence, so the effects come out as they do in the game."""

    def __init__(self, seed=0):
        self.seed = seed & 0xFFFFFFFF

    def u(self):
        self.seed = (self.seed * 0x19660D + 0x3C6EF35F) & 0xFFFFFFFF
        return self.seed

    def f(self):
        return struct.unpack(">f", struct.pack(">I", (self.u() >> 9) | 0x3F800000))[0] - 1.0

    def zp(self):
        return self.f() * 2.0 - 1.0

    def zh(self):
        return self.f() - 0.5

    def ss(self):
        return s16(self.u() >> 16)


def mat_identity():
    return [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0]]


def mat_concat(a, b):  # PSMTXConcat: a * b
    out = [[0.0] * 4 for _ in range(3)]
    for i in range(3):
        for j in range(4):
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (a[i][3] if j == 3 else 0.0)
    return out


def mat_vec(m, v):  # PSMTXMultVec
    return [m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + m[i][3] for i in range(3)]


def mat_vec_sr(m, v):  # PSMTXMultVecSR
    return [m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] for i in range(3)]


def mat_scale(x, y, z):
    return [[x, 0.0, 0.0, 0.0], [0.0, y, 0.0, 0.0], [0.0, 0.0, z, 0.0]]


def xyz_rotate(x, y, z):  # JPAGetXYZRotateMtx
    cx, sx, cy, sy, cz, sz = cos_s(x), sin_s(x), cos_s(y), sin_s(y), cos_s(z), sin_s(z)
    return [[cy * cz, sx * cz * sy - cx * sz, sx * sz + cx * cz * sy, 0.0],
            [cy * sz, cx * cz + sx * sz * sy, cx * sz * sy - sx * cz, 0.0],
            [-sy, sx * cy, cx * cy, 0.0]]


def yz_rotate(y, z):  # JPAGetYZRotateMtx
    cy, cz, sy, sz = cos_s(y), cos_s(z), sin_s(y), sin_s(z)
    return [[cy * cz, -sz, sy * cz, 0.0], [cy * sz, cz, sy * sz, 0.0], [-sy, 0.0, cy, 0.0]]


def dir_matrix(d):  # JPAGetDirMtx
    ax, ay = d[1], -d[0]
    length = math.hypot(ax, ay)
    if length <= 1.1920929e-07:
        ax = ay = 0.0
    else:
        ax, ay = ax / length, ay / length
    xx, yy, xl, yl = ax * ax, ay * ay, ax * length, ay * length
    k = (1.0 - d[2]) * (ax * ay)
    return [[xx + d[2] * (1.0 - xx), k, -yl, 0.0], [k, yy + d[2] * (1.0 - yy), xl, 0.0], [yl, -xl, d[2], 0.0]]


def set_length(v, length):  # TVec3::setLength
    sq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2]
    if sq <= 3.8e-6:
        return [0.0, 0.0, 0.0]
    k = length / math.sqrt(sq)
    return [v[0] * k, v[1] * k, v[2] * k]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def normalized(v):
    length = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return [v[0] / length, v[1] / length, v[2] / length] if length > 1e-6 else None


def hermite(t, t0, p0, m0, t1, p1, m1):  # JMAHermiteInterpolation, operation by operation
    a = t - t0
    b = a / (t1 - t0)
    bb = b * b
    c = bb - b
    e = (b + b) * c - bb
    e = e * (p0 - p1) + p0
    f = m0 * c + m0
    f = m1 * c + f
    f = b * m0 - f
    return -(a * f - e)


def key_value(frame, keys):  # JPACalcKeyAnmValue: keys of (frame, value, in, out)
    if frame < keys[0][0]:
        return keys[0][1]
    if keys[-1][0] <= frame:
        return keys[-1][1]
    lo, n = 0, len(keys)
    while n > 1:
        half = n // 2
        if frame >= keys[lo + half][0]:
            lo += half
            n -= half
        else:
            n = half
    k0, k1 = keys[lo], keys[lo + 1]
    return hermite(frame, k0[0], k0[1], k0[3], k1[0], k1[1], k1[2])


# ##############################################################################################
# # One resource (JPAResource) and its blocks                                                  #
# ##############################################################################################
class Resource:
    def __init__(self, blocks):
        self.fields, self.keys = [], []
        self.esp = self.csp = self.etx = None
        self.tdb = []
        for tag, d in blocks:
            if tag == "BEM1":
                self._dynamics(d)
            elif tag == "BSP1":
                self._base_shape(d)
            elif tag == "ESP1":
                self._extra_shape(d)
            elif tag == "SSP1":
                self._child_shape(d)
            elif tag == "ETX1":
                flags = struct.unpack_from(">I", d, 8)[0]
                self.etx = {"indirect": bool(flags & 1), "second": bool(flags & 0x100), "second_index": d[0x26]}
            elif tag == "TDB1":
                self.tdb = list(struct.unpack_from(f">{(len(d) - 8) // 2}H", d, 8))
            elif tag == "KFA1":
                count = d[9]
                values = struct.unpack_from(f">{count * 4}f", d, 0x0C)
                self.keys.append((d[8], bool(d[0x0B]), [values[4 * i:4 * i + 4] for i in range(count)]))
            elif tag == "FLD1":
                self.fields.append(d)  # not run (see the top of this file)

    def _dynamics(self, d):  # JPADynamicsBlockData
        self.dyn_flags = struct.unpack_from(">I", d, 8)[0]
        f = struct.unpack_from(">22f", d, 0x10)
        self.emitter_scale, self.emitter_trans, self.emitter_dir = f[0:3], f[3:6], f[6:9]
        (self.vel_omni, self.vel_axis, self.vel_rndm, self.vel_dir, self.spread, self.vel_ratio, self.rate,
         self.rate_rndm, self.life_rndm, self.volume_sweep, self.volume_min_rad, self.air_resist,
         self.moment_rndm) = f[9:22]
        self.emitter_rot = struct.unpack_from(">3h", d, 0x68)
        self.max_frame, self.start_frame, self.life = struct.unpack_from(">3h", d, 0x6E)
        self.volume_size, self.div_number = struct.unpack_from(">HH", d, 0x74)
        self.rate_step = d[0x78]
        self.volume_type = (self.dyn_flags >> 8) & 7

    def _base_shape(self, d):  # JPABaseShapeData
        self.bsp_flags = flags = struct.unpack_from(">I", d, 8)[0]
        prm_offset, env_offset = struct.unpack_from(">hh", d, 0x0C)
        self.base_size = struct.unpack_from(">2f", d, 0x10)
        self.blend = struct.unpack_from(">H", d, 0x18)[0]
        self.tex_flags, self.tex_anm_count, self.tex_index = d[0x1E], d[0x1F], d[0x20]
        self.clr_flags, prm_keys, env_keys = d[0x21], d[0x22], d[0x23]
        self.clr_frame_max = struct.unpack_from(">h", d, 0x24)[0]
        self.prm, self.env = tuple(d[0x26:0x2A]), tuple(d[0x2A:0x2E])
        self.anm_rndm, self.clr_loop_mask, self.tex_loop_mask = d[0x2E], d[0x2F], d[0x30]
        self.shape_type = flags & 0xF
        self.dir_type = (flags >> 4) & 7
        self.rot_type = (flags >> 7) & 7
        self.plane_type = (flags >> 10) & 1
        self.colour_mode = (flags >> 15) & 7
        self.alpha_mode = (flags >> 18) & 1
        self.tiling = ((flags >> 25) & 1, (flags >> 26) & 1)
        self.global_clr_anm = bool(flags & 0x1000)
        self.prj_tex = bool(flags & 0x100000)
        self.draw_fwd_ahead = bool(flags & 0x200000)
        self.draw_parent_ahead = bool(flags & 0x400000)
        self.no_draw_parent = bool(flags & 0x08000000)
        self.no_draw_child = bool(flags & 0x10000000)
        self.tex_crd_anm = struct.unpack_from(">10f", d, 0x34) if flags & 0x01000000 else None
        self.prm_table = _colour_table(d, prm_offset, prm_keys, self.clr_frame_max) if self.clr_flags & 2 else None
        self.env_table = _colour_table(d, env_offset, env_keys, self.clr_frame_max) if self.clr_flags & 8 else None
        self.clr_anm_type = (self.clr_flags >> 4) & 7

    def _extra_shape(self, d):  # JPAExtraShapeData and JPAExtraShape::init
        flags = struct.unpack_from(">I", d, 8)[0]
        f = struct.unpack_from(">7f", d, 0x0C)
        self.scale_in_timing, self.scale_out_timing, in_x, out_x, in_y, out_y, self.scale_rndm = f
        self.scale_in_value = (in_x, in_y)
        self.scale_cycle = struct.unpack_from(">2h", d, 0x28)
        a = struct.unpack_from(">8f", d, 0x2C)
        (self.alpha_in_timing, self.alpha_out_timing, self.alpha_in_value, self.alpha_base, self.alpha_out_value,
         self.alpha_freq, self.alpha_freq_rndm, self.alpha_amp) = a
        self.rot_angle, self.rot_angle_rndm, self.rot_speed, self.rot_speed_rndm, self.rot_direction = \
            struct.unpack_from(">5f", d, 0x4C)
        self.scale_anm = bool(flags & 1)
        self.scale_xy_diff = bool(flags & 2)
        self.scale_anm_type = ((flags >> 8) & 3, (flags >> 10) & 3)
        self.scale_center = ((flags >> 12) & 3, (flags >> 14) & 3)
        self.alpha_anm = bool(flags & 0x10000)
        self.alpha_flick = bool(flags & 0x20000)
        self.rotate_anm = bool(flags & 0x1000000)
        t_in, t_out = self.alpha_in_timing, self.alpha_out_timing
        self.alpha_inc = (self.alpha_base - self.alpha_in_value) / t_in if t_in != 0.0 else 1.0
        self.alpha_dec = (self.alpha_out_value - self.alpha_base) / (1.0 - t_out) if t_out != 1.0 else 1.0
        if self.scale_in_timing != 0.0:
            self.scale_inc = ((1.0 - in_x) / self.scale_in_timing, (1.0 - in_y) / self.scale_in_timing)
        else:
            self.scale_inc = (1.0, 1.0)
        if self.scale_out_timing != 1.0:
            self.scale_dec = ((out_x - 1.0) / (1.0 - self.scale_out_timing), (out_y - 1.0) / (1.0 - self.scale_out_timing))
        else:
            self.scale_dec = (1.0, 1.0)
        self.esp = True

    def _child_shape(self, d):  # JPAChildShapeData
        flags = struct.unpack_from(">I", d, 8)[0]
        (self.c_pos_rndm, self.c_base_vel, self.c_base_vel_rndm, self.c_vel_inh, self.c_gravity, sx, sy,
         self.c_scale_inh, self.c_alpha_inh, self.c_colour_inh) = struct.unpack_from(">10f", d, 0x0C)
        self.c_scale = (sx, sy)
        self.c_prm, self.c_env = tuple(d[0x34:0x38]), tuple(d[0x38:0x3C])
        self.c_timing = struct.unpack_from(">f", d, 0x3C)[0]
        self.c_life, self.c_rate = struct.unpack_from(">hh", d, 0x40)
        self.c_step, self.c_tex = d[0x44], d[0x45]
        self.c_rot_speed = struct.unpack_from(">h", d, 0x46)[0]
        self.c_type = flags & 0xF
        self.c_dir_type = (flags >> 4) & 7
        self.c_rot_type = (flags >> 7) & 7
        self.c_plane_type = (flags >> 10) & 1
        self.c_scale_inherited = bool(flags & 0x10000)
        self.c_alpha_inherited = bool(flags & 0x20000)
        self.c_colour_inherited = bool(flags & 0x40000)
        self.c_field_affected = bool(flags & 0x200000)
        self.c_scale_out = bool(flags & 0x400000)
        self.c_alpha_out = bool(flags & 0x800000)
        self.c_rotate = bool(flags & 0x1000000)
        self.csp = True


def _colour_table(d, offset, count, frame_max):  # makeColorTable
    keys = [(struct.unpack_from(">h", d, offset + 6 * i)[0], tuple(d[offset + 6 * i + 2:offset + 6 * i + 6]))
            for i in range(count)]
    table = []
    r, g, b, a = (float(c) for c in keys[0][1])
    step = (0.0, 0.0, 0.0, 0.0)
    j = 0
    for i in range(frame_max + 1):
        if j < len(keys) and i == keys[j][0]:
            table.append(keys[j][1])
            r, g, b, a = (float(c) for c in keys[j][1])
            j += 1
            if j < count:
                base = 1.0 / (keys[j][0] - keys[j - 1][0])
                step = tuple(base * (n - c) for n, c in zip(keys[j][1], (r, g, b, a)))
            else:
                step = (0.0, 0.0, 0.0, 0.0)
        else:
            r, g, b, a = r + step[0], g + step[1], b + step[2], a + step[3]
            table.append((int(r) & 255, int(g) & 255, int(b) & 255, int(a) & 255))
    return table


# ##############################################################################################
# # Particles and emitters                                                                     #
# ##############################################################################################
class Particle:
    __slots__ = ("age", "life", "time", "status", "local", "offset", "position", "vel1", "vel0", "vel2", "velocity",
                 "moment", "drag", "base_axis", "prm", "env", "anm_random", "scale_x", "scale_y", "scale_out",
                 "alpha_anm", "alpha_wave", "rot_angle", "rot_speed")


class Emitter:
    """JPABaseEmitter with its resource's calc and draw (JPAResource::calc, drawP, drawC)."""

    def __init__(self, res, seed, position=(0.0, 0.0, 0.0), scale=1.0):
        self.res = res
        self.local_scale = list(res.emitter_scale)
        self.local_trans = list(res.emitter_trans)
        d = list(res.emitter_dir)
        self.local_dir = normalized(d) or d
        self.local_rot = res.emitter_rot
        self.max_frame, self.life = res.max_frame, res.life
        self.volume_size, self.rate, self.rate_step = res.volume_size, res.rate, res.rate_step
        self.volume_sweep, self.volume_min_rad = res.volume_sweep, res.volume_min_rad
        self.away_center, self.away_axis = res.vel_omni, res.vel_axis
        self.dir_speed, self.spread, self.rndm_speed = res.vel_dir, res.spread, res.vel_rndm
        self.air_resist = res.air_resist
        self.rnd = Random(seed)
        self.global_rot = mat_identity()
        self.global_scale = [scale, scale, scale]
        self.global_trans = list(position)
        self.global_pscale = (scale, scale)
        self.prm, self.env = res.prm, res.env
        self.scale_out = 1.0
        self.emit_count = 0.0
        self.status = 0x30  # first emit, rate-step emit
        self.tick = self.wait = self.rate_step_timer = 0
        self.particles, self.children = [], []
        self.stopped = False  # no more particles (the effect is ending)

    # Random numbers (JPABaseEmitter::get_r_*).
    def r_f(self):
        return self.rnd.f()

    def r_zp(self):
        return self.rnd.zp()

    def r_zh(self):
        return self.rnd.zh()

    def r_ss(self):
        return self.rnd.ss()

    def flag(self, bit):  # JPABaseEmitter::checkFlag: the dynamics block's flags
        return self.res.dyn_flags & bit

    def alive(self):
        return bool(self.particles or self.children) or not self.stopped

    # JPAResource::calc.
    def calc(self):
        res = self.res
        if self.wait < res.start_frame:  # processTillStartFrame
            self.wait += 1
            return
        if self.max_frame > 0 and self.tick >= self.max_frame:  # processTermination
            self.stopped = True
        for ident, loop, keys in res.keys:  # calcKey
            frame = float(self.tick)
            if loop:
                period = int(keys[-1][0]) + 1
                frame -= (int(frame) // period) * period
            value = key_value(frame, keys)
            if ident == 0:
                self.rate = value
            elif ident == 1:
                self.volume_size = value
            elif ident == 3:
                self.volume_min_rad = value
            elif ident == 4:
                self.life = value
            elif ident == 6:
                self.away_center = value
            elif ident == 7:
                self.away_axis = value
            elif ident == 8:
                self.dir_speed = value
            elif ident == 9:
                self.spread = value
            elif ident == 10:
                self.scale_out = value
        self._work_data()
        if res.global_clr_anm:
            self.prm, self.env = self._colour_at(self.tick, 0, self.prm, self.env)
        if not self.stopped:
            self._create()
        for p in list(self.particles):
            if self._calc_p(p):
                self.particles.remove(p)
        for p in list(self.children):
            if self._calc_c(p):
                self.children.remove(p)
        self.tick += 1

    def _work_data(self):  # JPAResource::calcWorkData_c
        local_scale = mat_scale(*self.local_scale)
        local_rot = xyz_rotate(self.local_rot[0] * 182, self.local_rot[1] * 182, self.local_rot[2] * 182)
        glob = mat_concat(self.global_rot, mat_scale(*self.global_scale))
        glob[0][3], glob[1][3], glob[2][3] = self.global_trans
        self.w_rot = mat_concat(self.global_rot, local_rot)
        self.w_sr = mat_concat(self.w_rot, local_scale)
        self.w_global_scale = [a * b for a, b in zip(self.global_scale, self.local_scale)]
        self.w_dir = dir_matrix(self.local_dir)
        self.w_public_scale = list(self.global_scale)
        self.w_global_pos = mat_vec(glob, self.local_trans)

    def _create(self):  # JPADynamicsBlock::create
        res = self.res
        if self.status & 0x20:
            count = self.rate * (res.rate_rndm * self.r_zp() + 1.0)
            self.emit_count += count
            emit = int(self.emit_count)
            self.emit_count -= emit
            if self.status & 0x10 and 0.0 < count < 1.0:
                emit = 1
            for _ in range(emit):
                self._volume()
                self.particles.insert(0, self._init_p())  # push_front
        self.rate_step_timer += 1
        if self.rate_step_timer >= self.rate_step + 1:
            self.rate_step_timer -= self.rate_step + 1
            self.status |= 0x20
        else:
            self.status &= ~0x20
        self.status &= ~0x10

    def _volume(self):
        """The volume shapes (JPAVolumePoint, Cube, Sphere, Cylinder, Circle, Line, Torus) without
        their fixed-interval and fixed-density options."""
        kind, size, rnd = self.res.volume_type, self.volume_size, self.rnd
        gs = self.w_global_scale
        if kind == 4:  # point
            z, y, x = rnd.f() - 0.5, rnd.f() - 0.5, rnd.f() - 0.5
            self.v_pos, self.v_omni, self.v_axis = [0.0, 0.0, 0.0], [x, y, z], [x, 0.0, z]
            return
        if kind == 0:  # cube
            z, y, x = rnd.f() - 0.5, rnd.f() - 0.5, rnd.f() - 0.5
            pos = [size * x, size * y, size * z]
        elif kind == 1:  # sphere
            phi = s16(self.r_ss() >> 1)
            theta = s16(self.volume_sweep * self.r_ss())
            r = size * (self.volume_min_rad + self.r_f() * (1.0 - self.volume_min_rad))
            pos = [r * cos_s(phi) * sin_s(theta), -r * sin_s(phi), r * cos_s(phi) * cos_s(theta)]
        elif kind == 2:  # cylinder
            theta = s16(self.volume_sweep * self.r_ss())
            r = size * (self.volume_min_rad + self.r_f() * (1.0 - self.volume_min_rad))
            pos = [r * sin_s(theta), size * self.r_zp(), r * cos_s(theta)]
        elif kind == 5:  # circle
            theta = s16(self.volume_sweep * self.r_ss())
            r = size * (self.volume_min_rad + self.r_f() * (1.0 - self.volume_min_rad))
            pos = [r * sin_s(theta), 0.0, r * cos_s(theta)]
        elif kind == 6:  # line
            pos = [0.0, 0.0, size * (rnd.f() - 0.5)]
            self.v_pos, self.v_omni, self.v_axis = pos, [0.0, 0.0, pos[2] * gs[2]], [0.0, 0.0, pos[2]]
            return
        else:  # torus
            theta = s16(self.volume_sweep * self.r_ss())
            phi = self.r_ss()
            r = size * self.volume_min_rad
            axis = [r * sin_s(theta) * cos_s(phi), r * sin_s(phi), r * cos_s(theta) * cos_s(phi)]
            pos = [axis[0] + size * sin_s(theta), axis[1], axis[2] + size * cos_s(theta)]
            self.v_pos, self.v_omni, self.v_axis = pos, [a * b for a, b in zip(pos, gs)], axis
            return
        self.v_pos, self.v_omni, self.v_axis = pos, [a * b for a, b in zip(pos, gs)], [pos[0], 0.0, pos[2]]

    def _init_p(self):  # JPABaseParticle::init_p
        res = self.res
        p = Particle()
        p.age = -1
        p.life = int((1.0 - res.life_rndm * self.r_f()) * self.life)
        p.time = 0.0
        p.status = 0x20 if self.flag(8) else 0
        p.local = mat_vec_sr(self.w_sr, self.v_pos)
        p.offset = list(self.w_global_pos)
        p.position = [p.offset[i] + p.local[i] * self.w_public_scale[i] for i in range(3)]
        omni = set_length(self.v_omni, self.away_center) if self.away_center else [0.0] * 3
        axis = set_length(self.v_axis, self.away_axis) if self.away_axis else [0.0] * 3
        if self.dir_speed:
            m = mat_concat(self.w_dir, yz_rotate(s16(self.r_zp() * 0x8000 * self.spread), self.r_ss()))
            vdir = [self.dir_speed * m[0][2], self.dir_speed * m[1][2], self.dir_speed * m[2][2]]
        else:
            vdir = [0.0] * 3
        if self.rndm_speed:
            z, y, x = self.r_zh(), self.r_zh(), self.r_zh()
            vrnd = [self.rndm_speed * x, self.rndm_speed * y, self.rndm_speed * z]
        else:
            vrnd = [0.0] * 3
        ratio = self.r_zp() * res.vel_ratio + 1.0
        v = [ratio * (omni[i] + axis[i] + vdir[i] + vrnd[i]) for i in range(3)]
        if self.flag(4):
            v = [v[i] * self.local_scale[i] for i in range(3)]
        p.vel1 = mat_vec_sr(self.w_rot, v)
        p.vel0 = [0.0, 0.0, 0.0]
        p.vel2 = [0.0, 0.0, 0.0]
        p.velocity = [0.0, 0.0, 0.0]
        p.moment = 1.0 - res.moment_rndm * self.r_f()
        p.drag = 1.0
        p.base_axis = [self.w_rot[0][1], self.w_rot[1][1], self.w_rot[2][1]]
        p.prm, p.env = self.prm, self.env
        p.anm_random = int(self.r_f() * res.anm_rndm) & 255
        if res.esp and res.scale_anm:
            p.scale_x = p.scale_y = p.scale_out = self.scale_out * (self.r_zp() * res.scale_rndm + 1.0)
        else:
            p.scale_x = p.scale_y = p.scale_out = self.scale_out
        p.alpha_anm = 255
        p.alpha_wave = self.r_zp() * res.alpha_freq_rndm + 1.0 if res.esp and res.alpha_flick else 1.0
        if res.esp and res.rotate_anm:
            p.rot_angle = s16(res.rot_angle + res.rot_angle_rndm * self.r_zh())
            speed = s16(res.rot_speed * (res.rot_speed_rndm * self.r_zp() + 1.0))
            p.rot_speed = speed if self.r_zp() < res.rot_direction else s16(-speed)
        else:
            p.rot_angle = p.rot_speed = 0
        return p

    def _init_c(self, parent):  # JPABaseParticle::init_c
        res = self.res
        p = Particle()
        p.age = -1
        p.life = res.c_life
        p.time = 0.0
        p.status = 4
        p.local = list(parent.local)
        if res.c_pos_rndm != 0.0:
            rnd = set_length([self.r_zh(), self.r_zh(), self.r_zh()], res.c_pos_rndm * self.r_f())
            p.local = [p.local[i] + rnd[i] for i in range(3)]
        if self.flag(0x10):
            p.status |= 0x20
        p.offset = list(parent.offset)
        speed = res.c_base_vel * (res.c_base_vel_rndm * self.r_zp() + 1.0)
        base = set_length([self.r_zp(), self.r_zp(), self.r_zp()], speed)
        p.vel1 = [res.c_vel_inh * parent.vel1[i] + base[i] for i in range(3)]
        p.vel0 = [res.c_vel_inh * parent.vel2[i] for i in range(3)]
        p.moment = parent.moment
        if res.c_field_affected:
            p.drag = parent.drag
        else:
            p.status |= 0x40
            p.drag = 1.0
        p.vel2 = list(p.vel0)
        k = p.moment * p.drag
        p.velocity = [k * (p.vel1[i] + p.vel2[i]) for i in range(3)]
        p.base_axis = list(parent.base_axis)
        if res.c_scale_inherited:
            p.scale_x = p.scale_out = parent.scale_x * res.c_scale_inh
            p.scale_y = p.alpha_wave = parent.scale_y * res.c_scale_inh
        else:
            p.scale_x = p.scale_out = p.scale_y = p.alpha_wave = 1.0
        if res.c_colour_inherited:
            prm = tuple(int(c * res.c_colour_inh) & 255 for c in parent.prm[:3]) + (parent.prm[3],)
            env = tuple(int(c * res.c_colour_inh) & 255 for c in parent.env[:3]) + (parent.env[3],)
        else:
            prm, env = res.c_prm, res.c_env
        p.alpha_anm = 255
        if res.c_alpha_inherited:
            alpha = int(_colour_multi(parent.prm[3], parent.alpha_anm) * res.c_alpha_inh) & 255
        else:
            alpha = res.c_prm[3]
        p.prm, p.env = prm[:3] + (alpha,), env
        p.rot_angle = parent.rot_angle
        p.rot_speed = res.c_rot_speed if res.c_rotate else 0
        p.anm_random = 0
        return p

    def _calc_p(self, p):  # JPABaseParticle::calc_p; True when it dies
        p.age += 1
        if p.age >= p.life:
            return True
        p.time = p.age / p.life
        if p.status & 0x20:
            p.offset = list(self.w_global_pos)
        p.vel2 = list(p.vel0)
        p.vel1 = [v * self.air_resist for v in p.vel1]
        k = p.moment * p.drag
        p.velocity = [k * (p.vel1[i] + p.vel2[i]) for i in range(3)]
        self._animate(p)
        p.rot_angle = s16((p.rot_angle & 0xFFFF) + p.rot_speed)
        res = self.res
        if res.csp:  # canCreateChild
            t = p.age - int((p.life - 1) * res.c_timing)
            if t >= 0 and t % (res.c_step + 1) == 0:
                for _ in range(res.c_rate):
                    self.children.insert(0, self._init_c(p))
        p.local = [p.local[i] + p.velocity[i] for i in range(3)]
        p.position = [p.offset[i] + p.local[i] * self.w_public_scale[i] for i in range(3)]
        return False

    def _calc_c(self, p):  # JPABaseParticle::calc_c
        res = self.res
        p.age += 1
        if p.age >= p.life:
            return True
        p.time = p.age / p.life
        if p.age != 0:
            if p.status & 0x20:
                p.offset = list(self.w_global_pos)
            p.vel1[1] -= res.c_gravity
            p.vel2 = list(p.vel0)
            p.vel1 = [v * self.air_resist for v in p.vel1]
            k = p.moment * p.drag
            p.velocity = [k * (p.vel1[i] + p.vel2[i]) for i in range(3)]
        if res.c_alpha_out:  # JPACalcChildAlphaOut
            p.alpha_anm = _to_u8((1.0 - p.time) * 255.0)
        if res.c_scale_out:  # JPACalcChildScaleOut
            p.scale_x = p.scale_out * (1.0 - p.time)
            p.scale_y = p.alpha_wave * (1.0 - p.time)
        p.rot_angle = s16((p.rot_angle & 0xFFFF) + p.rot_speed)
        p.local = [p.local[i] + p.velocity[i] for i in range(3)]
        p.position = [p.offset[i] + p.local[i] * self.w_public_scale[i] for i in range(3)]
        return False

    def _animate(self, p):
        """A particle's calc functions in the order JPAResource runs them (its list backwards):
        scale, colour, alpha."""
        res = self.res
        if res.esp and res.scale_anm and res.shape_type is not None:
            cycle_x, cycle_y = res.scale_cycle
            scale_anm = _scale_anm(res.scale_anm_type[0], p, cycle_x)
            p.scale_x = _scale_value(res, 0, scale_anm, p.scale_out)
            if res.shape_type != 0:
                if res.scale_xy_diff:
                    if res.scale_anm_type[0] or res.scale_anm_type[1]:
                        scale_anm = _scale_anm(res.scale_anm_type[1], p, cycle_y)
                    p.scale_y = _scale_value(res, 1, scale_anm, p.scale_out)
                else:
                    p.scale_y = p.scale_x
        if res.global_clr_anm:
            p.prm, p.env = self.prm, self.env  # JPACalcColorCopy
        elif res.prm_table or res.env_table:
            p.prm, p.env = self._colour_at(p.age, p.anm_random, p.prm, p.env, p)
        if res.esp and res.alpha_anm and res.shape_type not in (5, 6):
            if p.time < res.alpha_in_timing:
                alpha = res.alpha_in_value + res.alpha_inc * p.time
            elif p.time > res.alpha_out_timing:
                alpha = (p.time - res.alpha_out_timing) * res.alpha_dec + res.alpha_base
            else:
                alpha = res.alpha_base
            if res.alpha_flick:  # JPACalcAlphaFlickAnm
                theta = int(p.alpha_wave * p.age * 16384.0 * (1.0 - res.alpha_freq))
                alpha *= (1.0 + res.alpha_amp * (sin_s(theta) - 1.0) * 0.5)
            p.alpha_anm = _to_u8(alpha * 255.0)

    def _colour_at(self, age, random, prm, env, particle=None):
        """JPACalcClrIdx* then JPACalcPrm and JPACalcEnv."""
        res = self.res
        top = res.clr_frame_max
        kind = res.clr_anm_type
        offset = (res.clr_loop_mask & random) if particle is not None else 0
        if kind == 0:
            frame = min(age, top)
        elif kind == 1:
            frame = (age + offset) % (top + 1)
        elif kind == 2:
            tick = age + offset
            rem = tick % top
            frame = rem + ((tick // top) & 1) * (top - rem * 2)
        elif kind == 3:
            frame = (int(particle.time * (top + 1)) + offset) % (top + 1) if particle is not None else 0
        else:
            frame = offset % (top + 1) if particle is not None else 0
        if res.prm_table:
            prm = res.prm_table[frame]
        if res.env_table:
            env = res.env_table[frame]
        return prm, env

    # Drawing (JPAResource::drawP and drawC): quads for Renderer.
    def quads(self, view=None):
        """[(texture slot ('base', 'child'), [(x, y, u, v, u2, v2)] * 4, prm, env)] in the order the
        game draws them. view (left, bottom, right, top) is the screen in world units, for the
        projected texture coordinates."""
        res = self.res
        out = []
        if res.draw_parent_ahead and res.csp:
            out += self._quads_child()
        if not res.no_draw_parent:
            out += self._quads_parent(view)
        if not res.draw_parent_ahead and res.csp:
            out += self._quads_child()
        return out

    def quads_projected(self, to_view, project):
        """quads() for a 3D camera, in the order the game draws them. to_view maps a point of the
        effect's world into the camera's space (looking down -z), project maps a camera point to
        the screen (x and y from -1 to 1), or None behind the camera. Billboards face the camera
        (JPADrawBillboard with the camera's matrix); directional quads keep their world frame."""
        res = self.res
        out = []
        if res.draw_parent_ahead and res.csp:
            out += self._projected(True, to_view, project)
        if not res.no_draw_parent:
            out += self._projected(False, to_view, project)
        if not res.draw_parent_ahead and res.csp:
            out += self._projected(True, to_view, project)
        return out

    def _projected(self, child, to_view, project):
        res = self.res
        if child:
            if res.no_draw_child:
                return []
            base = res.base_size if res.c_scale_inherited else res.c_scale
            kind, dir_type, rot_type, plane_type = res.c_type, res.c_dir_type, res.c_rot_type, res.c_plane_type
            rotating = (res.esp and res.rotate_anm) or res.c_rotate
            corners, crd, particles = _corners(1, 1), _tex_coords(0, 0), self.children
        else:
            base = res.base_size
            kind, dir_type, rot_type, plane_type = res.shape_type, res.dir_type, res.rot_type, res.plane_type
            rotating = res.esp and res.rotate_anm
            centre = res.scale_center if res.esp else (1, 1)
            corners, crd, particles = _corners(centre[0], centre[1]), _tex_coords(*res.tiling), self.particles
        size = (self.global_pscale[0] * base[0], self.global_pscale[1] * base[1])
        out = []
        for p in (reversed(particles) if res.draw_fwd_ahead else particles):
            # Off the screen (its centre well outside it, or behind the camera): not drawn.
            centre = project(to_view(p.position))
            if centre is None or abs(centre[0]) > 1.25 or abs(centre[1]) > 1.25:
                continue
            if kind == 2:  # billboard: the quad in the camera's plane, at the particle's place
                cx_, cy_, cz_ = to_view(p.position)
                sx, sy = size[0] * p.scale_x, size[1] * p.scale_y
                s, c = (sin_s(p.rot_angle), cos_s(p.rot_angle)) if rotating else (0.0, 1.0)
                points = [(cx_ + c * sx * x - s * sy * y, cy_ + s * sx * x + c * sy * y, cz_) for x, y in corners]
            else:
                frame = self._frame(p, kind, dir_type, rot_type, plane_type, rotating, size)
                if frame is None:
                    continue
                points = [to_view(_transform(frame, x, y)) for x, y in corners]
            screen = [project(q) for q in points]
            if any(q is None for q in screen):
                continue
            verts = []
            for (x, y), (u, v) in zip(screen, crd):
                uu, vv = (u, v) if child else self._parent_uv(p, u, v, (x, y, 0.0), None)
                verts.append((x, y, uu, vv, u, v))
            if child:
                if res.c_alpha_out or res.c_alpha_inherited or res.c_colour_inherited:  # JPARegistPrmAlphaEnv
                    prm, env = p.prm[:3] + (_colour_multi(p.prm[3], p.alpha_anm),), p.env
                else:  # JPARegistChildPrmEnv
                    prm, env = res.c_prm, res.c_env
                out.append(("child", verts, prm, env))
            else:
                prm, env = self._parent_colours(p)
                out.append(("base", verts, prm, env))
        return out

    def _quads_parent(self, view):
        res = self.res
        size = (self.global_pscale[0] * res.base_size[0], self.global_pscale[1] * res.base_size[1])
        centre = res.scale_center if res.esp else (1, 1)
        corners = _corners(centre[0], centre[1])
        crd = _tex_coords(*res.tiling)
        out = []
        order = reversed(self.particles) if res.draw_fwd_ahead else self.particles
        for p in order:
            frame = self._frame(p, res.shape_type, res.dir_type, res.rot_type, res.plane_type,
                                res.esp and res.rotate_anm, size)
            if frame is None:
                continue
            verts = []
            for (cx, cy), (u, v) in zip(corners, crd):
                x, y, z = _transform(frame, cx, cy)
                uv = self._parent_uv(p, u, v, (x, y, z), view)
                verts.append((x, y, uv[0], uv[1], u, v))
            prm, env = self._parent_colours(p)
            out.append(("base", verts, prm, env))
        return out

    def _parent_uv(self, p, u, v, position, view):
        res = self.res
        anm = res.tex_crd_anm
        if res.prj_tex:  # loadPrj / loadPrjAnm: coordinates from the screen position
            left, bottom, right, top = view or (-1.0, -1.0, 1.0, 1.0)
            s = (position[0] - left) / (right - left)
            t = (top - position[1]) / (top - bottom)
            if anm is None:
                return s, t
            m = _tex_matrix(anm, self.tick, res.tiling)
            return m[0][0] * s + m[0][1] * t + m[0][3], m[1][0] * s + m[1][1] * t + m[1][3]
        if anm is None:
            return u, v
        m = _tex_matrix(anm, p.age, res.tiling)  # JPALoadCalcTexCrdMtxAnm
        return m[0][0] * u + m[0][1] * v + m[0][3], m[1][0] * u + m[1][1] * v + m[1][3]

    def _parent_colours(self, p):
        """The JPARegist* functions JPAResource::init picks for the particles."""
        res = self.res
        prm = p.prm if (res.prm_table and not res.global_clr_anm) else self.prm
        env = p.env if (res.env_table and not res.global_clr_anm) else self.env
        alpha = prm[3]
        if res.prm_table or (res.esp and res.alpha_anm):
            alpha = _colour_multi(alpha, p.alpha_anm)
        return prm[:3] + (alpha,), env

    def _quads_child(self):
        res = self.res
        if res.no_draw_child:
            return []
        if res.c_scale_inherited:
            size = (self.global_pscale[0] * res.base_size[0], self.global_pscale[1] * res.base_size[1])
        else:
            size = (self.global_pscale[0] * res.c_scale[0], self.global_pscale[1] * res.c_scale[1])
        corners = _corners(1, 1)
        crd = _tex_coords(0, 0)
        rotating = (res.esp and res.rotate_anm) or res.c_rotate
        out = []
        order = reversed(self.children) if res.draw_fwd_ahead else self.children
        for p in order:
            frame = self._frame(p, res.c_type, res.c_dir_type, res.c_rot_type, res.c_plane_type, rotating, size)
            if frame is None:
                continue
            verts = []
            for (cx, cy), (u, v) in zip(corners, crd):
                x, y, _z = _transform(frame, cx, cy)
                verts.append((x, y, u, v, u, v))
            if res.c_alpha_out or res.c_alpha_inherited or res.c_colour_inherited:  # JPARegistPrmAlphaEnv
                prm = p.prm[:3] + (_colour_multi(p.prm[3], p.alpha_anm),)
                env = p.env
            else:  # JPARegistChildPrmEnv
                prm, env = res.c_prm, res.c_env
            out.append(("child", verts, prm, env))
        return out

    def _frame(self, p, kind, dir_type, rot_type, plane_type, rotating, size):
        """The particle's 3x4 matrix as JPADrawBillboard, JPADrawRotBillboard, JPADrawDirection
        and JPADrawRotDirection make it, with the camera's matrix the identity (the window looks
        straight down the z axis, as the game's 2D particles do)."""
        sx, sy = size[0] * p.scale_x, size[1] * p.scale_y
        if kind == 2:  # billboard
            x, y, z = p.position
            if rotating:
                s, c = sin_s(p.rot_angle), cos_s(p.rot_angle)
                return [[c * sx, -s * sy, 0.0, x], [s * sx, c * sy, 0.0, y], [0.0, 0.0, 1.0, z]]
            return [[sx, 0.0, 0.0, x], [0.0, sy, 0.0, y], [0.0, 0.0, 1.0, z]]
        if kind in (3, 4):  # direction
            if dir_type == 0:
                d = list(p.velocity)
            elif dir_type == 1:
                d = list(p.local)
            elif dir_type == 2:
                d = [-c for c in p.local]
            else:
                d = mat_vec_sr(self.w_rot, self.local_dir)
            d = normalized(d)
            if d is None:
                return None
            side = normalized(cross(p.base_axis, d))
            if side is None:
                return None
            p.base_axis = normalized(cross(d, side)) or p.base_axis
            m = [[p.base_axis[i], d[i], side[i], p.position[i]] for i in range(3)]
            if rotating:
                r = _rot_matrix(rot_type, sin_s(p.rot_angle), cos_s(p.rot_angle))
                _plane(r, plane_type, sx, sy)
                return mat_concat(m, r)
            _plane(m, plane_type, sx, sy)
            return m
        return None  # points, lines, stripes and the others: not used here


def _colour_multi(a, b):  # COLOR_MULTI
    return (a * b) // 255


def _to_u8(v):  # __OSf32tou8
    return max(0, min(255, int(v)))


def _scale_anm(kind, p, cycle):  # JPACalcScaleAnmNormal, RepeatX/Y, ReverseX/Y
    if kind == 0:
        return p.time
    if kind == 1:
        return (p.age % cycle) / float(cycle)
    turn = (p.age // cycle) & 1
    base = (p.age % cycle) / float(cycle)
    return base + turn * (1.0 - base * 2.0)


def _scale_value(res, axis, scale_anm, scale_out):  # JPACalcScaleX / JPACalcScaleY
    if scale_anm < res.scale_in_timing:
        return scale_out * (res.scale_inc[axis] * scale_anm + res.scale_in_value[axis])
    if scale_anm > res.scale_out_timing:
        return scale_out * (res.scale_dec[axis] * (scale_anm - res.scale_out_timing) + 1.0)
    return scale_out


def _tex_matrix(anm, age, tiling):
    """JPALoadCalcTexCrdMtxAnm: the texture matrix of a particle of that age."""
    t = float(age)
    cs, ct = 0.5 * (1.0 + tiling[0]), 0.5 * (1.0 + tiling[1])
    tx, ty = t * anm[5] + anm[0], t * anm[6] + anm[1]
    sx, sy = t * anm[7] + anm[2], t * anm[8] + anm[3]
    angle = int(t * anm[9] + anm[4])
    s, c = sin_s(angle), cos_s(angle)
    return [[sx * c, -sx * s, 0.0, cs + sx * (s * (ct + ty) - c * (cs + tx))],
            [sy * s, sy * c, 0.0, ct + -sy * (s * (cs + tx) + c * (ct + ty))]]


def _rot_matrix(kind, s, c):  # p_rot: rotTypeY, X, Z, XYZ, Y
    if kind == 1:
        return [[1.0, 0.0, 0.0, 0.0], [0.0, c, -s, 0.0], [0.0, s, c, 0.0]]
    if kind == 2:
        return [[c, -s, 0.0, 0.0], [s, c, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0]]
    if kind == 3:
        third = 0.333333 * (1.0 - c)
        ss = 0.57735 * s
        plus = third + ss
        return [[third + c, third - ss, plus, 0.0], [plus, third + c, third - ss, 0.0], [third - ss, plus, third + c, 0.0]]
    return [[c, 0.0, -s, 0.0], [0.0, 1.0, 0.0, 0.0], [s, 0.0, c, 0.0]]


def _plane(m, kind, sx, sy):  # p_plane: basePlaneTypeXY, XZ, X
    for i in range(3):
        m[i][0] *= sx
        if kind == 1:
            m[i][2] *= sy
        else:
            m[i][1] *= sy
            if kind == 2:
                m[i][2] *= sx


def _corners(cx, cy):
    """jpa_pos: the quad's corners for a scale centre (0 left or top, 1 middle, 2 right or
    bottom), 50 units a side, y up; in the order of jpa_dl (top left, top right, bottom right,
    bottom left)."""
    left = (0.0, -25.0, -50.0)[cx]
    top = (0.0, 25.0, 50.0)[cy]
    return [(left, top), (left + 50.0, top), (left + 50.0, top - 50.0), (left, top - 50.0)]


def _tex_coords(tile_s, tile_t):  # jpa_crd
    s, t = 1.0 + tile_s, 1.0 + tile_t
    return [(0.0, 0.0), (s, 0.0), (s, t), (0.0, t)]


def _transform(m, x, y):
    return (m[0][0] * x + m[0][1] * y + m[0][3], m[1][0] * x + m[1][1] * y + m[1][3], m[2][0] * x + m[2][1] * y + m[2][3])


# ##############################################################################################
# # Effects: the emitters of one name, as the game's effect system starts them                 #
# ##############################################################################################
class Library:
    """The effects copied from the disc (particles/particles.json)."""

    def __init__(self, folder, entry):
        self.folder = Path(folder)
        data = json.loads((self.folder / entry).read_text(encoding="utf-8"))
        self.effects = data["effects"]
        self.textures = data["textures"]
        self.resources = {int(k): Resource([(tag, base64.b64decode(body)) for tag, body in blocks])
                          for k, blocks in data["resources"].items()}
        self.seed = Random(0)  # the manager's work data (JPAEmitterWorkData: mRndm(0))

    def has(self, name):
        return bool(self.effects.get(name))

    def emit(self, name, position=(0.0, 0.0, 0.0), scale=1.0):
        """The emitters of an effect: every resource named name00, name01..."""
        return [Emitter(self.resources[n], self.seed.u(), position, scale) for n in self.effects.get(name, [])
                if n in self.resources]

    def texture_path(self, res, slot):
        """(file, wrap s, wrap t) of a resource's texture: the base shape's, the child's or the
        second one (JPAResource::getTexIdx through the TDB1 block)."""
        if slot == "base":
            local = res.tex_index
        elif slot == "child":
            local = res.c_tex
        else:
            local = res.etx["second_index"]
        if local >= len(res.tdb):
            return None
        info = self.textures.get(str(res.tdb[local]))
        if info is None:
            return None
        return self.folder / "particles" / info["file"], info["wrap"][0], info["wrap"][1]


# ##############################################################################################
# # Drawing with OpenGL                                                                        #
# #                                                                                            #
# # The first TEV stage of JPABaseShape::setGX: colour = d + (1 - c) a + c b with the inputs  #
# # of st_ca (C0 is the prm colour, C1 the env colour), alpha by st_aa; a second texture       #
# # multiplies both (JPAResource::setPTev). The picture is premultiplied: a blended particle #
# # adds its alpha, an added one only light (no alpha), and builder_layout.draw_lit lays it    #
# # over the window.                                                                           #
# ##############################################################################################
VERTEX = """#version 120
attribute vec2 position;
attribute vec4 prm;
attribute vec4 env;
attribute vec2 uv0;
attribute vec2 uv1;
uniform vec4 view;
varying vec4 vPrm;
varying vec4 vEnv;
varying vec2 vUv0;
varying vec2 vUv1;
void main() {
  vPrm = prm; vEnv = env; vUv0 = uv0; vUv1 = uv1;
  gl_Position = vec4(position.x * view.x + view.z, position.y * view.y + view.w, 0.0, 1.0);
}
"""

FRAGMENT = """#version 120
varying vec4 vPrm;
varying vec4 vEnv;
varying vec2 vUv0;
varying vec2 vUv1;
uniform sampler2D tex0;
uniform sampler2D tex1;
uniform int mode;
uniform int alphaMode;
uniform int second;
uniform int added;
void main() {
  vec4 t = texture2D(tex0, vUv0);
  vec3 c;
  if (mode == 0) c = t.rgb;
  else if (mode == 1) c = t.rgb * vPrm.rgb;
  else if (mode == 2) c = (vec3(1.0) - t.rgb) * vPrm.rgb + t.rgb;
  else if (mode == 3) c = (vec3(1.0) - t.rgb) * vEnv.rgb + t.rgb * vPrm.rgb;
  else if (mode == 4) c = vEnv.rgb + t.rgb * vPrm.rgb;
  else c = vPrm.rgb;
  c = clamp(c, 0.0, 1.0);
  float a = clamp(alphaMode == 0 ? t.a * vPrm.a : vPrm.a, 0.0, 1.0);
  if (second == 1) {
    vec4 t2 = texture2D(tex1, vUv1);
    c *= t2.rgb;
    a *= t2.a;
  }
  if (added == 1) {
    // Light added to the picture (source alpha, one): the colour premultiplied here; it
    // covers nothing, so no alpha (builder_layout.draw_lit adds it to the window).
    gl_FragColor = vec4(c * a, 0.0);
  } else {
    gl_FragColor = vec4(c, a);
  }
}
"""

# GX blend factors (JPABaseShape::st_bf) as OpenGL's. GX has one value for the source's and the
# destination's colour: as a source factor it is the destination's colour, as a destination
# factor the source's.
SRC_FACTORS = (0, 1, 0x0306, 0x0307, 0x0306, 0x0307, 0x0302, 0x0303, 0x0304, 0x0305)
DST_FACTORS = (0, 1, 0x0300, 0x0301, 0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305)


class Renderer:
    """Draws emitters into a premultiplied QImage, through builder_layout's OpenGL context."""

    def __init__(self, library):
        import builder_layout
        self.library = library
        self.base = builder_layout.Renderer.get()
        self.program = None
        self.fbo = None
        self.textures = {}

    def ready(self):
        if self.base is None:
            return False
        if self.program is None:
            from PySide6.QtOpenGL import QOpenGLShader, QOpenGLShaderProgram
            self.base.context.makeCurrent(self.base.surface)
            program = QOpenGLShaderProgram()
            ok = program.addShaderFromSourceCode(QOpenGLShader.ShaderTypeBit.Vertex, VERTEX) and \
                program.addShaderFromSourceCode(QOpenGLShader.ShaderTypeBit.Fragment, FRAGMENT)
            for i, name in enumerate(("position", "prm", "env", "uv0", "uv1")):
                program.bindAttributeLocation(name, i)
            if not ok or not program.link():
                self.base = None
                return False
            self.program = program
        return True

    def _texture(self, found):
        if found is None:
            return None
        return self.base._texture(*found)

    def render(self, emitters, width, height, view):
        """A width x height QImage of the emitters' particles; view is (left, bottom, right, top)
        in the effects' units."""
        if not self.ready():
            return None
        base = self.base
        base.context.makeCurrent(base.surface)
        readback = base.target(("particles", id(self)), width, height)  # kept framebuffers (builder_layout.Readback)
        readback.bind()
        gl = base.gl
        gl.glViewport(0, 0, width, height)
        gl.glClearColor(0.0, 0.0, 0.0, 0.0)
        gl.glClear(0x4000)
        gl.glEnable(0x0BE2)
        gl.glDisable(0x0B71)
        gl.glDisable(0x0B44)
        left, bottom, right, top = view
        program = self.program
        program.bind()
        flip = -1.0 if readback.flipped else 1.0  # drawn upside down for the Readback's rows
        program.setUniformValue("view", 2.0 / (right - left), flip * 2.0 / (top - bottom),
                                -1.0 - 2.0 * left / (right - left), flip * (-1.0 - 2.0 * bottom / (top - bottom)))
        gl.glUniform1i(program.uniformLocation("tex0"), 0)
        gl.glUniform1i(program.uniformLocation("tex1"), 1)
        for emitter in emitters:
            self._draw_emitter(emitter, view)
        gl.glBlendEquationSeparate(0x8006, 0x8006)
        program.release()
        image = readback.image()
        base.context.doneCurrent()
        return image

    def _draw_emitter(self, emitter, view, quads=None):
        res = emitter.res
        quads = emitter.quads(view) if quads is None else quads
        if not quads:
            return
        gl, program = self.base.gl, self.program
        mode = res.blend & 3  # st_bm: none, blend, logic (drawn as none)
        src, dst = (res.blend >> 2) & 15, (res.blend >> 6) & 15
        added = mode == 1 and SRC_FACTORS[src] == 0x0302 and DST_FACTORS[dst] == 1
        if added:
            # Added light (source alpha, one): its colour adds up and the alpha stays as the
            # blended particles left it (the shader's added branch writes none).
            gl.glBlendEquationSeparate(0x8006, 0x8006)
            gl.glBlendFuncSeparate(1, 1, 1, 1)
        elif mode == 1 and src < 10 and dst < 10:
            # The colour as GX blends it; a blended particle covers what is under it.
            gl.glBlendEquationSeparate(0x8006, 0x8006)
            gl.glBlendFuncSeparate(SRC_FACTORS[src], DST_FACTORS[dst], 1, 0x0303)
        else:
            gl.glBlendEquationSeparate(0x8006, 0x8006)
            gl.glBlendFuncSeparate(1, 0, 1, 0)
        gl.glUniform1i(program.uniformLocation("added"), 1 if added else 0)
        gl.glUniform1i(program.uniformLocation("mode"), int(min(res.colour_mode, 5)))
        gl.glUniform1i(program.uniformLocation("alphaMode"), int(res.alpha_mode))
        second = res.etx is not None and res.etx["second"]
        for slot in ("base", "child"):
            batch = [q for q in quads if q[0] == slot]
            if not batch:
                continue
            found = self._texture(self.library.texture_path(res, slot))
            if found is None:
                continue
            found[0].bind(0)
            use_second = second and slot == "base"
            if use_second:
                found2 = self._texture(self.library.texture_path(res, "second"))
                if found2 is not None:
                    found2[0].bind(1)
                else:
                    use_second = False
            gl.glUniform1i(program.uniformLocation("second"), 1 if use_second else 0)
            data = array.array("f")
            for _slot, verts, prm, env in batch:
                pc = [c / 255.0 for c in prm]
                ec = [c / 255.0 for c in env]
                for i in (0, 1, 2, 0, 2, 3):
                    x, y, u, v, u2, v2 = verts[i]
                    data.extend((x, y))
                    data.extend(pc)
                    data.extend(ec)
                    data.extend((u, v, u2, v2))
            self._draw(data)

    def _draw(self, data):
        from PySide6.QtOpenGL import QOpenGLBuffer
        buffer = QOpenGLBuffer(QOpenGLBuffer.Type.VertexBuffer)
        buffer.create()
        buffer.bind()
        raw = data.tobytes()
        buffer.allocate(raw, len(raw))
        stride = 14 * 4
        for location, (offset, size) in enumerate(((0, 2), (8, 4), (24, 4), (40, 2), (48, 2))):
            self.program.enableAttributeArray(location)
            self.program.setAttributeBuffer(location, 0x1406, offset, size, stride)
        self.base.gl.glDrawArrays(0x0004, 0, len(data) // 14)
        for location in range(5):  # off again, for the next drawing in this context (builder_layout._draw)
            self.program.disableAttributeArray(location)
        buffer.release()
        buffer.destroy()


class SceneRenderer(Renderer):
    """Draws emitters with a 3D camera straight into the OpenGL context current now (a
    QOpenGLWidget's, as the title screen's sky): its own program and textures in that context."""

    def __init__(self, library):
        self.library = library
        self.base = self  # what Renderer's drawing reads: gl and _texture
        self.gl = None
        self.program = None
        self.textures = {}

    def ready(self):
        if self.program is None:
            from PySide6.QtGui import QOpenGLContext
            from PySide6.QtOpenGL import QOpenGLShader, QOpenGLShaderProgram
            context = QOpenGLContext.currentContext()
            if context is None:
                return False
            self.gl = context.functions()
            program = QOpenGLShaderProgram()
            ok = program.addShaderFromSourceCode(QOpenGLShader.ShaderTypeBit.Vertex, VERTEX) and \
                program.addShaderFromSourceCode(QOpenGLShader.ShaderTypeBit.Fragment, FRAGMENT)
            for i, name in enumerate(("position", "prm", "env", "uv0", "uv1")):
                program.bindAttributeLocation(name, i)
            if not ok or not program.link():
                self.program = False
                return False
            self.program = program
        return bool(self.program)

    def _texture(self, found):
        if found is None:
            return None
        path, wrap_s, wrap_t = found
        key = (str(path), wrap_s, wrap_t)
        if key not in self.textures:
            from PySide6.QtGui import QImage
            from PySide6.QtOpenGL import QOpenGLTexture
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

    def draw(self, emitters, to_view, project):
        """The emitters' particles over what is drawn, in screen coordinates from project. The
        alpha already drawn stays (the sky keeps its picture opaque)."""
        if not self.ready():
            return
        gl = self.gl
        gl.glEnable(0x0BE2)
        gl.glDisable(0x0B71)
        gl.glDisable(0x0B44)
        gl.glColorMask(True, True, True, False)
        program = self.program
        program.bind()
        program.setUniformValue("view", 1.0, 1.0, 0.0, 0.0)  # the quads are in clip space already
        gl.glUniform1i(program.uniformLocation("tex0"), 0)
        gl.glUniform1i(program.uniformLocation("tex1"), 1)
        for emitter in emitters:
            self._draw_emitter(emitter, None, emitter.quads_projected(to_view, project))
        gl.glBlendEquationSeparate(0x8006, 0x8006)
        gl.glBlendFuncSeparate(0x0302, 0x0303, 1, 0x0303)
        gl.glDisable(0x0BE2)
        gl.glColorMask(True, True, True, True)
        program.release()


class Player:
    """Emitters run at the game's 60 frames a second against the clock, drawn on demand."""

    FRAME = 1.0 / 60.0

    def __init__(self, library, renderer=None):
        self.library = library
        self.renderer = renderer or Renderer(library)
        self.emitters = []
        self.clock = None

    def emit(self, name, position=(0.0, 0.0, 0.0), scale=1.0):
        made = self.library.emit(name, position, scale)
        self.emitters += made
        return made

    def stop(self, emitters=None):
        """The emitters let their particles live out and make no more (forceDelete would cut
        them)."""
        for emitter in emitters if emitters is not None else self.emitters:
            emitter.stopped = True

    def clear(self):
        self.emitters = []

    def advance(self, now):
        """Runs as many frames as the time since the last call holds (at most half a second)."""
        if self.clock is None:
            self.clock = now
            frames = 1
        else:
            frames = int((now - self.clock) / self.FRAME)
            self.clock += frames * self.FRAME
            if frames > 30:
                frames, self.clock = 30, now
        for _ in range(frames):
            for emitter in self.emitters:
                emitter.calc()
            self.emitters = [e for e in self.emitters if e.alive()]
        return frames

    def image(self, width, height, view):
        return self.renderer.render(self.emitters, width, height, view)
