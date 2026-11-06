#pragma once

// ##############################################################################################
// # The TEV translation: terms and halves                                                      #
// #                                                                                            #
// # The Wii combines textures and colours in up to 16 TEV stages, each computing (d +/- ((1 -  #
// # c) * a + c * b) + bias) * scale for colour and for alpha. The 3DS has 6 combiner stages,   #
// # each a fixed function of up to 3 inputs (replace, modulate, add, subtract,                 #
// # interpolate...). Here one TEV function becomes one or more combiner stages, with the       #
// # approximations the PICA needs. Used by tev_compile.cpp; the converter does the same for    #
// # the .n3d materials (tools/3ds/native/mat.py). Nothing here reads GX state.                 #
// ##############################################################################################
#include "host.h"
#include <revolution/types.h>

// PICA values (libctru gpu/enums.h): combiner sources, operands and functions.
enum {
    SRC_PRIMARY = 0x00,
    SRC_TEXTURE0 = 0x03,
    SRC_CONSTANT = 0x0E,
    SRC_PREVIOUS = 0x0F,

    OP_RGB_COLOR = 0x00,
    OP_RGB_ALPHA = 0x02,
    OP_RGB_R = 0x04,
    OP_RGB_G = 0x08,
    OP_RGB_B = 0x0C,
    OP_A_ALPHA = 0x00,
    OP_A_R = 0x02,
    OP_A_G = 0x04,
    OP_A_B = 0x06,

    FUNC_REPLACE = 0,
    FUNC_MODULATE = 1,
    FUNC_ADD = 2,
    FUNC_ADD_SIGNED = 3,
    FUNC_INTERPOLATE = 4,
    FUNC_SUBTRACT = 5,
    FUNC_MULTIPLY_ADD = 8,

    TEST_NEVER = 0,
    TEST_ALWAYS = 1,
    TEST_EQUAL = 2,
    TEST_NOTEQUAL = 3,
    TEST_LESS = 4,
    TEST_LEQUAL = 5,
    TEST_GREATER = 6,
    TEST_GEQUAL = 7,

    BLEND_ADD = 0,
    BLEND_REVERSE_SUBTRACT = 2,
    FACTOR_ZERO = 0,
    FACTOR_ONE = 1,

    CULL_NONE = 0,
    CULL_FRONT_CCW = 1,
    CULL_BACK_CCW = 2,

    WRITE_COLOR = 0x07,
    WRITE_ALPHA = 0x08,
    WRITE_DEPTH = 0x10,
};

// ##############################################################################################
// # Terms and halves                                                                           #
// #                                                                                            #
// # A Term is one input of a combiner function (a source and an operand, or a constant); a     #
// # Half is the colour or the alpha part of one combiner stage. The constructors below         #
// # simplify 0 and 1, which are written as constants but need no constant register.            #
// ##############################################################################################
// One input of a combiner function.
struct Term {
    u8 src;        // PICA source
    u8 op;         // PICA operand (colour or alpha)
    u8 value[3];   // constant value (rgb, or alpha in value[0]) when src is SRC_CONSTANT
    bool zero;     // the constant 0 (used to simplify)
    bool one;      // the constant 1
    bool previous; // reads the result of the previous stage
    bool negative; // a constant whose value is minus value[] (TEV registers are signed)
};

// One half (colour or alpha) of a combiner stage.
struct Half {
    u8 func;
    u8 scale;  // 0: x1, 1: x2, 2: x4
    u8 count;
    Term t[3];
};

static const u32 kMaxHalves = 6;  // worst case: 3 for the lerp, then subtract, bias and scale

static inline Term constant(u8 r, u8 g, u8 b) {
    Term t = {};
    t.src = SRC_CONSTANT;
    t.value[0] = r;
    t.value[1] = g;
    t.value[2] = b;
    // 0 and 1 simplify the lerps below (a material colour of 0 or 255 is the same input).
    t.zero = r == 0 && g == 0 && b == 0;
    t.one = r == 255 && g == 255 && b == 255;
    return t;
}

static inline Term constantAlpha(u8 a) {
    return constant(a, a, a);
}

static inline Term zero() {
    Term t = constant(0, 0, 0);
    t.zero = true;
    return t;
}

static inline Term one() {
    Term t = constant(255, 255, 255);
    t.one = true;
    return t;
}

static inline Term source(u8 src, u8 op, bool previous = false) {
    Term t = {};
    t.src = src;
    t.op = op;
    t.previous = previous;
    return t;
}

// 1 - x, for an input of a combiner (operands come in pairs: x, 1 - x).
static inline Term inverse(const Term& term) {
    if (term.zero) {
        return one();
    }
    if (term.one) {
        return zero();
    }
    Term t = term;
    t.op ^= 1;
    return t;
}

