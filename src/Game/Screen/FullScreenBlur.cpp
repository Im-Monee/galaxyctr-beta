#include "Game/Screen/FullScreenBlur.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/Color.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include <JSystem/JUtility/JUTTexture.hpp>

namespace MR {
    void drawFullScreenBlur(f32 param1) {
        drawFullScreenBlur(param1, param1, param1 / 30.0f, (param1 / 30.0f) / 2.0f);
    }

    void drawFullScreenBlur(f32 param1, f32 param2, u8 param3, u8 param4) {
        f32 screenWidth = MR::getScreenWidth();

#ifdef __3DS__
        TDDraw::setupDirect(1, 1, 2);
        TDDraw::setDirectBlendState(0, 6, 7);
        TDDraw::setDirectAlphaTest(1, 0);
        TDDraw::setDirectDepthMode(false, 5, false);
#else
        TDDraw::setup(1, 1, 2);
#endif

        JUTTexture texture = JUTTexture(MR::getScreenResTIMG(), 0);

#ifndef __3DS__
        GXSetScissorBoxOffset(0, 1584);
#endif
#ifndef __3DS__
        TDDraw::drawTexture(TVec2f(0.0f, 0.0f), &texture, TVec2f((screenWidth / 640.0f) * 128.0f, 64.0f));
#endif
#ifndef __3DS__
        GXSetScissorBoxOffset(0, 0);
        GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
        GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

        GXColorS10 color1 = {0, 0, 0, param3};
        GXSetTevColorS10(GX_TEVREG0, color1);
#endif

#ifdef __3DS__
        TDDraw::drawTexture(TVec2f(-param1, -param1), &texture, TVec2f(screenWidth + param1 * 2.0f, 456.0f + param1 * 2.0f), param3);
#else
        TDDraw::drawTexture(TVec2f(-param1, -param1), &texture, TVec2f(screenWidth + param1 * 2.0f, 456.0f + param1 * 2.0f));
#endif

#ifndef __3DS__
        GXColorS10 color2 = {0, 0, 0, param4};
        GXSetTevColorS10(GX_TEVREG0, color2);
#endif
#ifdef __3DS__
        // The Wii keeps a small copy of the screen off screen and blends in last frame's. The 3DS
        // has no such area: the screen again, at the second offset.
        TDDraw::drawTexture(TVec2f(-param2, -param2), &texture, TVec2f(screenWidth + param2 * 2.0f, 456.0f + param2 * 2.0f), param4);
        // On the Wii that second layer also darkens the screen: measured on footage of the Dino
        // Piranha's roars, about 74% black at param4 80 (CenterScreenBlur), following its fades.
        const u32 dark = param4 * 189 / 80;
        HostGfxStage black = {};
        black.colorSrc[0] = black.alphaSrc[0] = 0x0E;  // the constant
        black.constant = (dark > 255 ? 255 : dark) << 24;
        TDDraw::drawTextureStages(TVec2f(-param2, -param2), &texture, TVec2f(screenWidth + param2 * 2.0f, 456.0f + param2 * 2.0f), &black, 1, false);
#else
        TDDraw::drawTexture(TVec2f(-param2, -param2), MR::getFullScreenBlurTexture(), TVec2f(screenWidth + param2 * 2.0f, 456.0f + param2 * 2.0f));
#endif
        MR::nonFilteredCapture(MR::getFullScreenBlurTexture(), 0, 464);
        MR::loadProjectionMtx();
        MR::loadViewMtx();
    }
};  // namespace MR
