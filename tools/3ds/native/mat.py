"""Materials of J3D models (MAT3) compiled for the 3DS GPU, for the .n3d files (n3d.py).

Each J3D material (libs/JSystem/include/JSystem/J3DGraphLoader/J3DMaterialFactory.hpp) describes
up to 16 Wii TEV stages: (d +/- ((1 - c) * a + c * b) + bias) * scale on four colour registers,
the texture, the rasterized colour and a konst colour. They are compiled here once into at most
six 3DS texture combiner stages, with the same rules the GX emulation used at run time
(platform/3ds/gx/tev_terms.h): see the approximations there. Longer TEVs of opaque materials go
on in later passes that rebuild the colour in the framebuffer (compile_material).

Constants: a combiner stage holds one colour. Its colour and alpha halves each record where
they come from (a fixed value, a TEV colour register or a konst colour, a component of it,
negated or not), so the runtime refreshes them from the material's current colours (colour
animations change them every frame); the stage structure itself is fixed.

Material record (MATERIAL_SIZE bytes, little-endian):
  0x00 u8 combiner stage count, u8 texture unit count, u8 rasterized channel (0 COLOR0A0,
       1 COLOR1A1; a later pass's own, lit by the GPU), u8 flags (FLAG_*)
  0x04 u8 cull (0 none, 1 front, 2 back, 3 all: GX), u8 blend equation, u8 blend source,
       u8 blend destination (PICA), u8 alpha test (PICA), u8 alpha reference, u8 depth test,
       u8 depth function (PICA, reversed depth), u8 write mask (PICA), u8 z compare before
       texturing, u8 J3D material mode, u8 count of passes after the first
  0x10 units[3]: u16 texture (TEX1 index, 0xFFFF none), u8 texture map, u8 coordinate (a later
       pass's are the first pass's, or with one unit taking a texture of its own: own_unit)
  0x1C stages[6] (STAGE_SIZE): u8 colour source[3], colour operand[3], alpha source[3], alpha
       operand[3], colour function, alpha function, colour scale, alpha scale; colour constant
       reference (4 bytes), alpha constant reference (4 bytes); u32 constant (initial RGBA)
  0xC4 u8 colour channel count, u8 texture generation count, u16 record of the first later
       pass (records after the J3D materials, same layout); colour channels[4]
       (J3DColorChanInfo, 8 bytes); u32 material colours[2], ambient colours[2] (RGBA);
       texture generations[8] (J3DTexCoordInfo, 4 bytes); s16 TEV colours[4][4] (RGBA);
       u32 konst colours[4] (RGBA)
  0x148 u8 combiner buffer updates of the colour and of the alpha (bit n: the buffer takes stage
       n's result, n 0-3; GPUREG_TEXENV_UPDATE_BUFFER), u8 second alpha test (PICA), u8 its
       reference (FLAG_ALPHA_SPLIT)
Constant reference: u8 kind (REF_*), u8 index (TEV register 1-3 or konst colour 0-3), u8 part
(PART_*), u8 negate (1 the value negated, 2 its low 8 bits: a TEV register read as input a, b
or c, as the TEV reads it; 3 the value plus 128, for FUNC_ADD_SIGNED; 4 its part over 255, 5 half
of that, 6 half the value negated: added or taken off after the clamped value).
"""

import copy
import struct

# ##############################################################################################
# # The record's values                                                                        #
# #                                                                                            #
# # The PICA's combiner sources, operands and functions, and the flags of a material record,   #
# # as platform/3ds/gx/native.cpp reads them.                                                  #
# ##############################################################################################
# PICA values (libctru gpu/enums.h).
SRC_PRIMARY, SRC_TEXTURE0, SRC_BUFFER, SRC_CONSTANT, SRC_PREVIOUS = 0x00, 0x03, 0x0D, 0x0E, 0x0F
OP_RGB_COLOR, OP_RGB_ALPHA, OP_RGB_R, OP_RGB_G, OP_RGB_B = 0x00, 0x02, 0x04, 0x08, 0x0C
OP_A_ALPHA, OP_A_R, OP_A_G, OP_A_B = 0x00, 0x02, 0x04, 0x06
FUNC_REPLACE, FUNC_MODULATE, FUNC_ADD, FUNC_ADD_SIGNED, FUNC_INTERPOLATE, FUNC_SUBTRACT, FUNC_MULTIPLY_ADD = 0, 1, 2, 3, 4, 5, 8
TEST_NEVER, TEST_ALWAYS, TEST_EQUAL, TEST_NOTEQUAL, TEST_LESS, TEST_LEQUAL, TEST_GREATER, TEST_GEQUAL = range(8)
BLEND_ADD, BLEND_SUBTRACT, BLEND_REVERSE_SUBTRACT = 0, 1, 2
FACTOR_ZERO, FACTOR_ONE, FACTOR_SRC_COLOR, FACTOR_DST_COLOR = 0, 1, 2, 4
WRITE_COLOR, WRITE_ALPHA, WRITE_DEPTH = 0x07, 0x08, 0x10

MAX_STAGES = 6
MAX_UNITS = 3
MAX_PASSES = 6  # framebuffer passes after the first (compile_material)
OWN_UNIT = SRC_TEXTURE0 + MAX_UNITS  # a pass's own texture before own_unit gives it a unit

REF_FIXED, REF_TEVREG, REF_KONST = 0, 1, 2
PART_RGB, PART_ALPHA, PART_R, PART_G, PART_B = 0, 1, 2, 3, 4  # PART_ALPHA..B: one component, replicated

FLAG_INEXACT = 1  # approximation marker (stages that did not fit are left out)
FLAG_FOG = 2
FLAG_INDIRECT = 4
# GX's two alpha comparisons pass "A <= a or A >= b": the material is drawn twice, with the
# record's test and then with the second one (0x14A); their values do not overlap, so each pixel
# is drawn once, as on the Wii (split_alpha_test).
FLAG_ALPHA_SPLIT = 8

STAGE_SIZE = 28
MATERIAL_SIZE = 0x1C + MAX_STAGES * STAGE_SIZE + 4 + 32 + 16 + 32 + 32 + 16 + 4

KONST_FRACTIONS = [255, 223, 191, 159, 128, 96, 64, 32]
DEPTH_FUNC = [TEST_NEVER, TEST_GREATER, TEST_EQUAL, TEST_GEQUAL, TEST_LESS, TEST_NOTEQUAL, TEST_LEQUAL, TEST_ALWAYS]
ALPHA_FUNC = [TEST_NEVER, TEST_LESS, TEST_EQUAL, TEST_LEQUAL, TEST_GREATER, TEST_NOTEQUAL, TEST_GEQUAL, TEST_ALWAYS]
SRC_FACTOR = [0, 1, 4, 5, 6, 7, 8, 9]
DST_FACTOR = [0, 1, 2, 3, 6, 7, 8, 9]
COLOR_CHANNEL_OP = [OP_RGB_R, OP_RGB_G, OP_RGB_B, OP_RGB_ALPHA]
ALPHA_CHANNEL_OP = [OP_A_R, OP_A_G, OP_A_B, OP_A_ALPHA]


# ##############################################################################################
# # Combiner terms and halves                                                                  #
# #                                                                                            #
# # The same model as platform/3ds/gx/tev_terms.h: a Term is one input of a combiner function, #
# # a Half the colour or alpha part of one combiner stage. One TEV function, (d +/- lerp(a, b, #
# # c) + bias) * scale, becomes one or more halves; build_halves picks the form with the       #
# # fewest stages, and clamps where the Wii's TEV clamps.                                      #
# ##############################################################################################

class Term:
    __slots__ = ("src", "op", "value", "ref", "zero", "one", "previous", "negative", "below_zero", "signed_half",
                 "stage_input", "over")

    def __init__(self, src=SRC_PREVIOUS, op=0, value=(0, 0, 0), ref=None, zero=False, one=False, previous=False, negative=False,
                 below_zero=None, signed_half=None, stage_input=False, over=None):
        self.src = src
        self.op = op
        self.value = tuple(value)
        self.ref = ref or (REF_FIXED, 0, PART_RGB, 0)
        self.zero = zero
        self.one = one
        self.previous = previous
        self.negative = negative
        # An animated TEV register read as d: this term holds its part above zero and below_zero
        # the part under it (the animation's values can take either sign).
        self.below_zero = below_zero
        # The same register plus 128 (negate 3), when all its values lie in -128..127: added with
        # FUNC_ADD_SIGNED it takes one combiner instead of the two of above and below_zero.
        self.signed_half = signed_half
        # A previous value read as an input of the TEV stage (not one of its own combiners'
        # results): a later combiner of the stage reads it from the combiner buffer.
        self.stage_input = stage_input
        # A TEV register over 255 read as d: this term holds it clamped, over its part above 255
        # (negate 4), added after where the clamp would lose it (HalfGalaxySky's C0 = 481 - lerp).
        self.over = over

    def copy(self, **changes):
        t = Term(self.src, self.op, self.value, self.ref, self.zero, self.one, self.previous, self.negative, self.below_zero,
                 self.signed_half, self.stage_input, self.over)
        for k, v in changes.items():
            setattr(t, k, v)
        return t


def constant(r, g, b, ref=None):
    return Term(SRC_CONSTANT, 0, (r, g, b), ref)


def constant_alpha(a, ref=None):
    return constant(a, a, a, ref)


def zero():
    return Term(SRC_CONSTANT, 0, (0, 0, 0), zero=True)


def one():
    return Term(SRC_CONSTANT, 0, (255, 255, 255), one=True)


def source(src, op, previous=False):
    return Term(src, op, previous=previous)


# The combiner buffer holds its initial colour, 0.5 (0x80808080, set by the host's renderer), until
# a material saves a stage input in it (compile_material); from then on 0.5 is a stage constant.
_buffer_half = True
# While compiling a stage that can save its inputs in the buffer, the later combiners can read them.
_inputs_late = False


def half(alpha):
    """0.5: the combiner buffer while it holds its initial colour (it takes no stage constant)."""
    if not _buffer_half:
        return constant_alpha(128) if alpha else constant(128, 128, 128)
    return Term(SRC_BUFFER, OP_A_ALPHA if alpha else OP_RGB_COLOR, (128, 128, 128))


def inverse(t):
    if t.zero:
        return one()
    if t.one:
        return zero()
    return t.copy(op=t.op ^ 1)


class Half:
    def __init__(self, func, a=None, b=None, c=None, count=1):
        self.func = func
        self.scale = 0
        self.count = count
        self.t = [a or Term(), b or Term(), c or Term()]
        self.inexact = False  # reads a previous value an earlier combiner of the stage replaced


def previous_term(alpha):
    return source(SRC_PREVIOUS, OP_A_ALPHA if alpha else OP_RGB_COLOR, True)


def constant_key(t, alpha):
    return (t.value[0] if alpha else t.value, t.ref)


def is_scalar(t):
    """A constant whose three colour components are one value whatever the material's colours:
    a colour half can read it from the stage constant's alpha (Stage.store)."""
    return t.src == SRC_CONSTANT and (t.ref[0] == REF_FIXED and len(set(t.value)) == 1 or
                                      t.ref[2] in (PART_ALPHA, PART_R, PART_G, PART_B))


def split_constants(halves, alpha):
    """A combiner holds one constant: a function of two different ones (PowerStar's Empty, tex *
    C1 + C0) is split in combiners that each read one, the previous result carrying the rest."""
    out = []
    prev = previous_term(alpha)
    for h in halves:
        terms = h.t[: h.count]
        key = [constant_key(t, alpha) if t.src == SRC_CONSTANT else None for t in terms]
        if len(set(k for k in key if k is not None)) < 2:
            out.append(h)
            continue
        f = h.func
        late = not any(t.previous for t in terms[1:])  # read after the first combiner
        more = None
        if f in (FUNC_ADD, FUNC_SUBTRACT, FUNC_MODULATE, FUNC_ADD_SIGNED) and h.count == 2 and late:
            more = [Half(FUNC_REPLACE, terms[0]), Half(f, prev, terms[1], None, 2)]
        elif f == FUNC_MULTIPLY_ADD:
            x, y, z = terms
            if (key[0] is None or key[1] is None or key[0] == key[1]) and not z.previous:
                more = [Half(FUNC_MODULATE, x, y, None, 2), Half(FUNC_ADD, prev, z, None, 2)]
            elif late and key[2] in (None, key[1]):
                more = [Half(FUNC_REPLACE, x), Half(FUNC_MULTIPLY_ADD, prev, y, z, 3)]
            elif late:
                more = [Half(FUNC_REPLACE, x), Half(FUNC_MODULATE, prev, y, None, 2), Half(FUNC_ADD, prev, z, None, 2)]
        elif f == FUNC_INTERPOLATE:
            b, a, c = terms  # b * c + a * (1 - c)
            if key[2] is None and not a.previous:
                # c read twice: a previous c comes from the combiner buffer (compile_material saves
                # the stage inputs; IceBall's lerp(K0, C0, PREV)), else the second read is wrong.
                more = [Half(FUNC_MODULATE, b, c, None, 2), Half(FUNC_MULTIPLY_ADD, a, inverse(c), prev, 3)]
                more[-1].inexact = c.previous and not _inputs_late
            elif key[0] in (None, key[2]) and not b.previous and not c.previous:
                more = [Half(FUNC_REPLACE, a), Half(FUNC_MODULATE, prev, inverse(c), None, 2),
                        Half(FUNC_MULTIPLY_ADD, b, c, prev, 3)]
            elif key[1] in (None, key[2]) and not a.previous and not c.previous:
                more = [Half(FUNC_REPLACE, b), Half(FUNC_MODULATE, prev, c, None, 2),
                        Half(FUNC_MULTIPLY_ADD, a, inverse(c), prev, 3)]
            elif not alpha and is_scalar(c) and not a.previous and not c.previous:
                # Three constants, c one value (FlagDiscPlanet's lerp(C1, C0, A0)): each combiner
                # reads c from the constant's alpha (Stage.store).
                more = [Half(FUNC_MODULATE, b, c, None, 2), Half(FUNC_MULTIPLY_ADD, a, inverse(c), prev, 3)]
        if more is None:
            out.append(h)
            continue
        more[-1].scale = h.scale
        more[-1].inexact = more[-1].inexact or h.inexact
        out += more
    return out


def build_halves(a, b, c, d, bias, subtract, scale, alpha):
    """Combiner functions for (d +/- lerp(a, b, c) + bias) * scale (tev_terms.h buildHalves)."""
    return split_constants(build_halves_unsplit(a, b, c, d, bias, subtract, scale, alpha), alpha)


def build_halves_unsplit(a, b, c, d, bias, subtract, scale, alpha):
    if d.below_zero is not None and not d.previous and scale == 3 and bias == 0 and not subtract and \
            not (c.one and b.zero or c.zero and a.zero or a.zero and b.zero):
        # (lerp + d) / 2, d of both signs (the lava's animated C0, -x..315): (lerp + d above 0) / 2,
        # + its part over 255 / 2, - its part under 0 / 2; in each component one of them is 0.
        above = d.copy(below_zero=None, signed_half=None, over=None)
        out = build_halves_clamped(a, b, c, above, 0, False, 3, alpha)
        prev = previous_term(alpha)
        if d.over is not None:
            out.append(Half(FUNC_ADD, prev, over_half(d.over), None, 2))
        out.append(Half(FUNC_SUBTRACT, prev, over_half(d.below_zero), None, 2))
        return out
    over = d.over
    if over is None:
        return build_halves_clamped(a, b, c, d, bias, subtract, scale, alpha)
    # d over 255: d - lerp is (d clamped - lerp) + over, (d + lerp) / 2 is (d clamped + lerp) / 2
    # + over / 2 (the lava's (C0 + ras * tex) / 2, C0 = 315); d + lerp with no bias clamps at 1
    # with d clamped too. Other biases keep d clamped.
    d = d.copy(over=None)
    out = build_halves_clamped(a, b, c, d, bias, subtract, scale, alpha)
    prev = previous_term(alpha)
    if bias == 0 and scale == 3:
        out.append(Half(FUNC_ADD, prev, over_half(over), None, 2))
    elif bias == 0 and subtract:
        scale_ = out[-1].scale
        out[-1].scale = 0
        out.append(Half(FUNC_ADD, prev, over, None, 2))
        out[-1].scale = scale_
    return out


