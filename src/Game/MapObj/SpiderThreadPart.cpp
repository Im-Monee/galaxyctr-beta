#include "Game/MapObj/SpiderThreadPart.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/LiveActor/PartsModel.hpp"
#include "Game/MapObj/SpiderThreadMainPoint.hpp"
#include "Game/MapObj/SpiderThreadPoint.hpp"
#include "Game/Util.hpp"
#include "Game/Util/Color.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/StarPointerUtil.hpp"
// 3DS: DirectDraw, TDDraw's Citro3D draws.
#ifdef __3DS__
#include "Game/Util/DirectDraw.hpp"
#endif
#include <revolution/gx/GXStruct.h>
#include <revolution/gx/GXVert.h>
#include <revolution/wpad.h>

void SpiderThreadPart_DUMMY() {
    TVec3f a(1.0f);
}

namespace {
    static Color8 sColorPlusX(0x64, 0x64, 0x64, 0xFF);
    static Color8 sColorMinusX(0x96, 0x96, 0x96, 0xFF);
    static Color8 sColorMinusY(0xFF, 0xFF, 0xFF, 0xFF);

    static const f32 sPointInterval = 50.0f;
    static const f32 sFricitonLengthMin = 200.0f;
    static const f32 sFricitonLengthMax = 700.0f;
    // static const _32 sFrictionLengthRateMin =
    // static const _32 sFrictionLengthRateMax =
    // static const _32 sFrictionTipMin =
    static const s32 sTouchTimerMax = 5;
    static const s32 sActorToucnTimerMax = 60;
    static const s32 sForceAttachTime = 20;
    static const f32 sWindFrictionRate = 0.93f;
    // static const f32 sScaleMin =
    // static const f32 sDrawWidthX =
    // static const f32 sDrawWidthY =
    // static const f32 sDrawWidthLongOffsetX =
    // static const f32 sDrawWidthShortOffsetX =
    // static const f32 sDrawWidthShortOffsetY =
    static const f32 sTexRateV = 0.002f;
    // static const f32 sIndirectTexEndU =
    static const f32 sIndirectTexRateV = 0.001f;
    static const f32 sIndirectTexSpeed = -0.002f;
};  // namespace

SpiderThreadPart::SpiderThreadPart(SpiderThreadMainPoint* pPointA, SpiderThreadMainPoint* pPointB, f32 boundBoxPadding)
    : mPointA(pPointA), mIsStill(true), mIsCut(false), mIsFree(false), _4(0), mNumHangingPoints(0), mStopTime(0), mTouchTimeout(0),
      mPointerPosPad0(0.0f, 0.0f, 0.0f), mPointerPosPad1(0.0f, 0.0f, 0.0f), mPointB(pPointB), mNumPoints(0), mPoints(nullptr),
      mSide(1.0f, 0.0f, 0.0f), mUp(0.0f, 1.0f, 0.0f), mFront(0.0f, 0.0f, 1.0f), mDistBetweenMainPoints(0.0f), mDistBetweenPoints(0.0f),
      mTextureOffset(0.0f) {
    TVec3f forward(mPointB->mPosition);
    forward.sub(mPointA->mPosition);
    mDistBetweenMainPoints = forward.length();
    mNumPoints = mDistBetweenMainPoints / ::sPointInterval;
    mDistBetweenPoints = mDistBetweenMainPoints / (mNumPoints + 1);
    MR::normalize(&forward);

    mPoints = new SpiderThreadPoint*[mNumPoints];

    for (s32 idx = 0; idx < mNumPoints; idx++) {
        TVec3f pos(forward);
        pos.scale(mDistBetweenPoints * (idx + 1));
        pos.add(mPointA->mPosition);

        s32 halfNumPoints = mNumPoints / 2;
        s32 index = MR::abs(halfNumPoints - idx);
        f32 indexNormalized = 1.0f - (static_cast< f32 >(index) / static_cast< f32 >(halfNumPoints));

        f32 length = mDistBetweenMainPoints - ::sFricitonLengthMin;
        if (length < 0.0f) {
            length = 0.0f;
        } else if (length > ::sFricitonLengthMax - ::sFricitonLengthMin) {
            length = ::sFricitonLengthMax - ::sFricitonLengthMin;
        }

        f32 lengthNormalized = length / (::sFricitonLengthMax - ::sFricitonLengthMin);

        f32 indexSpring =
            ((0.9f * (1.0f - MR::getEaseOutValue(indexNormalized, 0.0f, 1.0f, 1.0f))) + MR::getEaseOutValue(indexNormalized, 0.0f, 1.0f, 1.0f));
        f32 lengthSpring = (0.9f * (1.0f - lengthNormalized) + (0.9f * lengthNormalized));
        f32 friction = indexSpring * lengthSpring;

        mPoints[idx] = new SpiderThreadPoint(pos, friction);
    }

    mSide.set(0.0f, 0.0f, -1.0f);
    mUp.set< f32 >(forward.y, -forward.x, 0.0f);
    mFront.set< f32 >(-forward.y, forward.x, 0.0f);

    mBoundBox.i.zero();
    mBoundBox.f.zero();
    mBoundBox.extend(mPointA->mPosition);
    mBoundBox.extend(mPointB->mPosition);

    padBox(boundBoxPadding);

    f32 minZ = mPointA->mPosition.z >= mPointB->mPosition.z ? mPointB->mPosition.z : mPointA->mPosition.z;
    TVec3f far(mBoundBox.i.x, mBoundBox.i.y, minZ - 1000.0f);

    f32 maxZ = mPointA->mPosition.z >= mPointB->mPosition.z ? mPointA->mPosition.z : mPointB->mPosition.z;
    TVec3f near(mBoundBox.f.x, mBoundBox.f.y, maxZ + 100.0f);
    mBoundBox.extend(far);
    mBoundBox.extend(near);
}

