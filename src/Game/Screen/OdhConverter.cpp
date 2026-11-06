#include "Game/Screen/OdhConverter.hpp"
#include "Game/NameObj/NameObjAdaptor.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Screen/odh.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#ifdef __3DS__
#include "host.h"
#endif
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>

namespace {
    const u32 cCaptureWidth = 488;
    const u32 cCaptureHeight = 328;
    const f32 cCaptureOffsetX = 34.0f;
    const f32 cCaptureOffsetY = 44.0f;
    const u32 cLimitSize = 40960;
    const s32 cQuality = 80;
    const u32 cTexPosX = 0;
    const u32 cTexPosY = 0;

    OdhConverter* getConverter() {
        return MR::getSceneObj< OdhConverter >(SceneObj_OdhConverter);
    }
};  // namespace

OdhConverter::OdhConverter()
    : LayoutActor("ODH-jpeg変換", true), mCaptureWidth(::cCaptureWidth), mCaptureHeight(::cCaptureHeight), mLimitSize(::cLimitSize),
      mIsRequestedCapture(false), mImage(nullptr), mCaptureImage(nullptr) {
}

void OdhConverter::init(const JMapInfoIter& rIter) {
    MR::connectToScene(this, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_LayoutOnPause);
    MR::CurrentHeapRestorer heapRestorer(MR::getSceneHeapGDDR3());

    mImage = new (32) u8[mLimitSize];
    mCaptureImage = new (32) u8[mCaptureWidth * mCaptureHeight * 3];
}

void OdhConverter::draw() const {
}

void OdhConverter::convert() {
#ifdef __3DS__
    if (mCaptureImage == nullptr || mImage == nullptr || mCaptureWidth == 0 || mCaptureHeight == 0 ||
        mCaptureWidth > 640 || mCaptureHeight > 456 || mLimitSize == 0) {
        return;
    }
    if (!hostGfxReadRgb565(::cCaptureOffsetX, ::cCaptureOffsetY, mCaptureWidth, mCaptureHeight,
                           reinterpret_cast<u16*>(mCaptureImage))) {
        return;
    }
    const u32 imageSize = ODHEncodeRGB565(reinterpret_cast<u8*>(mCaptureImage), mImage, mCaptureWidth, mCaptureHeight,
                                          mLimitSize, ::cQuality, mCaptureImage);
    mImageSize = imageSize <= mLimitSize ? imageSize : 0;
#else
    const GXRenderModeObj* pRenderModeObj;
    u8* pScreenTexImage;

    pScreenTexImage = MR::getScreenTexImage();
    pRenderModeObj = JUTVideo::getManager()->getRenderMode();

    GXSetCopyFilter(GX_FALSE, pRenderModeObj->sample_pattern, GX_FALSE, pRenderModeObj->vfilter);
    GXSetTexCopySrc(::cTexPosX, ::cTexPosY, mCaptureWidth, mCaptureHeight);
    GXSetTexCopyDst(mCaptureWidth, mCaptureHeight, GX_TF_RGB565, GX_FALSE);
    GXCopyTex(pScreenTexImage, GX_FALSE);
    GXDrawDone();
    GXSetCopyFilter(GX_FALSE, pRenderModeObj->sample_pattern, GX_TRUE, pRenderModeObj->vfilter);
    DCStoreRange(pScreenTexImage, mCaptureWidth * mCaptureHeight * 2);

    mImageSize = ODHEncodeRGB565(pScreenTexImage, mImage, mCaptureWidth, mCaptureHeight, mLimitSize, ::cQuality, mCaptureImage);
#endif
}

namespace MR {
    void createOdhConverter() {
        createSceneObj(SceneObj_OdhConverter);
    }

    void requestCaptureOdhImage() {
        return ::getConverter()->requestCapture();
    }

    bool isRequestedCaptureOdhImage() {
        if (!isExistSceneObj(SceneObj_OdhConverter)) {
            return false;
        }

        return ::getConverter()->isRequestedCapture();
    }

    void captureOdhImage() {
        ::getConverter()->capture();
    }

    void setPortCaptureOdhImage() {
#ifdef __3DS__
        return;
#else
        Mtx44 mtx;
        f32 bottom;
        f32 right;
        f32 top;
        f32 left;

        top = getFrameBufferHeight() * 0.5f - ::cCaptureOffsetX;
        left = -getFrameBufferWidth() * 0.5f + ::cCaptureOffsetY;
        right = getFrameBufferWidth() + left;
        bottom = top - getFrameBufferHeight();

        C_MTXOrtho(mtx, top, bottom, left, right, -1000.0f, 1000.0f);
        GXSetProjection(mtx, GX_ORTHOGRAPHIC);
#endif
    }

    const u8* getOdhImage() {
        if (!isExistSceneObj(SceneObj_OdhConverter)) {
            return nullptr;
        }

        return ::getConverter()->getImage();
    }

    u32 getOdhImageSize() {
        if (!isExistSceneObj(SceneObj_OdhConverter)) {
#ifdef __3DS__
            return 0;
#else
            return nullptr;
#endif
        }

        return ::getConverter()->getImageSize();
    }

    NameObjAdaptor* createAdaptorAndConnectToWiiMessageBoard(const char* pParam1, const FunctorBase& rFunc) {
        NameObjAdaptor* pDrawAdaptor = createDrawAdaptor(pParam1, rFunc);

        connectToScene(pDrawAdaptor, -1, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_MessageBoardCapture);

        return pDrawAdaptor;
    }
};  // namespace MR