static inline Half half(u8 func, const Term& a, const Term& b = Term(), const Term& c = Term(), u8 count = 1) {
    Half h = {};
    h.func = func;
    h.count = count;
    h.t[0] = a;
    h.t[1] = b;
    h.t[2] = c;
    return h;
}

static inline Term previousTerm(bool alpha) {
    return source(SRC_PREVIOUS, alpha ? OP_A_ALPHA : OP_RGB_COLOR, true);
}

static inline bool readsPrevious(const Term& t) {
    return t.previous;
}

// ##############################################################################################
// # One TEV function as combiner stages                                                        #
// #                                                                                            #
// # The lerp first, in as few stages as its inputs allow (a constant 0 or 1 makes it a         #
// # multiply or a single input), then d added or subtracted, the bias, and the scale. The      #
// # combiners take no negative inputs: a negative constant d is handled by computing the rest  #
// # and subtracting |d| after it.                                                              #
// ##############################################################################################
// Builds the combiner functions for (d +/- lerp(a, b, c) + bias) * scale. Returns the count.
static inline u32 buildHalves(Term a, Term b, Term c, Term d, u32 bias, bool subtract, u32 scale, bool alpha, Half* out) {
    u32 n = 0;
    Term prev = previousTerm(alpha);

    // A negative constant d (signed TEV register): the combiners take no negative inputs, so the
    // rest is computed first and |d| subtracted after (a register of -0.5 is used to cancel a
    // term; read as 0 instead it whitened the Good Egg planet and the file select icon).
    // As a, b or c a negative register still reads as 0 (clamped, an approximation).
    if (a.negative) {
        a = zero();
    }
    if (b.negative) {
        b = zero();
    }
    if (c.negative) {
        c = zero();
    }
    Term negativeD;
    bool subtractD = false;
    if (d.negative) {
        if (subtract) {  // -|d| - lerp: never above 0
            out[n++] = half(FUNC_REPLACE, zero());
            return n;
        }
        negativeD = d;
        negativeD.negative = false;
        subtractD = true;
        d = zero();
    }

    // lerp = a * (1 - c) + b * c, simplified when inputs are 0 or 1.
    enum { LERP_ZERO, LERP_SINGLE, LERP_PRODUCT, LERP_FULL } kind;
    Term x, y;
    if (c.one || (a.zero && b.zero)) {
        kind = b.zero ? LERP_ZERO : LERP_SINGLE;
        x = b;
    } else if (c.zero) {
        kind = a.zero ? LERP_ZERO : LERP_SINGLE;
        x = a;
    } else if (a.zero) {
        kind = b.one ? LERP_SINGLE : LERP_PRODUCT;
        x = b.one ? c : b;
        y = c;
    } else if (b.zero) {
        Term ic = inverse(c);
        kind = a.one ? LERP_SINGLE : LERP_PRODUCT;
        x = a.one ? ic : a;
        y = ic;
    } else {
        kind = LERP_FULL;
    }

    switch (kind) {
    case LERP_ZERO:
        out[n++] = half(FUNC_REPLACE, d);
        break;
    case LERP_SINGLE:
        if (subtract) {
            out[n++] = half(FUNC_SUBTRACT, d, x, Term(), 2);
        } else if (d.zero) {
            out[n++] = half(FUNC_REPLACE, x);
        } else {
            out[n++] = half(FUNC_ADD, x, d, Term(), 2);
        }
        break;
    case LERP_PRODUCT:
        if (subtract && readsPrevious(d)) {
            // d - x * y = 1 - (x * y + (1 - d)): the previous result is read before it is
            // overwritten (subtracting it afterwards read the product instead, giving 0);
            // clamping the sum at 1 gives the clamped difference at 0.
            out[n++] = half(FUNC_MULTIPLY_ADD, x, y, inverse(d), 3);
            out[n++] = half(FUNC_REPLACE, inverse(prev));
        } else if (subtract) {
            out[n++] = half(FUNC_MODULATE, x, y, Term(), 2);
            out[n++] = half(FUNC_SUBTRACT, d, prev, Term(), 2);
        } else if (d.zero) {
            out[n++] = half(FUNC_MODULATE, x, y, Term(), 2);
        } else {
            out[n++] = half(FUNC_MULTIPLY_ADD, x, y, d, 3);
        }
        break;
    case LERP_FULL: {
        // PICA interpolate is s0 * s2 + s1 * (1 - s2).
        Half interpolate = half(FUNC_INTERPOLATE, b, a, c, 3);
        bool twoConstants = a.src == SRC_CONSTANT && b.src == SRC_CONSTANT &&
                            (a.value[0] != b.value[0] || a.value[1] != b.value[1] || a.value[2] != b.value[2]);
        // With c the previous result, the lerp is a + c * (b - a) (or a - c * (a - b)), one
        // constant per combiner, when b - a has the same sign in every component.
        bool rising = true, falling = true;
        for (u32 i = 0; i < 3; i++) {
            rising = rising && b.value[i] >= a.value[i];
            falling = falling && b.value[i] <= a.value[i];
        }
        if (twoConstants && readsPrevious(c) && !readsPrevious(d) && (rising || falling)) {
            // nw4r's C0/C1 stage after the texture: both constants in one combiner kept only
            // one of them (an alpha 0 -> 255 lerp became 255, opaque white boxes on the title).
            Term difference = rising ? constant(b.value[0] - a.value[0], b.value[1] - a.value[1], b.value[2] - a.value[2])
                                     : constant(a.value[0] - b.value[0], a.value[1] - b.value[1], a.value[2] - b.value[2]);
            out[n++] = half(FUNC_MODULATE, c, difference, Term(), 2);
            out[n++] = rising ? half(FUNC_ADD, prev, a, Term(), 2) : half(FUNC_SUBTRACT, a, prev, Term(), 2);
            if (subtract) {
                out[n++] = half(FUNC_SUBTRACT, d, prev, Term(), 2);
            } else if (!d.zero) {
                out[n++] = half(FUNC_ADD, prev, d, Term(), 2);
            }
        } else if (subtract && readsPrevious(d) && !readsPrevious(a) && !readsPrevious(c)) {
            // 1 - (b * c + (1 - d) + a * (1 - c)), as for the product
            out[n++] = half(FUNC_MULTIPLY_ADD, b, c, inverse(d), 3);
            out[n++] = half(FUNC_MULTIPLY_ADD, a, inverse(c), prev, 3);
            out[n++] = half(FUNC_REPLACE, inverse(prev));
        } else if (twoConstants && !readsPrevious(c) && !subtract && readsPrevious(d)) {
            // A stage holds one constant, and d is the previous result: it goes into the first
            // combiner (b * c + d), before a later one overwrites it (the title's sea and sky
            // added their lerp to itself instead, saturated cyan).
            out[n++] = half(FUNC_MULTIPLY_ADD, b, c, d, 3);
            out[n++] = half(FUNC_MULTIPLY_ADD, a, inverse(c), prev, 3);
        } else if (twoConstants && !readsPrevious(c)) {
            // A stage holds one constant: b * c first, then a * (1 - c) + that.
            out[n++] = half(FUNC_MODULATE, b, c, Term(), 2);
            out[n++] = half(FUNC_MULTIPLY_ADD, a, inverse(c), prev, 3);
            if (subtract) {
                out[n++] = half(FUNC_SUBTRACT, d, prev, Term(), 2);
            } else if (!d.zero) {
                out[n++] = half(FUNC_ADD, prev, d, Term(), 2);
            }
        } else if (subtract) {
            out[n++] = interpolate;
            out[n++] = half(FUNC_SUBTRACT, d, prev, Term(), 2);
        } else if (d.zero) {
            out[n++] = interpolate;
        } else if (!readsPrevious(d)) {
            out[n++] = interpolate;
            out[n++] = half(FUNC_ADD, prev, d, Term(), 2);
        } else if (!readsPrevious(b) && !readsPrevious(c)) {
            out[n++] = half(FUNC_MULTIPLY_ADD, a, inverse(c), d, 3);
            out[n++] = half(FUNC_MULTIPLY_ADD, b, c, prev, 3);
        } else {
            out[n++] = half(FUNC_MULTIPLY_ADD, b, c, d, 3);
            out[n++] = half(FUNC_MULTIPLY_ADD, a, inverse(c), prev, 3);
        }
        break;
    }
    }

    if (subtractD) {
        out[n++] = half(FUNC_SUBTRACT, prev, negativeD, Term(), 2);
    }

    // A replace of 0 or 1 still needs the value as a source: it is already a constant term.
    if (bias == 1) {
        out[n++] = half(FUNC_ADD, prev, alpha ? constantAlpha(128) : constant(128, 128, 128), Term(), 2);
    } else if (bias == 2) {
        out[n++] = half(FUNC_ADD_SIGNED, prev, zero(), Term(), 2);
    }

    switch (scale) {
    case 1:
        out[n - 1].scale = 1;
        break;
    case 2:
        out[n - 1].scale = 2;
        break;
    case 3:
        out[n++] = half(FUNC_MODULATE, prev, alpha ? constantAlpha(128) : constant(128, 128, 128), Term(), 2);
        break;
    default:
        break;
    }

    return n;
}

