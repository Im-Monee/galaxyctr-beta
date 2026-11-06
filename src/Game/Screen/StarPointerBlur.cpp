#include "Game/Screen/StarPointerBlur.hpp"
#include "Game/Screen/StarPointerDirector.hpp"
#include "Game/Util/MathUtil.hpp"
#ifdef __3DS__
#include "Game/Util/DirectDraw.hpp"
#endif
#include "Game/Util/StarPointerUtil.hpp"
#ifdef __3DS__
#include "JUTTexture3DS.hpp"
#endif
#include <JSystem/JUtility/JUTTexture.hpp>
#include <revolution/gx/GXStruct.h>
#include <revolution/gx/GXTransform.h>
#include <revolution/gx/GXVert.h>
#ifdef __3DS__
#include "GXToCitro3D.hpp"
#endif

namespace {
    static const f32 sBeginThick = 10.0f;
    static const s32 BLUR_POINT_NUM = 20;
    static const s32 sDrawMode = 2;
    static const f32 sEndThick = 0.0f;
};  // namespace

StarPointerBlur::~StarPointerBlur() {
}

StarPointerBlur::StarPointerBlur(JUTTexture* pTexture)
    : NameObj("スターポインタ用ブラー"), mTexture(pTexture), mPadChannel(WPAD_CHAN0), mMainColor(nullptr), mSubColor(nullptr), mBlurPoints(nullptr),
      mBlurThicks(nullptr), mBlurTexCoords(nullptr), mNumHiddenPoints(0), _2C(false), _2D(false), _30(0), _34(0), _38(0), _3C(0) {
    mBlurPoints = new BlurPoint[BLUR_POINT_NUM];
    mBlurThicks = new f32[BLUR_POINT_NUM];
    mBlurTexCoords = new f32[BLUR_POINT_NUM];

    f32 interval = 1.0f / BLUR_POINT_NUM;
    for (s32 idx = 0; idx < BLUR_POINT_NUM; idx++) {
        f32 t = idx * interval;
        mBlurThicks[idx] = (1.0f - t) * ::sBeginThick + t * ::sEndThick;
        mBlurTexCoords[idx] = t;
    }
}

StarPointerBlur::BlurPoint::BlurPoint() : mPos(0.0f, 0.0f), mDir(0.0f, 1.0f), mIsHidden(true) {
}

void StarPointerBlur::show() {
    updatePoint(MR::getStarPointerScreenPositionOrEdge(mPadChannel), false, nullptr);
}

void StarPointerBlur::hide() {
    if (mNumHiddenPoints >= BLUR_POINT_NUM) {
        return;
    }

    updatePoint(mBlurPoints[0].mPos, true, &mBlurPoints[0].mDir);
}

void StarPointerBlur::hideAll() {
    if (mNumHiddenPoints >= BLUR_POINT_NUM) {
        return;
    }

    for (s32 idx = 0; idx < BLUR_POINT_NUM; idx++) {
        mBlurPoints[idx].mIsHidden = true;
    }

    mNumHiddenPoints = BLUR_POINT_NUM;
}

void StarPointerBlur::movement() {
    s32 pastPointNum = StarPointerFunction::getPastPointNum(mPadChannel);
    s32 nextPastPointNum = StarPointerFunction::getNextPastPointNum(mPadChannel);

    if (pastPointNum <= 0) {
        hide();
        return;
    }

    for (s32 idx = pastPointNum - 1; idx >= 0; idx--) {
        TVec2f pastPos = StarPointerFunction::getPastPosition(mPadChannel, idx);
        StarPointerFunction::forceInsideScreenEdge(&pastPos);
        updatePoint(pastPos, false, nullptr);
    }
}

void StarPointerBlur::updatePoint(const TVec2f& rPos, bool hide, TVec2f* pDir) {
    if (hide == true) {
        mNumHiddenPoints++;
    } else {
        mNumHiddenPoints = 0;
    }

    for (s32 idx = BLUR_POINT_NUM - 1; idx > 0; idx--) {
        mBlurPoints[idx] = mBlurPoints[idx - 1];
    }

    mBlurPoints[0].mPos.set(rPos);
    mBlurPoints[0].mIsHidden = hide;

    if (pDir != nullptr) {
        mBlurPoints[0].mDir.set(*pDir);
    } else {
        mBlurPoints[0].mDir = rPos - mBlurPoints[1].mPos;
    }

    MR::normalizeOrZero(&mBlurPoints[0].mDir);
}

u16 StarPointerBlur::findDrawPointRange(u16* start, u16* end) const {
    // [WARNING] LIKELY A FAKEMATCH: I do not believe this is correct, as nearly every other case a goto has
    // been "required" has been able to be rewritten without a goto

    u16 range;
    u16 idx = *start;
    for (; idx < BLUR_POINT_NUM; idx++) {
        if (mBlurPoints[idx].mIsHidden == false) {
            *start = idx;
            range = 1;
            goto findEnd;
        }
    }

    *end = BLUR_POINT_NUM;
    *start = BLUR_POINT_NUM;
    return 0;

findEnd:
    for (idx = *start + 1; idx < BLUR_POINT_NUM; idx++) {
        if (mBlurPoints[idx].mIsHidden == true) {
            *end = idx;
            return range;
        }
        range++;
    }

    *end = BLUR_POINT_NUM;
    return range;
}