void SpiderThreadPart::cut(SpiderThreadMainPoint* pPointA, SpiderThreadMainPoint* pPointB) {
    if (pPointA == mPointA) {
        mPointA = mPointB;
        mPointB = pPointB;
    } else {
        mPointB = pPointB;
    }

    if (mIsCut) {
        mIsFree = true;
    }

    mIsCut = true;
    mIsStill = false;
}

void SpiderThreadPart::updatePointerPos() {
    if (mIsCut || mIsFree) {
        return;
    }

    TVec3f center(mPointA->mPosition);
    center.add(mPointB->mPosition);
    center.scale(0.5f);

    if (MR::isStarPointerInScreen(WPAD_CHAN0)) {
        MR::calcStarPointerPosOnPlane(&mPointerPosPad0, center, TVec3f(0.0f, 0.0f, 1.0f), WPAD_CHAN0, false);
    }

    if (MR::isStarPointerInScreen(WPAD_CHAN1)) {
        MR::calcStarPointerPosOnPlane(&mPointerPosPad1, center, TVec3f(0.0f, 0.0f, 1.0f), WPAD_CHAN1, false);
    }
}

void SpiderThreadPart::update() {
    if (mIsFree) {
        return;
    }

    mTextureOffset += ::sIndirectTexSpeed;

    if (mIsCut) {
        updateCutPoints();
        return;
    }

    if (mIsStill) {
        if (_4 > 0) {
            updatePointPosToLine();
        }
        mTouchTimeout = 0;
        return;
    }

    if (mNumHangingPoints > 0) {
        updateDrawVec();
        return;
    }

    if (mTouchTimeout > 0) {
        mTouchTimeout--;
    }

    updateBasePos();

    if (mStopTime > 0) {
        updateForceAttach();
        updateDrawVec();
        return;
    }

    mIsStill = true;
    for (s32 idx = 0; idx < mNumPoints; idx++) {
        if (!getPoint(idx)->updateSpring()) {
            mIsStill = false;
        }
    }

    if (!mIsStill) {
        updateDrawVec();
    }
}