// ##############################################################################################
// # Comparisons                                                                                #
// #                                                                                            #
// # TEV's compare functions (a > b ? c : d and the like) have no combiner equivalent; the      #
// # common forms are approximated with arithmetic, as explained below.                         #
// ##############################################################################################
// PICA has no GX packed-component comparator. For the common 8-bit forms, approximate its
// conditional C + D output with an arithmetic mask. GT uses saturating A-B; EQ uses
// A*B + (1-A)*(1-B), which is exact for binary mask inputs and fades smoothly between them.
// Wider GX compares retain the low (red/alpha) component as a documented visual fallback.
static inline u32 buildCompareHalves(Term a, Term b, Term c, Term d, u32 mode, bool alpha, Half* out) {
    if (alpha) {
        a.op = OP_A_ALPHA;
        b.op = OP_A_ALPHA;
    } else {
        auto red = [](Term& t) {
            if (t.src == SRC_CONSTANT) {
                t.value[1] = t.value[2] = t.value[0];
            } else {
                t.op = OP_RGB_R;
            }
        };
        red(a);
        red(b);
    }

    const bool equal = (mode & 1) != 0;
    if (equal) {
        // GPU_INTERPOLATE is s0*s2 + s1*(1-s2): with s0=B, s1=1-B, s2=A
        // the result is one for equal binary values and zero for unequal values.
        out[0] = half(FUNC_INTERPOLATE, b, inverse(b), a, 3);
    } else {
        out[0] = half(FUNC_SUBTRACT, a, b, Term(), 2);
    }

    Term mask = previousTerm(alpha);
    return 1 + buildHalves(zero(), c, mask, d, 0, false, 0, alpha, out + 1);
}

