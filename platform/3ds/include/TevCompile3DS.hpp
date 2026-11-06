#pragma once

// 3DS: a Wii TEV configuration given as data (the values of GXSetTevOrder, GXSetTevColorIn/Op,
// GXSetTevAlphaIn/Op, GXSetTevKColorSel/KAlphaSel, GXSetTevSwapMode(Table), GXSetTevColorS10 and
// GXSetTevKColor) compiled into 3DS texture combiner stages, by the rules and approximations of
// platform/3ds/gx/tev_terms.h. For draws that set their TEV without GX (nw4r layout materials).

#include "host.h"
#include <revolution/types.h>

struct RvlTevStageDesc {
    u8 texCoord;  // GXTexCoordID
    u8 texMap;    // GXTexMapID, 0xFF: none
    u8 channel;   // GXChannelID, 0xFF: none
    u8 colorIn[4], colorOp, colorBias, colorScale, colorOut;  // GXTevColorArg a-d, GXTevOp, ...
    u8 alphaIn[4], alphaOp, alphaBias, alphaScale, alphaOut;
    u8 kcsel, kasel;     // GXTevKColorSel, GXTevKAlphaSel
    u8 rasSwap, texSwap;  // GXTevSwapSel
};

struct RvlTevDesc {
    u8 stageCount;
    RvlTevStageDesc stages[16];
    u8 swapTables[4][4];  // GXTevColorChan per r, g, b, a
    s16 registers[4][4];  // GX_TEVPREV, GX_TEVREG0-2 as RGBA (signed, GXColorS10)
    u8 konst[4][4];       // GX_KCOLOR0-3 as RGBA
};

struct RvlTevProgram {
    u8 stageCount;
    u8 unitCount;      // texture units used: unit u samples unitMap[u] with unitCoord[u]
    u8 unitMap[3], unitCoord[3];
    u8 rasChannel;     // 0: COLOR0A0, 1: COLOR1A1
    bool inexact;      // an approximation was needed (see tev_terms.h)
    HostGfxStage stages[HOST_GFX_STAGES];
};

// The default swap tables (GXSetTevSwapModeTable at GX initialisation).
void rvlTevDefaultSwapTables(RvlTevDesc& rDesc);

// False when no texture unit or combiner stage was available for part of the TEV; the program
// then holds the stages that fitted.
bool rvlTevCompile(const RvlTevDesc& rDesc, RvlTevProgram& rProgram);