// 3DS: the draw takes the thread's texture, which the Citro3D draw binds itself.
#ifdef __3DS__
void SpiderThreadPart::draw(JUTTexture* texture) const {
#else
void SpiderThreadPart::draw() const {
#endif
    if (mIsFree) {
        return;
    }

    // 3DS: the thread's lines and points through DirectDraw.
#ifdef __3DS__
    if (mIsStill) {
        drawLineDirect(texture);
        return;
    }
    drawPointsDirect(texture);
#else
    if (mIsStill) {
        drawLine();
        return;
    }

    drawPoints();
#endif
}

void SpiderThreadPart::startHang(s32 index) {
    mNumHangingPoints++;
    mIsStill = false;
    mTouchTimeout = 0;
    mPointA->addNearPointToRadial(getPoint(index));
    if (mPointA->mRadialLine != mPointB->mRadialLine) {
        mPointB->addNearPointToRadial(getPoint(index));
    }
    updatePointPosHanging(index);
}

void SpiderThreadPart::updateHang(const TVec3f& rPos, s32 index) {
    getPoint(index)->updateHang(rPos);
    updatePointPosHanging(index);
}

void SpiderThreadPart::endHang(s32 cutIndex) {
    TVec3f front(mPointB->mPosition);
    front.sub(mPointA->mPosition);
    MR::normalize(&front);

    SpiderThreadPoint* point = getPoint(cutIndex);

    for (s32 idx = 0; idx < cutIndex; idx++) {
        f32 t = MR::getEaseOutValue(static_cast< f32 >(idx + 1) / static_cast< f32 >(cutIndex + 1), 0.0f, 1.0f, 1.0f);

        TVec3f v1;
        v1.x = point->mPosition.x * t + (1.0f - t) * mPointA->mPosition.x;
        v1.y = point->mPosition.y * t + (1.0f - t) * mPointA->mPosition.y;
        v1.z = point->mPosition.z * t + (1.0f - t) * mPointA->mPosition.z;

        TVec3f v2(v1);
        v2.sub(getPoint(idx)->mPosition);
        MR::vecKillElement(v2, front, &v1);
        v1.add(getPoint(idx)->mPosition);

        getPoint(idx)->setPos(v1);
    }

    for (s32 idx = cutIndex + 1; idx < mNumPoints; idx++) {
        f32 t = MR::getEaseOutValue(static_cast< f32 >(mNumPoints - idx) / static_cast< f32 >(mNumPoints - cutIndex), 0.0f, 1.0f, 1.0f);

        TVec3f v1;
        v1.x = point->mPosition.x * t + (1.0f - t) * mPointB->mPosition.x;
        v1.y = point->mPosition.y * t + (1.0f - t) * mPointB->mPosition.y;
        v1.z = point->mPosition.z * t + (1.0f - t) * mPointB->mPosition.z;

        TVec3f v2(v1);
        v2.sub(getPoint(idx)->mPosition);
        MR::vecKillElement(v2, front, &v1);
        v1.add(getPoint(idx)->mPosition);

        getPoint(idx)->setPos(v1);
    }

    u32 index = cutIndex;
    mPointA->removeNearPointFromRadial(getPoint(index));
    if (mPointA->mRadialLine != mPointB->mRadialLine) {
        mPointB->removeNearPointFromRadial(getPoint(index));
    }
    mIsStill = false;
    mNumHangingPoints--;
}

void SpiderThreadPart::stopAllPoints() {
    if (mIsStill) {
        return;
    }

    mStopTime = ::sForceAttachTime;
}

f32 SpiderThreadPart::calcNearestPointInfo(const TVec3f** pPos, const TVec3f** pNeutralPos, s32* pPointNum, const TVec3f& rPos) {
    if (mIsStill) {
        updatePointPosToLine();
    }
    f32 dist = findNearestPointNo(pPointNum, rPos);
    *pPos = &getPoint(*pPointNum)->mPosition;
    *pNeutralPos = &getPoint(*pPointNum)->mBasePos;
    return dist;
}

void SpiderThreadPart::updatePointPosToLine() {
    TVec3f front(mPointB->mPosition);
    front.sub(mPointA->mPosition);

    f32 delta = front.length() / (mNumPoints + 1);
    MR::normalize(&front);

    SpiderThreadPoint* point;
    for (s32 idx = 0; idx < mNumPoints; idx++) {
        TVec3f pos(front);
        pos.scale(delta * (idx + 1));
        pos.add(mPointA->mPosition);

        point = getPoint(idx);
        point->setPos(pos);
    }
}

void SpiderThreadPart::updatePointPosAndBasePosToLine() {
    updatePointPosToLine();
    for (s32 idx = 0; idx < mNumPoints; idx++) {
        getPoint(idx)->mBasePos.set(getPoint(idx)->mPosition);
    }
}

bool SpiderThreadPart::touchActor(s32 index, const TVec3f& rVel) {
    if (mTouchTimeout > 0) {
        return false;
    }

    // TODO: this really screams vector arithmetic inline
    f32 projUp = mUp.dot(rVel);
    f32 projFront = mFront.dot(rVel);
    TVec3f scaleUp(mUp);
    TVec3f scaleFront(mFront);
    scaleUp.scale(projUp);
    scaleFront.scale(projFront);
    TVec3f vel(scaleUp);
    vel.add(scaleFront);

    getPoint(index)->mVelocity.add(vel);
    updatePointVelocityTouch(index, vel);
    mIsStill = false;
    mTouchTimeout = ::sActorToucnTimerMax;

    return true;
}

void SpiderThreadPart::tryTouch(f32 radius, const TVec3f& rVel, s32 padChannel) {
    if (mIsCut || mIsFree) {
        return;
    }

    if (mTouchTimeout > 0) {
        return;
    }

    bool inBounds;
    TVec3f* pointerPos = &mPointerPosPad0;
    if (padChannel == WPAD_CHAN1) {
        pointerPos = &mPointerPosPad1;
    }

    if (mIsStill) {
        if (!mBoundBox.intersectsPoint(*pointerPos)) {
            return;
        }
    }

    if (mIsStill) {
        updatePointPosToLine();
    }

    s32 index = findTouchPoint(radius, rVel, padChannel);
    if (index >= 0) {
        updatePointVelocityTouch(index, rVel);
    }
}

s32 SpiderThreadPart::findTouchPoint(f32 radius, const TVec3f& rVel, s32 padChannel) {
    for (s32 idx = 0; idx < mNumPoints; idx++) {
        if (getPoint(idx)->tryTouch(radius, rVel, padChannel)) {
            mTouchTimeout = ::sTouchTimerMax;
            mIsStill = false;
            return idx;
        }
    }
    return -1;
}

f32 SpiderThreadPart::findNearestPointNo(s32* pPointNum, const TVec3f& rPos) const {
    *pPointNum = 0;
    f32 minDistance = MR::calcDistanceXY(getPoint(0)->mPosition, rPos);

    f32 dist;
    for (s32 idx = 1; idx < mNumPoints; idx++) {
        dist = MR::calcDistanceXY(getPoint(idx)->mPosition, rPos);
        if (dist < minDistance) {
            *pPointNum = idx;
            minDistance = dist;
        }
    }

    return minDistance;
}

void SpiderThreadPart::updatePointPosHanging(s32 hangIndex) {
    SpiderThreadPoint* point = getPoint(hangIndex);

    for (s32 idx = 0; idx < hangIndex; idx++) {
        f32 t = static_cast< f32 >(idx + 1) / static_cast< f32 >(hangIndex + 1);

        TVec3f v1;
        v1.x = point->mPosition.x * t + (1.0f - t) * mPointA->mPosition.x;
        v1.y = point->mPosition.y * t + (1.0f - t) * mPointA->mPosition.y;
        v1.z = point->mPosition.z * t + (1.0f - t) * mPointA->mPosition.z;

        getPoint(idx)->setPos(v1);
    }

    for (s32 idx = hangIndex + 1; idx < mNumPoints; idx++) {
        f32 t = static_cast< f32 >(mNumPoints - idx) / static_cast< f32 >(mNumPoints - hangIndex);

        TVec3f v1;
        v1.x = point->mPosition.x * t + (1.0f - t) * mPointB->mPosition.x;
        v1.y = point->mPosition.y * t + (1.0f - t) * mPointB->mPosition.y;
        v1.z = point->mPosition.z * t + (1.0f - t) * mPointB->mPosition.z;

        getPoint(idx)->setPos(v1);
    }
}

void SpiderThreadPart::updatePointVelocityTouch(s32 hangIndex, const TVec3f& rVel) {
    SpiderThreadPoint* point = getPoint(hangIndex);

    SpiderThreadPoint* hangPoint;
    for (s32 idx = 0; idx < hangIndex; idx++) {
        f32 t = MR::getEaseOutValue(static_cast< f32 >(idx + 1) / static_cast< f32 >(hangIndex + 1), 0.0f, 1.0f, 1.0f);

        getPoint(idx)->mVelocity.add(rVel * t);
    }

    for (s32 idx = hangIndex + 1; idx < mNumPoints; idx++) {
        f32 t = MR::getEaseOutValue(static_cast< f32 >(mNumPoints - idx) / static_cast< f32 >(mNumPoints - hangIndex), 0.0f, 1.0f, 1.0f);

        getPoint(idx)->mVelocity.add(rVel * t);
    }
}

void SpiderThreadPart::updateBasePos() {
    TVec3f front(mPointB->mPosition);
    front.sub(mPointA->mPosition);

    if (MR::isNearZero(front)) {
        return;
    }

    f32 delta = front.length() / (mNumPoints + 1);
    MR::normalize(&front);

    for (s32 idx = 0; idx < mNumPoints; idx++) {
        TVec3f pos(front);
        pos.scale((idx + 1) * delta);
        pos.add(mPointA->mPosition);
        getPoint(idx)->mBasePos.set(pos);
    }
}

void SpiderThreadPart::updateForceAttach() {
    if (--mStopTime <= 0) {
        mIsStill = true;
        return;
    }

    f32 t = mStopTime / static_cast< f32 >(::sForceAttachTime);
    for (s32 idx = 0; idx < mNumPoints; idx++) {
        TVec3f vel(getPoint(idx)->mVelocity);
        getPoint(idx)->updateSpring();

        getPoint(idx)->setPos(getPoint(idx)->mPosition * t + getPoint(idx)->mBasePos * (1.0f - t));
        getPoint(idx)->mVelocity.set(vel);
    }
}

void SpiderThreadPart::updateCutPoints() {
    for (s32 idx = 0; idx < mNumPoints; idx++) {
        getPoint(idx)->updateWind(::sWindFrictionRate);
    }

    TVec3f* pos = &mPointA->mPosition;
    for (s32 idx = 0; idx < mNumPoints; idx++) {
        getPoint(idx)->restrict(pos, ::sPointInterval);
        pos = &getPoint(idx)->mPosition;
    }

    for (s32 idx = 0; idx < mNumPoints; idx++) {
        getPoint(idx)->updatePos();
    }

    TVec3f direction(getPoint(mNumPoints - 2)->mPosition);
    direction.sub(getPoint(mNumPoints - 1)->mPosition);

    if (!MR::isNearZero(direction)) {
        MR::normalize(&direction);
        direction.scale(::sPointInterval);
        direction.add(getPoint(mNumPoints - 1)->mPosition);
        mPointB->setPos(direction);
    }
}

void SpiderThreadPart::updateDrawVec() {
    TVec3f* pos = &mPointA->mPosition;
    for (s32 idx = 0; idx < mNumPoints - 1; idx++) {
        getPoint(idx)->updateDrawVec(pos, &getPoint(idx + 1)->mPosition);
        pos = &getPoint(idx)->mPosition;
    }

    getPoint(mNumPoints - 1)->updateDrawVec(pos, &mPointB->mPosition);
}

#ifndef __3DS__
namespace {
    // this is extremely hacky, and I'm not sure I like this. However, this matches.
    inline void upperPoint1(const TVec3f& rPos, const TVec3f& rSide, const TVec3f& rFront, f32 x, f32 y) {
        GXPosition3f32(rPos.x - x * rSide.x + y * rFront.x, rPos.y - x * rSide.y + y * rFront.y, rPos.z - x * rSide.z + y * rFront.z);
    }
    inline void upperPoint2(const TVec3f& rPos, const TVec3f& rSide, const TVec3f& rFront, f32 x, f32 y) {
        GXPosition3f32(rPos.x + x * rSide.x + y * rFront.x, rPos.y + x * rSide.y + y * rFront.y, rPos.z + x * rSide.z + y * rFront.z);
    }

    inline void rightLowerPoint1(const TVec3f& rPos, const TVec3f& rSide, const TVec3f& rFront, f32 x, f32 y) {
        GXPosition3f32(rPos.x + x * rSide.x + y * rFront.x, rPos.y + x * rSide.y + y * rFront.y, rPos.z + x * rSide.z + y * rFront.z);
    }
    inline void rightLowerPoint2(const TVec3f& rPos, const TVec3f& rSide, const TVec3f& rFront, f32 x, f32 y) {
        GXPosition3f32(rPos.x - x * rSide.x - y * rFront.x, rPos.y - x * rSide.y - y * rFront.y, rPos.z - x * rSide.z - y * rFront.z);
    }

    inline void leftLowerPoint1(const TVec3f& rPos, const TVec3f& rSide, const TVec3f& rFront, f32 x, f32 y) {
        GXPosition3f32(rPos.x + x * rSide.x - y * rFront.x, rPos.y + x * rSide.y - y * rFront.y, rPos.z + x * rSide.z - y * rFront.z);
    }
    inline void leftLowerPoint2(const TVec3f& rPos, const TVec3f& rSide, const TVec3f& rFront, f32 x, f32 y) {
        GXPosition3f32(rPos.x - x * rSide.x + y * rFront.x, rPos.y - x * rSide.y + y * rFront.y, rPos.z - x * rSide.z + y * rFront.z);
    }

    void sendPointsUpper(const TVec3f& rPos, const TVec3f& rSide, const TVec3f& rFront, f32 texY1, f32 texY2) {
        upperPoint1(rPos, rSide, rFront, 18.0f, 3.0f);
        GXColor1u32(sColorMinusX);
        GXTexCoord2f32(0.0f, texY1);
        GXTexCoord2f32(0.0f, texY2);

        upperPoint2(rPos, rSide, rFront, 18.0f, 3.0f);
        GXColor1u32(sColorPlusX);
        GXTexCoord2f32(1.0f, texY1);
        GXTexCoord2f32(1.0f, texY2);
    }

    void sendPointsRightLower(const TVec3f& rPos, const TVec3f& rSide, const TVec3f& rFront, f32 texY1, f32 texY2) {
        rightLowerPoint1(rPos, rSide, rFront, 18.0f, 18.0f);
        GXColor1u32(sColorPlusX);
        GXTexCoord2f32(0.0f, texY1);
        GXTexCoord2f32(0.0f, texY2);

        rightLowerPoint2(rPos, rSide, rFront, 15.0f, 18.0f);
        GXColor1u32(sColorMinusY);
        GXTexCoord2f32(1.0f, texY1);
        GXTexCoord2f32(1.0f, texY2);
    }

    void sendPointsLeftLower(const TVec3f& rPos, const TVec3f& rSide, const TVec3f& rFront, f32 texY1, f32 texY2) {
        leftLowerPoint1(rPos, rSide, rFront, 15.0f, 18.0f);
        GXColor1u32(sColorMinusY);
        GXTexCoord2f32(0.0f, texY1);
        GXTexCoord2f32(0.0f, texY2);

        leftLowerPoint2(rPos, rSide, rFront, 18.0f, 18.0f);
        GXColor1u32(sColorMinusX);
        GXTexCoord2f32(1.0f, texY1);
        GXTexCoord2f32(1.0f, texY2);
    }
};  // namespace

void SpiderThreadPart::drawLine() const {
    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
    {
        ::sendPointsUpper(mPointA->mPosition, mSide, mUp, 0.0f, mTextureOffset);
        ::sendPointsUpper(mPointB->mPosition, mSide, mUp, mDistBetweenMainPoints * ::sTexRateV,
                          mTextureOffset + mDistBetweenMainPoints * ::sIndirectTexRateV);
    }
    GXEnd();

    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
    {
        ::sendPointsRightLower(mPointA->mPosition, mSide, mUp, 0.0f, mTextureOffset);
        ::sendPointsRightLower(mPointB->mPosition, mSide, mUp, mDistBetweenMainPoints * ::sTexRateV,
                               mTextureOffset + mDistBetweenMainPoints * ::sIndirectTexRateV);
    }
    GXEnd();

    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
    {
        ::sendPointsLeftLower(mPointA->mPosition, mSide, mUp, 0.0f, mTextureOffset);
        ::sendPointsLeftLower(mPointB->mPosition, mSide, mUp, mDistBetweenMainPoints * ::sTexRateV,
                              mTextureOffset + mDistBetweenMainPoints * ::sIndirectTexRateV);
    }
    GXEnd();
}

void SpiderThreadPart::drawPoints() const {
    f32 offset2, offset1;
    f32 delta1 = (mDistBetweenMainPoints * ::sTexRateV) / (mNumPoints + 1);
    f32 delta2 = (mDistBetweenMainPoints * ::sIndirectTexRateV) / (mNumPoints + 1);

    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, (mNumPoints + 2) * 2);
    {
        ::sendPointsUpper(mPointA->mPosition, mSide, mUp, 0.0f, mTextureOffset);

        offset1 = delta1;
        offset2 = delta2 + mTextureOffset;
        for (s32 idx = 0; idx < mNumPoints; idx++) {
            ::sendPointsUpper(getPoint(idx)->mPosition, mSide, getPoint(idx)->mUp, offset1, offset2);
            offset1 += delta1;
            offset2 += delta2;
        }

        ::sendPointsUpper(mPointB->mPosition, mSide, getPoint(mNumPoints - 1)->mUp, mDistBetweenMainPoints * ::sTexRateV,
                          mTextureOffset + mDistBetweenMainPoints * ::sIndirectTexRateV);
    }
    GXEnd();

    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, (mNumPoints + 2) * 2);
    {
        ::sendPointsRightLower(mPointA->mPosition, mSide, mUp, 0.0f, mTextureOffset);
        offset1 = delta1;
        offset2 = delta2 + mTextureOffset;
        for (s32 idx = 0; idx < mNumPoints; idx++) {
            ::sendPointsRightLower(getPoint(idx)->mPosition, mSide, getPoint(idx)->mUp, offset1, offset2);
            offset1 += delta1;
            offset2 += delta2;
        }

        ::sendPointsRightLower(mPointB->mPosition, mSide, getPoint(mNumPoints - 1)->mUp, mDistBetweenMainPoints * ::sTexRateV,
                               mTextureOffset + mDistBetweenMainPoints * ::sIndirectTexRateV);
    }
    GXEnd();

    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, (mNumPoints + 2) * 2);
    {
        ::sendPointsLeftLower(mPointA->mPosition, mSide, mUp, 0.0f, mTextureOffset);
        offset1 = delta1;
        offset2 = delta2 + mTextureOffset;
        for (s32 idx = 0; idx < mNumPoints; idx++) {
            ::sendPointsLeftLower(getPoint(idx)->mPosition, mSide, getPoint(idx)->mUp, offset1, offset2);
            offset1 += delta1;
            offset2 += delta2;
        }

        ::sendPointsLeftLower(mPointB->mPosition, mSide, getPoint(mNumPoints - 1)->mUp, mDistBetweenMainPoints * ::sTexRateV,
                              mTextureOffset + mDistBetweenMainPoints * ::sIndirectTexRateV);
    }
    GXEnd();
}
#endif