// ##############################################################################################
// # Swap tables                                                                                #
// #                                                                                            #
// # TEV can reorder the channels of a texture or of the rasterized colour (swap tables); the   #
// # combiners can read a single channel as grey through the operand, which covers the          #
// # reorderings the game uses.                                                                 #
// ##############################################################################################
static const u8 kColorChannelOp[4] = {OP_RGB_R, OP_RGB_G, OP_RGB_B, OP_RGB_ALPHA};
static const u8 kAlphaChannelOp[4] = {OP_A_R, OP_A_G, OP_A_B, OP_A_ALPHA};

static inline u8 swappedColorOp(const u32* ch) {
    if (ch[0] == 0 && ch[1] == 1 && ch[2] == 2) {
        return OP_RGB_COLOR;
    }
    if (ch[0] == ch[1] && ch[1] == ch[2]) {
        return kColorChannelOp[ch[0]];
    }
    return OP_RGB_COLOR;
}

// ##############################################################################################
// # Writing a stage                                                                            #
// #                                                                                            #
// # A combiner stage has one constant colour, shared by its colour and alpha halves: two       #
// # halves needing different constants in one stage cannot both be exact.                      #
// ##############################################################################################
// Writes a half into a combiner stage. Returns false if its constant differs from the one
// the stage already holds.
static inline bool storeHalf(const Half& h, bool alpha, HostGfxStage& stage, bool& hasColorConst, bool& hasAlphaConst) {
    bool exact = true;
    for (u32 i = 0; i < 3; i++) {
        const Term& t = h.t[i < h.count ? i : 0];
        u8 src = t.src;
        u8 op = t.op;
        if (i >= h.count) {
            src = SRC_PREVIOUS;
            op = 0;
        } else if (src == SRC_CONSTANT) {
            u32 rgba = stage.constant;
            if (alpha) {
                u32 a = t.value[0];
                if (hasAlphaConst && ((rgba >> 24) & 0xFF) != a) {
                    exact = false;
                } else {
                    stage.constant = (rgba & 0x00FFFFFF) | (a << 24);
                    hasAlphaConst = true;
                }
                op = (t.op & 1) ? 1 : OP_A_ALPHA;
            } else {
                u32 rgb = t.value[0] | (t.value[1] << 8) | (static_cast< u32 >(t.value[2]) << 16);
                if (hasColorConst && (rgba & 0x00FFFFFF) != rgb) {
                    exact = false;
                } else {
                    stage.constant = (rgba & 0xFF000000) | rgb;
                    hasColorConst = true;
                }
                op = (t.op & 1) ? 1 : OP_RGB_COLOR;
            }
        }
        if (alpha) {
            stage.alphaSrc[i] = src;
            stage.alphaOp[i] = op;
        } else {
            stage.colorSrc[i] = src;
            stage.colorOp[i] = op;
        }
    }
    if (alpha) {
        stage.alphaFunc = h.func;
        stage.alphaScale = h.scale;
    } else {
        stage.colorFunc = h.func;
        stage.colorScale = h.scale;
    }
    return exact;
}