def fixed_values(t):
    """A fixed constant term's components as read (1 - value for an inverted operand)."""
    return [255 - v if t.op & 1 else v for v in t.value]


def fixed_pair(f, g):
    """True when terms f and g are fixed constants (no colour reaches them at run time), neither
    0 nor 1: their product is one constant."""
    return all(t.src == SRC_CONSTANT and t.ref[0] == REF_FIXED and not t.zero and not t.one and
               not t.negative and t.below_zero is None and t.over is None for t in (f, g))


def build_halves_clamped(a, b, c, d, bias, subtract, scale, alpha):
    out = []
    prev = previous_term(alpha)
    a, b, c, d = (t.copy(stage_input=True) if t.previous else t for t in (a, b, c, d))
    if a.negative:
        a = zero()
    if b.negative:
        b = zero()
    if c.negative:
        c = zero()

    # (d + lerp + bias) / 2 and d + lerp - 1/2: the TEV clamps only at the end, so a sum over 1
    # comes back in range. Combiners clamp each result: the sum is kept in range with
    # FUNC_INTERPOLATE(lerp, d, 1/2) (the coins' (ras + lerp - 1/2) / 2) and FUNC_ADD_SIGNED.
    d_readable = not d.previous or _inputs_late or (not alpha and d.op in (OP_RGB_ALPHA, OP_RGB_ALPHA | 1))
    plain_d = not d.zero and not d.negative and d.below_zero is None and d.signed_half is None and d_readable
    lerp_zero = (c.one and b.zero) or (c.zero and a.zero) or (a.zero and b.zero)
    register_d = d.src == SRC_CONSTANT and d.ref[0] == REF_TEVREG and d.ref[3] in (0, 1) and not d.previous
    single_x = b if c.one else a if c.zero else c if a.zero and b.one else None
    if single_x is not None and not single_x.zero and not subtract and scale == 3 and not d.zero and \
            not d.negative and d.below_zero is None and d.over is None:
        # (x + d + bias) / 2 for a lerp of one input: one combiner reads both, d may be the
        # previous value (TripodBossCoreBloom's (texA + APREV - 1/2) / 2).
        out = [Half(FUNC_INTERPOLATE, single_x, d, half(alpha), 3)]
        if bias:
            quarter = constant_alpha(64) if alpha else constant(64, 64, 64)
            out.append(Half(FUNC_ADD if bias == 1 else FUNC_SUBTRACT, prev, quarter, None, 2))
        return out
    if not subtract and lerp_zero and bias == 1 and register_d and (d.below_zero is not None or d.negative):
        # d + 1/2 for a d below zero (PrizeRing's alpha -255 + 1/2 is 0, not 1/2).
        kind, reg, part, _ = d.ref
        out = [Half(FUNC_REPLACE, Term(SRC_CONSTANT, 0, d.value, (kind, reg, part, 3)))]
        if scale in (1, 2):
            out[-1].scale = scale
        elif scale == 3:
            out.append(Half(FUNC_MODULATE, prev, half(alpha), None, 2))
        return out
    if not subtract and plain_d and not lerp_zero and (scale == 3 or bias == 2):
        out = build_halves_unsplit(a, b, c, zero(), 0, False, 0, alpha)
        if scale == 3:
            out.append(Half(FUNC_INTERPOLATE, prev, d, half(alpha), 3))
            if bias:
                quarter = constant_alpha(64) if alpha else constant(64, 64, 64)
                out.append(Half(FUNC_ADD if bias == 1 else FUNC_SUBTRACT, prev, quarter, None, 2))
        else:
            out.append(Half(FUNC_ADD_SIGNED, prev, d, None, 2))
            out[-1].scale = scale
        return out

    subtract_d = False
    negative_d = None
    below_zero_d = d.below_zero
    above_zero_d = None
    # Below zero only with a bias: lerp - |d| needs no offset (and rounds closer after a scale).
    signed_d = d.signed_half if not subtract and (not d.negative or bias in (1, 2) and scale != 3) else None
    if signed_d is not None:
        # lerp + d as FUNC_ADD_SIGNED(lerp, d + 128), after the lerp's own combiners.
        below_zero_d = None
        d = zero()
    elif below_zero_d is not None:
        d = d.copy(below_zero=None, signed_half=None)
        if not subtract:
            # lerp + above - below, each in a combiner of its own: the lerp may need a constant
            # of its own (the Lumas' konst alpha times the rasterized alpha).
            above_zero_d = d
            d = zero()
    if d.negative:
        if subtract:
            return [Half(FUNC_REPLACE, zero())]
        negative_d = d.copy(negative=False)
        subtract_d = True
        d = zero()

    if c.one or (a.zero and b.zero):
        kind = "zero" if b.zero else "single"
        x = b
    elif c.zero:
        kind = "zero" if a.zero else "single"
        x = a
    elif a.zero:
        kind = "single" if b.one else "product"
        x = c if b.one else b
        y = c
    elif b.zero:
        ic = inverse(c)
        kind = "single" if a.one else "product"
        x = ic if a.one else a
        y = ic
    else:
        kind = "full"
    # Terms read after the lerp's first combiner: a previous colour is gone by then, the previous
    # alpha of a colour half is not (delayed_alpha keeps it until the stage's last colour combiner).
    def readable_late(t):
        return not t.previous or _inputs_late or (not alpha and t.op in (OP_RGB_ALPHA, OP_RGB_ALPHA | 1))

    d_late = readable_late(d)

    if kind == "zero":
        out.append(Half(FUNC_REPLACE, d))
    elif kind == "single":
        if subtract:
            out.append(Half(FUNC_SUBTRACT, d, x, None, 2))
        elif d.zero:
            out.append(Half(FUNC_REPLACE, x))
        else:
            out.append(Half(FUNC_ADD, x, d, None, 2))
    elif kind == "product":
        if subtract and d.previous:
            # d - x * y = 1 - (x * y + (1 - d)): the previous result is read before it is
            # overwritten (clamping the sum at 1 gives the clamped difference at 0).
            out.append(Half(FUNC_MULTIPLY_ADD, x, y, inverse(d), 3))
            out.append(Half(FUNC_REPLACE, inverse(prev)))
        elif subtract:
            out.append(Half(FUNC_MODULATE, x, y, None, 2))
            out.append(Half(FUNC_SUBTRACT, d, prev, None, 2))
        elif d.zero:
            out.append(Half(FUNC_MODULATE, x, y, None, 2))
        else:
            out.append(Half(FUNC_MULTIPLY_ADD, x, y, d, 3))
    elif (fixed_pair(b, c) or fixed_pair(a, inverse(c))) and not subtract and bias == 0 and scale != 3 and \
            (d.previous or d.zero):
        # Two fixed constants of the lerp folded into one (fixed registers: BossPlanetR50's
        # lerp(ras, C1, C0) + PREV is ras * (1 - C0) + PREV, then + C1 * C0); a sum clamped at 1
        # first still clamps the result at 1 with no bias or halving after.
        x, y = (a, inverse(c)) if fixed_pair(b, c) else (b, c)
        f, g = (b, c) if fixed_pair(b, c) else (a, inverse(c))
        k = constant(*((fv * (gv + (gv >> 7))) >> 8 for fv, gv in zip(fixed_values(f), fixed_values(g))))
        out.append(Half(FUNC_MULTIPLY_ADD, x, y, d, 3) if d.previous else Half(FUNC_MODULATE, x, y, None, 2))
        out.append(Half(FUNC_ADD, prev, k, None, 2))
    else:
        interpolate = Half(FUNC_INTERPOLATE, b, a, c, 3)
        two_constants = a.src == SRC_CONSTANT and b.src == SRC_CONSTANT and (a.value != b.value or a.ref != b.ref)
        rising = all(bv >= av for av, bv in zip(a.value, b.value))
        falling = all(bv <= av for av, bv in zip(a.value, b.value))
        # With the stage inputs in the buffer, registers keep their own values (an animation
        # reaches them: AstroDomeComet's C0) in b * c + a * (1 - c), split by split_constants.
        live = _inputs_late and (a.ref[0] != REF_FIXED or b.ref[0] != REF_FIXED)
        if two_constants and c.previous and d_late and (rising or falling) and not live:
            # lerp(a, b, prev) = a + prev * (b - a) (or a - prev * (a - b)), one constant per
            # combiner (tev_terms.h). The difference is the converted value: a colour animation
            # of a or b does not reach it.
            if rising:
                out.append(Half(FUNC_MODULATE, c, constant(*(bv - av for av, bv in zip(a.value, b.value))), None, 2))
                out.append(Half(FUNC_ADD, prev, a, None, 2))
            else:
                out.append(Half(FUNC_MODULATE, c, constant(*(av - bv for av, bv in zip(a.value, b.value))), None, 2))
                out.append(Half(FUNC_SUBTRACT, a, prev, None, 2))
            if subtract:
                out.append(Half(FUNC_SUBTRACT, d, prev, None, 2))
            elif not d.zero:
                out.append(Half(FUNC_ADD, prev, d, None, 2))
        elif subtract and d.previous and not a.previous and not c.previous and not _inputs_late:
            # 1 - (b * c + (1 - d) + a * (1 - c)), as for the product
            out.append(Half(FUNC_MULTIPLY_ADD, b, c, inverse(d), 3))
            out.append(Half(FUNC_MULTIPLY_ADD, a, inverse(c), prev, 3))
            out.append(Half(FUNC_REPLACE, inverse(prev)))
        elif two_constants and not c.previous and not subtract and d.previous:
            out.append(Half(FUNC_MULTIPLY_ADD, b, c, d, 3))
            out.append(Half(FUNC_MULTIPLY_ADD, a, inverse(c), prev, 3))
        elif two_constants and not c.previous:
            out.append(Half(FUNC_MODULATE, b, c, None, 2))
            out.append(Half(FUNC_MULTIPLY_ADD, a, inverse(c), prev, 3))
            if subtract:
                out.append(Half(FUNC_SUBTRACT, d, prev, None, 2))
            elif not d.zero:
                out.append(Half(FUNC_ADD, prev, d, None, 2))
        elif subtract:
            out.append(interpolate)
            out.append(Half(FUNC_SUBTRACT, d, prev, None, 2))
            out[-1].inexact = not d_late
        elif d.zero:
            out.append(interpolate)
        elif d_late:
            out.append(interpolate)
            out.append(Half(FUNC_ADD, prev, d, None, 2))
        elif not b.previous and not c.previous:
            out.append(Half(FUNC_MULTIPLY_ADD, a, inverse(c), d, 3))
            out.append(Half(FUNC_MULTIPLY_ADD, b, c, prev, 3))
        else:
            out.append(Half(FUNC_MULTIPLY_ADD, b, c, d, 3))
            out.append(Half(FUNC_MULTIPLY_ADD, a, inverse(c), prev, 3))
            out[-1].inexact = not (readable_late(a) and readable_late(c))

    if subtract_d:
        out.append(Half(FUNC_SUBTRACT, prev, negative_d, None, 2))
    if signed_d is not None and bias == 1 and scale != 3:
        # lerp + d + 1/2 and lerp + d - 1/2 clamped once: lerp + (d + 1/2), lerp - (1/2 - d)
        # (WaterBullet's ras + C0 + 1/2, C0 below zero).
        out.append(Half(FUNC_ADD, prev, signed_d, None, 2))
        bias = 0
    elif signed_d is not None and bias == 2 and scale != 3:
        out.append(Half(FUNC_SUBTRACT, prev, inverse(signed_d), None, 2))
        bias = 0
    elif signed_d is not None:
        out.append(Half(FUNC_ADD_SIGNED, prev, signed_d, None, 2))
    if below_zero_d is not None and above_zero_d is not None and bias in (1, 2) and scale != 3:
        # lerp + above - below +/- 1/2, clamped once: in each component one of above and below
        # is zero, so lerp + above - 1/2 then - below, or lerp + (1 - below) - 1/2 then + above,
        # clamp where the TEV does (silver_v; PrizeRing's alpha).
        if bias == 2:
            out.append(Half(FUNC_ADD_SIGNED, prev, above_zero_d, None, 2))
            out.append(Half(FUNC_SUBTRACT, prev, below_zero_d, None, 2))
        else:
            out.append(Half(FUNC_ADD_SIGNED, prev, inverse(below_zero_d), None, 2))
            out.append(Half(FUNC_ADD, prev, above_zero_d, None, 2))
        bias = 0
    elif below_zero_d is not None:
        # d + lerp or d - lerp with d = above - below: the part under zero is taken off after.
        last = out[-1] if len(out) == 1 else None
        free = last is not None and all(t.src != SRC_CONSTANT for t in last.t[: last.count])
        if above_zero_d is not None and free and last.func == FUNC_MODULATE:
            # x * y + above in one combiner (Polta's tex * ras + C0, C0 of both signs)
            out[-1] = Half(FUNC_MULTIPLY_ADD, last.t[0], last.t[1], above_zero_d, 3)
        elif above_zero_d is not None and free and last.func == FUNC_REPLACE:
            out[-1] = Half(FUNC_ADD, last.t[0], above_zero_d, None, 2)
        elif above_zero_d is not None:
            out.append(Half(FUNC_ADD, prev, above_zero_d, None, 2))
        out.append(Half(FUNC_SUBTRACT, prev, below_zero_d, None, 2))
    if bias == 1:
        out.append(Half(FUNC_ADD, prev, half(alpha), None, 2))
    elif bias == 2:
        out.append(Half(FUNC_ADD_SIGNED, prev, zero(), None, 2))
    first = out[0].t[0]
    if len(out) > 1 and out[0].func == FUNC_REPLACE and first.src == SRC_PREVIOUS and first.op == prev.op:
        out = out[1:]  # d = PREV kept, the next combiner reads it (BossPlanetR50's (PREV - 1/2) * 2)
    if scale in (1, 2):
        out[-1].scale = scale
    elif scale == 3:
        out.append(Half(FUNC_MODULATE, prev, half(alpha), None, 2))
    return out


# ##############################################################################################
# # Material inputs                                                                            #
# #                                                                                            #
# # What a TEV stage reads, as terms: its texture, the rasterized colour, the colour registers #
# # (constant in the record, or left to the run time when the game animates them), the         #
# # constant colours and the swap tables.                                                      #
# ##############################################################################################

def clamp(v):
    return 0 if v < 0 else 255 if v > 255 else v


class Inputs:
    def __init__(self, mat):
        self.mat = mat
        self.unit = -1
        self.ras_zero = False
        self.tex_swap = [0, 1, 2, 3]
        self.ras_swap = [0, 1, 2, 3]
        self.kcsel = 0
        self.kasel = 0
        self.written = [False] * 4
        # (register, alpha) -> the input a stage copied into a register other than PREV: later
        # stages read the input itself (forwarded_input).
        self.forward = {}
        # The register the combiners' previous colour and alpha hold (the last one written).
        self.prev_color_reg = 0
        self.prev_alpha_reg = 0
        # alpha -> the register whose value the combiner buffer keeps while a stage writes another
        # one (ScrewSwitch: C2 = tex * C1, then PREV + tex * C2 reads PREV from the buffer).
        self.parked = {False: None, True: None}
        self.fixed = False  # registers no animation sets are their file values (register_term)
        self.compare_saves_alpha = False  # compare_stage: the combiner before it keeps the alpha in the buffer
        self.inexact = False