#ifdef __3DS__
namespace {
    TVec3f makeThreadRibbonVertex(const TVec3f& position, const TVec3f& side, const TVec3f& front, int ribbon, int edge) {
        f32 sideAmount = 0.0f;
        f32 frontAmount = 0.0f;
        if (ribbon == 0) {
            sideAmount = edge == 0 ? -18.0f : 18.0f;
            frontAmount = 3.0f;
        } else if (ribbon == 1) {
            sideAmount = edge == 0 ? 18.0f : -15.0f;
            frontAmount = edge == 0 ? 18.0f : -18.0f;
        } else {
            sideAmount = edge == 0 ? 15.0f : -18.0f;
            frontAmount = edge == 0 ? -18.0f : 18.0f;  // leftLowerPoint1 (15, 18), leftLowerPoint2 (18, 18)
        }
        return TVec3f(position.x + sideAmount * side.x + frontAmount * front.x,
                      position.y + sideAmount * side.y + frontAmount * front.y,
                      position.z + sideAmount * side.z + frontAmount * front.z);
    }

    u32 getThreadRibbonColor(int ribbon, int edge) {
        if (ribbon == 0) return edge == 0 ? 0x969696FFu : 0x646464FFu;
        if (ribbon == 1) return edge == 0 ? 0x646464FFu : 0xFFFFFFFFu;
        return edge == 0 ? 0xFFFFFFFFu : 0x969696FFu;
    }

