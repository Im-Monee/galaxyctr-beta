// TEV configurations given as data compiled into 3DS combiner stages (TevCompile3DS.hpp), with
// the terms and halves of tev_terms.h.

#include "TevCompile3DS.hpp"
#include "tev_terms.h"

namespace {
    struct Inputs {
        const RvlTevDesc* desc;
        s32 unit;  // texture unit, or -1 when the stage has no texture
        bool rasZero;
        u32 texSwap[4];
        u32 rasSwap[4];
        u32 kcsel, kasel;
        bool written[4];
    };

    u8 clampColor(s32 v) {
        return v < 0 ? 0 : v > 255 ? 255 : static_cast< u8 >(v);
    }

    // Kept in "previous" once a stage wrote it (an approximation); a negative register
    // becomes a negative term, subtracted by buildHalves.
    Term registerTerm(u32 reg, bool alphaOfRegister, bool alpha, const Inputs& in) {
        if (in.written[reg]) {
            return source(SRC_PREVIOUS, alpha ? OP_A_ALPHA : (alphaOfRegister ? OP_RGB_ALPHA : OP_RGB_COLOR), true);
        }
        const s16* c = in.desc->registers[reg];
        if (alpha || alphaOfRegister) {
            if (c[3] < 0) {
                Term t = constantAlpha(clampColor(-c[3]));
                t.negative = true;
                return t;
            }
            return constantAlpha(clampColor(c[3]));
        }
        if (c[0] <= 0 && c[1] <= 0 && c[2] <= 0 && (c[0] < 0 || c[1] < 0 || c[2] < 0)) {
            Term t = constant(clampColor(-c[0]), clampColor(-c[1]), clampColor(-c[2]));
            t.negative = true;
            return t;
        }
        return constant(clampColor(c[0]), clampColor(c[1]), clampColor(c[2]));
    }

    const u8 kKonstFractions[8] = {255, 223, 191, 159, 128, 96, 64, 32};

    Term konstColor(u32 sel, const Inputs& in) {
        if (sel < 8) {
            u8 v = kKonstFractions[sel];
            return sel == 0 ? one() : constant(v, v, v);
        }
        const u8* k = in.desc->konst[sel & 3];
        if (sel >= 0x0C && sel <= 0x0F) {
            return constant(k[0], k[1], k[2]);
        }
        if (sel >= 0x10) {
            u8 v = k[(sel - 0x10) >> 2];
            return constant(v, v, v);
        }
        return one();
    }

    Term konstAlpha(u32 sel, const Inputs& in) {
        if (sel < 8) {
            return sel == 0 ? one() : constantAlpha(kKonstFractions[sel]);
        }
        if (sel >= 0x10) {
            return constantAlpha(in.desc->konst[sel & 3][(sel - 0x10) >> 2]);
        }
        return one();
    }

    Term colorInput(u32 arg, const Inputs& in) {
        switch (arg) {
        case 0:
        case 2:
        case 4:
        case 6:
            return registerTerm(arg / 2, false, false, in);
        case 1:
        case 3:
        case 5:
        case 7:
            return registerTerm(arg / 2, true, false, in);
        case 8:
            return in.unit < 0 ? one() : source(SRC_TEXTURE0 + in.unit, swappedColorOp(in.texSwap));
        case 9:
            return in.unit < 0 ? one() : source(SRC_TEXTURE0 + in.unit, kColorChannelOp[in.texSwap[3]]);
        case 10:
            return in.rasZero ? zero() : source(SRC_PRIMARY, swappedColorOp(in.rasSwap));
        case 11:
            return in.rasZero ? zero() : source(SRC_PRIMARY, kColorChannelOp[in.rasSwap[3]]);
        case 12:
            return one();
        case 13:
            return constant(128, 128, 128);
        case 14:
            return konstColor(in.kcsel, in);
        default:
            return zero();
        }
    }

    Term alphaInput(u32 arg, const Inputs& in) {
        switch (arg) {
        case 0:
        case 1:
        case 2:
        case 3:
            return registerTerm(arg, true, true, in);
        case 4:
            return in.unit < 0 ? one() : source(SRC_TEXTURE0 + in.unit, kAlphaChannelOp[in.texSwap[3]]);
        case 5:
            return in.rasZero ? zero() : source(SRC_PRIMARY, kAlphaChannelOp[in.rasSwap[3]]);
        case 6:
            return konstAlpha(in.kasel, in);
        default:
            return zero();
        }
    }

