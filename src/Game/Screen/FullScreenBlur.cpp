#include "Game/Screen/FullScreenBlur.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/Color.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include <JSystem/JUtility/JUTTexture.hpp>

// 3DS: screen copies through the GPU (JUTTexture3DS.hpp) and the frame count (host.h).
#ifdef __3DS__
#include "JUTTexture3DS.hpp"
#include "host.h"

namespace {
    // ##############################################################################################
    // # The blur's copy of the last frame (3DS)                                                    #
    // #                                                                                            #
    // # The Wii draws the screen shrunk to 128x64 below the visible frame buffer (rows 464-528,    #
    // # GXSetScissorBoxOffset), copies that corner into MarioActor's 128x64 RGB565 texture         #
    // # (nonFilteredCapture at 0, 464) and, the next frame, stretches that copy over the screen     #
    // # with param2 more on each side at alpha param4. The copy is blurry, being so small, and     #
    // # each frame it comes back larger: the soft trails outward when Dino Piranha roars.          #
    // # The 3DS has no frame buffer outside the screen, so the same steps take a corner of it:    #
    // # the screen is copied whole, the screen capture is drawn shrunk to 128x64 pixels in the    #
    // # top left corner (1/128 of the width and 1/64 of the height a texel, as the Wii's) and that #
    // # corner copied, then the whole copy is drawn back. Two 128x64 copies take turns: this       #
    // # frame's goes into one while the other, the last frame's, is stretched over the screen.    #
    // # Both layers take their alpha from the constant alone, as the Wii's TEV stage              #
    // # (GXSetTevAlphaIn(ZERO, ZERO, ZERO, A0)), not from the copied frame buffer's alpha.         #
    // # Until CIA 0.21.4 the second layer was the current screen with 74% black over it, an       #
    // # estimate from footage: the roar dimmed the scene instead of blurring it (AppleBerries,     #
    // # 9 Nov). The 128x64 copies are RGB565, as the Wii's (hostGfxCaptureToTextureRgb565).       #
    // ##############################################################################################
    const f32 kCopyWidth = 128.0f;  // the Wii's copy, in pixels of the 400x240 screen
    const f32 kCopyHeight = 64.0f;

    ResTIMG sBlurCopyTIMG[2];
    JUTTexture sBlurCopies[2];
    s32 sBlurCopyLast = -1;        // the copy taken last, -1 for none
    unsigned long sBlurCopyFrame;  // the frame it was taken in (hostGfxFrameId)
    ResTIMG sScreenTIMG;
    JUTTexture sScreen;            // the screen, while the corner holds the shrunk copy

    // A texture whose image is a capture only: the TIMG names it (its size and format are the
    // capture's key, rvl/jut_texture.cpp) and holds no pixels.
    JUTTexture* captureTexture(JUTTexture& texture, ResTIMG& timg, u16 width, u16 height) {
        if (texture.getTexInfo() == nullptr) {
            timg.mFormat = GX_TF_RGB565;
            timg.mWidth = width;
            timg.mHeight = height;
            timg.mMinType = GX_LINEAR;
            timg.mMagType = GX_LINEAR;
            texture.storeTIMG(&timg, static_cast< u8 >(0));
        }
        return &texture;
    }

    JUTTexture* blurCopy(s32 index) {
        return captureTexture(sBlurCopies[index], sBlurCopyTIMG[index], 128, 64);
    }

    // The texture over the rectangle: its colour, and the constant alpha (the Wii's TEV stage
    // after GXSetTevAlphaIn(ZERO, ZERO, ZERO, A0)), or its own alpha when alpha is negative.
    void drawLayer(const TVec2f& rPosition, JUTTexture* pTexture, const TVec2f& rSize, s32 alpha) {
        HostGfxStage stage = {};
        stage.colorSrc[0] = 0x03;  // texture 0, replacing
        if (alpha < 0) {
            stage.alphaSrc[0] = 0x03;
        } else {
            stage.alphaSrc[0] = 0x0E;  // the constant
            stage.constant = static_cast< u32 >(alpha) << 24;
        }
        TDDraw::drawTextureStages(rPosition, pTexture, rSize, &stage, 1, false);
    }
}  // namespace
#endif

namespace MR {
    void drawFullScreenBlur(f32 param1) {
        drawFullScreenBlur(param1, param1, param1 / 30.0f, (param1 / 30.0f) / 2.0f);
    }

