#include "nw4r/ut/CharWriter.h"
#include "nw4r/ut/Font.h"
#include <revolution/gx/GXVert.h>
#ifdef __3DS__
#include "Game/Util/DirectDraw.hpp"
#include "GXToCitro3D.hpp"

namespace {
    // The glyphs of a text box, drawn together while the font's texture and mode stay the same: a
    // draw per glyph made 900 draws a frame of the prologue's picture book, about 45 ms a frame at
    // 268 MHz (CIA 0.16.1). TextBox::DrawSelf opens the batch; a tag's own drawing flushes it.
    const u32 kGlyphBatch = 192;
    bool sGlyphBatching;
    u32 sGlyphCount;
    TVec3f sGlyphPositions[kGlyphBatch];
    TVec2f sGlyphUvs[kGlyphBatch];
    u32 sGlyphColors[kGlyphBatch];
    const void* sGlyphImage;
    u16 sGlyphWidth, sGlyphHeight;
    u32 sGlyphFormat, sGlyphMode, sGlyphMin, sGlyphMax;
    u8 sGlyphFilter;
}  // namespace

void rvl3DSGlyphBatchFlush() {
    if (sGlyphCount != 0) {
        const u32 count = sGlyphCount;
        sGlyphCount = 0;
        TDDraw::drawNativeTexturedTriangles(sGlyphPositions, sGlyphUvs, sGlyphColors, sGlyphImage, sGlyphWidth, sGlyphHeight, sGlyphFormat,
                                            nullptr, 0, 0, 0, 0, sGlyphFilter, count, sGlyphMode, sGlyphMin, sGlyphMax);
    }
}

void rvl3DSGlyphBatch(bool on) {
    rvl3DSGlyphBatchFlush();
    sGlyphBatching = on;
}
#endif

namespace {
    // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
    // (DETAILS.md, section 5).
#ifndef __3DS__
    void SetupGXCommon() {
        static const nw4r::ut::Color fog = 0;
        GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, fog);
        GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
        GXSetZTexture(GX_ZT_DISABLE, GX_TF_Z8, 0);
        GXSetNumChans(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetNumTexGens(1);
        GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, 0x7D);
        GXSetNumIndStages(0);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    }
#endif
};  // namespace

namespace nw4r {
    namespace ut {
        CharWriter::LoadingTexture CharWriter::mLoadingTexture;

        CharWriter::CharWriter() : mAlpha(Color::ALPHA_MAX), mIsWidthFixed(false), mFixedWidth(0), mFont(NULL) {
            mLoadingTexture.Reset();
            ResetColorMapping();
            SetGradationMode(GRADMODE_NONE);
            SetTextColor(Color::WHITE);
            SetScale(1, 1);
            SetCursor(0, 0, 0);
            EnableLinearFilter(true, true);
        }

        void CharWriter::LoadTexture(const Glyph& glyph, GXTexMapID slot) {
            // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not
            // have (DETAILS.md, section 5).
#ifdef __3DS__
            (void)glyph;
            (void)slot;
            return;
#else
            LoadingTexture loadInfo;

            loadInfo.slot = slot;
            loadInfo.texture = glyph.pTexture;
            loadInfo.filter = mFilter;

            if (loadInfo != mLoadingTexture) {
                GXTexObj tobj;

                GXInitTexObj(&tobj, const_cast< void* >(glyph.pTexture), glyph.texWidth, glyph.texHeight, glyph.texFormat, GX_CLAMP, GX_CLAMP,
                             GX_FALSE);
                GXInitTexObjLOD(&tobj, mFilter.atSmall, mFilter.atLarge, 0, 0, 0, GX_DISABLE, GX_DISABLE, GX_ANISO_1);
                GXLoadTexObj(&tobj, slot);

                mLoadingTexture = loadInfo;
            }
#endif
        }

        CharWriter::~CharWriter() {
        }