    u32 buildFunction(const u8* args, u8 op, u8 bias, u8 scale, bool alpha, const Inputs& in, Half* out) {
        Term t[4];
        for (u32 i = 0; i < 4; i++) {
            t[i] = alpha ? alphaInput(args[i], in) : colorInput(args[i], in);
        }
        if (op >= 8) {  // GX_TEV_COMP_*: the register form's (scale << 1) | subtract is op & 7
            return buildCompareHalves(t[0], t[1], t[2], t[3], op & 7, alpha, out);
        }
        return buildHalves(t[0], t[1], t[2], t[3], bias, op == 1, scale, alpha, out);
    }

    // A colour stage lerp(a, b, x) of two constants by a texture or rasterized input, writing PREV.
    struct ConstantLerp {
        bool valid;
        Term x;
        u8 a[3], b[3];
    };

    ConstantLerp constantLerp(const RvlTevStageDesc& st, const Inputs& in) {
        ConstantLerp l = {};
        if (st.colorOp != 0 || st.colorBias != 0 || st.colorScale != 0 || (st.colorOut & 3) != 0 || st.colorIn[3] != 15) {
            return l;
        }
        Term a = colorInput(st.colorIn[0], in), b = colorInput(st.colorIn[1], in);
        l.x = colorInput(st.colorIn[2], in);
        if (a.src != SRC_CONSTANT || b.src != SRC_CONSTANT || a.negative || b.negative || l.x.src == SRC_CONSTANT || l.x.previous) {
            return l;
        }
        for (u32 i = 0; i < 3; i++) {
            l.a[i] = a.value[i];
            l.b[i] = b.value[i];
        }
        l.valid = true;
        return l;
    }

    // lerp(a, b, P) after P = lerp(a', b', x) is E + F * x, E = lerp(a, b, a') and
    // F = (b - a) * (b' - a'): both constants, in two combiners, when F has one sign. nw4r's
    // C0/C1 stage after a texture already lerped by C0/C1 (a = a', b = b': F >= 0) needed both
    // constants with the previous result, of mixed signs (the pointer's nozzle, the Mii select
    // arrows, the signboard: U6 audit).
    u32 composedLerp(const ConstantLerp& before, const ConstantLerp& now, Half* out) {
        s32 e[3], f[3];
        bool rising = true, falling = true;
        for (u32 i = 0; i < 3; i++) {
            const s32 db = now.b[i] - now.a[i];
            e[i] = now.a[i] + (db * before.a[i] + 127) / 255;
            f[i] = db * (before.b[i] - before.a[i]);
            f[i] = (f[i] >= 0 ? f[i] + 127 : f[i] - 127) / 255;
            rising = rising && f[i] >= 0;
            falling = falling && f[i] <= 0;
        }
        if (!rising && !falling) {
            return 0;
        }
        Term fTerm = rising ? constant(clampColor(f[0]), clampColor(f[1]), clampColor(f[2]))
                            : constant(clampColor(-f[0]), clampColor(-f[1]), clampColor(-f[2]));
        Term eTerm = constant(clampColor(e[0]), clampColor(e[1]), clampColor(e[2]));
        out[0] = half(FUNC_MODULATE, before.x, fTerm, Term(), 2);
        out[1] = rising ? half(FUNC_ADD, previousTerm(false), eTerm, Term(), 2) : half(FUNC_SUBTRACT, eTerm, previousTerm(false), Term(), 2);
        return 2;
    }
}  // namespace

void rvlTevDefaultSwapTables(RvlTevDesc& rDesc) {
    static const u8 kTables[4][4] = {{0, 1, 2, 3}, {0, 0, 0, 3}, {1, 1, 1, 3}, {2, 2, 2, 3}};
    for (u32 i = 0; i < 4; i++) {
        for (u32 c = 0; c < 4; c++) {
            rDesc.swapTables[i][c] = kTables[i][c];
        }
    }
}

