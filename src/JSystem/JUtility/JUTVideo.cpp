#include "JSystem/JUtility/JUTVideo.hpp"
#include "JSystem/JUtility/JUTDirectPrint.hpp"
#include "JSystem/JUtility/JUTXfb.hpp"

JUTVideo* JUTVideo::sManager;

OSTick JUTVideo::sVideoLastTick;

OSTick JUTVideo::sVideoInterval;

static bool data_80451544;

JUTVideo* JUTVideo::createManager(_GXRenderModeObj const* param_0) {
    if (sManager == nullptr) {
        sManager = new JUTVideo(param_0);
    }
    return sManager;
}

void JUTVideo::destroyManager() {
    if (sManager != nullptr) {
        delete sManager;
        sManager = nullptr;
    }
}

JUTVideo::JUTVideo(GXRenderModeObj const* param_0) {
    mRenderObj = nullptr;
    VIInit();
    mSetBlack = true;
    mSetBlackFrameCount = 2;
    setRenderMode(param_0);
    VISetBlack(1);
    VIFlush();
    field_0x8 = 0;
    mRetraceCount = VIGetRetraceCount();
    field_0x10 = 1;
    field_0x18 = 0;
    sVideoLastTick = OSGetTick();
    sVideoInterval = 670000;
    mPreRetraceCallback = VISetPreRetraceCallback(preRetraceProc);
    mPostRetraceCallback = VISetPostRetraceCallback(postRetraceProc);
    mPreCallback = nullptr;
    mPostCallback = nullptr;
    OSInitMessageQueue(&mMessageQueue, &mMessage, 1);
    // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
    // (DETAILS.md, section 5).
#ifndef __3DS__
    GXSetDrawDoneCallback(drawDoneCallback);
#endif
}

JUTVideo::~JUTVideo() {
    VISetPreRetraceCallback(mPreRetraceCallback);
    VISetPostRetraceCallback(mPostRetraceCallback);
}

void JUTVideo::preRetraceProc(u32 retrace_count) {
#ifdef __3DS__
    // The 3DS interrupt shim can dispatch a pending retrace as soon as callback
    // registration restores interrupts. createManager() publishes sManager only after
    // this constructor returns, so ignore that early callback until the object is ready.
    if (sManager == nullptr) {
        return;
    }
#endif
    if (sManager->mPreCallback) {
        (*sManager->mPreCallback)(retrace_count);
    }

    OSTick tick = OSGetTick();
    sVideoInterval = tick - sVideoLastTick;
    sVideoLastTick = tick;

    JUTXfb* xfb = JUTXfb::sManager;
    if (!xfb) {
        VISetBlack(TRUE);
        VIFlush();
        return;
    }

    static void* frameBuffer = nullptr;

    if (frameBuffer) {
        JUTVideo* videoManager = JUTGetVideoManager();
        const GXRenderModeObj* renderMode = videoManager->getRenderMode();
        JUTDirectPrint* directPrint = JUTDirectPrint::getManager();
        directPrint->changeFrameBuffer(frameBuffer, renderMode->fbWidth, renderMode->efbHeight);
    }

    if (sManager->mSetBlack == 1) {
        s32 frame_count = sManager->mSetBlackFrameCount;
        if (frame_count > 0) {
            frame_count--;
        }

        sManager->mSetBlackFrameCount = frame_count;
        sManager->mSetBlack = frame_count != 0;
        VISetBlack(TRUE);
        VIFlush();
        return;
    }

    if (!xfb) {
        VISetBlack(TRUE);
        VIFlush();
        return;
    }

    u32 bufferNum = xfb->getBufferNum();
    if (bufferNum == 2 || bufferNum == 3) {
        if (!data_80451544) {
            s16 index = xfb->getDrawnXfbIndex();
            xfb->setDisplayingXfbIndex(index);
            if (index < 0) {
                VISetBlack(1);
                VIFlush();
            } else {
                VISetNextFrameBuffer(xfb->getDisplayingXfb());
                VIFlush();
                VISetBlack(FALSE);
                frameBuffer = xfb->getDisplayingXfb();
            }
        }
    } else if (xfb->getBufferNum() == 1) {
        if (xfb->getSDrawingFlag() == 0) {
            s16 index = xfb->getDrawnXfbIndex();
            if (index >= 0) {
                xfb->setDisplayingXfbIndex(index);
                // Wii only: GX state, display lists and material setup; the 3DS draws this through
                // Citro3D (DETAILS.md, section 5).
#ifndef __3DS__
                GXCopyDisp(xfb->getDisplayingXfb(), GX_TRUE);
                GXFlush();
#endif
                xfb->setSDrawingFlag(2);
                frameBuffer = xfb->getDisplayingXfb();
                if (VIGetNextFrameBuffer()) {
                    VISetBlack(FALSE);
                }
            } else {
                VISetBlack(TRUE);
            }
        }
        VIFlush();
    }
}

void JUTVideo::drawDoneStart() {
    data_80451544 = true;
    // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
    // (DETAILS.md, section 5).
#ifdef __3DS__
    return;
#else
    GXSetDrawDone();
#endif
}

void JUTVideo::dummyNoDrawWait() {
    data_80451544 = false;
}

void JUTVideo::drawDoneCallback() {
    JUTXfb* xfb = JUTXfb::sManager;
    if (!xfb) {
        return;
    }

    data_80451544 = false;
    if (xfb->getBufferNum() == 1) {
        if (xfb->getSDrawingFlag() == 1) {
            xfb->setSDrawingFlag(0);

            void* frameBuffer = xfb->getDrawnXfb();
            if (frameBuffer) {
                VISetNextFrameBuffer(xfb->getDrawnXfb());
                VIFlush();
            }
        }
    }
}

void JUTVideo::postRetraceProc(u32 retrace_count) {
#ifdef __3DS__
    // See preRetraceProc: the post callback can also run before createManager() returns.
    if (sManager == nullptr) {
        return;
    }
#endif
    if (sManager->mPostCallback != nullptr) {
        sManager->mPostCallback(retrace_count);
    }

    OSMessage message = reinterpret_cast< OSMessage >(VIGetRetraceCount());
    OSSendMessage(&sManager->mMessageQueue, message, OS_MESSAGE_NOBLOCK);
}

void JUTVideo::setRenderMode(GXRenderModeObj const* pObj) {
    if (mRenderObj != nullptr && pObj->viTVmode != mRenderObj->viTVmode) {
        mSetBlack = true;
        mSetBlackFrameCount = 4;
    }
    mRenderObj = const_cast< GXRenderModeObj* >(pObj);
    VIConfigure(mRenderObj);
    VIFlush();

    if (mSetBlack) {
        VIWaitForRetrace();
        VIWaitForRetrace();
    }
}

void JUTVideo::waitRetraceIfNeed() {
}