def register_term(reg, alpha_of_register, alpha, inp, low_byte=False):
    """A TEV register as a stage input. As input a, b or c (low_byte) the TEV reads its low 8 bits
    (an 11-bit signed value, so -122 reads as 134: Mario's eye alpha); as d, the signed value."""
    t = inp.forward.get((reg, alpha or alpha_of_register))
    if t is not None:
        if alpha_of_register and not alpha and t.src != SRC_CONSTANT:
            t = t.copy(op={OP_A_ALPHA: OP_RGB_ALPHA, OP_A_R: OP_RGB_R, OP_A_G: OP_RGB_G, OP_A_B: OP_RGB_B}[t.op & ~1] | (t.op & 1))
        return t
    if inp.written[reg]:
        # Combiners have one previous value: a register written before the last write is gone
        # (ScrewSwitch: a stage writes C2, the next reads C2 and PREV).
        held = inp.prev_alpha_reg if alpha or alpha_of_register else inp.prev_color_reg
        if held != reg:
            if inp.parked[alpha or alpha_of_register] == reg:
                return source(SRC_BUFFER, OP_A_ALPHA if alpha else (OP_RGB_ALPHA if alpha_of_register else OP_RGB_COLOR))
            inp.inexact = True
        return source(SRC_PREVIOUS, OP_A_ALPHA if alpha else (OP_RGB_ALPHA if alpha_of_register else OP_RGB_COLOR), True)
    r, g, b, a = inp.mat["tev_colors"][reg - 1] if reg > 0 else (0, 0, 0, 0)
    if inp.fixed and reg not in inp.mat.get("animated_registers", {}):
        # A material that does not fit otherwise (compile_material): a register no animation sets
        # is taken as its file value, black or white without a constant (Polta's C1).
        values = {a} if alpha or alpha_of_register else {r, g, b}
        if values == {0}:
            return zero()
        if values == {255}:
            return one()
        if low_byte:
            # A fixed constant as input a, b or c: products of two fold (build_halves_clamped).
            if alpha or alpha_of_register:
                return constant_alpha(a & 0xFF)
            return constant(r & 0xFF, g & 0xFF, b & 0xFF)
    if low_byte:
        # The low byte for values out of range and for animated registers, which an animation
        # can take out of it (AstroCore's GlowMat animates its blue to 256, read as 0).
        animated = reg in inp.mat.get("animated_registers", {})
        if alpha or alpha_of_register:
            return constant_alpha(a & 0xFF, (REF_TEVREG, reg, PART_ALPHA, 2 if animated or not 0 <= a <= 255 else 0))
        in_range = all(0 <= x <= 255 for x in (r, g, b))
        return constant(r & 0xFF, g & 0xFF, b & 0xFF, (REF_TEVREG, reg, PART_RGB, 2 if animated or not in_range else 0))
    animated = inp.mat.get("animated_registers", {}).get(reg)
    if animated is not None:
        # A colour animation (.brk) sets this register at run time. Values of one sign (the
        # file's and the animation's) take one combiner, as below, by that sign; values of
        # both (the Lumas' colours: a white texture plus a register mostly below zero) take
        # the part above zero added and the part below taken off (refValue: negate 0 and 1).
        part = alpha or alpha_of_register
        values = animated["alpha"] + [a] if part else animated["rgb"] + [r, g, b]
        if min(values) >= 0:
            return over_term(values, (r, g, b, a), reg, part)
        if max(values) <= 0:
            signed = signed_half_term(values, (r, g, b, a), reg, part)
            if part:
                return constant_alpha(clamp(-a), (REF_TEVREG, reg, PART_ALPHA, 1)).copy(negative=True, signed_half=signed)
            return constant(clamp(-r), clamp(-g), clamp(-b), (REF_TEVREG, reg, PART_RGB, 1)).copy(negative=True, signed_half=signed)
        above = over_term(values, (r, g, b, a), reg, part)
        if part:
            below = constant_alpha(clamp(-a), (REF_TEVREG, reg, PART_ALPHA, 1))
        else:
            below = constant(clamp(-r), clamp(-g), clamp(-b), (REF_TEVREG, reg, PART_RGB, 1))
        return above.copy(below_zero=below, signed_half=signed_half_term(values, (r, g, b, a), reg, part))
    if alpha or alpha_of_register:
        if a < 0:
            return constant_alpha(clamp(-a), (REF_TEVREG, reg, PART_ALPHA, 1)).copy(
                negative=True, signed_half=signed_half_term([a], (r, g, b, a), reg, True))
        return over_term([a], (r, g, b, a), reg, True)
    if r <= 0 and g <= 0 and b <= 0 and (r < 0 or g < 0 or b < 0):
        return constant(clamp(-r), clamp(-g), clamp(-b), (REF_TEVREG, reg, PART_RGB, 1)).copy(
            negative=True, signed_half=signed_half_term([r, g, b], (r, g, b, a), reg, False))
    if r >= 0 and g >= 0 and b >= 0:
        return over_term([r, g, b], (r, g, b, a), reg, False)
    above = over_term([r, g, b], (r, g, b, a), reg, False)
    if r < 0 or g < 0 or b < 0:
        # Both signs (TicoLow's (14, 2, -107)): the part below zero is taken off, as above; a part
        # over 255 stays added (SearchBeamer's (271, -73, -162)).
        return above.copy(below_zero=constant(clamp(-r), clamp(-g), clamp(-b), (REF_TEVREG, reg, PART_RGB, 1)),
                          signed_half=signed_half_term([r, g, b], (r, g, b, a), reg, False))
    return above


def over_term(values, rgba, reg, part):
    """A register of values from 0 up: clamped, with its part over 255 when some value passes it."""
    r, g, b, a = rgba
    if part:
        t = constant_alpha(clamp(a), (REF_TEVREG, reg, PART_ALPHA, 0))
        over = constant_alpha(clamp(a - 255), (REF_TEVREG, reg, PART_ALPHA, 4))
    else:
        t = constant(clamp(r), clamp(g), clamp(b), (REF_TEVREG, reg, PART_RGB, 0))
        over = constant(clamp(r - 255), clamp(g - 255), clamp(b - 255), (REF_TEVREG, reg, PART_RGB, 4))
    return t.copy(over=over) if max(values) > 255 else t