    void appendThreadRibbonSegment(const TVec3f& posA, const TVec3f& sideA, const TVec3f& frontA, const TVec3f& posB,
                                   const TVec3f& sideB, const TVec3f& frontB, int ribbon, f32 texA, f32 texB,
                                   TVec3f* positions, TVec2f* texcoords, u32* colors, u32& count) {
        const TVec3f p[4] = {makeThreadRibbonVertex(posA, sideA, frontA, ribbon, 0), makeThreadRibbonVertex(posA, sideA, frontA, ribbon, 1),
                             makeThreadRibbonVertex(posB, sideB, frontB, ribbon, 0), makeThreadRibbonVertex(posB, sideB, frontB, ribbon, 1)};
        const u32 c[4] = {getThreadRibbonColor(ribbon, 0), getThreadRibbonColor(ribbon, 1), getThreadRibbonColor(ribbon, 0),
                          getThreadRibbonColor(ribbon, 1)};
        const u8 order[6] = {0, 1, 2, 2, 1, 3};
        for (int i = 0; i < 6; i++) {
            const u8 idx = order[i];
            positions[count + i].set(p[idx]);
            // s across the ribbon (0 on edge 0, 1 on edge 1), t along the thread, as the Wii's
            // sendPoints*. With s along each segment, every segment of a moving thread showed the
            // texture's transparent sides at its ends: the thread looked dashed once Mario had hung
            // on it (a tester).
            texcoords[count + i].set((idx == 0 || idx == 2) ? 0.0f : 1.0f, (idx == 0 || idx == 1) ? texA : texB);
            colors[count + i] = c[idx];
        }
        count += 6;
    }

