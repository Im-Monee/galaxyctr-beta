#include "Game/Screen/WipeFade.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/MathUtil.hpp"

namespace {
    static const s32 sFadeFrame = 30;
};  // namespace

WipeFade::WipeFade(const char* pName, const Color8& rFillColor)
    : WipeLayoutBase(pName), mIsWipeIn(false), mFrame(::sFadeFrame), mStep(::sFadeFrame), mFillColor(rFillColor) {
}

void WipeFade::init(const JMapInfoIter& rIter) {
    kill();
}

void WipeFade::control() {
    mStep++;

    if (isOpen()) {
        kill();
    }
}

void WipeFade::draw() const {
    if (MR::isDead(this)) {
        return;
    }

    f32 rate = static_cast< f32 >(mStep) / mFrame;
    f32 alpha = MR::clamp(rate, 0.0f, 1.0f);

    if (mIsWipeIn) {
        alpha = 1.0f - alpha;
    }

#ifndef __3DS__
    GXSetColorUpdate(GX_TRUE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);

#endif
    GXColor fillColor;
    fillColor.r = mFillColor.r;
    fillColor.g = mFillColor.g;
    fillColor.b = mFillColor.b;
#ifdef __3DS__
    fillColor.a = static_cast<u8>(255.0f * alpha);
    MR::fillScreenAlpha(fillColor);
#else
#ifdef __3DS__
    GXSetColorUpdate(GX_TRUE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
#endif
    fillColor.a = 255.0f * alpha;
    MR::fillScreen(fillColor);
#endif
}

void WipeFade::wipe(s32 frame) {
    mIsWipeIn = !mIsWipeIn;

    if (frame < 0) {
        mFrame = ::sFadeFrame;
    } else if (frame == 0) {
        mFrame = 1;
    } else {
        mFrame = frame;
    }

    mStep = 0;

    if (MR::isDead(this)) {
        appear();
    }
}

void WipeFade::forceClose() {
    mIsWipeIn = false;
    mStep = mFrame;

    if (MR::isDead(this)) {
        appear();
    }
}

void WipeFade::forceOpen() {
    mIsWipeIn = true;
    mStep = mFrame;

    kill();
}

bool WipeFade::isOpen() const {
    return mIsWipeIn && mStep >= mFrame;
}

bool WipeFade::isClose() const {
    return !mIsWipeIn && mStep >= mFrame;
}

bool WipeFade::isWipeIn() const {
    return mIsWipeIn && mStep < mFrame;
}

bool WipeFade::isWipeOut() const {
    return !mIsWipeIn && mStep < mFrame;
}