def over_half(t):
    """Half of a register's part over 255 (negate 5) or under 0 (negate 6, from its negate 1 term)."""
    kind, reg, part, negate = t.ref
    return Term(SRC_CONSTANT, 0, tuple(v // 2 for v in t.value), (kind, reg, part, 6 if negate == 1 else 5))


def signed_half_term(values, rgba, reg, part):
    """The register plus 128 (negate 3), or None when some value lies outside -128..127."""
    if min(values) < -128 or max(values) > 127:
        return None
    r, g, b, a = rgba
    if part:
        return constant_alpha(clamp(a + 128), (REF_TEVREG, reg, PART_ALPHA, 3))
    return constant(clamp(r + 128), clamp(g + 128), clamp(b + 128), (REF_TEVREG, reg, PART_RGB, 3))


def konst_color(sel, inp):
    if sel < 8:
        v = KONST_FRACTIONS[sel]
        return one() if sel == 0 else constant(v, v, v)
    k = inp.mat["konst_colors"][sel & 3]
    if 0x0C <= sel <= 0x0F:
        return constant(k[0], k[1], k[2], (REF_KONST, sel & 3, PART_RGB, 0))
    if sel >= 0x10:
        c = (sel - 0x10) >> 2
        return constant(k[c], k[c], k[c], (REF_KONST, sel & 3, [PART_R, PART_G, PART_B, PART_ALPHA][c], 0))
    return one()


def konst_alpha(sel, inp):
    if sel < 8:
        return one() if sel == 0 else constant_alpha(KONST_FRACTIONS[sel])
    k = inp.mat["konst_colors"][sel & 3]
    if sel >= 0x10:
        c = (sel - 0x10) >> 2
        return constant_alpha(k[c], (REF_KONST, sel & 3, [PART_R, PART_G, PART_B, PART_ALPHA][c], 0))
    return one()


def swapped_color_op(ch):
    if ch[0] == 0 and ch[1] == 1 and ch[2] == 2:
        return OP_RGB_COLOR
    if ch[0] == ch[1] == ch[2]:
        return COLOR_CHANNEL_OP[ch[0]]
    return OP_RGB_COLOR


def color_input(arg, inp, low_byte=False):
    if arg in (0, 2, 4, 6):
        return register_term(arg // 2, False, False, inp, low_byte)
    if arg in (1, 3, 5, 7):
        return register_term(arg // 2, True, False, inp, low_byte)
    if arg == 8:
        return one() if inp.unit < 0 else source(SRC_TEXTURE0 + inp.unit, swapped_color_op(inp.tex_swap))
    if arg == 9:
        return one() if inp.unit < 0 else source(SRC_TEXTURE0 + inp.unit, COLOR_CHANNEL_OP[inp.tex_swap[3]])
    if arg == 10:
        return zero() if inp.ras_zero else source(SRC_PRIMARY, swapped_color_op(inp.ras_swap))
    if arg == 11:
        return zero() if inp.ras_zero else source(SRC_PRIMARY, COLOR_CHANNEL_OP[inp.ras_swap[3]])
    if arg == 12:
        return one()
    if arg == 13:
        return half(False)
    if arg == 14:
        return konst_color(inp.kcsel, inp)
    return zero()


def alpha_input(arg, inp, low_byte=False):
    if arg <= 3:
        return register_term(arg, True, True, inp, low_byte)
    if arg == 4:
        return one() if inp.unit < 0 else source(SRC_TEXTURE0 + inp.unit, ALPHA_CHANNEL_OP[inp.tex_swap[3]])
    if arg == 5:
        return zero() if inp.ras_zero else source(SRC_PRIMARY, ALPHA_CHANNEL_OP[inp.ras_swap[3]])
    if arg == 6:
        return konst_alpha(inp.kasel, inp)
    return zero()


# ##############################################################################################
# # Combiner stages                                                                            #
# #                                                                                            #
# # A compiled stage holds a colour half and an alpha half side by side; the helpers below     #
# # pack the halves of consecutive TEV stages into as few combiner stages as possible.         #
# ##############################################################################################
class Stage:
    def __init__(self):
        self.color_src = [SRC_PREVIOUS] * 3
        self.color_op = [0] * 3
        self.alpha_src = [SRC_PREVIOUS] * 3
        self.alpha_op = [0] * 3
        self.color_func = FUNC_REPLACE
        self.alpha_func = FUNC_REPLACE
        self.color_scale = 0
        self.alpha_scale = 0
        self.color_ref = None
        self.alpha_ref = None
        self.constant = [0, 0, 0, 0]

    def store(self, h, alpha):
        """Writes a half; False when its constant differs from the one the stage holds."""
        exact = True
        if not alpha and self.color_ref is None:
            # A colour constant takes the constant's colour before a scalar would (is_scalar).
            for t in h.t[: h.count]:
                if t.src == SRC_CONSTANT and not is_scalar(t):
                    self.color_ref = (t.value, t.ref)
                    self.constant[0:3] = list(t.value)
                    break
        for i in range(3):
            t = h.t[i if i < h.count else 0]
            src, op = t.src, t.op
            if i >= h.count:
                src, op = SRC_PREVIOUS, 0
            elif src == SRC_CONSTANT:
                if alpha:
                    key = (t.value[0], t.ref)
                    if self.alpha_ref is not None and self.alpha_ref != key:
                        exact = False
                    else:
                        self.alpha_ref = key
                        self.constant[3] = t.value[0]
                    op = 1 if t.op & 1 else OP_A_ALPHA
                else:
                    key = (t.value, t.ref)
                    op = 1 if t.op & 1 else OP_RGB_COLOR
                    if self.color_ref is not None and self.color_ref != key:
                        # A scalar reads the constant's alpha while the alpha half leaves it free.
                        alpha_key = (t.value[0], t.ref)
                        if is_scalar(t) and (self.alpha_ref is None or self.alpha_ref == alpha_key):
                            self.alpha_ref = alpha_key
                            self.constant[3] = t.value[0]
                            op = OP_RGB_ALPHA | (t.op & 1)
                        else:
                            exact = False
                    else:
                        self.color_ref = key
                        self.constant[0:3] = list(t.value)
            if alpha:
                self.alpha_src[i], self.alpha_op[i] = src, op
            else:
                self.color_src[i], self.color_op[i] = src, op
        if alpha:
            self.alpha_func, self.alpha_scale = h.func, h.scale
        else:
            self.color_func, self.color_scale = h.func, h.scale
        return exact

    def pack(self):
        def ref(r):
            return bytes(r[1]) if r is not None else bytes(4)

        return (
            bytes(self.color_src + self.color_op + self.alpha_src + self.alpha_op)
            + bytes([self.color_func, self.alpha_func, self.color_scale, self.alpha_scale])
            + ref(self.color_ref)
            + ref(self.alpha_ref)
            + bytes(self.constant)
        )


def reads_previous_alpha(h):
    return any(t.previous and t.op in (OP_RGB_ALPHA, OP_RGB_ALPHA | 1) for t in h.t[: h.count])


def delayed_alpha(color, alpha):
    """A TEV stage's colour and alpha both read the registers as they were before it. A colour
    half after the first that reads the previous alpha must see that alpha, so the alpha halves
    start no earlier than the last such half (the Lumas' (CPREV + lerp(APREV, RASC, APREV)) * 4:
    its second combiner read the alpha the stage had already changed)."""
    late = [i for i in range(1, len(color)) if reads_previous_alpha(color[i])]
    if late:
        return [Half(FUNC_REPLACE, previous_term(True))] * late[-1] + alpha
    return alpha


def store_halves(color, alpha):
    """Combiner stages for a list of colour halves and one of alpha halves; (stages, exact)."""
    stages = []
    exact = not any(h.inexact for h in color + alpha)
    for i in range(max(len(color), len(alpha))):
        st = Stage()
        # The alpha first: a colour half may then take the constant's alpha for a scalar it reads.
        exact &= st.store(alpha[i] if i < len(alpha) else Half(FUNC_REPLACE, previous_term(True)), True)
        exact &= st.store(color[i] if i < len(color) else Half(FUNC_REPLACE, previous_term(False)), False)
        stages.append(st)
    return stages, exact


# ##############################################################################################
# # More passes                                                                                #
# #                                                                                            #
# # A material that needs more than the PICA's 6 combiner stages is split into passes, each    #
# # drawn over the last with a blending that adds or multiplies its result into the frame      #
# # buffer the way the TEV would have combined it.                                             #
# ##############################################################################################
# Framebuffer operations of the passes after the first (blend equation, source, destination):
# the framebuffer holds the colour so far, the pass draws src.
FB_MUL = (BLEND_ADD, FACTOR_ZERO, FACTOR_SRC_COLOR)         # dst * src
FB_MUL1P = (BLEND_ADD, FACTOR_DST_COLOR, FACTOR_ONE)        # dst * (1 + src)
FB_ADD = (BLEND_ADD, FACTOR_ONE, FACTOR_ONE)                # dst + src
FB_RSUB = (BLEND_REVERSE_SUBTRACT, FACTOR_ONE, FACTOR_ONE)  # dst - src
FB_SUB = (BLEND_SUBTRACT, FACTOR_ONE, FACTOR_ONE)           # src - dst
FB_REPLACE = (BLEND_ADD, FACTOR_ONE, FACTOR_ZERO)           # src


def is_keep(halves, alpha):
    """True when the halves only keep the previous colour (alpha)."""
    if len(halves) != 1 or halves[0].func != FUNC_REPLACE or halves[0].scale != 0:
        return False
    t = halves[0].t[0]
    return t.src == SRC_PREVIOUS and t.op == (OP_A_ALPHA if alpha else OP_RGB_COLOR)


def is_prev_color(t):
    return t.previous and t.op == OP_RGB_COLOR


def is_zero_value(halves):
    return len(halves) == 1 and halves[0].func == FUNC_REPLACE and halves[0].t[0].zero


def stage_ops(color_env, inp):
    """A TEV stage's colour (d +/- lerp(a, b, c) + bias) * scale as framebuffer operations on
    the previous colour P: [(FB_*, colour halves)], or None.

    No src reads P (the framebuffer holds it); one may read the previous alpha (pass_stages
    computes it again). The lerp's coefficient of P is (1 - c) for a, c for b, b - a for c.
    Each operation clamps to [0, 1]; for one stage's adds, subtracts, bias and scale in that
    order this matches the TEV's final clamp in the usual cases."""
    ca, cb, cc, cd, cop, cbias, cscale, _, _ = color_env
    if cop not in (0, 1):
        return None
    a, b, c, d = color_input(ca, inp, True), color_input(cb, inp, True), color_input(cc, inp, True), color_input(cd, inp)
    ka, kb, kc, kd = (is_prev_color(t) for t in (a, b, c, d))
    a0 = zero() if ka else a
    b0 = zero() if kb else b
    d0 = zero() if kd else d
    sub = cop == 1

    def value(x, y, z, w):  # a lerp without P, as halves
        return build_halves(x, y, z, w, 0, False, 0, False)

    def single(term):
        return [Half(FUNC_REPLACE, term)]

    def d_ops(sub):
        """d added (taken off for sub) in its own operations: a register below zero in part or
        whole takes its part under zero off (Koopa's Tail_v: P + C0, C0 = (60, -100, -100))."""
        if d.below_zero is not None:
            above = d.copy(below_zero=None, signed_half=None, over=None)
            return [(FB_SUB if sub else FB_ADD, single(above)), (FB_RSUB, single(d.below_zero))]
        if d.negative:
            return [(FB_REPLACE, single(zero()))] if sub else [(FB_RSUB, single(d.copy(negative=False, signed_half=None)))]
        return [(FB_SUB if sub else FB_ADD, single(d))]

    signed_d = not kd and (d.below_zero is not None or d.negative)
    if signed_d:
        d0 = zero()
    ops = []
    y_args = None  # the inputs of the last FB_ADD's y, lerp(a, b, c) + d, when it is one
    if kc:
        if ka or kb or kd:
            return None
        # lerp(a, b, P) = a + P * (b - a)
        both_constant = a.src == SRC_CONSTANT and b.src == SRC_CONSTANT
        if a.zero:
            if not b.one:
                ops.append((FB_MUL, single(b)))
        elif both_constant and all(bv >= av for av, bv in zip(a.value, b.value)):
            ops.append((FB_MUL, [Half(FUNC_REPLACE, b), Half(FUNC_SUBTRACT, previous_term(False), a, None, 2)]))
            ops.append((FB_ADD, single(a)))
        elif both_constant and all(bv <= av for av, bv in zip(a.value, b.value)):
            ops.append((FB_MUL, [Half(FUNC_REPLACE, a), Half(FUNC_SUBTRACT, previous_term(False), b, None, 2)]))
            ops.append((FB_SUB, single(a)))
        else:
            return None
        if not d.zero:
            ops += d_ops(sub)
    elif not sub:
        if ka and kb:
            y_args = (zero(), zero(), zero(), d0)
            coefficient = one()
        elif ka:
            y_args = (zero(), b0, c, d0)
            coefficient = inverse(c)
        elif kb:
            y_args = (a0, zero(), c, d0)
            coefficient = c
        else:
            y_args = (a, b, c, d0)
            coefficient = zero()
        y = value(*y_args)
        if kd:  # P * (1 + coefficient)
            if not coefficient.zero:
                ops.append((FB_MUL1P, single(coefficient)))
        elif coefficient.zero:
            ops.append((FB_REPLACE, y))
            y = None
        elif not coefficient.one:
            ops.append((FB_MUL, single(coefficient)))
        if y is not None and not is_zero_value(y):
            ops.append((FB_ADD, y))
        if signed_d:
            ops += d_ops(False)
            y_args = None
    else:
        # d - lerp
        if ka and kb:
            coefficient, y = one(), None
        elif ka:
            coefficient, y = inverse(c), value(zero(), b0, c, zero())
        elif kb:
            coefficient, y = c, value(a0, zero(), c, zero())
        else:
            coefficient, y = zero(), value(a, b, c, zero())
        if kd:  # P * (1 - coefficient) - y
            if coefficient.one:
                ops.append((FB_REPLACE, single(zero())))
            elif not coefficient.zero:
                ops.append((FB_MUL, single(inverse(coefficient))))
            if y is not None and not is_zero_value(y):
                ops.append((FB_RSUB, y))
        elif coefficient.zero:
            ops.append((FB_REPLACE, build_halves(a, b, c, d, 0, True, 0, False)))
        else:  # d - (P * coefficient + y)
            if not coefficient.one:
                ops.append((FB_MUL, single(coefficient)))
            if y is not None and not is_zero_value(y):
                ops.append((FB_ADD, y))
            ops.append((FB_SUB, single(d)))

    if cscale == 3 and kd and not signed_d and len(ops) == 1 and ops[0][0] == FB_ADD:
        # (P + y + bias) / 2: P halved first, then y / 2 and the bias / 2 (IceMountainPlanet's
        # (P + lerp - 1/2) / 2; P + y first clamped at 1 a sum the halving brings back).
        halved = ops[0][1] + [Half(FUNC_MODULATE, previous_term(False), half(False), None, 2)]
        ops = [(FB_MUL, single(constant(128, 128, 128))), (FB_ADD, halved)]
        if cbias == 1:
            ops.append((FB_ADD, single(constant(64, 64, 64))))
        elif cbias == 2:
            ops.append((FB_RSUB, single(constant(64, 64, 64))))
        return ops
    if cbias == 2 and cscale == 0 and len(ops) == 2 and ops[0][0] == FB_MUL1P and ops[1][0] == FB_ADD:
        # P * (1 + k) + y - 1/2 (TicoComet's P + lerp(P, 1, tex) - 1/2): P * (1 + k) passes 1 before
        # the bias. At half scale nothing clamps early: P * (1 - (1 - k) / 2), + max(y / 2 - 1/4, 0),
        # - max(1/4 - y / 2, 0), then doubled.
        k = ops[0][1][0].t[0]
        prev = previous_term(False)
        quarter = constant(64, 64, 64)
        y = ops[1][1]
        halved = y + [Half(FUNC_MODULATE, prev, half(False), None, 2)]
        return [(FB_MUL, [Half(FUNC_MODULATE, inverse(k), half(False), None, 2), Half(FUNC_REPLACE, inverse(prev))]),
                (FB_ADD, halved + [Half(FUNC_SUBTRACT, prev, quarter, None, 2)]),
                (FB_RSUB, halved + [Half(FUNC_SUBTRACT, quarter, prev, None, 2)]),
                (FB_MUL1P, single(one()))]
    if cbias == 1:
        ops.append((FB_ADD, single(constant(128, 128, 128))))
    elif cbias == 2 and ops and ops[-1][0] == FB_ADD and len(ops[-1][1]) < MAX_STAGES:
        # P + y - 1/2 clamped once: add max(y - 1/2, 0), then take off max(1/2 - y, 0) (each
        # framebuffer operation clamps; adding y and taking 1/2 off after lost a sum over 1).
        # max(y - 1/2, 0) is built with its bias where y is known (y over 1 clamped first lost it:
        # Peach's tex + ras / 2 - 1/2); max(1/2 - y, 0) needs y below 1/2 only.
        y = ops[-1][1]
        if y_args is not None:
            ops[-1] = (FB_ADD, build_halves(*y_args, 2, False, 0, False))
        else:
            ops[-1] = (FB_ADD, y + [Half(FUNC_ADD_SIGNED, previous_term(False), zero(), None, 2)])
        ops.append((FB_RSUB, y + [Half(FUNC_ADD_SIGNED, inverse(previous_term(False)), zero(), None, 2)]))
    elif cbias == 2:
        ops.append((FB_RSUB, single(constant(128, 128, 128))))
    if cscale == 1:
        ops.append((FB_MUL1P, single(one())))
    elif cscale == 2:
        ops += [(FB_MUL1P, single(one()))] * 2
    elif cscale == 3:
        ops.append((FB_MUL, single(constant(128, 128, 128))))
    return ops


def pass_stages(color, alpha_chain):
    """(stages, exact) of a later pass drawing the colour halves, or None when they do not fit.
    A half reading the previous alpha gets it from the alpha halves of the TEV so far,
    computed again in front of it (alpha halves never read colours)."""
    reads_alpha = any(t.previous and t.op in (OP_RGB_ALPHA, OP_RGB_ALPHA | 1) for h in color for t in h.t[: h.count])
    if reads_alpha:
        color = [Half(FUNC_REPLACE, previous_term(False))] * len(alpha_chain) + color
        alpha = alpha_chain
    else:
        alpha = [Half(FUNC_REPLACE, one())]
    if max(len(color), len(alpha)) > MAX_STAGES:
        return None
    return store_halves(color, alpha)


def forwarded_input(env, alpha, inp):
    """The input a stage copies unchanged into a register other than PREV (RosettaHairMat's
    C2 = TEXC while PREV holds the colour so far), or None. The combiners have one previous
    value; such a register costs none: the stages after read the input itself."""
    a, b, c, d, op, bias, scale, _, reg = env
    if reg & 3 == 0 or op != 0 or bias != 0 or scale != 0:
        return None
    if alpha:
        h = build_halves(alpha_input(a, inp, True), alpha_input(b, inp, True), alpha_input(c, inp, True), alpha_input(d, inp), 0, False, 0, True)
    else:
        h = build_halves(color_input(a, inp, True), color_input(b, inp, True), color_input(c, inp, True), color_input(d, inp), 0, False, 0, False)
    if len(h) != 1 or h[0].func != FUNC_REPLACE or h[0].scale or h[0].inexact:
        return None
    t = h[0].t[0]
    if t.previous or t.negative or t.below_zero is not None or t.src == SRC_BUFFER:
        return None
    return t.copy(stage_input=False)


def red_as_alpha(arg, inp):
    """The red component of TEV colour input arg (an alpha input's own value) as an alpha term,
    as a compare stage reads it (GX_TEV_COMP_R8_*: the 8 low bits)."""
    if arg == 8 or arg == 9:
        if inp.unit < 0:
            return one()
        return source(SRC_TEXTURE0 + inp.unit, ALPHA_CHANNEL_OP[inp.tex_swap[0 if arg == 8 else 3]])
    if arg == 10 or arg == 11:
        if inp.ras_zero:
            return zero()
        return source(SRC_PRIMARY, ALPHA_CHANNEL_OP[inp.ras_swap[0 if arg == 10 else 3]])
    t = color_input(arg, inp, True)
    if t.src == SRC_CONSTANT:
        if t.negative or t.below_zero is not None or t.over is not None:
            return None
        kind, index, part, negate = t.ref
        return constant_alpha(t.value[0], (kind, index, PART_R if part == PART_RGB else part, negate))
    if t.src == SRC_BUFFER or t.op & 1:
        return None
    return t.copy(op={OP_RGB_COLOR: OP_A_R, OP_RGB_ALPHA: OP_A_ALPHA, OP_RGB_R: OP_A_R, OP_RGB_G: OP_A_G,
                      OP_RGB_B: OP_A_B}[t.op], previous=False)


def scalar_compare(mat, s):
    """Whether TEV stage s compares, greater than, two inputs whose red, green and blue are the
    same value (alpha inputs, ONE, HALF, ZERO, a konst fraction or one konst component): then
    GX_TEV_COMP_GR16_GT, BGR24_GT and RGB8_GT (each channel alike) decide as R8_GT does
    (SunshineMario's TEXA > K0_A)."""
    (ca, cb, _, _, cop, _, _, _, _), _ = mat["stages"][s]
    sel = mat["kcsel"][s]

    def scalar(arg):
        return arg in (1, 3, 5, 7, 9, 11, 12, 13, 15) or (arg == 14 and (sel < 8 or 0x10 <= sel <= 0x1F))
    return cop in (10, 12, 14) and scalar(ca) and scalar(cb)


def indirect_textures(data, block, size):
    """TEX1 indices of the textures the model's materials read as indirect textures, at least once."""
    return {m["tex_numbers"][m["ind_map"]] for m in parse(data, block, size)
            if m["indirect"] and m["ind_map"] < 8 and m["tex_numbers"][m["ind_map"]] != 0xFFFF}


def indirect_only_textures(data, block, size):
    """TEX1 indices of the textures the model's materials read only as indirect textures."""
    indirect, other = set(), set()
    for mat in parse(data, block, size):
        if mat["indirect"] and mat["ind_map"] < 8 and mat["tex_numbers"][mat["ind_map"]] != 0xFFFF:
            indirect.add(mat["tex_numbers"][mat["ind_map"]])
        for _, tex_map, _ in mat["orders"][:mat["stage_count"]]:
            if tex_map < 8 and mat["tex_numbers"][tex_map] != 0xFFFF:
                other.add(mat["tex_numbers"][tex_map])
    return indirect - other


def compare_stage(mat, s, inp, room):
    """A TEV compare stage d + (a.r > b.r ? c : 0) (GX_TEV_COMP_R8_GT; the electric rails' sparks,
    the crystals' vanishing edge, some water): the 3DS has no comparison, so the alpha halves
    build a mask, (a.r - b.r) * 4 four times over, which saturates to exactly 0 or 1 for 8-bit
    values, while the colour halves keep the previous colour; the last combiner adds c times the
    mask to d and computes the stage's own alpha. None when the stage does not fit that form.
    The previous colour and the mask are read from the combiner before, not as the TEV stage's
    previous value (previous=False keeps the buffer and delayed-alpha rules off them)."""
    (ca, cb, cc, cd, cop, cbias, cscale, cclamp, creg), (aa, ab, ac, ad, aop, abias, ascale, aclamp, areg) = mat["stages"][s]
    if (cop != 8 and not scalar_compare(mat, s)) or aop >= 2:
        return None
    a, b = red_as_alpha(ca, inp), red_as_alpha(cb, inp)
    if a is None or b is None:
        return None
    c, d = color_input(cc, inp, True), color_input(cd, inp)
    for t in (c, d):
        if t.negative or t.below_zero is not None or t.over is not None or t.src == SRC_BUFFER:
            return None
    alpha = build_halves(alpha_input(aa, inp, True), alpha_input(ab, inp, True), alpha_input(ac, inp, True), alpha_input(ad, inp),
                         abias, aop == 1, ascale, True)
    # A stage that keeps the alpha (KoopaPlateDamage's glass, alpha-tested): the combiner before the
    # mask saves it in the buffer, the last one takes it back. The buffer reaches combiners 1-4.
    keeps = (aa, ab, ac, ad, aop, abias, ascale) in ((7, 7, 7, 0, 0, 0, 0), (0, 7, 7, 7, 0, 0, 0)) and 2 <= room <= 5
    if not keeps and (len(alpha) != 1 or any(t.previous or t.src == SRC_PREVIOUS for t in alpha[0].t[: alpha[0].count])):
        return None  # the stage's alpha must take one combiner and not read the alpha the mask replaced
    c, d = c.copy(previous=False), d.copy(previous=False)
    if room < 5:
        # The last stage of an additive material with d = 0 (the sparks: ONE, ONE): where the
        # comparison fails the pixel adds nothing, as if discarded. The alpha test discards it,
        # a.r - b.r > 0, in one combiner; the stage's own alpha (0) is not written.
        if room < 1 or s != mat["stage_count"] - 1 or not d.zero or mat["blend"][:3] != (1, 1, 1) or                 tuple(mat["alpha_comp"][i] for i in (0, 3)) != (7, 7) and mat["alpha_comp"][:4] != (4, 0, 0, 7):
            return None
        mat["alpha_comp"] = (4, 0, 0, 7, 0)  # GX_GREATER 0 AND always
        return [Half(FUNC_REPLACE, c)], [Half(FUNC_SUBTRACT, a, b, None, 2)]
    if c.src == SRC_CONSTANT and d.src == SRC_CONSTANT and not d.zero and constant_key(c, False) != constant_key(d, False):
        return None
    mask = Half(FUNC_SUBTRACT, a, b, None, 2)
    mask.scale = 2  # x4
    alpha_halves = [mask]
    for _ in range(3):
        h = Half(FUNC_REPLACE, source(SRC_PREVIOUS, OP_A_ALPHA))
        h.scale = 2
        alpha_halves.append(h)
    alpha_halves.append(Half(FUNC_REPLACE, Term(SRC_BUFFER, OP_A_ALPHA)) if keeps else alpha[0])
    inp.compare_saves_alpha = keeps
    keep = Half(FUNC_REPLACE, source(SRC_PREVIOUS, OP_RGB_COLOR))
    masked = source(SRC_PREVIOUS, OP_RGB_ALPHA)
    if d.zero:
        last = Half(FUNC_MODULATE, c, masked, None, 2)
    else:
        last = Half(FUNC_MULTIPLY_ADD, c, masked, d, 3)
    return [keep, keep, keep, keep, last], alpha_halves


def stage_setup(mat, s, inp, units):
    """Binds stage s's texture to a unit (added when there is room) and sets its inputs.
    Returns (rasterized channel or 7, texture bound)."""
    tex_coord, tex_map, channel = mat["orders"][s]
    inp.unit = -1
    bound = True
    if tex_map != 0xFF:
        for u, (m, c) in enumerate(units):
            if m == tex_map and c == tex_coord:
                inp.unit = u
        if inp.unit < 0 and len(units) < MAX_UNITS:
            units.append((tex_map, tex_coord))
            inp.unit = len(units) - 1
        elif inp.unit < 0:
            bound = False  # read as 1 (tev_compile.cpp's approximation)
    # GXChannelID -> rasterized colour: COLOR0A0 (4) and COLOR1A1 (5), else zero.
    ras = {4: 0, 5: 1, 0: 0, 1: 1, 2: 0, 3: 1}.get(channel, 7)
    inp.ras_zero = ras == 7
    ras_sel, tex_sel = mat["swap_modes"][s]
    inp.tex_swap = mat["swap_tables"][tex_sel]
    inp.ras_swap = mat["swap_tables"][ras_sel]
    inp.kcsel = mat["kcsel"][s]
    inp.kasel = mat["kasel"][s]
    return ras, bound


def pass_channel_allowed(mat, ch):
    """A later pass can light rasterized channel ch itself (the GPU lights each draw's channel)
    when its material and ambient colours come from registers: the vertices hold the first
    pass's channel colour only."""
    if mat["chan_count"] <= ch:
        return False
    return all(c[1] & 1 == 0 and c[5] & 1 == 0 for c in mat["chans"][2 * ch: 2 * ch + 2])


def multipass_allowed(mat):
    """Passes after the first rebuild the colour in the framebuffer: the first pass must leave
    its unblended result there (opaque), and the later ones find their pixels by depth."""
    enable, _, update = mat["zmode"]
    return mat["blend"][0] == 0 and enable and update


# ##############################################################################################
# # Compiling a material                                                                       #
# #                                                                                            #
# # Stage by stage: the inputs, each TEV function as halves, comparisons and indirect stages,  #
# # then the halves packed into combiner stages and passes. When the result does not fit or is #
# # not exact, simpler forms are tried (folding constants, reordering terms); the record says  #
# # when the material is inexact. tevcheck.py checks the result against the Wii's TEV.         #
# ##############################################################################################
def compile_material(mat, chains=True, fixed=False):
    """(stages, units, ras channel, inexact, passes, buffer masks) for a decoded material; passes
    are (stages, blend, rasterized channel, units).

    The TEV stages that fit in MAX_STAGES combiners make the first pass. The rest, for an
    opaque depth-writing material whose textures fit in MAX_UNITS units, become framebuffer
    passes (affine_passes; each a list of stages and a blend). Otherwise the material keeps
    the stages that fit (inexact)."""
    stages = []
    units = []  # (texture map, texture coordinate)
    passes = []
    ras_channel = 0
    ras_chosen = False
    inexact = False
    in_passes = False
    alpha_chain = []  # alpha halves of the TEV so far (pass_stages)
    inp = Inputs(mat)
    inp.fixed = fixed
    global _buffer_half, _inputs_late
    _buffer_half, _inputs_late = True, False
    buffer_rgb = buffer_alpha = 0  # stages whose result the combiner buffer takes (bit per stage)
    chained = None  # an unclamped stage's half in the buffer: its offset (unclamped_chain)
    alpha_chained = False  # an unclamped alpha's half in the buffer (halved_alpha_chain)
    chain_d = (0, 0, 0)  # d of the "signed" chain's first stage (chain_offset)
    offset_chain = None  # (K, last stage, first stage, register) of alpha_offset_chain
    factor = None  # [x, y, swapped, R, k, register PREV holds] of factored_lerp
    park_at = None  # the combiner whose result the buffer keeps for a register (Inputs.parked)
    pass_alpha = []  # alpha halves of stages in later passes (None: they cannot go in the first)
    for s in range(mat["stage_count"]):
        ras, bound = stage_setup(mat, s, inp, units)
        own = None  # (texture map, coordinate) of a stage whose texture takes a unit in its passes
        if not bound and multipass_allowed(mat) and chained is None and not alpha_chained and factor is None:
            # A fourth texture (GreenPlanetOrbitSky): the stage goes to later passes, which bind it
            # in a unit of their own (own_unit).
            tex_coord, tex_map, _ = mat["orders"][s]
            own = (tex_map, tex_coord)
            inp.unit = MAX_UNITS  # reads OWN_UNIT
            in_passes = True
        elif not bound:
            inexact = True
        if ras != 7 and not ras_chosen:
            ras_channel = ras
            ras_chosen = True
        elif ras != 7 and ras != ras_channel:
            # One rasterized channel per pass: the stages from this one on go to later passes
            # lit with their own (Bakky's and Peach's specular COLOR1 added to COLOR0's colour).
            if not in_passes and chained is None and not alpha_chained and multipass_allowed(mat) and \
                    pass_channel_allowed(mat, ras):
                in_passes = True
            elif not in_passes or not pass_channel_allowed(mat, ras):
                inexact = True

        color_env, alpha_env = mat["stages"][s]
        # A colour or alpha copied unchanged into a register other than PREV is read from its input.
        forward_color = forwarded_input(color_env, False, inp) if color_env[4] < 8 else None
        forward_alpha = forwarded_input(alpha_env, True, inp) if alpha_env[4] < 8 else None
        if (forward_color is not None or forward_alpha is not None) and \
                (forward_color is not None and forward_alpha is not None or inp.written[0]):
            if forward_color is not None:
                inp.forward[(color_env[8] & 3, False)] = forward_color
                color_env = (15, 15, 15, 0, 0, 0, 0, 1, 0)  # keeps CPREV
            if forward_alpha is not None:
                inp.forward[(alpha_env[8] & 3, True)] = forward_alpha
                alpha_env = (7, 7, 7, 0, 0, 0, 0, 1, 0)  # keeps APREV
            if forward_color is not None and forward_alpha is not None:
                continue
        ca, cb, cc, cd, cop, cbias, cscale, _, creg = color_env
        aa, ab, ac, ad, aop, abias, ascale, _, areg = alpha_env
        if factor is not None and s > factor[4]:
            factor = None
        if factor is None and not in_passes and chained is None and not alpha_chained and park_at is None:
            found = factored_lerp(mat, s)
            r_reg = found[1] // 2 if found is not None else 0
            if found is not None and (not inp.written[r_reg] or inp.prev_color_reg == r_reg) and \
                    inp.parked[False] is None:
                # x and y read where their stages bind them (textures, the rasterized colour)
                factor = [product_input(mat, s, inp), None, found[0], found[1], found[2], inp.prev_color_reg]
        elif factor is not None and s == factor[4] - 1:
            factor[1] = product_input(mat, s, inp)
        if not in_passes and chained is None and not alpha_chained and factor is None:
            # A stage writing another register than the one the combiners' previous value holds,
            # which a later stage reads: the combiner before the stage saves it in the buffer.
            at = len(stages) - 1
            for alpha, written in ((False, creg & 3), (True, areg & 3)):
                old = inp.prev_alpha_reg if alpha else inp.prev_color_reg
                if written != old and inp.written[old] and inp.parked[alpha] is None and \
                        park_at in (None, at) and 0 <= at <= 3 and register_read_later(mat, s, old, alpha):
                    inp.parked[alpha] = old
                    park_at = at
                    _buffer_half = False
                    if alpha:
                        buffer_alpha |= 1 << at
                    else:
                        buffer_rgb |= 1 << at
        parking = any(v is not None for v in inp.parked.values())
        if offset_chain is not None and s > offset_chain[1]:
            offset_chain = None
        if offset_chain is None and not in_passes:
            found = alpha_offset_chain(mat, s)
            offset_chain = (found[0], found[1], s, found[2]) if found is not None else None
        if not in_passes:
            def build_stage():
                if (cop == 8 or scalar_compare(mat, s)) and offset_chain is None:
                    found = compare_stage(mat, s, inp, MAX_STAGES - len(stages))
                    if found is not None:
                        return found
                if cop >= 8:  # compare mode: keep d (an approximation)
                    inp.inexact = True
                    color = build_halves(zero(), zero(), zero(), color_input(cd, inp), 0, False, 0, False)
                else:
                    color = folded_stage(color_env, False, inp) or build_halves(color_input(ca, inp, True), color_input(cb, inp, True), color_input(cc, inp, True), color_input(cd, inp), cbias, cop == 1, cscale, False)
                if aop >= 8:
                    alpha = build_halves(zero(), zero(), zero(), alpha_input(ad, inp), 0, False, 0, True)
                elif offset_chain is not None and s == offset_chain[2]:
                    alpha = build_halves(zero(), zero(), zero(), zero(), 0, False, 0, True)  # K comes last
                elif offset_chain is not None and s == offset_chain[1]:
                    k = offset_chain[0]
                    alpha = build_halves(alpha_input(aa, inp, True), alpha_input(ab, inp, True), alpha_input(ac, inp, True), alpha_input(ad, inp), 0, False, 0, True)
                    alpha.append(Half(FUNC_ADD if k > 0 else FUNC_SUBTRACT, previous_term(True), constant_alpha(abs(k), (REF_TEVREG, offset_chain[3], PART_ALPHA, 1 if k < 0 else 0)), None, 2))
                    alpha[-1].scale = ascale
                else:
                    alpha = folded_stage(alpha_env, True, inp) or build_halves(alpha_input(aa, inp, True), alpha_input(ab, inp, True), alpha_input(ac, inp, True), alpha_input(ad, inp), abias, aop == 1, ascale, True)
                return color, alpha

            inexact_before = inp.inexact
            color, alpha = build_stage()
            factoring = factor is not None and s <= factor[4]
            if factoring and s < factor[4]:
                color = [Half(FUNC_REPLACE, previous_term(False))]  # keeps R (factored_lerp)
            elif factoring:
                x, y = (factor[1], factor[0]) if factor[2] else (factor[0], factor[1])
                d_term = color_input(cd, inp)
                at = len(stages) - 1
                held = inp.written[factor[3] // 2]
                if d_term.negative or d_term.below_zero is not None or d_term.over is not None or \
                        held and not 0 <= at <= 3:
                    inexact = True
                else:
                    if held:
                        # R, the previous colour, goes to the buffer before the lerp replaces it.
                        buffer_rgb |= 1 << at
                        _buffer_half = False
                        r_term = source(SRC_BUFFER, OP_RGB_COLOR)
                    else:
                        r_term = color_input(factor[3], inp, True)
                    color = build_halves(x, y, color_input(cc, inp, True), zero(), 0, False, 0, False) + \
                        [Half(FUNC_MULTIPLY_ADD, previous_term(False), r_term, d_term, 3)]
                    color[-1].scale = cscale
                    inp.inexact = inexact_before  # the stage's own form read Ra and Rb, gone
            chain_start = None
            chain_tail_stage = chained is not None
            if chained is not None:
                # The stage after an unclamped one: 2 * (lerp / 2 + H) + bias - offset, H from the buffer.
                lerp = build_halves(color_input(ca, inp, True), color_input(cb, inp, True), color_input(cc, inp, True), zero(), 0, False, 0, False)
                if chained == "signed":
                    # lerp + (X - 1/2), lerp + X - 1 and (lerp + X) / 2 - 1/4, X in the buffer, clamped once.
                    buffer = Term(SRC_BUFFER, OP_RGB_COLOR)
                    prev = previous_term(False)
                    if cscale == 3:
                        # + d / 2 for a fixed d (RockPlanetOrbitSky's C0): + (d / 2 + 1/4) - 1/2.
                        offset = Half(FUNC_ADD_SIGNED, prev, constant(*((v >> 1) + 64 for v in chain_d)), None, 2) \
                            if any(chain_d) else Half(FUNC_SUBTRACT, prev, constant(64, 64, 64), None, 2)
                        color = lerp + [Half(FUNC_INTERPOLATE, prev, buffer, half(False), 3), offset]
                    elif cbias == 2:
                        color = lerp + [Half(FUNC_SUBTRACT, prev, inverse(buffer), None, 2)]
                    else:
                        color = lerp + [Half(FUNC_ADD_SIGNED, prev, buffer, None, 2)]
                else:
                    color = lerp + chain_tail(chained, -0.5 if cbias == 2 else 0.0)
                chained = None
            elif chains and not parking and not factoring and unclamped_chain(mat, s):
                # An unclamped stage whose sum a stage after it reads as d: its half, (d + lerp) / 2,
                # goes to the buffer (in range for sums up to 2), the next stage doubles it back.
                half_sum = build_halves(color_input(ca, inp, True), color_input(cb, inp, True), color_input(cc, inp, True), color_input(cd, inp), 0, False, 3, False)
                lerp = build_halves(color_input(ca, inp, True), color_input(cb, inp, True), color_input(cc, inp, True), zero(), 0, False, 0, False)
                d_fixed = chain_offset(mat, s, cd)
                if d_fixed is not None and (not any(d_fixed) or mat["stages"][s + 1][0][6] == 3) and cbias == 2 and \
                        0 <= len(stages) + len(lerp) - 1 <= 3:
                    chain_d = d_fixed
                    # lerp - 1/2 (AssemblyBlockParts' coreA_v, in -1/2..1/2): the lerp goes to the
                    # buffer, the next stage adds it with FUNC_ADD_SIGNED.
                    color = lerp
                    chain_start = "signed"
                elif mat["stages"][s + 1][0][6] == 0 and 0 <= len(stages) + len(half_sum) - 1 <= 3:
                    color = half_sum
                    chain_start = 0.5 if cbias == 2 else 0.0
            alpha_tail_stage = alpha_chained
            alpha_start = False
            if alpha_chained:
                # lerp / 2 + H, H = the alpha before doubled / 2 in the buffer, clamped once.
                alpha = build_halves(alpha_input(aa, inp, True), alpha_input(ab, inp, True), alpha_input(ac, inp, True), zero(), 0, False, 0, True)
                alpha.append(Half(FUNC_MULTIPLY_ADD, previous_term(True), half(True), Term(SRC_BUFFER, OP_A_ALPHA), 3))
                alpha_chained = False
            elif chains and not parking and not factoring and halved_alpha_chain(mat, s):
                lerp = build_halves(alpha_input(aa, inp, True), alpha_input(ab, inp, True), alpha_input(ac, inp, True), zero(), 0, False, 0, True)
                if 0 <= len(stages) + len(delayed_alpha(color, lerp)) - 1 <= 3:
                    alpha = lerp
                    alpha_start = True
            # A later combiner of the stage reading the stage's previous colour or alpha, or a sum
            # that may pass 1 before its bias or halving with d the previous value: the stage
            # inputs are saved in the combiner buffer by the combiner before (stages 0-3 can).
            save_at = len(stages) - 1
            d_previous = (cop < 8 and color_input(cd, inp).previous and (cbias == 2 or cscale == 3)) or \
                (aop < 8 and alpha_input(ad, inp).previous and (abias == 2 or ascale == 3)) or \
                (cop < 8 and cc in (0, 1) and {ca, cb} <= {2, 3, 4, 5, 6, 7, 14})  # lerp(register, register, PREV)
            if chain_start is None and not chain_tail_stage and not alpha_start and not alpha_tail_stage and 0 <= save_at <= 3 and \
                    park_at in (None, save_at) and not factoring and \
                    (late_inputs(color, False) or late_inputs(alpha, True) or d_previous):
                saved_half = _buffer_half
                _buffer_half, _inputs_late = False, True
                late_color, late_alpha = build_stage()
                _inputs_late = False
                rgb, alpha_needed = late_inputs(late_color, False), late_inputs(late_alpha, True)
                late_color, late_alpha = buffered(late_color, False), buffered(late_alpha, True)
                # Only when the buffered stage holds its constants (0.5 is one now).
                late_exact = store_halves(late_color, delayed_alpha(late_color, late_alpha))[1]
                if (rgb or alpha_needed) and late_exact:
                    color, alpha = late_color, late_alpha
                    buffer_rgb |= (1 << save_at) if rgb else 0
                    buffer_alpha |= (1 << save_at) if alpha_needed else 0
                else:
                    _buffer_half = saved_half
            stored_alpha = delayed_alpha(color, alpha)
            if stages and is_keep(color, False) and is_keep(alpha, True) and chain_start is None and not alpha_start:
                color, stored_alpha, alpha = [], [], []  # keeps both: no combiner (GrassMask's stage 2)
            if len(stages) + max(len(color), len(stored_alpha)) <= MAX_STAGES:
                if chain_start is not None:
                    buffer_rgb |= 1 << (len(stages) + len(color) - 1)
                    _buffer_half = False
                    chained = chain_start
                if alpha_start:
                    buffer_alpha |= 1 << (len(stages) + len(stored_alpha) - 1)
                    _buffer_half = False
                    alpha_chained = True
                if inp.compare_saves_alpha:
                    buffer_alpha |= 1 << (len(stages) - 1)
                    _buffer_half = False
                    inp.compare_saves_alpha = False
                more, exact = store_halves(color, stored_alpha)
                stages += more
                inexact |= not exact
                alpha_chain += alpha
                inp.written[creg & 3] = True
                inp.written[areg & 3] = True
                inp.forward.pop((creg & 3, False), None)
                inp.forward.pop((areg & 3, True), None)
                inp.prev_color_reg, inp.prev_alpha_reg = creg & 3, areg & 3
                if factoring and s < factor[4]:
                    inp.prev_color_reg = factor[5]  # the combiners still hold R
                for alpha, written in ((False, creg & 3), (True, areg & 3)):
                    reg = inp.parked[alpha]
                    if reg is not None and (reg == written or not register_read_later(mat, s, reg, alpha)):
                        inp.parked[alpha] = None
                continue
            if chained is not None or chain_tail_stage or alpha_tail_stage:
                # The stage after an unclamped one does not fit: compiled without the halving.
                return compile_material(mat, False, fixed)
            if not multipass_allowed(mat):
                inexact = True
                break
            in_passes = True

        if parking:
            inexact = True  # later passes have no buffer of the first pass
        ops = stage_ops(color_env, inp) if bound or own is not None else None
        built = [pass_stages(halves, alpha_chain) for _, halves in ops] if ops is not None else [None]
        if None in built or len(passes) + len(ops) > MAX_PASSES:
            inexact = True
            break
        channel = ras if ras != 7 else ras_channel
        if channel != ras_channel and any(t.src == SRC_PRIMARY for h in alpha_chain for t in h.t[: h.count]) and \
                any(reads_previous_alpha(h) for _, halves in ops for h in halves):
            inexact = True  # the pass computes the alpha so far again, from its own channel
        for (blend, _), (more, exact) in zip(ops, built):
            inexact |= not exact
            pass_units = units
            if own is not None and any(OWN_UNIT in st.color_src + st.alpha_src for st in more):
                pass_units = own_unit(mat, units, own, more)
                if pass_units is None:
                    inexact = True
                    pass_units = units
            passes.append((more, blend, channel, pass_units))
        if aop >= 8:
            new_alpha = build_halves(zero(), zero(), zero(), alpha_input(ad, inp), 0, False, 0, True)
        else:
            new_alpha = build_halves(alpha_input(aa, inp, True), alpha_input(ab, inp, True), alpha_input(ac, inp, True), alpha_input(ad, inp), abias, aop == 1, ascale, True)
        if own is not None and any(t.src == OWN_UNIT for h in new_alpha for t in h.t[: h.count]):
            inexact = True  # the first pass computes the alpha, without the pass's own texture
        alpha_chain += new_alpha
        # Later passes write colour only: the first pass computes the alpha of their stages too, in
        # its last combiners (place_alpha; AstroDomeEntrance's door, alpha-tested).
        keeps_alpha = (aa, ab, ac, ad, aop, abias, ascale) == (7, 7, 7, 0, 0, 0, 0)
        if areg & 3 != 0:
            keeps_alpha = True
            if register_read_later(mat, s, areg & 3, True):
                pass_alpha = None
        if not keeps_alpha and pass_alpha is not None:
            reads_ras = any(t.src == SRC_PRIMARY for h in new_alpha for t in h.t[: h.count])
            pass_alpha = pass_alpha + new_alpha if channel == ras_channel or not reads_ras else None
        inp.written[creg & 3] = True
        inp.written[areg & 3] = True
        inp.forward.pop((creg & 3, False), None)
        inp.forward.pop((areg & 3, True), None)
        inp.prev_color_reg, inp.prev_alpha_reg = creg & 3, areg & 3
    _buffer_half = True
    if pass_alpha:
        placed = place_alpha(stages, pass_alpha)
        if placed is not None:
            stages = placed
        elif fragment_state(mat)[3] != TEST_ALWAYS:
            inexact = True  # the alpha test sees the first pass's alpha
    elif pass_alpha is None and fragment_state(mat)[3] != TEST_ALWAYS:
        inexact = True
    if (inexact or inp.inexact) and not fixed:
        # Registers no animation sets taken as fixed values may save the combiners that were missing.
        retry = compile_material(mat, chains, True)
        if not retry[3]:
            return retry
        moved = additive_head_last(mat)
        if moved is not None:
            retry = compile_material(moved, chains)
            if not retry[3]:
                return retry
    stages, buffer_rgb, buffer_alpha = buffer_safe(stages, buffer_rgb, buffer_alpha)
    passes = [(buffer_safe(pst, 0, 0)[0],) + tuple(rest) for pst, *rest in passes]
    return stages, units, ras_channel, inexact or inp.inexact, passes, (buffer_rgb, buffer_alpha)


# ##############################################################################################
# # Folding and chaining                                                                       #
# #                                                                                            #
# # The rewrites compile_material tries to make a material fit: constants folded into a stage, #
# # alpha placed where it is free, lerps factored, chains of offsets and halvings collapsed.   #
# ##############################################################################################
def buffer_safe(stages, buffer_rgb, buffer_alpha):
    """(stages, buffer masks) whose first combiner does not read the combiner buffer. The buffer
    reaches a combiner one stage late and is 0 in the first one on the 3DS as Azahar computes it
    (sw_rasterizer.cpp WriteTevConfig), while these rules read its initial 0.5 from the first
    combiner on (half()): a leading combiner that changes nothing puts the reader second, where
    both behaviours give 0.5 (TicoComet, GlareGlow, ShockWaveGenerator: found by the material audit)."""
    if not stages or len(stages) >= MAX_STAGES or (buffer_rgb, buffer_alpha) != (0, 0):
        return stages, buffer_rgb, buffer_alpha
    first = stages[0]
    if SRC_BUFFER not in list(first.color_src) + list(first.alpha_src):
        return stages, buffer_rgb, buffer_alpha
    return [Stage()] + list(stages), buffer_rgb, buffer_alpha


def fixed_value(arg, alpha, inp, low_byte):
    """The value of a TEV stage input that no animation or earlier stage changes (Inputs.fixed),
    as a list of components (one for an alpha), or None."""
    if alpha:
        if arg == 7:
            return [0]
        if arg in (1, 2, 3):
            reg = arg
        elif arg == 6 and inp.kasel < 8:
            return [255 if inp.kasel == 0 else KONST_FRACTIONS[inp.kasel]]
        else:
            return None
    else:
        if arg in (12, 13, 15):
            return [{12: 255, 13: 128, 15: 0}[arg]] * 3
        if arg == 14 and inp.kcsel < 8:
            return [255 if inp.kcsel == 0 else KONST_FRACTIONS[inp.kcsel]] * 3
        if arg not in (2, 3, 4, 5, 6, 7):
            return None
        reg = arg // 2
    if not inp.fixed or inp.written[reg] or reg in inp.mat.get("animated_registers", {}) or \
            (reg, alpha or arg & 1 == 1) in inp.forward:
        return None
    rgba = inp.mat["tev_colors"][reg - 1]
    values = [rgba[3]] * (1 if alpha else 3) if alpha or arg & 1 else list(rgba[:3])
    return [v & 0xFF for v in values] if low_byte else values


def folded_stage(env, alpha, inp):
    """With fixed registers, (d +/- lerp(a, b, c) + bias) * scale of fixed a, b and d is a lerp of
    two fixed values where no component clamps between c = 0 and 1 (HoneyQueenWing's
    lerp(1, C0, tex) + C0, red and green always 1); None otherwise."""
    a, b, c, d, op, bias, scale, clamp_ = env[:8]
    if op >= 2:
        return None
    va, vb, vd = fixed_value(a, alpha, inp, True), fixed_value(b, alpha, inp, True), fixed_value(d, alpha, inp, False)
    if va is None or vb is None or vd is None:
        return None
    sign = -1 if op == 1 else 1
    offset = [0, 128, -128, 0][bias]
    factor = [1, 2, 4, 0.5][scale]
    vc = fixed_value(c, alpha, inp, True)
    if vc is not None:
        # All inputs fixed: one value (SunsetAir's alpha (0 + 1/2) / 2).
        out = []
        for i in range(len(va)):
            weight = vc[i] + (vc[i] >> 7)
            v = (vd[i] + sign * ((va[i] * (256 - weight) + vb[i] * weight) >> 8) + offset) * factor
            if not clamp_ and not 0 <= v <= 255:
                return None
            out.append(clamp(int(round(v))))
        return [Half(FUNC_REPLACE, constant_alpha(out[0]) if alpha else constant(*out))]
    if not inp.fixed:
        return None
    ends = ([], [])
    for i in range(len(va)):
        f0 = (vd[i] + sign * va[i] + offset) * factor
        f1 = (vd[i] + sign * vb[i] + offset) * factor
        if min(f0, f1) >= 255 and clamp_:
            f0 = f1 = 255
        elif max(f0, f1) <= 0 and clamp_:
            f0 = f1 = 0
        elif not (0 <= f0 <= 255 and 0 <= f1 <= 255):
            return None
        ends[0].append(int(round(f0)))
        ends[1].append(int(round(f1)))
    ca = constant_alpha(ends[0][0]) if alpha else constant(*ends[0])
    cb = constant_alpha(ends[1][0]) if alpha else constant(*ends[1])
    ct = alpha_input(c, inp, True) if alpha else color_input(c, inp, True)
    return build_halves(ca, cb, ct, zero(), 0, False, 0, alpha)


def place_alpha(stages, halves):
    """The first pass's combiners with alpha halves added in their free alpha slots at the end (and
    in new combiners), or None when they do not fit or a colour half after would read the alpha."""
    free = 0
    for st in reversed(stages):
        if st.alpha_src != [SRC_PREVIOUS] * 3 or st.alpha_op != [0] * 3 or st.alpha_func != FUNC_REPLACE or \
                st.alpha_scale != 0 or st.alpha_ref is not None:
            break
        free += 1
    start = len(stages) - free
    extra = max(0, len(halves) - free)
    if len(stages) + extra > MAX_STAGES:
        return None
    out = [copy.deepcopy(st) for st in stages] + [Stage() for _ in range(extra)]
    for st in out[start + 1:]:
        if any(src == SRC_PREVIOUS and op & ~1 == OP_RGB_ALPHA for src, op in zip(st.color_src, st.color_op)):
            return None
    for i, h in enumerate(halves):
        if not out[start + i].store(h, True):
            return None
    return out


def additive_head_last(mat):
    """The material with its TEV reordered so that a product added through a register comes last,
    or None. CrestModel's PREV = tex0 * ras, C2 = tex1 * k + PREV, PREV = ..., PREV = x + C2 (a
    fourth texture in x) is C2 = tex1 * k, PREV = ..., PREV = x + C2, PREV = tex0 * ras + PREV: a
    sum of values from 0 up clamped once at 1 either way. The product then goes to a later pass,
    whose own unit takes its texture (own_unit). All stages of the head's form are tried. (The
    3DS has no indirect texturing: CrestModel's indirect stage, which moves no stage, is left out
    either way.)"""
    n = mat["stage_count"]
    plain = set(range(8, 16))  # textures, the rasterized colour, constants: from 0 up
    for i in range(n - 3):
        head, j, nxt, last = mat["stages"][i], mat["stages"][i + 1], mat["stages"][i + 2], mat["stages"][n - 1]
        hc, ha = head
        jc, ja = j
        if hc[0] != 15 or hc[3] != 15 or hc[4:9] != (0, 0, 0, 1, 0) or not {hc[1], hc[2]} <= plain:
            continue
        reg = jc[8] & 3
        # j adds the head's colour into another register and replaces the head's alpha.
        if jc[3] != 0 or jc[4:8] != (0, 0, 0, 1) or reg == 0 or not set(jc[:3]) <= plain:
            continue
        if ja[8] & 3 != 0 or 0 in ja[:4] or ja[4] >= 8 or 1 in jc[:3]:
            continue
        # The stage after replaces PREV without reading it; the last reads the register as d only.
        nc = nxt[0]
        if nc[8] & 3 != 0 or {0, 1} & set(nc[:4]) or i + 2 == n - 1:
            continue
        lc = last[0]
        if lc[3] != 2 * reg or lc[4:9] != (0, 0, 0, 1, 0) or not set(lc[:3]) <= plain | {0, 1}:
            continue
        if any(2 * reg in mat["stages"][t][0][:4] or 2 * reg + 1 in mat["stages"][t][0][:4] or reg in mat["stages"][t][1][:4]
               for t in range(i + 2, n - 1)) or 2 * reg + 1 in lc[:3] or reg in last[1][:4]:
            continue
        out = dict(mat)
        order = list(range(i)) + list(range(i + 1, n)) + [i]
        for key in ("orders", "kcsel", "kasel", "swap_modes"):
            out[key] = [mat[key][t] for t in order] + list(mat[key][n:])
        stages = [mat["stages"][t] for t in order]
        stages[i] = (jc[:3] + (15,) + jc[4:], ja)  # C = b * c alone
        stages[n - 1] = (hc[:3] + (0,) + hc[4:], (7, 7, 7, 0, 0, 0, 0, 1, 0))  # + PREV, alpha kept
        out["stages"] = stages + list(mat["stages"][n:])
        return out
    return None


def texgen_row(mat, coord):
    """The vertex data a texture coordinate is generated from (native.cpp setupUnits: 0 position,
    1 normal, 5 + n texture set n, None other), the same uploaded vertices when equal."""
    kind, src = mat["texgens"][coord & 7][:2]
    if kind > 1:
        return None
    if src in (0, 1):
        return src
    return 5 + src - 4 if 4 <= src <= 11 else None


def own_unit(mat, units, own, stages):
    """The units of a later pass whose stages read its own texture (OWN_UNIT): the first pass's,
    one of them, which the pass does not read, replaced by it, and the stages changed to read that
    unit; None when every unit is read. A unit generating its coordinate from the same vertex data
    comes first: the pass draws the first pass's vertices (native.cpp uploads them again for one
    from other data)."""
    reads = {src for st in stages for src in st.color_src + st.alpha_src}
    free = [u for u in range(len(units)) if SRC_TEXTURE0 + u not in reads]
    if not free:
        return None
    row = texgen_row(mat, own[1])
    free.sort(key=lambda u: texgen_row(mat, units[u][1]) != row)
    u = free[0]
    for st in stages:
        st.color_src = [SRC_TEXTURE0 + u if src == OWN_UNIT else src for src in st.color_src]
        st.alpha_src = [SRC_TEXTURE0 + u if src == OWN_UNIT else src for src in st.alpha_src]
    return units[:u] + [own] + units[u + 1:]


def register_read_later(mat, s, reg, alpha):
    """True when a TEV stage after s reads register reg's colour (alpha False) or alpha before
    another stage writes it."""
    for t in range(s + 1, mat["stage_count"]):
        color_env, alpha_env = mat["stages"][t]
        if alpha:
            if 2 * reg + 1 in color_env[:4] or reg in alpha_env[:4]:
                return True
            if alpha_env[8] & 3 == reg:
                return False
        else:
            if 2 * reg in color_env[:4]:
                return True
            if color_env[8] & 3 == reg:
                return False
    return False


def factored_lerp(mat, s):
    """(swapped, R, k) when TEV stage s writes Ra = x * R, stage s + 1 writes Rb = y * R (R one
    colour register, x and y inputs that are not registers) and stage k = s + 2 adds lerp(Ra, Rb,
    c) or lerp(Rb, Ra, c) to d, reading Ra and Rb nowhere else (GrassMaskMat_v: C1 = tex0 * C2,
    PREV = tex1 * C2, then 2 * (lerp(C1, PREV, tex2) + APREV)). Stage k computes it as
    R * lerp(x, y, c) + d, stages s and s + 1 keep R (their colour costs no combiner)."""
    if s + 2 >= mat["stage_count"]:
        return None

    def product(t):
        a, b, c, d, op, bias, scale, clamp_, reg = mat["stages"][t][0]
        if (a, d, op, bias, scale, clamp_) != (15, 15, 0, 0, 0, 1):
            return None
        regs = [v for v in (b, c) if v in (0, 2, 4, 6)]
        others = [v for v in (b, c) if v >= 8 and v != 15]
        return (regs[0], others[0], reg & 3) if len(regs) == 1 and len(others) == 1 else None

    first, second = product(s), product(s + 1)
    if first is None or second is None or first[0] != second[0]:
        return None
    r_arg, x_arg, ra = first
    _, y_arg, rb = second
    k = s + 2
    a, b, c, d, op, bias, scale, clamp_, reg = mat["stages"][k][0]
    if ra == rb or r_arg // 2 in (ra, rb) or op != 0 or bias != 0 or scale == 3 or not clamp_:
        return None
    if (a, b) not in ((2 * ra, 2 * rb), (2 * rb, 2 * ra)):
        return None
    if c < 8 or d in (2 * ra, 2 * rb):
        return None
    if (ra != reg & 3 and register_read_later(mat, k, ra, False)) or (rb != reg & 3 and register_read_later(mat, k, rb, False)):
        return None
    return (a, b) == (2 * rb, 2 * ra), r_arg, k


def product_input(mat, s, inp):
    """The input that is not a register of a product stage (factored_lerp)."""
    b, c = mat["stages"][s][0][1:3]
    return color_input(b if b >= 8 else c, inp, True)


def alpha_offset_chain(mat, s):
    """(K, last stage, K's register) when TEV stage s sets APREV to a fixed register's alpha K unclamped, the next
    stages add lerps to it unclamped, and the last one clamps (sum + K) * 2 or * 4, reading APREV
    only as d (WaterBazookaCapsule's -25 + texA * rasA + ...): the combiners add the lerps first and
    K at the end (a sum clamped at 1 still clamps the result at 1 for K >= 1/factor - 1)."""
    a, b, c, d, op, bias, scale, clamp_, reg = mat["stages"][s][1]
    if (a, b, c, op, bias, scale, clamp_, reg & 3) != (7, 7, 7, 0, 0, 0, 0, 0) or d not in (1, 2, 3):
        return None
    if d in mat.get("animated_registers", {}) or any(mat["stages"][i][1][8] & 3 == d for i in range(s)):
        return None
    reg_k = d
    k = mat["tev_colors"][d - 1][3]
    for t in range(s + 1, mat["stage_count"]):
        color_env, (a, b, c, d, op, bias, scale, clamp_, reg) = mat["stages"][t]
        if 1 in color_env[:4] or 0 in (a, b, c) or d != 0 or op != 0 or bias != 0 or reg & 3 != 0:
            return None
        if clamp_:
            factor = [1, 2, 4, 0.5][scale]
            return (k, t, reg_k) if scale in (1, 2) and -255 <= k <= 255 and (255 + k) * factor >= 255 else None
        if scale != 0:
            return None
    return None


def chain_offset(mat, s, arg):
    """Colour input d of TEV stage s as fixed values in -128..127 (a register no animation and no
    earlier stage changes, its halves in range for FUNC_ADD_SIGNED), or None."""
    if zero_color(mat, s, arg):
        return (0, 0, 0)
    reg = arg // 2
    if arg not in (2, 4, 6) or reg in mat.get("animated_registers", {}):
        return None
    if any(mat["stages"][i][0][8] & 3 == reg for i in range(s)):
        return None
    values = tuple(mat["tev_colors"][reg - 1][:3])
    return values if all(-128 <= v <= 127 for v in values) else None


def zero_color(mat, s, arg):
    """True when colour input arg of TEV stage s is 0: ZERO, or a register holding black that no
    animation and no earlier stage changes (AuroraSky's C0)."""
    if arg == 15:
        return True
    reg = arg // 2
    if arg not in (2, 4, 6) or reg in mat.get("animated_registers", {}):
        return False
    if any(mat["stages"][i][0][8] & 3 == reg for i in range(s)):
        return False
    return tuple(mat["tev_colors"][reg - 1][:3]) == (0, 0, 0)


def halved_alpha_chain(mat, s):
    """True when TEV stage s doubles its alpha unclamped into APREV (LifeBubble_Mat's 2 * texA,
    up to 2) and the next stage halves it with a lerp, (APREV + lerp) / 2, with no other read of
    it: stage s keeps half (its lerp, in range) in the buffer for the next stage."""
    if s + 1 >= mat["stage_count"]:
        return False
    a, b, c, d, op, bias, scale, clamp_, reg = mat["stages"][s][1]
    na, nb, nc, nd, nop, nbias, nscale, _, nreg = mat["stages"][s + 1][1]
    return (op == 0 and clamp_ == 0 and reg == 0 and scale == 1 and bias == 0 and d == 7 and
            nop == 0 and nscale == 3 and nbias == 0 and nd == 0 and nreg == 0 and 0 not in (na, nb, nc) and
            1 not in mat["stages"][s + 1][0][:4])


def unclamped_chain(mat, s):
    """True when TEV stage s keeps its colour unclamped in PREV (sums up to 2 come back with a
    bias of -1/2: the metals' ras + lerp - 1/2, then + lerp - 1/2) and the next stage adds it as d,
    clamped, with no other read of it (its alpha, APREV, is not in the chain)."""
    if s + 1 >= mat["stage_count"]:
        return False
    a, b, c, d, op, bias, scale, clamp_, reg = mat["stages"][s][0]
    na, nb, nc, nd, nop, nbias, nscale, nclamp, _ = mat["stages"][s + 1][0]
    # A halving next stage only after a lerp - 1/2 (the "signed" chain of compile_material).
    halved = nscale == 3 and nbias == 0 and chain_offset(mat, s, d) is not None and bias == 2
    return (op == 0 and clamp_ == 0 and reg == 0 and scale == 0 and bias in (0, 2) and
            nop == 0 and nd == 0 and 0 not in (na, nb, nc) and nclamp == 1 and (nscale == 0 or halved) and nbias in (0, 2))


def chain_tail(offset, bias):
    """After the lerp: Y = 2 * (lerp / 2 + H) + bias - offset, H = (unclamped sum + offset) / 2
    in the buffer, clamped once (Z = lerp / 2 + H clamps only where Y does)."""
    buffer = Term(SRC_BUFFER, OP_RGB_COLOR)
    prev = previous_term(False)
    rest = bias - offset
    # Z clamped at 1 leaves Y at 1 or more for rests 0 and -1/2: one FUNC_MULTIPLY_ADD makes it.
    if rest == 0:
        out = [Half(FUNC_MULTIPLY_ADD, prev, half(False), buffer, 3)]
        out[-1].scale = 1
        return out
    if rest == -0.5:
        return [Half(FUNC_MULTIPLY_ADD, prev, half(False), buffer, 3), Half(FUNC_ADD_SIGNED, prev, prev, None, 2)]
    out = [Half(FUNC_MODULATE, prev, half(False), None, 2), Half(FUNC_ADD_SIGNED, prev, buffer, None, 2)]
    out[-1].scale = 1
    return out


def late_inputs(halves, alpha):
    """True when a combiner after the first of a stage reads the stage's previous value: its
    colour (colour halves; their previous alpha stays, delayed_alpha) or its alpha (alpha halves)."""
    for h in halves[1:]:
        for t in h.t[: h.count]:
            if t.stage_input and (alpha or t.op not in (OP_RGB_ALPHA, OP_RGB_ALPHA | 1)):
                return True
    return False


def buffered(halves, alpha):
    """The halves with the stage inputs read after the first combiner taken from the buffer (a
    colour half's previous alpha stays: delayed_alpha keeps it)."""
    out = []
    for i, h in enumerate(halves):
        if i > 0:
            n = Half(h.func, count=h.count)
            n.scale = h.scale
            n.t = [t.copy(src=SRC_BUFFER, previous=False)
                   if t.stage_input and (alpha or t.op not in (OP_RGB_ALPHA, OP_RGB_ALPHA | 1)) else t for t in h.t]
            h = n
        out.append(h)
    return out


# ##############################################################################################
# # Reading MAT3                                                                               #
# #                                                                                            #
# # The model's materials from its MAT3 block: every material's TEV stages, orders, colours,   #
# # texture coordinate generations, alpha test, blending and depth.                            #
# ##############################################################################################

def _u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def name_table(data, start, byteorder):
    """The strings of a J3D name table (JUTNameTab: count, then hash and offset pairs)."""
    count = struct.unpack_from(byteorder + "H", data, start)[0]
    names = []
    for i in range(count):
        offset = struct.unpack_from(byteorder + "H", data, start + 4 + i * 4 + 2)[0]
        end = data.index(b"\0", start + offset)
        names.append(data[start + offset : end].decode("cp932", "replace"))
    return names


def parse(data, block, size):
    """The decoded materials of a MAT3 block, in J3D material order."""
    count = _u16(data, block + 0x08)
    t = {slot: struct.unpack_from(">I", data, block + slot)[0] for slot in range(0x0C, 0x84, 4)}

    def table(slot, elem, index, default=None):
        if index == 0xFFFF or not t.get(slot):
            return default
        return block + t[slot] + index * elem

    def u8_at(slot, index, default=0):
        p = table(slot, 1, index)
        return data[p] if p is not None else default

    def rgba(slot, index, default=(255, 255, 255, 255)):
        p = table(slot, 4, index)
        return tuple(data[p : p + 4]) if p is not None else default

    names = name_table(data, block + t[0x14], ">") if t.get(0x14) else []
    mats = []
    for m in range(count):
        init = block + t[0x0C] + _u16(data, block + t[0x10] + m * 2) * 0x14C

        def idx(off, n=1):
            return [_u16(data, init + off + 2 * i) for i in range(n)]

        mat = {"mode": data[init], "name": names[m] if m < len(names) else ""}
        cull_p = table(0x1C, 4, data[init + 1])
        mat["cull"] = struct.unpack_from(">I", data, cull_p)[0] if cull_p is not None else 2
        mat["chan_count"] = u8_at(0x24, data[init + 2])
        mat["texgen_count"] = u8_at(0x34, data[init + 3])
        mat["stage_count"] = min(u8_at(0x58, data[init + 4], 1), 16)
        mat["zcomploc"] = u8_at(0x78, data[init + 5], 1)
        zp = table(0x74, 4, data[init + 6])
        mat["zmode"] = tuple(data[zp : zp + 3]) if zp is not None else (1, 3, 1)
        mat["mat_colors"] = [rgba(0x20, i) for i in idx(0x08, 2)]
        chans = []
        for i in idx(0x0C, 4):
            p = table(0x28, 8, i)
            chans.append(bytes(data[p : p + 8]) if p is not None else bytes(8))
        mat["chans"] = chans
        mat["amb_colors"] = [rgba(0x2C, i, (50, 50, 50, 50)) for i in idx(0x14, 2)]
        gens = []
        for i in idx(0x28, 8):
            p = table(0x38, 4, i)
            gens.append(bytes(data[p : p + 4]) if p is not None else bytes(4))
        mat["texgens"] = gens
        mat["tex_numbers"] = []
        for i in idx(0x84, 8):
            p = table(0x48, 2, i)
            mat["tex_numbers"].append(_u16(data, p) if p is not None else 0xFFFF)
        mat["konst_colors"] = [rgba(0x54, i) for i in idx(0x94, 4)]
        mat["kcsel"] = list(data[init + 0x9C : init + 0xAC])
        mat["kasel"] = list(data[init + 0xAC : init + 0xBC])
        orders = []
        for i in idx(0xBC, 16):
            p = table(0x4C, 4, i)
            orders.append(tuple(data[p : p + 3]) if p is not None else (0xFF, 0xFF, 0xFF))
        mat["orders"] = orders
        tev_colors = []
        for i in idx(0xDC, 4):
            p = table(0x50, 8, i)
            tev_colors.append(struct.unpack_from(">4h", data, p) if p is not None else (255, 255, 255, 255))
        mat["tev_colors"] = tev_colors
        stages = []
        for i in idx(0xE4, 16):
            p = table(0x5C, 0x14, i)
            if p is None:
                stages.append(((15, 15, 15, 15, 0, 0, 0, 1, 0), (7, 7, 7, 7, 0, 0, 0, 1, 0)))
                continue
            e = data[p : p + 0x14]
            stages.append((tuple(e[1:10]), tuple(e[10:19])))
        mat["stages"] = stages
        swaps = []
        for i in idx(0x104, 16):
            p = table(0x60, 4, i)
            swaps.append((data[p], data[p + 1]) if p is not None else (0, 0))
        mat["swap_modes"] = swaps
        tables = []
        for i in idx(0x124, 4):
            p = table(0x64, 4, i)
            tables.append(list(data[p : p + 4]) if p is not None else [0, 1, 2, 3])
        mat["swap_tables"] = tables
        mat["fog"] = idx(0x144)[0] != 0xFFFF and bool(t.get(0x68))
        ap = table(0x6C, 8, idx(0x146)[0])
        mat["alpha_comp"] = tuple(data[ap : ap + 5]) if ap is not None else (7, 0, 0, 7, 0)
        bp = table(0x70, 4, idx(0x148)[0])
        mat["blend"] = tuple(data[bp : bp + 4]) if bp is not None else (0, 1, 0, 3)
        mat["indirect"] = False
        if t.get(0x18):
            ind = block + t[0x18] + m * 0x138
            mat["indirect"] = ind + 0x138 <= block + size and data[ind] != 0
            mat["ind_map"] = data[ind + 5] if mat["indirect"] else 0xFF
        wrapped_alpha_test(mat)
        mats.append(mat)
    return mats


# ##############################################################################################
# # The alpha test                                                                             #
# #                                                                                            #
# # GX tests alpha with two comparisons and a logic operation, the PICA with one comparison:   #
# # the one that passes the same values is found by trying all 256 values, and a range is      #
# # split into two passes. The Wii wraps an unclamped alpha to 8 bits where the 3DS clamps it: #
# # the materials that rely on it (the eyelids of Mario and others, some water) get stages     #
# # that reproduce the wrap.                                                                   #
# ##############################################################################################
def _gx_alpha(mat, regs, konst, tex, ras):
    """The GX TEV's alpha before the alpha test (Dolphin's rules): unclamped stages keep 11
    signed bits; the last result is wrapped to 8 bits (prev & 255) before the test, not clamped.
    regs: [PREV, REG0, REG1, REG2] alphas; tex: {(map, coord): rgba}; ras: [chan0, chan1] alphas."""
    regs = list(regs)
    for s in range(mat["stage_count"]):
        coord, tmap, chan = mat["orders"][s]
        t = 255
        if tmap != 0xFF:
            t = tex[(tmap, coord)][mat["swap_tables"][mat["swap_modes"][s][1]][3]]
        r = {4: ras[0], 5: ras[1], 0: ras[0], 1: ras[1], 2: ras[0], 3: ras[1]}.get(chan, 0)
        ka = mat["kasel"][s]
        k = KONST_FRACTIONS[ka] if ka < 8 else konst[ka & 3][(ka - 0x10) >> 2] if ka >= 0x10 else 255
        a, b, c, d, op, bias, scale, clamp, reg = mat["stages"][s][1]
        if op >= 2:
            return None  # compare modes: not modelled

        def inp(x):
            return regs[x] if x <= 3 else {4: t, 5: r, 6: k}.get(x, 0)

        av, bv, cv = inp(a) & 255, inp(b) & 255, inp(c) & 255
        # Dolphin's Tev::DrawAlphaRegular, bit for bit: the lerp is rounded (+128, +127 when
        # subtracting, none for divide-by-2) before >> 8, and the scale shifts both terms. Without
        # the rounding the eyelids' test passed from texture alpha 4 instead of 2 (material audit).
        c2 = cv + (cv >> 7)
        temp = (av * (256 - c2) + bv * c2) << [0, 1, 2, 0][scale]
        temp += 0 if scale == 3 else (127 if op == 1 else 128)
        temp = (-temp >> 8) if op == 1 else (temp >> 8)
        x = ((inp(d) + [0, 128, -128, 0][bias if bias < 3 else 0]) << [0, 1, 2, 0][scale]) + temp
        x >>= [0, 0, 0, 1][scale]
        regs[reg & 3] = min(255, max(0, x)) if clamp else min(1023, max(-1024, x))
    return regs[0]


def _alpha_passes(comp0, ref0, logic, comp1, ref1, a):
    def test(c, r):
        return [False, a < r, a == r, a <= r, a > r, a != r, a >= r, True][c & 7]

    x, y = test(comp0, ref0), test(comp1, ref1)
    return [x and y, x or y, x != y, x == y][logic & 3]


def wrapped_alpha_test(mat):
    """Eyelids (Mario, Luigi, Peach, Bowser...) end their TEV with an unclamped alpha above 255
    that the Wii wraps to 8 bits before the alpha test: 255 + 0.5 + texA / 4 passes ">= 128" only
    where the texture is not transparent. The 3DS clamps it to 255, the test always passed and the
    lid's transparent (black) texels covered the eyes. When the test's outcome depends on one
    texture's alpha alone, over a range of it, and blending does not read the alpha, the last
    stage passes that alpha through and the test checks the range: exact."""
    n = mat["stage_count"]
    if mat["blend"][0] != 0 or mat["alpha_comp"][0] == mat["alpha_comp"][3] == 7 or n == 0:
        return
    last = mat["stages"][n - 1][1]
    coord, tmap, _ = mat["orders"][n - 1]
    if last[7] or last[8] != 0 or tmap == 0xFF:
        return  # clamped, or not in PREV: nothing wraps
    key = (tmap, coord)
    channel = mat["swap_tables"][mat["swap_modes"][n - 1][1]][3]
    keys = {(mat["orders"][s][1], mat["orders"][s][0]) for s in range(n) if mat["orders"][s][1] != 0xFF}
    import random
    rng = random.Random(0)
    passes = []
    wraps = False  # the 3DS's clamped result fails the test somewhere the Wii's passes, or the reverse
    for value in range(256):
        outcome = set()
        for _ in range(8):
            # Registers, constants and rasterized colours vary at run time (animations, deltas).
            regs = [rng.randrange(-1024, 1024) if i == 0 else rng.randrange(256) for i in range(4)]
            konst = [[rng.randrange(256) for _ in range(4)] for _ in range(4)]
            tex = {k: [rng.randrange(256) for _ in range(4)] for k in keys}
            tex[key][channel] = value
            a = _gx_alpha(mat, regs, konst, tex, [rng.randrange(256), rng.randrange(256)])
            if a is None:
                return
            outcome.add(_alpha_passes(*mat["alpha_comp"], a & 255))
            wraps |= _alpha_passes(*mat["alpha_comp"], a & 255) != _alpha_passes(*mat["alpha_comp"], min(255, max(0, a)))
        if len(outcome) != 1:
            return  # depends on something else too
        if outcome.pop():
            passes.append(value)
    if not wraps or not passes or passes != list(range(passes[0], passes[-1] + 1)):
        return
    if passes[0] > 0 and passes[-1] < 255:
        return  # a range the 3DS's single test cannot check
    if passes == list(range(256)):
        test = (7, 0, 0, 7, 0)
    elif passes[0] > 0:
        test = (6, passes[0], 0, 7, 0)  # GEQUAL
    else:
        test = (3, passes[-1], 0, 7, 0)  # LEQUAL
    stages = list(mat["stages"])
    mat["stages"] = stages
    stages[n - 1] = (stages[n - 1][0], (7, 7, 7, 4, 0, 0, 0, 1, 0))  # alpha = texA of the last stage
    mat["alpha_comp"] = test
    mat["wrapped_alpha"] = True


def _alpha_input_max(mat, s, arg, regs):
    """The largest value alpha input arg of TEV stage s can take, or None when unknown."""
    if arg <= 3:
        return regs[arg]
    if arg == 4:
        coord, tmap, _ = mat["orders"][s]
        get = mat.get("texture_max")
        if tmap == 0xFF or get is None or tmap >= len(mat["tex_numbers"]):
            return None
        maxima = get(mat["tex_numbers"][tmap])
        if maxima is None:
            return None
        return maxima[mat["swap_tables"][mat["swap_modes"][s][1]][3]]
    if arg == 5:
        return 255
    if arg == 6:
        ka = mat["kasel"][s]
        return KONST_FRACTIONS[ka] if ka < 8 else 255
    return 0


def wrapped_last_alpha(mat):
    """The water of FlowerPlanet, OceanJShapePlanet, PeachCastleGarden, PoihanaRelayPlanet and
    PukupukuGambolPlanet ends its TEV with an unclamped alpha 4 * m (m = APREV * TEXA) that the Wii
    wraps to 8 bits: rings of 4 * (m - 64) where m reaches 64, transparent or cut by the alpha test.
    The 3DS clamps to 255 (opaque white there). When the last alpha is (X * Y) * 2 or * 4 with d and
    a zero, unclamped, and the largest m the stages before and the textures allow (texture_max)
    wraps at most once, the material compiles with that stage clamped and unscaled (alpha m), its
    result kept in the combiner buffer, then four combiners: an indicator of m >= 256 / scale, 0
    or 1 (m - (256 / scale - 1), x 4, x 8, x 8: each step one 1 / 255 at least), sat(m - 256 /
    scale) beside it, and the alpha lerp(m, that, indicator) * scale with the colour from the
    buffer: the Wii's wrapped value. Returns compile_material's tuple, or None."""
    n = mat["stage_count"]
    if n == 0:
        return None
    color_env, alpha_env = mat["stages"][n - 1]
    a, b, c, d, op, bias, scale, clamp, reg = alpha_env
    if clamp or (reg & 3) != 0 or op != 0 or bias != 0 or scale not in (1, 2) or a != 7 or d != 7 or not color_env[7]:
        return None
    if mat.get("animated_registers"):
        return None
    # Upper bound of the alpha registers through the stages before (each lerp(0, x, y) + 0,
    # clamped, unscaled: non-decreasing in its inputs).
    regs = [255] * 4  # registers as loaded (which TEV register each colour is does not matter here)
    for s in range(n - 1):
        sa, sb, sc, sd, sop, sbias, sscale, sclamp, sreg = mat["stages"][s][1]
        if sa != 7 or sop != 0 or sbias != 0 or sscale != 0 or not sclamp or sd not in (7,):
            return None
        x, y = _alpha_input_max(mat, s, sb, regs), _alpha_input_max(mat, s, sc, regs)
        if x is None or y is None:
            return None
        regs[sreg & 3] = x * y // 255
    x, y = _alpha_input_max(mat, n - 1, b, regs), _alpha_input_max(mat, n - 1, c, regs)
    if x is None or y is None:
        return None
    period = 256 >> scale
    bound = (x * y + 254) // 255
    if bound < period or bound >= 2 * period:
        return None  # never wraps, or more than once
    base = dict(mat)
    stages = list(mat["stages"])
    stages[n - 1] = (color_env, (a, b, c, d, op, bias, 0, 1, reg))
    base["stages"] = stages
    compiled, units, ras_channel, inexact, passes, buffer = compile_material(base)
    k = len(compiled)
    if inexact or passes or buffer != (0, 0) or k == 0 or k > 4 or k + 4 > MAX_STAGES:
        return None
    fixed = (REF_FIXED, 0, PART_RGB, 0)
    t = period - 1
    s1 = Stage()  # colour: 4 * sat(m - (period - 1)); alpha: sat(m - period)
    s1.color_src = [SRC_PREVIOUS, SRC_CONSTANT, SRC_PREVIOUS]
    s1.color_op = [OP_RGB_ALPHA, OP_RGB_COLOR, 0]
    s1.color_func, s1.color_scale = FUNC_SUBTRACT, 2
    s1.alpha_src = [SRC_PREVIOUS, SRC_CONSTANT, SRC_PREVIOUS]
    s1.alpha_func = FUNC_SUBTRACT
    s1.color_ref, s1.alpha_ref = ((t, t, t), fixed), (period, fixed)
    s1.constant = [t, t, t, period]
    steps = []
    for _ in range(2):  # x 8, x 8: the indicator saturates to 1 from m = period on
        s = Stage()
        s.color_src = [SRC_PREVIOUS, SRC_PREVIOUS, SRC_PREVIOUS]
        s.color_func, s.color_scale = FUNC_ADD, 2
        steps.append(s)
    last = Stage()  # colour from the buffer; alpha lerp(m, sat(m - period), indicator) * scale
    last.color_src = [SRC_BUFFER, SRC_PREVIOUS, SRC_PREVIOUS]
    last.alpha_src = [SRC_PREVIOUS, SRC_BUFFER, SRC_PREVIOUS]
    last.alpha_op = [OP_A_ALPHA, OP_A_ALPHA, OP_A_R]
    last.alpha_func, last.alpha_scale = FUNC_INTERPOLATE, scale
    keep = 1 << (k - 1)
    return compiled + [s1] + steps + [last], units, ras_channel, False, [], (keep, keep)


def exact_alpha_test(comp0, ref0, logic, comp1, ref1):
    """(PICA test, reference) passing exactly the alpha values the Wii's two comparisons and
    their logic pass, or None when no single comparison does (a range inside 0-255, two ranges).
    As JPA3DS::reduceAlphaTest does for particles: the first comparison alone was wrong for 23
    materials by one value (material audit)."""
    passes = [_alpha_passes(comp0, ref0, logic, comp1, ref1, a) for a in range(256)]
    n = sum(passes)
    if n == 0:
        return TEST_NEVER, 0
    if n == 256:
        return TEST_ALWAYS, 0
    first = passes.index(True)
    last = 255 - passes[::-1].index(True)
    if n == last - first + 1:
        if last == 255:
            return TEST_GEQUAL, first
        if first == 0:
            return TEST_LEQUAL, last
        if first == last:
            return TEST_EQUAL, first
        return None
    if n == 255:
        return TEST_NOTEQUAL, passes.index(False)
    return None


def split_alpha_test(comp0, ref0, logic, comp1, ref1):
    """((test, reference), (test, reference)) passing, one after the other, exactly the alpha values
    the Wii's two comparisons pass when those are both ends of the range ("A <= a or A >= b",
    the water's LEQUAL 160 XOR GEQUAL 40); None otherwise (exact_alpha_test's cases, a range
    inside 0-255)."""
    passes = [_alpha_passes(comp0, ref0, logic, comp1, ref1, a) for a in range(256)]
    if not passes[0] or not passes[255] or all(passes):
        return None
    low = passes.index(False) - 1
    high = 256 - passes[::-1].index(False)
    if any(passes[low + 1:high]):
        return None
    return (TEST_LEQUAL, low), (TEST_GEQUAL, high)


# ##############################################################################################
# # The record                                                                                 #
# #                                                                                            #
# # Blending, depth and alpha test as PICA values, and the record native.cpp reads (its layout #
# # is MATERIAL_SIZE bytes, matching kMaterialSize there).                                     #
# ##############################################################################################
def fragment_state(mat):
    """(blend equation, src, dst, alpha test, alpha ref, depth test, depth func, write mask)."""
    btype, src, dst, _ = mat["blend"]
    if btype == 3:
        blend = (BLEND_REVERSE_SUBTRACT, FACTOR_ONE, FACTOR_ONE)
    elif btype == 1:
        blend = (BLEND_ADD, SRC_FACTOR[src & 7], DST_FACTOR[dst & 7])
    else:
        blend = (BLEND_ADD, FACTOR_ONE, FACTOR_ZERO)

    comp0, ref0, logic, comp1, ref1 = mat["alpha_comp"]
    func, ref = TEST_ALWAYS, 0
    exact = exact_alpha_test(comp0, ref0, logic, comp1, ref1)
    split = split_alpha_test(comp0, ref0, logic, comp1, ref1)
    if exact is not None:
        func, ref = exact
    elif split is not None:
        func, ref = split[0]
    elif logic == 0:
        if comp1 == 7:
            func, ref = ALPHA_FUNC[comp0 & 7], ref0
        elif comp0 == 7:
            func, ref = ALPHA_FUNC[comp1 & 7], ref1
        else:
            func, ref = ALPHA_FUNC[comp0 & 7], ref0
    elif logic == 1:
        if comp1 == 0:
            func, ref = ALPHA_FUNC[comp0 & 7], ref0
        elif comp0 == 0:
            func, ref = ALPHA_FUNC[comp1 & 7], ref1

    enable, zfunc, update = mat["zmode"]
    write = WRITE_COLOR  # J3D leaves colour update on and alpha update off
    if enable and update:
        write |= WRITE_DEPTH
    return blend + (func, ref, 1 if enable else 0, DEPTH_FUNC[zfunc & 7], write)


def record(mat, stages, units, ras_channel, flags, state, pass_count, first_pass, buffer=(0, 0), second=(0, 0)):
    out = bytearray(struct.pack("<BBBB", len(stages), len(units), ras_channel, flags))
    out += bytes([mat["cull"] & 3]) + bytes(state) + bytes([mat["zcomploc"], mat["mode"], pass_count])
    for u in range(MAX_UNITS):
        if u < len(units):
            tex_map, coord = units[u]
            out += struct.pack("<HBB", mat["tex_numbers"][tex_map & 7] if tex_map < 8 else 0xFFFF, tex_map, coord)
        else:
            out += struct.pack("<HBB", 0xFFFF, 0xFF, 0xFF)
    for s in range(MAX_STAGES):
        out += stages[s].pack() if s < len(stages) else Stage().pack()
    out += struct.pack("<BBH", mat["chan_count"], mat["texgen_count"], first_pass)
    for c in mat["chans"]:
        out += c
    for c in mat["mat_colors"] + mat["amb_colors"]:
        out += bytes(c)
    for g in mat["texgens"]:
        out += g
    for c in mat["tev_colors"]:
        out += struct.pack("<4h", *c)
    for c in mat["konst_colors"]:
        out += bytes(c)
    out += struct.pack("<BBBB", buffer[0], buffer[1], second[0], second[1])
    assert len(out) == MATERIAL_SIZE, len(out)
    return bytes(out)


def second_alpha_test(mat, passes):
    """The second draw's (test, reference) of a material whose alpha test split_alpha_test splits,
    or None. Not with later passes: those rebuild the colour on the first pass's pixels."""
    if passes or mat.get("alpha_comp") is None or exact_alpha_test(*mat["alpha_comp"]) is not None:
        return None
    split = split_alpha_test(*mat["alpha_comp"])
    return split[1] if split else None


def pack(mat, first_pass):
    """(record, inexact, pass records) of a material; its passes after the first will be the
    records first_pass, first_pass + 1, ... of the file."""
    stages, units, ras_channel, inexact, passes, buffer = wrapped_last_alpha(mat) or compile_material(mat)
    flags =(FLAG_INEXACT if inexact else 0) | (FLAG_FOG if mat["fog"] else 0) | (FLAG_INDIRECT if mat["indirect"] else 0)
    split = second_alpha_test(mat, passes)
    main = record(mat, stages, units, ras_channel, flags | (FLAG_ALPHA_SPLIT if split else 0), fragment_state(mat), len(passes),
                  first_pass if passes else 0, buffer, split or (0, 0))
    # Later passes: the same vertices (own_unit: a unit with another texture), their own combiners
    # and blend, on the pixels the first pass wrote (depth equal, no depth write), colour only.
    extra = [record(mat, pass_stages, pass_units, channel, flags, blend + (TEST_ALWAYS, 0, 1, TEST_EQUAL, WRITE_COLOR), 0, 0)
             for pass_stages, blend, channel, pass_units in passes]
    return main, inexact, extra