void StarPointerBlur::draw() const {
#ifdef __3DS__
    if (mNumHiddenPoints >= BLUR_POINT_NUM || mTexture == nullptr) {
        return;
    }

    TDDraw::setupDirect(1, 1, 2);
    // GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE as on the Wii.
    TDDraw::setDirectBlendState(GXToCitro3D::EQUATION_ADD, GXToCitro3D::FACTOR_SRC_ALPHA, GXToCitro3D::FACTOR_ONE);
    TDDraw::setDirectDepthMode(false, 5, false);

    const TVec2f directions[2] = {TVec2f(0.0f, 1.0f), TVec2f(1.0f, 0.0f)};
    for (u32 direction = 0; direction < 2; direction++) {
        u16 start = 0;
        u16 end = 0;
        while (start < BLUR_POINT_NUM) {
            const u16 range = findDrawPointRange(&start, &end);
            if (range < 2) {
                start = end;
                continue;
            }

            TVec3f positions[(BLUR_POINT_NUM - 1) * 6];
            TVec2f texcoords[(BLUR_POINT_NUM - 1) * 6];
            u32 count = 0;
            for (u16 point = start; point + 1 < end; point++) {
                const TVec2f& a = mBlurPoints[point].mPos;
                const TVec2f& b = mBlurPoints[point + 1].mPos;
                const TVec2f offsetA = directions[direction] * mBlurThicks[point];
                const TVec2f offsetB = directions[direction] * mBlurThicks[point + 1];
                const f32 u0 = mBlurTexCoords[point];
                const f32 u1 = mBlurTexCoords[point + 1];

                positions[count] = TVec3f(a.x - offsetA.x, a.y - offsetA.y, 0.0f);
                texcoords[count++] = TVec2f(0.0f, u0);
                positions[count] = TVec3f(a.x + offsetA.x, a.y + offsetA.y, 0.0f);
                texcoords[count++] = TVec2f(1.0f, u0);
                positions[count] = TVec3f(b.x + offsetB.x, b.y + offsetB.y, 0.0f);
                texcoords[count++] = TVec2f(1.0f, u1);
                positions[count] = TVec3f(a.x - offsetA.x, a.y - offsetA.y, 0.0f);
                texcoords[count++] = TVec2f(0.0f, u0);
                positions[count] = TVec3f(b.x + offsetB.x, b.y + offsetB.y, 0.0f);
                texcoords[count++] = TVec2f(1.0f, u1);
                positions[count] = TVec3f(b.x - offsetB.x, b.y - offsetB.y, 0.0f);
                texcoords[count++] = TVec2f(0.0f, u1);
            }

            TDDraw::drawTexturedTriangles(positions, texcoords, mTexture, count);
            start = end;
        }
    }

    TDDraw::closeDirect();
    return;
#else
    if (mNumHiddenPoints >= BLUR_POINT_NUM) {
        return;
    }

    initDraw();
    switch (::sDrawMode) {
    case 0:
        draw0((GXColor){0, 0, 0, 0});  // NOTE: indicated by debug symbols, color is a dummy
        break;
    case 1:
        draw1();  // NOTE: indicated by debug symbols
        break;
    case 2:
        draw2();
        break;
    }
#endif
}

void StarPointerBlur::draw1() const {
    // NOTE: im using this symbol to create a dummy TVec2f::TVec2f() call so the other does not get inlined.
    // In reality, the contents of this function are stripped from the build.
    TVec2f a(0.0f, 0.0f);
    TVec2f b(a);
}

void StarPointerBlur::draw2() const {
#ifdef __3DS__
    return;
#else
    u16 start, end;

    TVec2f dir(0.0f, 1.0f);
    start = 0;
    end = 0;
    while (start < BLUR_POINT_NUM) {
        u16 range = findDrawPointRange(&start, &end);
        if (range < 2) {
            start = end;
            continue;
        }

        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, range * 2);
        for (s32 point = start; point < end; point++) {
            const TVec2f& pos = mBlurPoints[point].mPos;
            const TVec2f& offset = dir * mBlurThicks[point];
            GXPosition2f32(pos.x - offset.x, pos.y - offset.y);
            GXTexCoord2f32(0.0f, mBlurTexCoords[point]);
            GXPosition2f32(pos.x + offset.x, pos.y + offset.y);
            GXTexCoord2f32(1.0f, mBlurTexCoords[point]);
        }
        GXEnd();

        start = end;
    }

    dir.set(1.0f, 0.0f);
    start = 0;
    end = 0;
    while (start < BLUR_POINT_NUM) {
        u16 range = findDrawPointRange(&start, &end);
        if (range < 2) {
            start = end;
            continue;
        }

        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, range * 2);
        for (s32 point = start; point < end; point++) {
            const TVec2f& pos = mBlurPoints[point].mPos;
            const TVec2f& offset = dir * mBlurThicks[point];
            GXPosition2f32(pos.x - offset.x, pos.y - offset.y);
            GXTexCoord2f32(0.0f, mBlurTexCoords[point]);
            GXPosition2f32(pos.x + offset.x, pos.y + offset.y);
            GXTexCoord2f32(1.0f, mBlurTexCoords[point]);
        }
        GXEnd();

        start = end;
    }
#endif
}

void StarPointerBlur::initDraw() const {
#ifdef __3DS__
    return;
#else
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);

    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_TEX0, 60, GX_FALSE, 125);
    mTexture->load(GX_TEXMAP0);

    TPos3f mtx;
    mtx.identity();
    GXLoadPosMtxImm(mtx, 0);
    GXSetCurrentMtx(0);

    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);

    GXSetNumIndStages(0);

    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_TEXA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_NOOP);

    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetZCompLoc(GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);

    GXSetAlphaCompare(GX_LEQUAL, 255, GX_AOP_OR, GX_LESS, 255);

    GXSetTevColor(GX_TEVREG0, *mMainColor);
#endif
}