bool rvlTevCompile(const RvlTevDesc& rDesc, RvlTevProgram& rProgram) {
    rProgram = RvlTevProgram();
    Inputs in = {};
    in.desc = &rDesc;
    bool complete = true;
    bool rasChosen = false;
    ConstantLerp lastLerp = {};  // the stage before, when it is a lerp of two constants (composedLerp)
    u32 out = 0;
    for (u32 s = 0; s < rDesc.stageCount && s < 16; s++) {
        const RvlTevStageDesc& st = rDesc.stages[s];
        in.unit = -1;
        if (st.texMap != 0xFF) {
            for (u32 u = 0; u < rProgram.unitCount; u++) {
                if (rProgram.unitMap[u] == st.texMap && rProgram.unitCoord[u] == st.texCoord) {
                    in.unit = u;
                }
            }
            if (in.unit < 0 && rProgram.unitCount < 3) {
                in.unit = rProgram.unitCount++;
                rProgram.unitMap[in.unit] = st.texMap;
                rProgram.unitCoord[in.unit] = st.texCoord;
            } else if (in.unit < 0) {
                complete = false;  // the stage reads its texture as 1
            }
        }
        // GXChannelID: COLOR0/ALPHA0/COLOR0A0 rasterize channel 0, the COLOR1 forms channel 1,
        // the rest (COLOR_ZERO, COLOR_NULL) zero.
        u32 ras = st.channel == 4 || st.channel == 0 || st.channel == 2 ? 0 : st.channel == 5 || st.channel == 1 || st.channel == 3 ? 1 : 7;
        in.rasZero = ras == 7;
        if (!in.rasZero && !rasChosen) {
            rProgram.rasChannel = static_cast< u8 >(ras);
            rasChosen = true;
        }
        for (u32 c = 0; c < 4; c++) {
            in.texSwap[c] = rDesc.swapTables[st.texSwap & 3][c] & 3;
            in.rasSwap[c] = rDesc.swapTables[st.rasSwap & 3][c] & 3;
        }
        in.kcsel = st.kcsel;
        in.kasel = st.kasel;

        Half color[kMaxHalves];
        Half alpha[kMaxHalves];
        u32 colorCount = 0;
        const ConstantLerp now = constantLerp(st, in);
        if (lastLerp.valid && st.colorOp == 0 && st.colorBias == 0 && st.colorScale == 0 && st.colorIn[2] == 0 && st.colorIn[3] == 15) {
            ConstantLerp ofPrevious = now;
            Term a = colorInput(st.colorIn[0], in), b = colorInput(st.colorIn[1], in);
            if (a.src == SRC_CONSTANT && b.src == SRC_CONSTANT && !a.negative && !b.negative) {
                for (u32 i = 0; i < 3; i++) {
                    ofPrevious.a[i] = a.value[i];
                    ofPrevious.b[i] = b.value[i];
                }
                colorCount = composedLerp(lastLerp, ofPrevious, color);
            }
        }
        if (colorCount == 0) {
            colorCount = buildFunction(st.colorIn, st.colorOp, st.colorBias, st.colorScale, false, in, color);
        }
        lastLerp = now;
        u32 alphaCount = buildFunction(st.alphaIn, st.alphaOp, st.alphaBias, st.alphaScale, true, in, alpha);
        if (st.colorOp >= 8 || st.alphaOp >= 8) {
            rProgram.inexact = true;
        }
        u32 count = colorCount > alphaCount ? colorCount : alphaCount;
        if (out + count > HOST_GFX_STAGES) {
            complete = false;
            break;
        }
        for (u32 i = 0; i < count; i++) {
            HostGfxStage& stage = rProgram.stages[out + i];
            bool hasColorConst = false, hasAlphaConst = false;
            if (!storeHalf(i < colorCount ? color[i] : half(FUNC_REPLACE, previousTerm(false)), false, stage, hasColorConst, hasAlphaConst)) {
                rProgram.inexact = true;
            }
            if (!storeHalf(i < alphaCount ? alpha[i] : half(FUNC_REPLACE, previousTerm(true)), true, stage, hasColorConst, hasAlphaConst)) {
                rProgram.inexact = true;
            }
        }
        out += count;
        in.written[st.colorOut & 3] = true;
        in.written[st.alphaOut & 3] = true;
    }
    rProgram.stageCount = static_cast< u8 >(out);
    if (!complete) {
        rProgram.inexact = true;
    }
    return complete;
}