    void drawFullScreenBlur(f32 param1, f32 param2, u8 param3, u8 param4) {
        f32 screenWidth = MR::getScreenWidth();

        // 3DS: the same state through DirectDraw: blending, alpha test and no depth.
#ifdef __3DS__
        TDDraw::setupDirect(1, 1, 2);
        TDDraw::setDirectBlendState(0, 6, 7);
        TDDraw::setDirectAlphaTest(1, 0);
        TDDraw::setDirectDepthMode(false, 5, false);
#else
        TDDraw::setup(1, 1, 2);
#endif

        JUTTexture texture = JUTTexture(MR::getScreenResTIMG(), 0);

        // 3DS: the Wii's shrunk copy below the frame buffer (next lines), in the screen's corner:
        // the screen kept, the copy drawn and taken, the screen drawn back (above). The last
        // frame's copy is the other one.
#ifdef __3DS__
        const unsigned long frame = hostGfxFrameId();
        const s32 lastCopy = sBlurCopyLast;
        const bool lastCopyFresh = lastCopy >= 0 && frame - sBlurCopyFrame <= 2;  // a skipped frame takes no copy
        const s32 nextCopy = lastCopy == 0 ? 1 : 0;
        JUTTexture* screen = captureTexture(sScreen, sScreenTIMG, 640, 456);
        rvl3DSCaptureJUTTexture(screen);
        if (rvl3DSLastCaptureDone()) {
            TDDraw::setDirectBlendState(0, 1, 0);  // GPU_ONE, GPU_ZERO: the copies as they are
            drawLayer(TVec2f(0.0f, 0.0f), &texture, TVec2f(screenWidth * kCopyWidth / 400.0f, 456.0f * kCopyHeight / 240.0f), -1);
            rvl3DSCaptureJUTTextureRegionRgb565(blurCopy(nextCopy), 0.0f, 0.0f, 640.0f * kCopyWidth / 400.0f, 456.0f * kCopyHeight / 240.0f);
            if (rvl3DSLastCaptureDone()) {
                sBlurCopyLast = nextCopy;
                sBlurCopyFrame = frame;
            }
            drawLayer(TVec2f(0.0f, 0.0f), screen, TVec2f(screenWidth, 456.0f), -1);
            TDDraw::setDirectBlendState(0, 6, 7);
        }
#endif

        // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
        // (DETAILS.md, section 5).
#ifndef __3DS__
        GXSetScissorBoxOffset(0, 1584);
#endif
        // Wii only: the small capture the Wii blurs; the 3DS draws the full screen capture below.
#ifndef __3DS__
        TDDraw::drawTexture(TVec2f(0.0f, 0.0f), &texture, TVec2f((screenWidth / 640.0f) * 128.0f, 64.0f));
#endif
        // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
        // (DETAILS.md, section 5).
#ifndef __3DS__
        GXSetScissorBoxOffset(0, 0);
        GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
        GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

        GXColorS10 color1 = {0, 0, 0, param3};
        GXSetTevColorS10(GX_TEVREG0, color1);
#endif

        // 3DS: the screen capture over the whole screen, larger by param1, at alpha param3 alone.
#ifdef __3DS__
        drawLayer(TVec2f(-param1, -param1), &texture, TVec2f(screenWidth + param1 * 2.0f, 456.0f + param1 * 2.0f), param3);
#else
        TDDraw::drawTexture(TVec2f(-param1, -param1), &texture, TVec2f(screenWidth + param1 * 2.0f, 456.0f + param1 * 2.0f));
#endif

        // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
        // (DETAILS.md, section 5).
#ifndef __3DS__
        GXColorS10 color2 = {0, 0, 0, param4};
        GXSetTevColorS10(GX_TEVREG0, color2);
#endif
#ifdef __3DS__
        // The last frame's 128x64 copy, larger by param2 on each side, at alpha param4, filtered
        // as the Wii's (linear). None yet: the Wii's holds whatever it held last, faded in by
        // param4 anyway.
        if (lastCopyFresh) {
            drawLayer(TVec2f(-param2, -param2), blurCopy(lastCopy), TVec2f(screenWidth + param2 * 2.0f, 456.0f + param2 * 2.0f), param4);
        }
#else
        TDDraw::drawTexture(TVec2f(-param2, -param2), MR::getFullScreenBlurTexture(), TVec2f(screenWidth + param2 * 2.0f, 456.0f + param2 * 2.0f));
#endif
        MR::nonFilteredCapture(MR::getFullScreenBlurTexture(), 0, 464);
        MR::loadProjectionMtx();
        MR::loadViewMtx();
    }
};  // namespace MR