        void CharWriter::SetupVertexFormat() {
            // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not
            // have (DETAILS.md, section 5).
#ifdef __3DS__
            return;
#else
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_U16, TEXTURE_COODINATE_FRACTION_BITS);
            GXClearVtxDesc();
            GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
            GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
            GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
#endif
        }

        void CharWriter::SetupGX() {
#ifdef __3DS__
            ResetTextureCache();
            // SetupGXCommon's blend mode: without it the text took the previous pane's (the life
            // counter's digit added to the disc over the additive PicHiLight: white, no outline).
            u8 equation, source, destination;
            int logic;
            GXToCitro3D::blendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET, equation, source, destination, logic);
            TDDraw::setDirectBlendState(equation, source, destination);
            return;
#else
            ResetTextureCache();

            if ((mColorMapping.min != 0x00000000UL) || (mColorMapping.max != 0xFFFFFFFFUL)) {
                SetupGXWithColorMapping(mColorMapping.min, mColorMapping.max);
            } else {
                if (mFont) {
                    GXTexFmt format = mFont->GetTextureFormat();

                    switch (format) {
                    case GX_TF_I4:
                    case GX_TF_I8:
                        SetupGXForI();
                        break;

                    case GX_TF_IA4:
                    case GX_TF_IA8:
                        SetupGXDefault();
                        break;

                    case GX_TF_RGB565:
                    case GX_TF_RGB5A3:
                    case GX_TF_RGBA8:
                        SetupGXForRGBA();
                        break;

                    default:
                        SetupGXDefault();
                        break;
                    }
                } else {
                    SetupGXDefault();
                }
            }
#endif
        }

        void CharWriter::SetupGXDefault() {
            // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not
            // have (DETAILS.md, section 5).
#ifdef __3DS__
            return;
#else
            SetupGXCommon();
            GXSetNumTevStages(1);
            GXSetTevDirect(GX_TEVSTAGE0);
            GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
            GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
            GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
            SetupVertexFormat();
#endif
        }

        void CharWriter::SetupGXForI() {
            // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not
            // have (DETAILS.md, section 5).
#ifdef __3DS__
            return;
#else
            SetupGXCommon();
            GXSetNumTevStages(1);
            GXSetTevDirect(GX_TEVSTAGE0);
            GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
            GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
            GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
            GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
            GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            SetupVertexFormat();
#endif
        }

        void CharWriter::SetupGXForRGBA() {
            // 3DS: no GX state to set: the glyphs' Citro3D draw sets its own.
#ifdef __3DS__
            return;
#else
            SetupGXDefault();
#endif
        }

        void CharWriter::SetFontSize(f32 width, f32 height) {
            SetScale(width / mFont->GetWidth(), height / mFont->GetHeight());
        }

        f32 CharWriter::GetFontWidth() const {
            return mFont->GetWidth() * mScale.x;
        }

        f32 CharWriter::GetFontHeight() const {
            return mFont->GetHeight() * mScale.y;
        }

        f32 CharWriter::GetFontAscent() const {
            return mFont->GetAscent() * mScale.y;
        }

        void CharWriter::EnableLinearFilter(bool atSmall, bool atLarge) {
            mFilter.atSmall = atSmall ? GX_LINEAR : GX_NEAR;
            mFilter.atLarge = atLarge ? GX_LINEAR : GX_NEAR;
        }

        f32 CharWriter::Print(CharCode code) {
            Glyph glyph;
            f32 width;
            f32 left;

            mFont->GetGlyph(&glyph, code);

            CharWidths& widths = glyph.widths;

            if (mIsWidthFixed) {
                f32 margin = (mFixedWidth - widths.charWidth * mScale.x) / 2;

                width = mFixedWidth;
                left = margin + widths.left * mScale.x;
            } else {
                width = widths.charWidth * mScale.x;
                left = widths.left * mScale.x;
            }

            PrintGlyph(mCursorPos.x + left, mCursorPos.y, mCursorPos.z, glyph);

            mCursorPos.x += width;

            return width;
        }

        void CharWriter::PrintGlyph(f32 x, f32 y, f32 z, const Glyph& glyph) {
            const f32 posLeft = x;
            const f32 posTop = y;
            const f32 posRight = posLeft + glyph.widths.glyphWidth * mScale.x;
            const f32 posBottom = posTop + glyph.height * mScale.y;
            const f32 posZ = z;
            const u16 texLeft = static_cast< u16 >(glyph.cellX * TEXTURE_COODINATE_ONE / glyph.texWidth);
            const u16 texTop = static_cast< u16 >(glyph.cellY * TEXTURE_COODINATE_ONE / glyph.texHeight);
            const u16 texRight = static_cast< u16 >((glyph.cellX + glyph.widths.glyphWidth) * TEXTURE_COODINATE_ONE / glyph.texWidth);
            const u16 texBottom = static_cast< u16 >((glyph.cellY + glyph.height) * TEXTURE_COODINATE_ONE / glyph.texHeight);

#ifdef __3DS__
            // Citro3D has no GX immediate-mode quad path.  Keep the Wii setup above intact,
            // but submit the glyph through the same decoded-texture triangle path used by
            // other native 3DS overlays.
            const TVec3f positions[4] = {TVec3f(posLeft, posTop, posZ), TVec3f(posRight, posTop, posZ),
                                         TVec3f(posRight, posBottom, posZ), TVec3f(posLeft, posBottom, posZ)};
            const TVec2f texcoords[6] = {
                TVec2f(static_cast< f32 >(texLeft) / TEXTURE_COODINATE_ONE, static_cast< f32 >(texTop) / TEXTURE_COODINATE_ONE),
                TVec2f(static_cast< f32 >(texRight) / TEXTURE_COODINATE_ONE, static_cast< f32 >(texTop) / TEXTURE_COODINATE_ONE),
                TVec2f(static_cast< f32 >(texRight) / TEXTURE_COODINATE_ONE, static_cast< f32 >(texBottom) / TEXTURE_COODINATE_ONE),
                TVec2f(static_cast< f32 >(texLeft) / TEXTURE_COODINATE_ONE, static_cast< f32 >(texBottom) / TEXTURE_COODINATE_ONE),
                TVec2f(static_cast< f32 >(texLeft) / TEXTURE_COODINATE_ONE, static_cast< f32 >(texTop) / TEXTURE_COODINATE_ONE),
                TVec2f(static_cast< f32 >(texRight) / TEXTURE_COODINATE_ONE, static_cast< f32 >(texBottom) / TEXTURE_COODINATE_ONE)};
            const TVec3f triangles[6] = {positions[0], positions[1], positions[2], positions[0], positions[2], positions[3]};
            const u32 colors[6] = {mVertexColor.lu, mVertexColor.ru, mVertexColor.rd, mVertexColor.lu, mVertexColor.rd, mVertexColor.ld};
            const TVec2f triangleUvs[6] = {texcoords[0], texcoords[1], texcoords[2], texcoords[4], texcoords[5], texcoords[3]};
            // SetupGX's choice: the colour mapping, the intensity setup for I4/I8 fonts, else modulate.
            u32 mode = TDDraw::NATIVE_TEX_MODULATE;
            if (mColorMapping.min != 0x00000000UL || mColorMapping.max != 0xFFFFFFFFUL) {
                mode = TDDraw::NATIVE_TEX_COLOR_MAPPING;
            } else if (glyph.texFormat == GX_TF_I4 || glyph.texFormat == GX_TF_I8) {
                mode = TDDraw::NATIVE_TEX_INTENSITY;
            }
            if (sGlyphBatching) {
                const u8 filter = mFilter.atLarge == GX_LINEAR;
                if (sGlyphCount + 6 > kGlyphBatch || sGlyphImage != glyph.pTexture || sGlyphWidth != glyph.texWidth ||
                    sGlyphHeight != glyph.texHeight || sGlyphFormat != glyph.texFormat || sGlyphMode != mode || sGlyphFilter != filter ||
                    sGlyphMin != mColorMapping.min || sGlyphMax != mColorMapping.max) {
                    rvl3DSGlyphBatchFlush();
                }
                sGlyphImage = glyph.pTexture;
                sGlyphWidth = glyph.texWidth;
                sGlyphHeight = glyph.texHeight;
                sGlyphFormat = glyph.texFormat;
                sGlyphMode = mode;
                sGlyphFilter = filter;
                sGlyphMin = mColorMapping.min;
                sGlyphMax = mColorMapping.max;
                for (u32 i = 0; i < 6; i++) {
                    sGlyphPositions[sGlyphCount] = triangles[i];
                    sGlyphUvs[sGlyphCount] = triangleUvs[i];
                    sGlyphColors[sGlyphCount] = colors[i];
                    sGlyphCount++;
                }
                return;
            }
            TDDraw::drawNativeTexturedTriangles(triangles, triangleUvs, colors, glyph.pTexture, glyph.texWidth, glyph.texHeight,
                                                glyph.texFormat, nullptr, 0, 0, 0, 0, mFilter.atLarge == GX_LINEAR, 6, mode,
                                                mColorMapping.min, mColorMapping.max);
            return;
#else
            LoadTexture(glyph, GX_TEXMAP0);

            GXBegin(GX_QUADS, GX_VTXFMT0, 4);
            {
                GXPosition3f32(posLeft, posTop, posZ);
                GXColor1u32(mVertexColor.lu);
                GXTexCoord2u16(texLeft, texTop);

                GXPosition3f32(posRight, posTop, posZ);
                GXColor1u32(mVertexColor.ru);
                GXTexCoord2u16(texRight, texTop);

                GXPosition3f32(posRight, posBottom, posZ);
                GXColor1u32(mVertexColor.rd);
                GXTexCoord2u16(texRight, texBottom);

                GXPosition3f32(posLeft, posBottom, posZ);
                GXColor1u32(mVertexColor.ld);
                GXTexCoord2u16(texLeft, texBottom);
            }
            GXEnd();
#endif
        }

        void CharWriter::UpdateVertexColor() {
            mVertexColor.lu = mTextColor.start;
            mVertexColor.ru = (mTextColor.gradationMode != GRADMODE_H) ? mTextColor.start : mTextColor.end;
            mVertexColor.ld = (mTextColor.gradationMode != GRADMODE_V) ? mTextColor.start : mTextColor.end;
            mVertexColor.rd = (mTextColor.gradationMode == GRADMODE_NONE) ? mTextColor.start : mTextColor.end;

            mVertexColor.lu.a = static_cast< u8 >(mVertexColor.lu.a * mAlpha / Color::ALPHA_MAX);
            mVertexColor.ru.a = static_cast< u8 >(mVertexColor.ru.a * mAlpha / Color::ALPHA_MAX);
            mVertexColor.ld.a = static_cast< u8 >(mVertexColor.ld.a * mAlpha / Color::ALPHA_MAX);
            mVertexColor.rd.a = static_cast< u8 >(mVertexColor.rd.a * mAlpha / Color::ALPHA_MAX);
        }

        void CharWriter::SetupGXWithColorMapping(Color min, Color max) {
            // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not
            // have (DETAILS.md, section 5).
#ifdef __3DS__
            (void)min;
            (void)max;
            return;
#else
            SetupGXCommon();
            GXSetNumTevStages(2);
            GXSetTevDirect(GX_TEVSTAGE0);
            GXSetTevDirect(GX_TEVSTAGE1);
            GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
            GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
            GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
            GXSetTevColor(GX_TEVREG0, min);
            GXSetTevColor(GX_TEVREG1, max);
            GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_C1, GX_CC_TEXC, GX_CC_ZERO);
            GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_A0, GX_CA_A1, GX_CA_TEXA, GX_CA_ZERO);
            GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
            GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASC, GX_CC_ZERO);
            GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_APREV, GX_CA_RASA, GX_CA_ZERO);
            GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

            SetupVertexFormat();
#endif
        }
    };  // namespace ut
};  // namespace nw4r
