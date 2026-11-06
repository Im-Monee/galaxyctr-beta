#include "Game/LiveActor/HitSensor.hpp"
#include "Game/Player/DLchanger.hpp"
#include "Game/Player/J3DModelX.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioConst.hpp"
#include "Game/Player/MarioParts.hpp"
#include "Game/Player/MarioState.hpp"
#include "Game/Player/MarioSwim.hpp"
#include "Game/Screen/FullScreenBlur.hpp"
#include "Game/Util.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/FurMulti.hpp"
#include "Game/Util/SchedulerUtil.hpp"
#include <JSystem/JGeometry/TBox.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <new>
#include <revolution/gd.h>
#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXFrameBuf.h>
#include <revolution/gx/GXPixel.h>
#include <revolution/os.h>
#ifdef __3DS__
#include "GXDelta3DS.hpp"
#include "GXToCitro3D.hpp"
#include "JUTTexture3DS.hpp"
#include "host.h"

namespace {
    // captureScreenBox's copy and the raster effects' copy of the screen (on its side, with their
    // coordinate mappings).
    void* sScreenBox;
    f32 sScreenBoxMap[4];
    void* sRasterCopy;
    f32 sRasterMap[4];

    // A rectangle of the Wii frame buffer (640x456): a colour (0xRRGGBBAA) or a copy, at a GX depth
    // (0 near, 1 far), with the given blend factors and writes, depth written with GX_ALWAYS.
    void drawFrameBox(f32 x0, f32 y0, f32 x1, f32 y1, u32 color, void* pTexture, const f32* pMap, f32 depth, u8 blendSrc, u8 blendDst,
                      u8 writeMask) {
        const f32 corners[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
        HostGfxVertex vertices[6] = {};
        for (u32 i = 0; i < 6; i++) {
            const f32 s = corners[i][0], t = corners[i][1];
            HostGfxVertex& v = vertices[i];
            v.x = (x0 + s * (x1 - x0)) / 320.0f - 1.0f;
            v.y = 1.0f - (y0 + t * (y1 - y0)) / 228.0f;
            v.z = depth - 1.0f;  // clip z in [-1, 0], 0 the far plane
            v.w = 1.0f;
            v.rgba[0] = color >> 24;
            v.rgba[1] = color >> 16;
            v.rgba[2] = color >> 8;
            v.rgba[3] = color;
            if (pTexture != nullptr) {
                v.s0 = pMap[0] + pMap[1] * t;
                v.t0 = pMap[2] + pMap[3] * s;
            }
        }
        HostGfxState state = {};
        state.textures[0] = pTexture;
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = state.stages[0].alphaSrc[0] = pTexture != nullptr ? 0x03 : 0x00;
        state.alphaTestFunc = GXToCitro3D::TEST_ALWAYS;
        state.depthTest = 1;
        state.depthFunc = GXToCitro3D::TEST_ALWAYS;
        state.writeMask = writeMask;
        state.blendSrc = blendSrc;
        state.blendDst = blendDst;
        state.scissor[2] = 640;
        state.scissor[3] = 456;
        hostGfxDraw(vertices, 6, nullptr, 6, HOST_GFX_TRIANGLES, &state);
    }

    // drawRasterScroll / drawColdWaterDamage: each screen row drawn shifted by its offset from a
    // copy of the screen taken before (the Wii copies each strip just before overwriting it).
    template < typename Offset >
    void drawShiftedRows(Offset offset) {
        const f32 frameWidth = MR::getFrameBufferWidth();
        const u32 height = MR::getScreenHeight();
        sRasterCopy = hostGfxCopyToTexture(sRasterCopy, 0.0f, 0.0f, frameWidth, height, sRasterMap);
        if (sRasterCopy == nullptr) {
            return;
        }
        TDDraw::setupDirect(1, 0, 2);
        HostGfxState state = {};
        state.textures[0] = sRasterCopy;
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = state.stages[0].alphaSrc[0] = 0x03;
        const f32 width = MR::getScreenWidth();
        const u32 kRows = 32;
        TVec3f positions[kRows * 6];
        TVec2f texcoords[kRows * 6][3];
        for (u32 first = 0; first < height; first += kRows) {
            u32 count = 0;
            for (u32 row = first; row < first + kRows && row < height; row++) {
                const f32 x = offset(row);
                const f32 t = (row + 0.5f) / height;
                const f32 corners[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
                for (u32 k = 0; k < 6; k++) {
                    const f32 s = corners[k][0];
                    positions[count].set(x + s * width, row + corners[k][1], 0.0f);
                    texcoords[count][0].set(sRasterMap[0] + sRasterMap[1] * t, sRasterMap[2] + sRasterMap[3] * s);
                    count++;
                }
            }
            TDDraw::drawStateTriangles(positions, texcoords, nullptr, state, count);
        }
        TDDraw::closeDirect();
    }
}  // namespace
#endif

void MarioActorSpecialDraw_FORCE_MATCH_SDATA2() {
    (void)80.0f;
    (void)0.99000001f;
    (void)0.999899983f;
    (void)300.0f;
    (void)1000.0f;
    (void)255.0f;
    (void)63.0f;
    (void)20.0f;
    (void)0.800000012f;
    (void)2607.59448f;
}

void MarioActor::initScreenBox() {
    _B44 = new (32) u8[0x20000];
    _B28 = 0.0f;
    _B24 = 0.0f;
    _B30 = 0.0f;
    _B2C = 0.0f;
    _B38 = 0.0f;
    _B34 = 0.0f;
    _B40 = 0.0f;
    _B3C = 0.0f;
}

bool MarioActor::isUseScreenBox() const {
    if (_A08 == 3) {
        return true;
    }

    return !(_A08 - 7);
}

void MarioActor::calcScreenBoxRange() {
    if (!isUseScreenBox()) {
        return;
    }

    TVec3f position;
    position = _B18;
    TVec2f center;
    TVec2f x;
    TVec2f y;
    TVec2f z;
    TVec2f minimum;
    TVec2f maximum;
    TVec2f frameMin;
    TVec2f frameMax;
    MR::calcScreenPosition(&center, position);
    MR::calcScreenPosition(&x, position + TVec3f(80.0f, 0.0f, 0.0f));
    MR::calcScreenPosition(&y, position + TVec3f(0.0f, 80.0f, 0.0f));
    MR::calcScreenPosition(&z, position + TVec3f(0.0f, 0.0f, 80.0f));
    TVec3f radius((x - center).length(), (y - center).length(), (z - center).length());
    f32 range = radius.length();
    minimum = center - TVec2f(range, range);
    maximum = center + TVec2f(range, range);
    _B24 = minimum.x < maximum.x ? minimum.x : maximum.x;
    _B2C = minimum.x > maximum.x ? minimum.x : maximum.x;
    _B28 = minimum.y < maximum.y ? minimum.y : maximum.y;
    _B30 = minimum.y > maximum.y ? minimum.y : maximum.y;
    MR::convertScreenPosToFrameBufferPos(&frameMin, mScreenBoxMin);
    MR::convertScreenPosToFrameBufferPos(&frameMax, mScreenBoxMax);
    s16 left = static_cast< s32 >(frameMin.x) & ~1;
    s16 top = static_cast< s32 >(frameMin.y) & ~1;
    s16 width = static_cast< s32 >(frameMax.x) - left;
    s16 height = static_cast< s32 >(frameMax.y) - top;
    width = (width + 1) & ~1;
    height = (height + 1) & ~1;

    if (width > 256) {
        width = 256;
    }

    if (height > 256) {
        height = 256;
    }

    if (!width) {
        width = 2;
    }

    if (!height) {
        height = 2;
    }

    _B34 = left;
    _B38 = top;
    _B3C = width;
    _B40 = height;
    TBox2f capture(_B34, _B38, _B34 + _B3C, _B38 + _B40);
    s32 screenHeight = MR::getScreenHeight();
    TBox2f screen(0.0f, 0.0f, MR::getFrameBufferWidth(), screenHeight);
    TBox2f clipped(capture);

    if (!clipped.intersect(screen)) {
        clipped.i.zero();
        clipped.f.zero();
    }

    _B34 = clipped.i.x;
    _B38 = clipped.i.y;
    _B3C = clipped.getWidth();
    _B40 = clipped.getHeight();
}

void MarioActor::captureScreenBox() const {
#ifdef __3DS__
    // The Wii copies the box (GXCopyTex) and clears it: colour 0, depth far (GXSetCopyClear).
    if (!isUseScreenBox()) {
        return;
    }

    if (_B3C < 1.0f || _B40 < 1.0f) {
        return;
    }

    sScreenBox = hostGfxCopyToTexture(sScreenBox, _B34, _B38, _B34 + _B3C, _B38 + _B40, sScreenBoxMap);
    if (sScreenBox == nullptr) {
        return;
    }
    drawFrameBox(_B34, _B38, _B34 + _B3C, _B38 + _B40, 0, nullptr, nullptr, 1.0f, GXToCitro3D::FACTOR_ONE, GXToCitro3D::FACTOR_ZERO,
                 GXToCitro3D::WRITE_RGB | GXToCitro3D::WRITE_ALPHA | GXToCitro3D::WRITE_DEPTH);
#else
    if (!isUseScreenBox()) {
        return;
    }

    if (_B3C < 1.0f || _B40 < 1.0f) {
        return;
    }

    GXSetTexCopyDst(_B3C, _B40, GX_TF_RGB565, GX_FALSE);
    GXSetTexCopySrc(_B34, _B38, _B3C, _B40);
    GXSetColorUpdate(GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);

    GXColor clear = {};

    GXSetCopyClear(clear, 0xFFFFFF);
    GXCopyTex(_B44, GX_TRUE);
    GXSetColorUpdate(GX_ENABLE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetDstAlpha(GX_FALSE, 0);
#endif
}

void MarioActor::writeBackScreenBox() const {
#ifdef __3DS__
    // The copy back where the reflection left the alpha at 0 (GX_BL_INVDSTALPHA, GX_BL_DSTALPHA),
    // depth written at 0.9999 over the box.
    if (!isUseScreenBox() || sScreenBox == nullptr) {
        return;
    }

    if (_B3C < 1.0f || _B40 < 1.0f) {
        return;
    }

    const f32 right = static_cast< s16 >(static_cast< s16 >(static_cast< s32 >(mScreenBoxPos.x + mScreenBoxSize.x + 0.99f) + 1) & ~1);
    drawFrameBox(mScreenBoxPos.x, mScreenBoxPos.y, right, mScreenBoxPos.y + mScreenBoxSize.y + 0.99f, 0xFFFFFFFF, sScreenBox,
                 sScreenBoxMap, 0.9999f, GXToCitro3D::sourceFactor(GX_BL_INVDSTALPHA), GXToCitro3D::destinationFactor(GX_BL_DSTALPHA),
                 GXToCitro3D::WRITE_RGB | GXToCitro3D::WRITE_DEPTH);
#else
    if (!isUseScreenBox()) {
        return;
    }

    if (_B3C < 1.0f || _B40 < 1.0f) {
        return;
    }

    GXTexObj texture;
    TVec2f minimum;
    TVec2f maximum;
    TDDraw::setup(1, 0, 2);
    GXInitTexObj(&texture, _B44, _B3C, _B40, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXLoadTexObj(&texture, GX_TEXMAP0);
    MR::convertFrameBufferPosToScreenPos(&minimum, mScreenBoxPos);
    MR::convertFrameBufferPosToScreenPos(&maximum, mScreenBoxPos + mScreenBoxSize + TVec2f(0.99f, 0.99f));
    GXSetBlendMode(GX_BM_BLEND, GX_BL_INVDSTALPHA, GX_BL_DSTALPHA, GX_LO_NOOP);
    GXSetDstAlpha(GX_TRUE, 0);
    GXSetAlphaUpdate(GX_TRUE);
    GXPixModeSync();
    GXTexModeSync();
    maximum.x = static_cast< s16 >(static_cast< s16 >(static_cast< s32 >(maximum.x) + 1) & ~1);
    GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(minimum.x, minimum.y, 0.9999f);
    GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(maximum.x, minimum.y, 0.9999f);
    GXTexCoord2f32(1.0f, 0.0f);
    GXPosition3f32(maximum.x, maximum.y, 0.9999f);
    GXTexCoord2f32(1.0f, 1.0f);
    GXPosition3f32(minimum.x, maximum.y, 0.9999f);
    GXTexCoord2f32(0.0f, 1.0f);
    GXEnd();
    GXSetDstAlpha(GX_FALSE, 0);
    GXSetAlphaUpdate(GX_FALSE);
    TDDraw::close();
#endif
}

void MarioActor::calc1stPersonView() {
    f32 length = (mCamPos - mPosition).length();
    f32 minVal = 300.0f;
    f32 maxVal = 1000.0f;
    f32 val;

    if (length > maxVal) {
        val = 1.0f;
    } else if (length < minVal) {
        val = 0.0f;
    } else {
        val = 1.0f - (maxVal - length) / (maxVal - minVal);
    }

    u8 compareVal = 255.0f * val;

    if (compareVal == 0) {
        _1A1 = true;
        hideBeeFur();
    } else {
        _1A1 = false;
        updateAlphaDL(compareVal);
    }
}

void MarioActor::hideBeeFur() {
    if (mMario->isPlayerModeBee()) {
        _9E8->kill();
        _9EC->offDraw(-1);
    }

    if (getCarrySensor() != nullptr) {
        MR::hideModel(getCarrySensor()->mHost);
    }

    if (mMario->isPlayerModeInvincible()) {
        _A6E = 0;
        stopEffect("無敵中");
    }
}

void MarioActor::calcFogLighting() {
    Color8 fog(240, 16, 80, 0);

    if (!mMario->isStatusActive(MarioStatus_Paralyze) && isNeedDamageFog()) {
        f32 phase = ((_1A8 + 31) & 63) / 63.0f;

        if (phase > 0.5f) {
            phase = 1.0f - phase;
        }

        f32 wave = 2.0f * phase;
        f32 low = getConst().getTable()->mDamageFogLow;
        _1A4 = (low + wave * (getConst().getTable()->mDamageFogHigh - getConst().getTable()->mDamageFogLow)) / 255.0f;
    } else if (_1AA) {
        fog.mGXColor = _1B0.mGXColor;

        if (_1B5) {
            fog.g = 255.0f * MR::sin(JGeometry::TUtil< f32 >::PI() * (_1AA / 20.0f));
        }

        _1A4 = _1AC * MR::sin((_1AA / static_cast< f32 >(getConst().getTable()->mStarPieceFogTime)) * JGeometry::TUtil< f32 >::PI());
    } else if (mMario->_434) {
        fog.set(255, 255, 0, 0);
        _1A4 = 0.8f * MR::sin(((mMario->_434 / static_cast< f32 >(getConst().getTable()->mItemDashTimer)) * JGeometry::TUtil< f32 >::PI()) / 2.0f);
    } else {
        resetFog();
    }

    Color8 ambient = *MR::getLightAmbientColor(this);
    updateLightDL(ambient, Color8(255, 255, 255, 255), fog, _1A4);
}

void MarioActor::resetFog() {
    Color8 color(0, 0, 0, 0);
    _1A8 = 0;
    _1A4 = 0.0f;
    _1AA = 0;
}

void MarioActor::updateAlphaDL(u8 alpha) {
#ifdef __3DS__
    // The display lists Mario's models call after their material (J3DModelX::drawIn) hold the
    // same GX changes as an RvlGXDelta on 3DS.
    static_assert(sizeof(RvlGXDelta) <= 0x100, "Mario's display list buffers hold an RvlGXDelta");
    DLholder* holder = mDLchanger->swap();
    RvlGXDelta& d = *reinterpret_cast< RvlGXDelta* >(holder->mDL);
    d.clear();
    d.setTevRegister(GX_TEVREG2, 0, 0, 0, alpha);
    // GDSetBlendMode leaves the colour and alpha updates as they are: colour on, alpha off when
    // Mario draws.
    d.setBlend(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP, true, false);
    d.setGenMode(1, 0, 1, GX_CULL_BACK);
    d.setChanCtrl(GX_COLOR0A0, false, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    d.setAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    d.setTevAlphaIn(GX_CA_A2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);
    d.setTevColorIn(GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);
    d.setTevOrder(GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    holder->mSize = sizeof(RvlGXDelta);
#else
    u8 buffer[256] ATTRIBUTE_ALIGN(32);
    GDLObj displayList;
    MR::ProhibitSchedulerAndInterrupts lock(false);
    GDInitGDLObj(&displayList, buffer, sizeof(buffer));
    __GDCurrentDL = &displayList;
    GXColor color = {0, 0, 0, 255};
    color.a = alpha;
    GDSetTevColor(GX_TEVREG2, color);
    GDSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
    GDSetGenMode2(1, 0, 1, 0, GX_CULL_BACK);
    GDSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    GDSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    GDSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_A2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV,
                             GX_TEV_SWAP0, GX_TEV_SWAP0);
    GDSetTevColorCalc(GX_TEVSTAGE0, GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GDSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
    GDPadCurr32();
    u32 size = (displayList.ptr - displayList.start + 31) & ~31;
    DLholder* holder = mDLchanger->swap();
    holder->mSize = displayList.ptr - displayList.start;
    MR::copyMemory(holder->mDL, buffer, size);
    DCStoreRange(holder->mDL, size);
#endif
}

void MarioActor::updateSimpleAlphaDL(u8 alpha) {
    // 3DS: the same GX change as data (RvlGXDelta) instead of a display list.
#ifdef __3DS__
    DLholder* holder = mDLchanger->swap();
    RvlGXDelta& d = *reinterpret_cast< RvlGXDelta* >(holder->mDL);
    d.clear();
    d.setDstAlpha(true, alpha);
    holder->mSize = sizeof(RvlGXDelta);
#else
    u8 buffer[256] ATTRIBUTE_ALIGN(32);
    GDLObj displayList;
    MR::ProhibitSchedulerAndInterrupts lock(false);
    GDInitGDLObj(&displayList, buffer, sizeof(buffer));
    __GDCurrentDL = &displayList;
    GDSetDstAlpha(GX_TRUE, alpha);
    GDPadCurr32();
    u32 size = (displayList.ptr - displayList.start + 31) & ~31;
    DLholder* holder = mDLchanger->swap();
    holder->mSize = displayList.ptr - displayList.start;
    MR::copyMemory(holder->mDL, buffer, size);
    DCStoreRange(holder->mDL, size);
#endif
}

void MarioActor::updateReflectAlphaDL(u8 alpha) {
    // 3DS: the same GX change as data (RvlGXDelta) instead of a display list.
#ifdef __3DS__
    DLholder* holder = mDLchanger->swap();
    RvlGXDelta& d = *reinterpret_cast< RvlGXDelta* >(holder->mDL);
    d.clear();
    d.setTevRegister(GX_TEVREG2, 0, 0, 0, alpha);
    d.setTevAlphaIn(GX_CA_A2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);
    holder->mSize = sizeof(RvlGXDelta);
#else
    u8 buffer[256] ATTRIBUTE_ALIGN(32);
    GDLObj displayList;
    MR::ProhibitSchedulerAndInterrupts lock(false);
    GDInitGDLObj(&displayList, buffer, sizeof(buffer));
    __GDCurrentDL = &displayList;
    GXColor color = {0, 0, 0, 255};
    color.a = alpha;
    GDSetTevColor(GX_TEVREG2, color);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_A2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV,
                             GX_TEV_SWAP0, GX_TEV_SWAP0);
    GDPadCurr32();
    u32 size = (displayList.ptr - displayList.start + 31) & ~31;
    DLholder* holder = mDLchanger->swap();
    holder->mSize = displayList.ptr - displayList.start;
    MR::copyMemory(holder->mDL, buffer, size);
    DCStoreRange(holder->mDL, size);
#endif
}

void MarioActor::updateLightDL(const Color8& rAmbient, const Color8& rMaterial, const Color8& rFog, f32 strength) {
#ifdef __3DS__
    mCurrDL = 1 - mCurrDL;
    RvlGXDelta& d = *reinterpret_cast< RvlGXDelta* >(mDL[mCurrDL]);
    d.clear();
    GXColor ambientColor = rAmbient.mGXColor;
    d.setAmbColor(GX_COLOR0A0, ambientColor.r, ambientColor.g, ambientColor.b, ambientColor.a);
    GXColor materialColor = rMaterial.mGXColor;
    d.setMatColor(GX_COLOR0A0, materialColor.r, materialColor.g, materialColor.b, materialColor.a);
    // GDSetChanCtrl(GX_COLOR1, ...): channel 1 is not used by Mario's native draws.
    if (!MR::isNearZero(strength)) {
        f32 nearZ = MR::getNearZ();
        f32 farZ = MR::getFarZ();
        GXColor color = rFog.mGXColor;
        f32 start;
        f32 end;
        MR::calcFogStartEnd(mPosition, strength, &start, &end);
        d.setFog(GX_FOG_LIN, start, end, nearZ, farZ, color.r, color.g, color.b, color.a);
    }
    mDLSize = sizeof(RvlGXDelta);
#else
    u8 buffer[512] ATTRIBUTE_ALIGN(32);
    GDLObj displayList;
    MR::ProhibitSchedulerAndInterrupts lock(false);
    GDInitGDLObj(&displayList, buffer, sizeof(buffer));
    __GDCurrentDL = &displayList;
    GXColor ambientColor = rAmbient.mGXColor;
    GDSetChanAmbColor(GX_COLOR0A0, ambientColor);
    GXColor materialColor = rMaterial.mGXColor;
    GDSetChanMatColor(GX_COLOR0A0, materialColor);
    GDSetChanCtrl(GX_COLOR1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);

    if (!MR::isNearZero(strength)) {
        f32 nearZ = MR::getNearZ();
        f32 farZ = MR::getFarZ();
        GXColor color = rFog.mGXColor;
        f32 start;
        f32 end;
        MR::calcFogStartEnd(mPosition, strength, &start, &end);
        GDSetFog(GX_FOG_LIN, start, end, nearZ, farZ, color);
    }

    GDPadCurr32();
    u32 length = displayList.ptr - displayList.start;
    mCurrDL = 1 - mCurrDL;
    mDLSize = length;
    u32 size = (length + 31) & ~31;
    MR::copyMemory(mDL[mCurrDL], buffer, size);
    DCStoreRange(mDL[mCurrDL], size);
#endif
}

void MarioActor::createRainbowDL() {
    // 3DS: the same GX changes as data (RvlGXDelta) instead of display lists.
#ifdef __3DS__
    for (u32 alpha = 0; alpha < 8; alpha++) {
        for (u32 index = 0; index < 8; index++) {
            DLchanger* changer = new DLchanger(1, sizeof(RvlGXDelta));
            _94[alpha * 8 + index] = changer;
            u8 r = (index & 1) ? 255 : 0;
            u8 g = (index & 2) ? 255 : 0;
            u8 b = (index & 4) ? 255 : 0;
            if (!index) {
                r = g = b = 64;
            }
            DLholder* holder = &changer->mBuffers[changer->mCurrentBuffer];
            RvlGXDelta& d = *reinterpret_cast< RvlGXDelta* >(holder->mDL);
            d.clear();
            d.setTevRegister(GX_TEVREG0, r, g, b, 16 + (alpha + 1) * 24);
            holder->mSize = sizeof(RvlGXDelta);
        }
    }
#else
    DLholder* holder;
    u8 buffer[256] ATTRIBUTE_ALIGN(32);
    GDLObj displayList;
    MR::ProhibitSchedulerAndInterrupts lock(false);

    for (u32 alpha = 0; alpha < 8; alpha++) {
        for (u32 index = 0; index < 8; index++) {
            _94[alpha * 8 + index] = new DLchanger(1, 32);
            GDInitGDLObj(&displayList, buffer, sizeof(buffer));
            __GDCurrentDL = &displayList;
            GXColor color = {0, 0, 0, 0};

            if (index & 1) {
                color.r = 255;
            }

            if (index & 2) {
                color.g = 255;
            }

            if (index & 4) {
                color.b = 255;
            }

            if (!index) {
                color.r = 64;
                color.g = 64;
                color.b = 64;
            }

            color.a = 16 + (alpha + 1) * 24;
            GDSetTevColor(GX_TEVREG0, color);
            GDPadCurr32();
            u32 length = displayList.ptr - displayList.start;
            u32 size = (length + 31) & ~31;
            DLchanger* changer = _94[alpha * 8 + index];
            holder = &changer->mBuffers[changer->mCurrentBuffer];
            holder->mSize = length;
            MR::copyMemory(holder->mDL, buffer, size);
            DCStoreRange(holder->mDL, size);
        }
    }
#endif
}

void MarioActor::drawScreenBlend() const {
    if (mMario->_97C != nullptr && mMario->_97C->getBlurOffset() != 0.0f) {
        MR::drawFullScreenBlur(mMario->_97C->getBlurOffset());
    }

    if (_1C6) {
        MR::drawFullScreenBlur(_1C8, _1CC, _1D0, _1D1);
    }

    if (mBeeWallWalk) {
        TVec2f position;
        TVec2f opposite;
        TVec2f minimum;
        TVec2f maximum;
        TDDraw::project2D(&position, mMario->mShadowPos);
        opposite = position + TVec2f(100.0f, 100.0f);
        position = position + TVec2f(-100.0f, -100.0f);

        if (position.x >= opposite.x) {
            minimum.x = opposite.x;
            maximum.x = position.x;
        } else {
            minimum.x = position.x;
            maximum.x = opposite.x;
        }

        if (position.y >= opposite.y) {
            minimum.y = opposite.y;
            maximum.y = position.y;
        } else {
            minimum.y = position.y;
            maximum.y = opposite.y;
        }

        position = minimum + TVec2f(-10.0f, -10.0f);
        opposite = maximum + TVec2f(10.0f, 10.0f);
        // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
        TDDraw::setupDirect(0, 0, 2);
        TDDraw::setDirectCullMode(0);
        TDDraw::setDirectBlendState(GXToCitro3D::EQUATION_ADD, GXToCitro3D::sourceFactor(GX_BL_ONE),
                                    GXToCitro3D::destinationFactor(GX_BL_INVDSTALPHA));
        TDDraw::setDirectDestinationAlpha(true, 0);
#else
        TDDraw::setup(0, 0, 2);
        GXSetCullMode(GX_CULL_NONE);
        GXSetAlphaUpdate(GX_FALSE);
        GXSetColorUpdate(GX_TRUE);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_INVDSTALPHA, GX_LO_NOOP);
        GXSetDstAlpha(GX_TRUE, 0);
#endif
        TDDraw::drawFillBox(position, opposite, 0);
        // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
        // (DETAILS.md, section 5).
#ifndef __3DS__
        GXSetAlphaUpdate(GX_FALSE);
#endif
        MR::loadProjectionMtx();
        MR::loadViewMtx();
    }

    if (_BC4) {
        u32 color = 0xFF000080;

        if (mMario->_1C._3) {
            color = 0x00004020;
        }

        TDDraw::setup(0, 1, 2);
        TDDraw::drawFillBox(TVec3f(0.0f, 0.0f, 0.0f),
                            TVec3f(static_cast< f32 >(MR::getScreenWidth()), static_cast< f32 >(MR::getScreenHeight()), 0.0f), color);
        TDDraw::close();
    }
}

void MarioActor::updateRandomTexture(f32 distance) {
    _B88 = 1 - _B88;
    u8* pixel;
    u8* image = mMaskTextures[_B88]->mImage;
    distance = 1.0f - distance / 1000.0f;
    f32 probability = MR::clamp(distance, 0.0f, 1.0f);

    for (u32 y = 0; y < 8; y++) {
        pixel = image + y * 8;

        for (u32 x = 0; x < 8; x++) {
            s32 alpha = *pixel >> 4;

            if (MR::getRandom() < probability) {
                alpha += 4;
            } else {
                alpha--;
            }

            s32 clamped = alpha < 0 ? 0 : alpha > 15 ? 15 : alpha;
            *pixel = (clamped * 16) & 0xF0;
            pixel++;
        }
    }

    DCStoreRange(image, 64);
}

namespace {
    inline TVec3f subtractOffset(const TVec3f& rBase, const TVec3f& rOffset) {
        const TVec3f* pOffset = &rOffset;
        return rBase - *pOffset;
    }
};  // namespace

#pragma push
#pragma global_optimizer off
void MarioActor::drawWallShade(const TVec3f& rPosition, const TVec3f& rNormal, f32 unused) const {
#ifdef __3DS__
    // The Wii's fan: black, alpha TEXA * RASA (128 at the centre, 1 on the rim) over the mask
    // texture, blended by alpha where the depth test passes GREATER, as triangles.
    (void)unused;
    const f32 radius = 100.0f;
    TDDraw::setupDirect(1, 1, 0);
    TDDraw::setDirectDepthMode(true, GXToCitro3D::depthTest(GX_GREATER), false);
    HostGfxStage stage = {};
    stage.colorSrc[0] = 0x0E;  // constant: black
    stage.alphaSrc[0] = 0x03;
    stage.alphaSrc[1] = 0x00;
    stage.alphaFunc = 1;  // modulate
    stage.constant = 0x00000000u;
    TVec3f tangent(0.0f, rNormal.z, -rNormal.y);
    if (MR::isNearZero(tangent)) {
        tangent.set< f32 >(rNormal.z, 0.0f, rNormal.x);
    }
    MR::normalizeOrZero(&tangent);
    Mtx rotation;
    PSMTXRotAxisRad(rotation, &rNormal, 0.3926991f);
    const TVec3f center(rPosition.x - 5.0f * rNormal.x, rPosition.y - 5.0f * rNormal.y, rPosition.z - 5.0f * rNormal.z);
    TVec3f rim[17];
    TVec2f rimUV[17];
    for (u32 i = 0; i <= 16; i++) {
        const f32 angle = 2.0f * ((i / 16.0f) * JGeometry::TUtil< f32 >::PI());
        rim[i] = subtractOffset(rPosition, rNormal * 5.0f) + tangent * radius;
        rimUV[i].set(10.0f * JMACosRadian(angle), 10.0f * MR::sin(angle));
        PSMTXMultVecSR(rotation, &tangent, &tangent);
    }
    TVec3f positions[48];
    TVec2f texcoords[48];
    u32 colors[48];
    for (u32 i = 0; i < 16; i++) {
        positions[i * 3] = center;
        texcoords[i * 3].set(0.0f, 0.0f);
        colors[i * 3] = 128;
        positions[i * 3 + 1] = rim[i];
        texcoords[i * 3 + 1] = rimUV[i];
        colors[i * 3 + 1] = 1;
        positions[i * 3 + 2] = rim[i + 1];
        texcoords[i * 3 + 2] = rimUV[i + 1];
        colors[i * 3 + 2] = 1;
    }
    TDDraw::drawStageTriangles(positions, texcoords, 48, mMaskTextures[_B88], 0, &stage, 1, colors);
    TDDraw::closeDirect();
#else
    f32 radius = 100.0f;
    TDDraw::setup(1, 1, 0);
    GXSetZMode(GX_TRUE, GX_GREATER, GX_FALSE);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    mMaskTextures[_B88]->load(GX_TEXMAP0);
    GXClearVtxDesc();
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    Mtx rotation;
    TVec3f tangent(0.0f, rNormal.z, -rNormal.y);

    if (MR::isNearZero(tangent)) {
        tangent.set< f32 >(rNormal.z, 0.0f, rNormal.x);
    }

    MR::normalizeOrZero(&tangent);
    PSMTXRotAxisRad(rotation, &rNormal, 0.3926991f);
    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, 18);
    GXPosition3f32(rPosition.x - 5.0f * rNormal.x, rPosition.y - 5.0f * rNormal.y, rPosition.z - 5.0f * rNormal.z);
    GXColor1u32(128);
    GXTexCoord2f32(0.0f, 0.0f);

    for (u32 i = 0; i <= 16; i++) {
        f32 angle = 2.0f * ((i / 16.0f) * JGeometry::TUtil< f32 >::PI());
        TVec3f point = subtractOffset(rPosition, rNormal * 5.0f) + tangent * radius;
        GXPosition3f32(point.x, point.y, point.z);
        GXColor1u32(1);
        GXTexCoord2f32(10.0f * JMACosRadian(angle), 10.0f * MR::sin(angle));
        PSMTXMultVecSR(rotation, &tangent, &tangent);
    }

    GXEnd();
#endif
}
#pragma pop

void MarioActor::drawSpinInhibit() const {
}

void MarioActor::drawColdWaterDamage() const {
    // 3DS: the cold water's wavy screen through DirectDraw strips of the screen capture.
#ifdef __3DS__
    f32 frequency = 89.0f * MR::sin((TWO_PI * mMario->mSwim->mColdWaterDamageInterval) / 120.0f);
    if (frequency > 0.0f) {
        frequency = 0.0f;
    }
    const f32 height = MR::getScreenHeight();
    const u32 frame = _37C;
    drawShiftedRows([=](u32 row) {
        return 5.0f * MR::sin(2.0f * (((frame + row * frequency) / height) * JGeometry::TUtil< f32 >::PI()));
    });
#else
    u32 next;
    u32 stripHeight = 4;
    GXTexObj textures[2];
    GXDrawDone();
    GXTexModeSync();
    GXPixModeSync();
    u16 width = MR::getFrameBufferWidth();
    TDDraw::setup(1, 0, 2);
    u16 current = 0;
    GXInitTexObj(&textures[0], _1D8, width, 4, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObj(&textures[1], _1DC, width, 4, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXSetLineWidth(6, GX_TO_ZERO);
    GXSetTexCopyDst(width, 4, GX_TF_RGB565, GX_FALSE);
    GXSetTexCopySrc(0, 0, width, 4);
    GXCopyTex(_1D8, GX_FALSE);
    f32 textureStep = 1.0f / static_cast< f32 >(stripHeight);
    f32 frequency = 89.0f * MR::sin((TWO_PI * mMario->mSwim->mColdWaterDamageInterval) / 120.0f);

    if (frequency > 0.0f) {
        frequency = 0.0f;
    }

    for (u32 y = 0; y < MR::getScreenHeight(); y += 4) {
        GXTexModeSync();
        GXPixModeSync();
        GXSetTexCopySrc(0, y + 4, width, 4);
        next = 1 - current;
        GXCopyTex(mRasterBuffers[next], GX_FALSE);
        GXLoadTexObj(&textures[current], GX_TEXMAP0);
        f32 v = 0.0f;

        for (u32 row = y; row < y + 4; row++) {
            f32 offset = 5.0f * MR::sin(2.0f * (((_37C + row * frequency) / MR::getScreenHeight()) * JGeometry::TUtil< f32 >::PI()));
            GXBegin(GX_LINES, GX_VTXFMT0, 2);
            GXPosition3f32(offset, row, 0.0f);
            GXTexCoord2f32(0.0f, v);
            GXPosition3f32(offset + MR::getScreenWidth(), row, 0.0f);
            GXTexCoord2f32(1.0f, v);
            GXEnd();
            v += textureStep;
        }

        current = next;
    }

    GXDrawDone();
#endif
}

void MarioActor::setRasterScroll(s32 i1, s32 i2, s32 i3) {
    _1E8 = i2;
    _1E4 = 0.9f * _1E4 + 0.1f * i1;
    _1EC = i3;
    _1E2 = 1;
}

void MarioActor::updateRasterScroll() {
    if (_1E2) {
        _1E2 = 0;
        return;
    }

    _1E4 *= 0.9f;

    if (_1E4 < 1.0f) {
        _1E8 = 0;
        _1E4 = 0.0f;
    }

    if (_1EC < 1.0f) {
        _1EC = 0.0f;
    }
}

void MarioActor::drawRasterScroll(f32 amplitude, s16 period, f32 wavelength) const {
    // 3DS: the same effect through DirectDraw.
#ifdef __3DS__
    if (wavelength == 0.0f) {
        return;
    }

    if (period == 0) {
        return;
    }

    const f32 phase = (_37C * JMath::TAngleConstant_< f32 >::RADIAN_DEG360()) / period;
    const f32 spatialPhase = TWO_PI / wavelength;
    drawShiftedRows([=](u32 row) {
        const f32 angle = MR::sin(spatialPhase * row) * JGeometry::TUtil< f32 >::PI();
        return amplitude * MR::sin(angle + phase);
    });
#else
    if (wavelength == 0.0f) {
        return;
    }

    if (period == 0) {
        return;
    }

    u32 next;
    u32 stripHeight = 4;
    GXTexObj textures[2];
    GXDrawDone();
    GXTexModeSync();
    GXPixModeSync();
    u16 width = MR::getFrameBufferWidth();
    TDDraw::setup(1, 0, 2);
    u16 current = 0;
    GXInitTexObj(&textures[0], _1D8, width, 4, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObj(&textures[1], _1DC, width, 4, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXSetLineWidth(6, GX_TO_ZERO);
    GXSetTexCopyDst(width, 4, GX_TF_RGB565, GX_FALSE);
    GXSetTexCopySrc(0, 0, width, 4);
    GXCopyTex(_1D8, GX_FALSE);
    f32 textureStep = 1.0f / static_cast< f32 >(stripHeight);
    f32 phase = (_37C * JMath::TAngleConstant_< f32 >::RADIAN_DEG360()) / period;
    f32 spatialPhase = TWO_PI / wavelength;

    for (u32 y = 0; y < MR::getScreenHeight(); y += 4) {
        GXTexModeSync();
        GXPixModeSync();
        GXSetTexCopySrc(0, y + 4, width, 4);
        next = 1 - current;
        GXCopyTex(mRasterBuffers[next], GX_FALSE);
        GXLoadTexObj(&textures[current], GX_TEXMAP0);
        f32 v = 0.0f;

        for (u32 row = y; row < y + 4; row++) {
            f32 angle = MR::sin(spatialPhase * row) * JGeometry::TUtil< f32 >::PI();
            f32 offset = amplitude * MR::sin(angle + phase);
            GXBegin(GX_LINES, GX_VTXFMT0, 2);
            GXPosition3f32(offset, row, 0.0f);
            GXTexCoord2f32(0.0f, v);
            GXPosition3f32(offset + MR::getScreenWidth(), row, 0.0f);
            GXTexCoord2f32(1.0f, v);
            GXEnd();
            v += textureStep;
        }

        current = next;
    }

    GXDrawDone();
#endif
}

void MarioActor::drawMosaic() const {
}

void MarioActor::drawLifeUp() const {
}

void MarioActor::calcSpinEffect() {
    f32 baseRadius = 30.0f;
    f32 radiusScale = 5.8f;
    _6D4 = 0.0f;
    _6D8 = 0.0f;

    if (selectAction("スピン回復エフェクト") != 1) {
        return;
    }

    if (!_945) {
        return;
    }

    if (_944) {
        return;
    }

    if (!_946) {
        return;
    }

    s32 frame = _946 - 9;

    if (frame <= 0 || frame > 15) {
        _6D4 = 0.0f;
        return;
    }

    if (frame < 10) {
        _6D4 = baseRadius + (frame / 10.0f) * (_945 * radiusScale);
        _6D8 = 120.0f;
        return;
    }

    _6D4 = baseRadius + _945 * radiusScale;
    _6D8 = 120.0f;
}

void MarioActor::drawSpinEffect() const {
    if (_6D4 == 0.0f) {
        return;
    }

    if (!isEnableNerveChange()) {
        return;
    }

    // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
    TDDraw::setupDirect(0, 1, 0);
    TDDraw::setDirectBlendState(GXToCitro3D::EQUATION_ADD, GXToCitro3D::FACTOR_SRC_ALPHA, GXToCitro3D::FACTOR_ONE);
#else
    TDDraw::setup(0, 1, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_NOOP);
#endif
    Mtx rotation;
    TVec3f center = _2A0 + mMario->mHeadVec * _6D8;
    TVec3f direction(mMario->mFrontVec);
    TVec3f previous;
    PSMTXRotAxisRad(rotation, &mMario->mHeadVec, 0.09817477f);
    // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
    // (DETAILS.md, section 5).
#ifndef __3DS__
    GXSetLineWidth(36, GX_TO_ZERO);
#endif

    for (u32 i = 0; i <= 64; i++) {
        TVec3f point(direction);
        point.setLength(_6D4);
        point += mMario->mHeadVec * MR::getRandom() * 10.0f;

        if (i) {
#ifdef __3DS__
            TDDraw::drawLineWidth(center + previous, center + point, 0xFFFFFF20, 36);  // GXSetLineWidth(36)
#else
            TDDraw::drawLine(center + previous, center + point, 0xFFFFFF20);
#endif
        }

        previous = point;
        PSMTXMultVecSR(rotation, &direction, &direction);
    }

    // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
    // (DETAILS.md, section 5).
#ifndef __3DS__
    GXSetLineWidth(18, GX_TO_ZERO);
#endif

    for (u32 i = 0; i <= 64; i++) {
        TVec3f point(direction);
        point.setLength(_6D4);
        point += mMario->mHeadVec * MR::getRandom() * 10.0f;
        f32 wave = MR::sin((((_37C + i) & 31) / 32.0f) * JGeometry::TUtil< f32 >::PI());
        f32 square = wave * wave;
        u32 alpha = 160.0f * (square * square);

        if (i) {
#ifdef __3DS__
            TDDraw::drawLineWidth(center + previous, center + point, alpha | 0x40FF00 | (alpha << 24), 18);  // GXSetLineWidth(18)
#else
            TDDraw::drawLine(center + previous, center + point, alpha | 0x40FF00 | (alpha << 24));
#endif
        }

        previous = point;
        PSMTXMultVecSR(rotation, &direction, &direction);
    }

    // 3DS: ends the DirectDraw state.
#ifdef __3DS__
    TDDraw::closeDirect();
#else
    TDDraw::close();
#endif
}

void MarioActor::drawSphereMask() const {
    if (!mMario->isVisibleRecoveryWarpBubble()) {
        return;
    }

    // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
    TDDraw::setupDirect(0, 1, 0);
    TDDraw::setDirectDepthMode(true, GXToCitro3D::depthTest(GX_GREATER), true);
    TDDraw::setDirectCullMode(2);
    TDDraw::setDirectBlendState(GXToCitro3D::EQUATION_ADD, GXToCitro3D::FACTOR_SRC_ALPHA, GXToCitro3D::FACTOR_ONE);
#else
    TDDraw::setup(0, 1, 0);
    GXSetZScaleOffset(1.0f, 0.00001f);
    GXSetCullMode(GX_CULL_FRONT);
    GXSetZMode(GX_TRUE, GX_GREATER, GX_TRUE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_NOOP);
#endif
    TVec3f position;
    getRealPos("Spine1", &position);
    TDDraw::drawSphere3D(position, 180.0f, 0xFFFFFF30, 16);
    // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
    TDDraw::closeDirect();
#else
    GXSetZScaleOffset(1.0f, 0.0f);
#endif
}

void MarioActor::initDarkMask() {
    for (u32 texture = 0; texture < 2; texture++) {
        u8* image = mMaskTextures[texture]->mImage;

        for (u32 y = 0; y < 8; y++) {
            u8* pixel = image + y * 8;

            for (u32 x = 0; x < 8; x++) {
                pixel[x] = 0;
            }
        }
    }

    _1C3 = true;
    _1C4 = 0;
}

void MarioActor::updateDarkMask(u16 unused) {
    if (!_1C3) {
        return;
    }

    u8* previous = mMaskTextures[_B88]->mImage;
    _B88 = 1 - _B88;
    u8* image = mMaskTextures[_B88]->mImage;
    u8 x = MR::getRandom(0L, 8L);
    u8 y = MR::getRandom(0L, 8L);
    s32 index = x + y * 8;
    image[index] = 0xF0;
    previous[index] = 0xF0;
    DCStoreRange(image, 64);
    _1C4++;
}

bool MarioActor::drawDarkMask() const {
#ifdef __3DS__
    // The Wii writes alpha 255 where the 8x8 noise texture (tiled 4 times over a square around
    // Mario) is above 1, and draws Mario with mode list 15 (GX_BL_INVDSTALPHA, GX_BL_DSTALPHA):
    // he is not drawn there. The mark goes in the stencil (J3DModelX's list 15 tests it).
    if (!_1C3) {
        return false;
    }

    TVec3f position(mPosition);
    TVec2f center;
    TVec2f screenX;
    TVec2f screenY;
    TVec2f origin;
    TVec2f frameOrigin;
    TVec2f minimum;
    TVec2f maximum;
    f32 radius = _1C4;

    if (radius >= 240.0f) {
        radius = 240.0f;
    }

    TDDraw::project2D(&center, position);
    TDDraw::project2D(&screenX, position + mCamDirX * radius);
    TDDraw::project2D(&screenY, position + mCamDirY * radius);
    u16 width = ((static_cast< u32 >((screenX - center).length()) + 3) & ~3) + 4;
    u16 height = ((static_cast< u32 >((screenY - center).length()) + 3) & ~3) + 4;
    TDDraw::project2D(&origin, position);
    origin.x -= width / 2;
    origin.y -= width / 2;
    MR::convertScreenPosToFrameBufferPos(&frameOrigin, origin);
    u32 left = static_cast< u32 >(frameOrigin.x);
    u32 top = static_cast< u32 >(frameOrigin.y);
    top &= ~1;
    left &= ~1;
    MR::convertFrameBufferPosToScreenPos(&minimum, TVec2f(left, top));
    MR::convertFrameBufferPosToScreenPos(&maximum, TVec2f(left + width, top + height));
    // The noise changes every frame on the CPU (updateDarkMask, updateRandomTexture).
    rvl3DSInvalidateJUTTexture(mMaskTextures[_B88]);
    TDDraw::setupDirect(1, 0, 2);
    TDDraw::setDirectPixelWriteMask(0);
    TDDraw::setDirectAlphaTest(GXToCitro3D::alphaTest(GX_GREATER), 1);
    TDDraw::setDirectStencil(true, GXToCitro3D::TEST_ALWAYS, 1, 0xFF, 0xFF, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP,
                             GXToCitro3D::STENCIL_REPLACE);
    const TVec3f corners[6] = {TVec3f(minimum.x, minimum.y, 0.0f), TVec3f(maximum.x, minimum.y, 0.0f), TVec3f(maximum.x, maximum.y, 0.0f),
                               TVec3f(minimum.x, minimum.y, 0.0f), TVec3f(maximum.x, maximum.y, 0.0f), TVec3f(minimum.x, maximum.y, 0.0f)};
    const TVec2f texcoords[6] = {TVec2f(0.0f, 0.0f), TVec2f(4.0f, 0.0f), TVec2f(4.0f, 4.0f),
                                 TVec2f(0.0f, 0.0f), TVec2f(4.0f, 4.0f), TVec2f(0.0f, 4.0f)};
    TDDraw::drawTexturedTriangles(corners, texcoords, mMaskTextures[_B88], 6);
    TDDraw::closeDirect();
    return true;
#else
    if (!_1C3) {
        return false;
    }

    TDDraw::setup(1, 0, 2);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetColorUpdate(GX_FALSE);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    mMaskTextures[_B88]->load(GX_TEXMAP0);
    GXSetZCompLoc(GX_FALSE);
    GXSetAlphaCompare(GX_GREATER, 1, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetDstAlpha(GX_TRUE, 255);
    TVec3f spine;
    TVec2f center;
    TVec2f screenX;
    TVec2f screenY;
    TVec2f origin;
    TVec2f frameOrigin;
    TVec2f minimum;
    TVec2f maximum;
    getRealPos("Spine1", &spine);
    f32 radius = _1C4;

    if (radius >= 240.0f) {
        radius = 240.0f;
    }

    TVec3f position(mPosition);
    TDDraw::project2D(&center, position);
    TDDraw::project2D(&screenX, position + mCamDirX * radius);
    TDDraw::project2D(&screenY, position + mCamDirY * radius);
    u16 width = ((static_cast< u32 >((screenX - center).length()) + 3) & ~3) + 4;
    u16 height = ((static_cast< u32 >((screenY - center).length()) + 3) & ~3) + 4;
    TDDraw::project2D(&origin, position);
    origin.x -= width / 2;
    origin.y -= width / 2;
    MR::convertScreenPosToFrameBufferPos(&frameOrigin, origin);
    u32 left = static_cast< u32 >(frameOrigin.x);
    u32 top = static_cast< u32 >(frameOrigin.y);
    top &= ~1;
    left &= ~1;
    MR::convertFrameBufferPosToScreenPos(&minimum, TVec2f(left, top));
    MR::convertFrameBufferPosToScreenPos(&maximum, TVec2f(left + width, top + height));
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(minimum.x, minimum.y, 0.0f);
    GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(maximum.x, minimum.y, 0.0f);
    GXTexCoord2f32(4.0f, 0.0f);
    GXPosition3f32(maximum.x, maximum.y, 0.0f);
    GXTexCoord2f32(4.0f, 4.0f);
    GXPosition3f32(minimum.x, maximum.y, 0.0f);
    GXTexCoord2f32(0.0f, 4.0f);
    GXEnd();
    GXSetDstAlpha(GX_FALSE, 0);
    GXSetColorUpdate(GX_TRUE);
    GXSetZCompLoc(GX_TRUE);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    TDDraw::close();
    return true;
#endif
}

void MarioActor::showBeeFur() {
    if (mMario->isPlayerModeBee()) {
        _9E8->appear();
        _9EC->onDraw(-1);
    }

    if (getCarrySensor() != nullptr) {
        MR::showModel(getCarrySensor()->mHost);
    }

    if (mMario->isPlayerModeInvincible()) {
        MR::showJoint(getJ3DModel(), "Face0");
        _A6E = 2;
        playEffect("無敵中");
    }
}

f32 MarioState::getBlurOffset() const {
    return 0.0f;
}
