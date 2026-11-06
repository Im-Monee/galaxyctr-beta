#pragma once

// ##############################################################################################
// # GX values to 3DS GPU values                                                                #
// #                                                                                            #
// # Wii GX state values turned into 3DS GPU (Citro3D) values, for the game code that draws     #
// # with Citro3D directly (DirectDraw, layouts, particles). Values are libctru's gpu/enums.h,  #
// # checked against both headers.                                                              #
// #                                                                                            #
// # The 3DS depth buffer keeps 1 at the near plane (the Wii keeps 0), so a depth comparison is #
// # reversed. A GX cull mode names the faces culled, GX front faces being clockwise on screen. #
// ##############################################################################################

#include <revolution/types.h>

namespace GXToCitro3D {
    enum {
        TEST_NEVER = 0,
        TEST_ALWAYS = 1,
        TEST_EQUAL = 2,
        TEST_NOTEQUAL = 3,
        TEST_LESS = 4,
        TEST_LEQUAL = 5,
        TEST_GREATER = 6,
        TEST_GEQUAL = 7,

        EQUATION_ADD = 0,
        EQUATION_SUBTRACT = 1,          // source - destination
        EQUATION_REVERSE_SUBTRACT = 2,  // destination - source

        FACTOR_ZERO = 0,
        FACTOR_ONE = 1,
        FACTOR_SRC_ALPHA = 6,
        FACTOR_ONE_MINUS_SRC_ALPHA = 7,

        CULL_NONE = 0,
        CULL_FRONT_CCW = 1,
        CULL_BACK_CCW = 2,

        // GPU_STENCILOP (the Wii has no stencil: the 3DS uses it where the Wii counts in the
        // destination alpha).
        STENCIL_KEEP = 0,
        STENCIL_ZERO = 1,
        STENCIL_REPLACE = 2,
        STENCIL_INCR = 3,  // saturated, as the Wii's clamped alpha
        STENCIL_DECR = 4,

        WRITE_ALPHA = 0x08,
        WRITE_RGB = 0x07,
        WRITE_DEPTH = 0x10,
    };

    // GXCompare (GX_NEVER .. GX_ALWAYS) of an alpha comparison.
    inline u8 alphaTest(u32 compare) {
        static const u8 kAlpha[8] = {TEST_NEVER, TEST_LESS, TEST_EQUAL, TEST_LEQUAL, TEST_GREATER, TEST_NOTEQUAL, TEST_GEQUAL, TEST_ALWAYS};
        return kAlpha[compare & 7];
    }

    // GXCompare of a depth comparison (depth reversed).
    inline u8 depthTest(u32 compare) {
        static const u8 kDepth[8] = {TEST_NEVER, TEST_GREATER, TEST_EQUAL, TEST_GEQUAL, TEST_LESS, TEST_NOTEQUAL, TEST_LEQUAL, TEST_ALWAYS};
        return kDepth[compare & 7];
    }

    // GXBlendFactor as a source factor (entries 2-3 read the destination colour).
    inline u8 sourceFactor(u32 factor) {
        static const u8 kSource[8] = {0, 1, 4, 5, 6, 7, 8, 9};
        return kSource[factor & 7];
    }

    // GXBlendFactor as a destination factor (entries 2-3 read the source colour).
    inline u8 destinationFactor(u32 factor) {
        static const u8 kDestination[8] = {0, 1, 2, 3, 6, 7, 8, 9};
        return kDestination[factor & 7];
    }

    // GXLogicOp -> GPU_LOGICOP.
    inline u8 logicOp(u32 op) {
        static const u8 kLogic[16] = {0, 1, 2, 3, 13, 6, 11, 9, 10, 12, 7, 14, 5, 15, 8, 4};
        return kLogic[op & 15];
    }

    // GXSetBlendMode(type, source, destination, logic op) as a blend equation and factors. A logic
    // operation is returned through pLogic (-1 when the mode does not use one).
    inline void blendMode(u32 type, u32 source, u32 destination, u32 op, u8& equation, u8& src, u8& dst, int& logic) {
        logic = -1;
        equation = EQUATION_ADD;
        switch (type) {
        case 1:  // GX_BM_BLEND
            src = sourceFactor(source);
            dst = destinationFactor(destination);
            break;
        case 2:  // GX_BM_LOGIC
            src = FACTOR_ONE;
            dst = FACTOR_ZERO;
            logic = logicOp(op);
            break;
        case 3:  // GX_BM_SUBTRACT: destination - source, factors ignored
            equation = EQUATION_REVERSE_SUBTRACT;
            src = FACTOR_ONE;
            dst = FACTOR_ONE;
            break;
        default:  // GX_BM_NONE
            src = FACTOR_ONE;
            dst = FACTOR_ZERO;
            break;
        }
    }

    // GXCullMode -> GPU_CULLMODE. GX_CULL_ALL has no 3DS equivalent: pCullAll tells the caller to
    // skip the polygons.
    inline u8 cullMode(u32 mode, bool& cullAll) {
        cullAll = mode == 3;
        return mode == 1 ? CULL_BACK_CCW : mode == 2 ? CULL_FRONT_CCW : CULL_NONE;
    }
}  // namespace GXToCitro3D