    void drawThreadPartDirect(const SpiderThreadPart* part, JUTTexture* texture, bool still) {
        const s32 segmentCount = still ? 1 : part->mNumPoints + 1;
        const f32 delta = part->mDistBetweenMainPoints * ::sTexRateV / segmentCount;
        for (int ribbon = 0; ribbon < 3; ribbon++) {
            TVec3f positions[192];
            TVec2f texcoords[192];
            u32 colors[192];
            u32 count = 0;
            for (s32 segment = 0; segment < segmentCount; segment++) {
                const TVec3f* posA;
                const TVec3f* sideA;
                const TVec3f* frontA;
                const TVec3f* posB;
                const TVec3f* sideB;
                const TVec3f* frontB;
                if (segment == 0) {
                    posA = &part->mPointA->mPosition;
                    sideA = &part->mSide;
                    frontA = &part->mUp;
                } else {
                    SpiderThreadPoint* point = part->getPoint(segment - 1);
                    posA = &point->mPosition;
                    sideA = &part->mSide;
                    frontA = &point->mUp;
                }
                if (segment == part->mNumPoints) {
                    posB = &part->mPointB->mPosition;
                    sideB = &part->mSide;
                    frontB = part->mNumPoints > 0 ? &part->getPoint(part->mNumPoints - 1)->mUp : &part->mUp;
                } else {
                    SpiderThreadPoint* point = part->getPoint(segment);
                    posB = &point->mPosition;
                    sideB = &part->mSide;
                    frontB = &point->mUp;
                }
                if (still) {
                    posA = &part->mPointA->mPosition;
                    frontA = &part->mUp;
                    posB = &part->mPointB->mPosition;
                    frontB = &part->mUp;
                }
                const f32 texA = segment * delta;
                const f32 texB = (segment + 1) * delta;
                // Citro3D workaround: retain the main thread texture and per-vertex shading;
                // GX indirect UV warping from Indirect.bti has no equivalent in this pass.
                appendThreadRibbonSegment(*posA, *sideA, *frontA, *posB, *sideB, *frontB, ribbon, texA, texB, positions, texcoords,
                                          colors, count);
                if (count == 192) {
                    TDDraw::drawColoredTexturedTriangles(positions, texcoords, colors, texture, count);
                    count = 0;
                }
            }
            if (count != 0) TDDraw::drawColoredTexturedTriangles(positions, texcoords, colors, texture, count);
        }
    }
};

void SpiderThreadPart::drawLineDirect(JUTTexture* texture) const {
    drawThreadPartDirect(this, texture, true);
}

void SpiderThreadPart::drawPointsDirect(JUTTexture* texture) const {
    drawThreadPartDirect(this, texture, false);
}
#endif