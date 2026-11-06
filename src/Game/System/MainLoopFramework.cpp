#include "Game/System/MainLoopFramework.hpp"
#include "Game/System/DrawSyncManager.hpp"
#include "Game/Util/BothDirList.hpp"
#include "Game/Util/SchedulerUtil.hpp"
#include <JSystem/J2DGraph/J2DOrthoGraph.hpp>
#include <JSystem/JUtility/JUTAssert.hpp>
#include <JSystem/JUtility/JUTConsole.hpp>
#include <JSystem/JUtility/JUTDirectPrint.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>
#include <JSystem/JUtility/JUTXfb.hpp>
#include <revolution/gx/GXRegs.h>
#include <runtime.h>

MainLoopFramework* MainLoopFramework::sManager;

MR::BothDirList< MainLoopFrameworkAlarm > MainLoopFrameworkAlarm::sList(false);

GXTexObj clear_z_tobj;
// 3DS: the frame phase for the stall report, and the 3DS renderer's frame end.
#ifdef __3DS__
extern "C" void rvlGXDebugSetFramePhase(int phase);
void rvl3DSEndFrame();
#include <host.h>
#endif

Mtx e_mtx = {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};

#ifdef __3DS__
// 3DS port: GX texture data is big-endian; written as bytes so it reads the same on a
// little-endian CPU (the Wii's 0x00FF00FF words).
u8 clearZTexData[] ATTRIBUTE_ALIGN(32) = {0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
                                          0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
#else
u32 clearZTexData[] ATTRIBUTE_ALIGN(32) = {0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF,
                                           0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
#endif

namespace {
    s32 getDisplayingXfbIndex() NO_INLINE;

    void waitForTick(u32, u16);

    void waitDrawDoneAndSetAlarm();

    void handleGXAbortAlarm(OSAlarm*, OSContext*);
};  // namespace

void MainLoopFramework::ctor_subroutine(bool useAlpha) {
    mClearColor = (GXColor){0, 0, 0, 0};
    mClearZ = 0xffffff;
    mDispCopyGamma = GX_GM_1_0;
    mCopyClamp = GX_CLAMP_TOP | GX_CLAMP_BOTTOM;
    _C = 1;
    mRetraceCount = 1;
    mTickDuration = 0;
    mUseAlpha = useAlpha;
    mUseVFilter = true;
    mCombinationRatio = 0.0f;
    mTick = OSGetTick();
    mLastFrameTime = 0;
    mLastVideoTickDelta = 0;
    mSingleBufferIndex = 0;
    _3E = 0;
    // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
    // (DETAILS.md, section 5).
#ifndef __3DS__
    GXInitTexObj(&clear_z_tobj, clearZTexData, 4, 4, GX_TF_Z24X8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GXInitTexObjLOD(&clear_z_tobj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
#endif
    mRenderInterval = 1;
    mRenderCounter = 0;
    mDoRenderFrame = false;
    mPreRenderCallback = nullptr;
}

MainLoopFramework::~MainLoopFramework() {
    if (JUTVideo::getManager()) {
        waitBlanking(2);
    }
    JUTXfb::destroyManager();
}

MainLoopFramework* MainLoopFramework::createManager(const GXRenderModeObj* pRenderModeObj, void* xfb1, void* xfb2, void* xfb3, bool useAlpha) {
    if (pRenderModeObj) {
        JUTVideo::getManager()->setRenderMode(pRenderModeObj);
    }
    if (!sManager) {
        sManager = new MainLoopFramework(xfb1, xfb2, xfb3, useAlpha);
    }
    return sManager;
}

void callDirectDraw() {
    u16 efbHeight, fbWidth;
    fbWidth = JUTVideo::getManager()->getRenderMode()->fbWidth;
    efbHeight = JUTVideo::getManager()->getRenderMode()->efbHeight;
    void* pXfb = JUTXfb::sManager->getDrawingXfb();
    JUTDirectPrint::sDirectPrint->changeFrameBuffer(pXfb, fbWidth, efbHeight);
    JUTAssertion::flushMessage();
}

#pragma push
#pragma global_optimizer off
void MainLoopFramework::prepareCopyDisp() {
#ifndef __3DS__  // 3DS: the frame reaches the screen through hostGfxEndFrame
    u16 fbWidth = JUTVideo::getManager()->getRenderMode()->fbWidth;
    u16 efbHeight = JUTVideo::getManager()->getRenderMode()->efbHeight;
    u16 xfbHeight = JUTVideo::getManager()->getRenderMode()->xfbHeight;
    f32 yscale = GXGetYScaleFactor(efbHeight, xfbHeight);
    u16 nlines = GXGetNumXfbLines(efbHeight, yscale);
    GXSetCopyClear(mClearColor, mClearZ);
    GXSetDispCopySrc(0, 0, fbWidth, efbHeight);
    GXSetDispCopyDst(fbWidth, nlines);
    GXSetDispCopyYScale(yscale);
    VIFlush();
    GXRenderModeObj* pMode = JUTVideo::getManager()->getRenderMode();
    u8* pVFilter = pMode->vfilter;
    GXSetCopyFilter(pMode->aa, pMode->sample_pattern, mUseVFilter, pVFilter);
    GXSetCopyClamp((GXFBClamp)mCopyClamp);
    GXSetDispCopyGamma((GXGamma)mDispCopyGamma);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    if (mUseAlpha) {
        GXSetAlphaUpdate(GX_TRUE);
    }
#endif
}
#pragma pop

void MainLoopFramework::drawendXfb_single() {
    JUTXfb* pXfbMgr = JUTXfb::sManager;
    if (pXfbMgr->mDrawingXfbIndex >= 0) {
        prepareCopyDisp();
        ::waitDrawDoneAndSetAlarm();
        // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
        // (DETAILS.md, section 5).
#ifndef __3DS__
        GXFlush();
#endif
        pXfbMgr->mDrawnXfbIndex = pXfbMgr->mDrawingXfbIndex;
    }
}

void MainLoopFramework::exchangeXfb_double() {
    // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
    // (DETAILS.md, section 5).
#ifdef __3DS__
    return;
#else
    JUTXfb* pXfbMgr = JUTXfb::sManager;
    if (pXfbMgr->mDrawnXfbIndex == pXfbMgr->mDisplayingXfbIndex) {
        if (pXfbMgr->mDrawingXfbIndex >= 0) {
            if (mPreRenderCallback) {
                mPreRenderCallback();
            }
            prepareCopyDisp();
            GXCopyDisp(pXfbMgr->getDrawingXfb(), GX_TRUE);
            if (!_C) {
                pXfbMgr->mDrawnXfbIndex = pXfbMgr->mDrawingXfbIndex;
                GXDrawDone();
                JUTVideo::dummyNoDrawWait();
            } else {
                JUTVideo::drawDoneStart();
            }
            if (!_C) {
                callDirectDraw();
            }
        }
        s16 lastIdx = pXfbMgr->mDrawingXfbIndex;
        pXfbMgr->mDrawnXfbIndex = lastIdx;
        pXfbMgr->mDrawingXfbIndex = lastIdx >= 0 ? lastIdx ^ 1 : 0;
    } else {
        clearEfb(mClearColor);
        if (pXfbMgr->mDrawingXfbIndex < 0) {
            pXfbMgr->mDrawingXfbIndex = 0;
        }
    }
#endif
}

#ifdef __3DS__
// 3DS port: exchangeXfb_triple spins until the retrace interrupt changes the displayed
// buffer; the port delivers interrupts only at OS calls, so the loop polls for them.
extern "C" void rvlPollInterrupts(void);
#endif

namespace {
    s32 getDisplayingXfbIndex() {
        // 3DS: delivers pending interrupts while the loop waits for the retrace.
#ifdef __3DS__
        rvlPollInterrupts();
#endif
        return JUTXfb::getManager()->getDisplayingXfbIndex();
    }
};  // namespace

void MainLoopFramework::exchangeXfb_triple() {
    JUTXfb* pXfbMgr = JUTXfb::sManager;
    if (pXfbMgr->mDrawingXfbIndex >= 0) {
        callDirectDraw();
    }
    pXfbMgr->mDrawnXfbIndex = pXfbMgr->mDrawingXfbIndex;
    s16 nextIdx = pXfbMgr->mDrawingXfbIndex + 1;
    do {
        if (nextIdx >= 3 || nextIdx < 0) {
            nextIdx = 0;
        }
    } while (nextIdx == ::getDisplayingXfbIndex());
    pXfbMgr->mDrawingXfbIndex = nextIdx;
}

void MainLoopFramework::copyXfb_triple() {
    // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
    // (DETAILS.md, section 5).
#ifdef __3DS__
    return;
#else
    JUTXfb* pXfbMgr = JUTXfb::sManager;
    if (pXfbMgr->mDrawingXfbIndex >= 0) {
        if (mPreRenderCallback) {
            mPreRenderCallback();
        }
        prepareCopyDisp();
        GXCopyDisp(pXfbMgr->getDrawingXfb(), GX_FALSE);
        GXPixModeSync();
    }
#endif
}

void MainLoopFramework::preGX() {
#ifdef __3DS__
    // Native Citro3D frames do not need Wii cache invalidation or pixel-format commands.
    // Leave the legacy state untouched until a fallback actually emits GX work.
    return;
#else
    GXInvalidateTexAll();
    GXInvalidateVtxCache();
    if (JUTVideo::getManager()->getRenderMode()->aa) {
        GXSetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
        GXSetDither(GX_TRUE);
    } else if (mUseAlpha) {
        GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
        GXSetDither(GX_TRUE);
    } else {
        GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
        GXSetDither(GX_FALSE);
    }
#endif
}

void MainLoopFramework::endGX() {
    // 3DS: the frame phase, for the stall report (debug.c).
#ifdef __3DS__
    rvlGXDebugSetFramePhase(9);
#endif
    J2DOrthoGraph ortho(0.0f, 0.0f, JUTVideo::getManager()->getRenderMode()->fbWidth, JUTVideo::getManager()->getRenderMode()->efbHeight, -1.0f,
                        1.0f);
    // 3DS: the frame phase, for the stall report (debug.c).
#ifdef __3DS__
    rvlGXDebugSetFramePhase(10);
#endif
    if (JUTConsoleManager::sManager) {
        ortho.setPort();
        JUTConsoleManager::sManager->draw();
    }
    // 3DS: the frame phase, for the stall report (debug.c).
#ifdef __3DS__
    rvlGXDebugSetFramePhase(11);
#endif
    if (_C || JUTXfb::sManager->mBufferNum == 1) {
        JUTAssertion::flushMessage_dbPrint();
    }
    // 3DS: the frame phase, for the stall report (debug.c).
#ifdef __3DS__
    rvlGXDebugSetFramePhase(12);
#endif
#ifndef __3DS__  // 3DS: no GX command stream to flush
    GXFlush();
#endif
}

void MainLoopFramework::waitForRetrace() {
    ::waitForTick(mTickDuration, mRetraceCount);
    JUTVideo::getManager()->waitRetraceIfNeed();
}

void MainLoopFramework::beginRender() {
    OSTick currentTick = OSGetTick();
    mLastFrameTime = currentTick - mTick;
    mTick = currentTick;

    mLastVideoTickDelta = currentTick - JUTVideo::getVideoLastTick();

    if (mDoRenderFrame) {
        JUTXfb* xfb = JUTXfb::sManager;
        switch (xfb->mBufferNum) {
        case 1:
            if (xfb->mSDrawingFlag != 2) {
                xfb->mSDrawingFlag = 1;
                clearEfb(mClearColor);
            } else {
                xfb->mSDrawingFlag = 1;
            }
            xfb->mDrawingXfbIndex = mSingleBufferIndex;
            break;
        case 2:
            exchangeXfb_double();
            break;
        case 3:
            exchangeXfb_triple();
            break;
        }
    }

    if ((mDoRenderFrame = ++mRenderCounter >= mRenderInterval)) {
        mRenderCounter = 0;
    }
    if (mDoRenderFrame) {
        clearEfb(mClearColor);
        preGX();
    }
}

void MainLoopFramework::endRender() {
    // 3DS: the frame phase, for the stall report (debug.c).
#ifdef __3DS__
    rvlGXDebugSetFramePhase(13);
#endif
    endGX();
    // 3DS: the frame ends here: the clear colour for the next frame, and the 3DS renderer's frame
    // end.
#ifdef __3DS__
    {
        GXColor clear = mClearColor;
        unsigned long clearValue = (static_cast< unsigned long >(clear.r) << 24) |
                                   (static_cast< unsigned long >(clear.g) << 16) |
                                   (static_cast< unsigned long >(clear.b) << 8) |
                                   static_cast< unsigned long >(clear.a);
        rvl3DSEndFrame();
        hostGfxEndFrame(clearValue);
    }
#endif
    // 3DS: the frame phase, for the stall report (debug.c).
#ifdef __3DS__
    rvlGXDebugSetFramePhase(14);
#endif
    if (mDoRenderFrame) {
        // Wii only: the Wii's external frame buffers; the 3DS shows its render target itself
        // (host/gfx.c).
#ifndef __3DS__
        JUTXfb* xfb = JUTXfb::sManager;
        switch (xfb->mBufferNum) {
        case 1:
            drawendXfb_single();
            break;
        case 2:
            break;
        case 3:
            copyXfb_triple();
            break;
        }
#endif
    }
    // 3DS: the frame phase, for the stall report (debug.c).
#ifdef __3DS__
    rvlGXDebugSetFramePhase(15);
#endif
    calcCombinationRatio();
    // 3DS: the frame phase, for the stall report (debug.c).
#ifdef __3DS__
    rvlGXDebugSetFramePhase(3);
#endif
}

void MainLoopFramework::endFrame() {
    if (mDoRenderFrame) {
        JUTXfb* xfb = JUTXfb::sManager;
        switch (xfb->mBufferNum) {
        case 1:
            break;
        case 2:
            ::waitDrawDoneAndSetAlarm();
            // Wii only: GX state, display lists and material setup; the 3DS draws this through
            // Citro3D (DETAILS.md, section 5).
#ifndef __3DS__
            GXFlush();
#endif
            break;
        case 3:
            ::waitDrawDoneAndSetAlarm();
            // Wii only: GX state, display lists and material setup; the 3DS draws this through
            // Citro3D (DETAILS.md, section 5).
#ifndef __3DS__
            GXFlush();
#endif
            break;
        }
    }

    if (mDoRenderFrame) {
        static s8 lbl_806B6ED4;
        if (!lbl_806B6ED4) {
            VIGetRetraceCount();
            lbl_806B6ED4 = 1;
        }
        VIGetRetraceCount();
    }
}

void MainLoopFramework::waitBlanking(int tickCount) {
    while (tickCount-- > 0) {
        ::waitForTick(mTickDuration, mRetraceCount);
    }
}

void MainLoopFramework::setTickRateFromFrame(u16 frame) {
    mTickDuration = frameToTick(frame);
    mRetraceCount = 0;
}

void MainLoopFramework::clearEfb(GXColor color) {
    u16 fbWidth = JUTVideo::getManager()->getRenderMode()->fbWidth;
    u16 efbHeight = JUTVideo::getManager()->getRenderMode()->efbHeight;
    clearEfb(0, 0, fbWidth, efbHeight, color);
}

void MainLoopFramework::clearEfb(int param1, int param2, int param3, int param4, GXColor color) {
    // 3DS: a clear of the whole frame buffer clears the render target (other areas are not
    // cleared).
#ifdef __3DS__
    if (param1 == 0 && param2 == 0 &&
        param3 == JUTVideo::getManager()->getRenderMode()->fbWidth &&
        param4 == JUTVideo::getManager()->getRenderMode()->efbHeight) {
        unsigned long clear = (static_cast< unsigned long >(color.r) << 24) |
                              (static_cast< unsigned long >(color.g) << 16) |
                              (static_cast< unsigned long >(color.b) << 8) |
                              static_cast< unsigned long >(color.a);
        hostGfxClearFrame(clear);
        return;
    }
#else
    u16 fbWidth = JUTVideo::getManager()->getRenderMode()->fbWidth;
    u16 efbHeight = JUTVideo::getManager()->getRenderMode()->efbHeight;
    Mtx44 proj;
    C_MTXOrtho(proj, 0.0f, efbHeight, 0.0f, fbWidth, 0.0f, 1.0f);
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
    GXSetViewport(0.0f, 0.0f, fbWidth, efbHeight, 0.0f, 1.0f);
    GXSetScissor(0, 0, fbWidth, efbHeight);
    GXLoadPosMtxImm(e_mtx, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_U16, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_POS_XYZ, GX_U8, 0);
    GXSetNumChans(0);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    GXLoadTexObj(&clear_z_tobj, GX_TEXMAP0);
    GXSetNumTevStages(1);
    GXSetTevColor(GX_TEVREG0, color);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetZTexture(GX_ZT_REPLACE, GX_TF_Z24X8, 0);
    GXSetZCompLoc(GX_FALSE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_NOOP);

    if (mUseAlpha) {
        GXSetAlphaUpdate(GX_TRUE);
        GXSetDstAlpha(GX_TRUE, color.a);
    }

    GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GXSetCullMode(GX_CULL_BACK);

    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    {
        GXPosition2u16(param1, param2);
        GXTexCoord2u8(0, 0);

        GXPosition2u16(param1 + param3, param2);
        GXTexCoord2u8(1, 0);

        GXPosition2u16(param1 + param3, param2 + param4);
        GXTexCoord2u8(1, 1);

        GXPosition2u16(param1, param2 + param4);
        GXTexCoord2u8(0, 1);
    }
    GXEnd();

    GXSetZTexture(GX_ZT_DISABLE, GX_TF_Z24X8, 0);
    GXSetZCompLoc(GX_TRUE);

    if (mUseAlpha) {
        GXSetDstAlpha(GX_FALSE, color.a);
    }
#endif
}

void MainLoopFramework::calcCombinationRatio() {
    s32 videoInterval = JUTVideo::getVideoInterval();
    s32 var1 = videoInterval;
    while (var1 < 2 * mLastFrameTime) {
        var1 += videoInterval;
    }
    s32 var2 = var1 - 2 * mLastFrameTime - mLastVideoTickDelta;
    if (var2 < 0) {
        var2 += videoInterval;
    }

    mCombinationRatio = (f32)var2 / (u32)mLastFrameTime;
    if (mCombinationRatio > 1.0f) {
        mCombinationRatio = 1.0f;
    }
}

u32 MainLoopFramework::frameToTick(f32 frame) {
    s32 tick = (s32)((OS_BUS_CLOCK / 4) * frame / 59.94) - 100;
    return (tick < 0) ? 0 : tick;
}

void MainLoopFramework::setForOSResetSystem() {
    MR::BothDirPtrLink* pLink = MainLoopFrameworkAlarm::sList.mHead;
    while (pLink) {
        OSCancelAlarm((MainLoopFrameworkAlarm*)pLink->mValue);
        pLink = pLink->mNextLink;
    }
    JUTVideo::destroyManager();
    VISetBlack(TRUE);
    VIFlush();
    VIWaitForRetrace();
    if (sManager) {
        delete sManager;
        sManager = nullptr;
    }
}

namespace {
    void waitForTick(u32 tickDuration, u16 retraceCount) {
        if (tickDuration) {
            static s64 nextTime;
            static s8 timeInitialized;
            if (!timeInitialized) {
                nextTime = OSGetTime();
                timeInitialized = 1;
            }
            s64 currentTime = OSGetTime();
            while (currentTime < nextTime) {
                OSSleepTicks(nextTime - currentTime);
                currentTime = OSGetTime();
            }
            nextTime = currentTime + tickDuration;
        } else {
            static u32 nextCount;
            static s8 countInitialized;
            if (!countInitialized) {
                nextCount = VIGetRetraceCount();
                countInitialized = 1;
            }

            u32 count = retraceCount ? retraceCount : 1;
            u32 dummy = 0;

            u32 msg;
            do {
                if (!OSReceiveMessage(JUTVideo::getManager()->getMessageQueue(), (OSMessage*)&msg, OS_MESSAGE_BLOCK)) {
                    msg = dummy;
                }
            } while ((s32)(msg - nextCount) < 0);
            nextCount = msg + count;
        }
    }

    void waitDrawDoneAndSetAlarm() {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        return;
#else
        MainLoopFrameworkAlarm alarm;
        {
            MR::ProhibitSchedulerAndInterrupts prohibit(false);
            OSCreateAlarm(&alarm);
            MainLoopFrameworkAlarm::sList.append(&alarm.mLink);
        }
        OSTime tick = __cvt_dbl_usll(OS_BUS_CLOCK / 4 * 0.5);
        OSSetAlarm(&alarm, tick, &handleGXAbortAlarm);
        GXDrawDone();
        DrawSyncManager::resetIfAborted();
        {
            MR::ProhibitSchedulerAndInterrupts prohibit(false);
            OSCancelAlarm(&alarm);
            MainLoopFrameworkAlarm::sList.remove(&alarm.mLink);
        }
#endif
    }

    void handleGXAbortAlarm(OSAlarm* alarm, OSContext* context) {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        return;
#else
        u32 ras_busy2, clocks2, xf_wait_in2, xf_wait_out2;
        u32 ras_busy, clocks, xf_wait_in, xf_wait_out;
        GXReadXfRasMetric(&xf_wait_in, &xf_wait_out, &ras_busy, &clocks);
        GXReadXfRasMetric(&xf_wait_in2, &xf_wait_out2, &ras_busy2, &clocks2);
        GXBool brkpt, cmdIdle, readIdle, underlow, overhi;
        GXGetGPStatus(&overhi, &underlow, &readIdle, &cmdIdle, &brkpt);
        GXDisableBreakPt();
        GXAbortFrame();
        DrawSyncManager::sInstance->clearFifo();
        GX_WRITE_U8(0x61);
        GX_WRITE_U32(0x5800000f);
        GXSetDrawDone();
        DrawSyncManager::prepareReset();
#endif
    }
};  // namespace
