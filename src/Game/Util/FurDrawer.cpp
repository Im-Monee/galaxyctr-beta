#include "Game/Util/FurDrawer.hpp"
#include "Game/LiveActor/MaterialCtrl.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/FurParam.hpp"
#include "Game/Util/MathUtil.hpp"
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <cmath>
#ifdef __3DS__
#include "GXToCitro3D.hpp"
#include "JUTTexture3DS.hpp"
#include "NativeFur3DS.hpp"

RvlNativeFurMaterial& rvl3DSFurMaterial() {
    static RvlNativeFurMaterial sMaterial;
    return sMaterial;
}

namespace {
    // HostGfxStage constants are 0xAABBGGRR.
    u32 furConstant(u8 r, u8 g, u8 b, u8 a) {
        return r | (static_cast< u32 >(g) << 8) | (static_cast< u32 >(b) << 16) | (static_cast< u32 >(a) << 24);
    }

    // GXSetChanCtrl's channel control word (XF 0x100E layout, as the native renderer reads it).
    u32 channelControl(bool enable, u32 ambientSource, u32 materialSource, u32 lightMask, u32 diffuse, u32 attenuation) {
        if (attenuation == GX_AF_SPEC) {
            diffuse = GX_DF_NONE;
        }
        const u32 attnEnable = attenuation != GX_AF_NONE ? 1 : 0, attnSelect = attenuation != GX_AF_SPEC ? 1 : 0;
        return materialSource | (static_cast< u32 >(enable) << 1) | ((lightMask & 0xF) << 2) | (ambientSource << 6) | (diffuse << 7) |
               (attnEnable << 9) | (attnSelect << 10) | (((lightMask >> 4) & 0xF) << 11);
    }
}  // namespace
#endif

FurDrawer::FurDrawer(u32 numLayers, ResTIMG* pBody, ResTIMG* pIndirect)
    : mBodyTexture(), mFurTexture(), mIndirectTexture(), mNumLayers(numLayers), mLength(3.0f, 1.0f), mIndirect(0.2f, 1.5f), mBrightness(40.0f, 4.5f),
      mAlpha(210.0f, 1.8f), mFurUVScale(1.0f), mBodyUVScale(1.0f), mColorBlend(0.0f, 1.0f), mCullMode(GX_CULL_BACK), mMatIntensity(255),
      mAmbIntensity(50), mUseIndirect(1), mZCompLoc(), mZWrite(), mAdditive(), mAlphaRef(32) {
    mBodyTexture = new JUTTexture(pBody, 0);

    if (mIndirectTexture) {
        mIndirectTexture = new JUTTexture(pIndirect, 0);
        mUseIndirect = 1;
    } else {
        mUseIndirect = 0;
    }

#ifdef __3DS__
    u32 size = 32 * 32 * 2;  // GXGetTexBufferSize(32, 32, GX_TF_IA8, GX_FALSE, 1): 16-bit texels
#else
    u32 size = GXGetTexBufferSize(32, 32, GX_TF_IA8, GX_FALSE, 1);
#endif
    ResTIMG* pImage = reinterpret_cast< ResTIMG* >(new (32) u8[size + sizeof(ResTIMG)]);
    pImage->mFormat = GX_TF_IA8;
    pImage->mWidth = 32;
    pImage->mHeight = 32;
    pImage->mWrapS = GX_REPEAT;
    pImage->mWrapT = GX_REPEAT;
    pImage->mPaletteName = GX_TLUT0;
    pImage->mPaletteFormat = GX_TL_IA8;
    pImage->mPaletteNum = 0;
    pImage->mPaletteDataOffset = 0;
    pImage->mMipmap = false;
    pImage->mDoEdgeLod = false;
    pImage->mBiasClamp = false;
    pImage->mMaxAnisotropy = GX_ANISO_1;
    pImage->mMinType = GX_LINEAR;
    pImage->mMagType = GX_LINEAR;
    pImage->mMinLod = 0;
    pImage->mMaxLod = 0;
    pImage->mImageNum = 1;
    pImage->mLodBias = 0;
    pImage->mImageDataOffset = sizeof(ResTIMG);
    mFurTexture = new JUTTexture(pImage, 0);
    mUseDensityMap = 0;
    PSMTXIdentity(mFurTexMtx);
    PSMTXIdentity(mIndirectTexMtx);
    mMixFog = 0;
    mFogPosition.x = mFogPosition.y = mFogPosition.z = 0.0f;
    mFogWidth = 0.0f;
    mColor.r = 0;
    mColor.g = 0;
    mColor.b = 0;
    mColor.a = 0;
    mDensity[0] = 0.8f;
    mThickness[0] = 0.517f;
    mLengthMap[0] = 26;
    mDensity[1] = 0.6f;
    mThickness[1] = 0.386f;
    mLengthMap[1] = 64;
    mDensity[2] = 0.3f;
    mThickness[2] = 0.3645f;
    mLengthMap[2] = 98;
    mDensity[3] = 0.09f;
    mThickness[3] = 0.1713f;
    mLengthMap[3] = 212;
    createFurMap();
    update();
}

void FurDrawer::update() {
    PSMTXScale(mFurTexMtx, mFurUVScale, mFurUVScale, 1.0f);
}

f32 FurDrawer::CLayerParam::calcValue(s32 layer, s32 numLayers) const {
    f32 t = pow(f32(layer + 1) / numLayers, mCurve);
    return mRoot + t * (mTip - mRoot);
}

void FurDrawer::setupMaterial(DynamicFurParam* pParam) const {
#ifdef __3DS__
    // The Wii's fur material (below) for the native fur layers (NativeFur3DS.hpp). Stage 0: body
    // texture x lighting + konst R, alpha konst G; stage 1: fur texture between C2 and the body
    // (x2), alpha previous x fur alpha + konst B. The Wii's other two inputs are never on: the
    // indirect offset of stage 1 needs mIndirectTexture, which the constructor tests while still
    // null, and the specular channel a light 1 switch, 0 in every .fur.txt of the game (and in
    // FurLightParam's default).
    FurLightParam* pLight = pParam->mLightParam;
    RvlNativeFurMaterial& m = rvl3DSFurMaterial();
    m = RvlNativeFurMaterial();
    HostGfxState& st = m.state;
    bool cullAll;
    st.cull = GXToCitro3D::cullMode(mCullMode, cullAll);
    u8 equation, source, destination;
    int logic;
    GXToCitro3D::blendMode(GX_BM_BLEND, GX_BL_SRCALPHA, mAdditive ? GX_BL_ONE : GX_BL_INVSRCALPHA, GX_LO_CLEAR, equation, source,
                           destination, logic);
    st.blendEquation = equation;
    st.blendSrc = source;
    st.blendDst = destination;
    st.depthTest = 1;
    st.depthFunc = GXToCitro3D::depthTest(GX_LEQUAL);
    st.writeMask = 0x07 | (mZWrite ? 0x10 : 0);  // colour; alpha not updated
    st.alphaTestFunc = GXToCitro3D::alphaTest(GX_GREATER);
    st.alphaRef = mAlphaRef;
    st.scissor[2] = 640;
    st.scissor[3] = 456;
    st.textures[0] = rvl3DSGetJUTTexture(mBodyTexture);
    st.textures[1] = rvl3DSGetJUTTexture(mFurTexture);
    st.stageCount = 2;
    enum { PRIMARY = 0x00, TEXTURE0 = 0x03, TEXTURE1 = 0x04, CONSTANT = 0x0E, PREVIOUS = 0x0F };
    HostGfxStage& s0 = st.stages[0];
    s0.colorSrc[0] = TEXTURE0;
    s0.colorSrc[1] = PRIMARY;
    s0.colorSrc[2] = CONSTANT;
    s0.colorFunc = 8;  // multiply-add
    s0.alphaSrc[0] = CONSTANT;
    s0.alphaFunc = 0;
    HostGfxStage& s1 = st.stages[1];
    s1.colorSrc[0] = PREVIOUS;
    s1.colorSrc[1] = CONSTANT;
    s1.colorSrc[2] = TEXTURE1;
    s1.colorFunc = 4;   // interpolate
    s1.colorScale = 1;  // x2
    s1.alphaSrc[0] = PREVIOUS;
    s1.alphaSrc[1] = TEXTURE1;
    s1.alphaSrc[2] = CONSTANT;
    s1.alphaFunc = 8;
    m.fog = pParam->mFogCtrl != nullptr ? &pParam->mFogCtrl->mFogInfo : nullptr;

    PSMTXScale(m.texMtx[0], mBodyUVScale, mBodyUVScale, 1.0f);
    PSMTXCopy(mFurTexMtx, m.texMtx[1]);

    u8 matIntensity = mMatIntensity, ambIntensity = mAmbIntensity;
    if (pLight != nullptr) {
        m.colorCtrl = channelControl(true, pLight->mColorSource & 1, (pLight->mColorSource >> 1) & 1, pLight->mLightMask, GX_DF_CLAMP,
                                     GX_AF_NONE);
        matIntensity = pLight->mMatIntensity;
        ambIntensity = pLight->mAmbIntensity;
    } else {
        m.colorCtrl = channelControl(true, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    }
    m.alphaCtrl = channelControl(false, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    m.matColor = (static_cast< u32 >(matIntensity) << 24) | (static_cast< u32 >(matIntensity) << 16) | (static_cast< u32 >(matIntensity) << 8) | 0xFF;
    m.ambColor = (static_cast< u32 >(ambIntensity) << 24) | (static_cast< u32 >(ambIntensity) << 16) | (static_cast< u32 >(ambIntensity) << 8) | 0xFF;
#else
    bool useSpecular = false;
    FurLightParam* pLight = pParam->mLightParam;

    if (pLight && pLight->mSpecularLightMask) {
        useSpecular = true;
    }

    GXSetCoPlanar(GX_FALSE);
    GXSetCullMode(static_cast< GXCullMode >(mCullMode));

    if (mAdditive) {
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
    } else {
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    }

    GXSetZMode(GX_TRUE, GX_LEQUAL, mZWrite);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetAlphaCompare(GX_GREATER, mAlphaRef, GX_AOP_OR, GX_GREATER, mAlphaRef);
    GXSetZCompLoc(mZCompLoc);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetNumTevStages(2);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K3_R);
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K3_G);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_KONST);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K3);
    GXSetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_K3_B);

    if (useSpecular) {
        GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR1A1);
        GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_C2, GX_CC_CPREV, GX_CC_TEXC, GX_CC_RASC);
    } else {
        GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
        GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_C2, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
    }

    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    if (mUseIndirect) {
        GXSetNumIndStages(1);
        GXSetTevIndirect(GX_TEVSTAGE1, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU, GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
    } else {
        GXSetNumIndStages(0);
        GXSetTevDirect(GX_TEVSTAGE1);
    }

    if (pParam->mFogCtrl) {
        const J3DFogInfo& fog = pParam->mFogCtrl->mFogInfo;
        GXSetFog(static_cast< GXFogType >(fog.mType), fog.mStartZ, fog.mEndZ, fog.mNearZ, fog.mFarZ, fog.mColor);
    } else if (mMixFog) {
        TDDraw::mixFogColor(mFogPosition, mFogWidth, 0xFF000080);
    } else {
        GXColor color = {0, 0, 0, 0};
        GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, color);
        GXSetFogRangeAdj(GX_FALSE, 0, nullptr);
    }

    Mtx bodyMtx;
    Mtx furMtx;
    Mtx indirectMtx;
    PSMTXCopy(mFurTexMtx, furMtx);

    if (mUseIndirect) {
        PSMTXCopy(mIndirectTexMtx, indirectMtx);
    } else {
        PSMTXIdentity(indirectMtx);
    }

    PSMTXScale(bodyMtx, mBodyUVScale, mBodyUVScale, 1.0f);
    GXLoadTexMtxImm(bodyMtx, GX_TEXMTX0, GX_MTX2x4);
    GXLoadTexMtxImm(furMtx, GX_TEXMTX1, GX_MTX2x4);

    if (mUseIndirect) {
        GXLoadTexMtxImm(indirectMtx, GX_TEXMTX2, GX_MTX2x4);
        GXSetNumTexGens(3);
        GXSetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD2, GX_TEXMAP2);
        GXSetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);
        GXSetTexCoordGen2(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX2, GX_FALSE, GX_PTIDENTITY);
    } else {
        GXSetNumTexGens(2);
    }

    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
    GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX1, GX_FALSE, GX_PTIDENTITY);
    GXSetClipMode(GX_CLIP_DISABLE);

    if (pLight) {
        u8 numChans = 1;
        GXSetChanCtrl(GX_ALPHA0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanCtrl(GX_COLOR0, GX_TRUE, static_cast< GXColorSrc >(pLight->mColorSource & 1),
                      static_cast< GXColorSrc >((pLight->mColorSource >> 1) & 1), pLight->mLightMask, GX_DF_CLAMP, GX_AF_NONE);

        if (useSpecular) {
            GXSetChanCtrl(GX_COLOR1, GX_TRUE, static_cast< GXColorSrc >((pLight->mColorSource >> 2) & 1),
                          static_cast< GXColorSrc >((pLight->mColorSource >> 3) & 1), pLight->mSpecularLightMask, GX_DF_NONE, GX_AF_SPEC);
            GXSetChanCtrl(GX_ALPHA1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
            numChans = 2;
        } else {
            GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        }

        GXColor matColor = {0, 0, 0, 255};
        matColor.r = pLight->mMatIntensity;
        matColor.g = pLight->mMatIntensity;
        matColor.b = pLight->mMatIntensity;
        GXColor ambColor = {0, 0, 0, 255};
        ambColor.r = pLight->mAmbIntensity;
        ambColor.g = pLight->mAmbIntensity;
        ambColor.b = pLight->mAmbIntensity;
        GXSetChanMatColor(GX_COLOR0A0, matColor);
        GXSetChanAmbColor(GX_COLOR0A0, ambColor);
        GXColor specularMatColor = {0, 0, 0, 255};
        specularMatColor.r = pLight->mSpecularMatIntensity;
        specularMatColor.g = pLight->mSpecularMatIntensity;
        specularMatColor.b = pLight->mSpecularMatIntensity;
        GXColor specularAmbColor = {0, 0, 0, 255};
        specularAmbColor.r = pLight->mSpecularAmbIntensity;
        specularAmbColor.g = pLight->mSpecularAmbIntensity;
        specularAmbColor.b = pLight->mSpecularAmbIntensity;
        GXSetChanMatColor(GX_COLOR1A1, specularMatColor);
        GXSetChanAmbColor(GX_COLOR1A1, specularAmbColor);
        GXSetNumChans(numChans);
    } else {
        GXSetChanCtrl(GX_COLOR0, GX_TRUE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
        GXSetChanCtrl(GX_ALPHA0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXColor matColor = {0, 0, 0, 255};
        matColor.r = mMatIntensity;
        matColor.g = mMatIntensity;
        matColor.b = mMatIntensity;
        GXColor ambColor = {0, 0, 0, 255};
        ambColor.r = mAmbIntensity;
        ambColor.g = mAmbIntensity;
        ambColor.b = mAmbIntensity;
        GXSetChanMatColor(GX_COLOR0A0, matColor);
        GXSetChanAmbColor(GX_COLOR0A0, ambColor);
        GXSetNumChans(1);
    }

    mBodyTexture->load(GX_TEXMAP0);
    mFurTexture->load(GX_TEXMAP1);

    if (mUseIndirect && mIndirectTexture) {
        mIndirectTexture->load(GX_TEXMAP2);
    }
#endif
}

void FurDrawer::setupLayerMaterial(s32 layer) const {
#ifdef __3DS__
    // The layer's konst (stage 0: R, G; stage 1: B) and C2 (stage 1), as GXSetTevKColor and GXSetTevColor.
    const u8 alpha = mBrightness.calcValue(layer, mNumLayers);
    const u8 brightness = mAlpha.calcValue(mNumLayers - layer - 1, mNumLayers);
    const u8 blend = mColorBlend.calcValue(mNumLayers - layer - 1, mNumLayers);
    HostGfxState& st = rvl3DSFurMaterial().state;
    st.stages[0].constant = furConstant(alpha, alpha, alpha, brightness);
    st.stages[1].constant = furConstant(mColor.r, mColor.g, mColor.b, blend);
#else
    f32 alpha = mBrightness.calcValue(layer, mNumLayers);
    f32 brightness = mAlpha.calcValue(mNumLayers - layer - 1, mNumLayers);
    f32 blend = mColorBlend.calcValue(mNumLayers - layer - 1, mNumLayers);
    GXColor kColor;
    kColor.r = alpha;
    kColor.g = brightness;
    kColor.b = blend;
    kColor.a = 255;
    GXSetTevKColor(GX_KCOLOR3, kColor);
    GXColor color;
    color.r = mColor.r;
    color.g = mColor.g;
    color.b = mColor.b;
    color.a = 255;
    GXSetTevColor(GX_TEVREG2, color);

    if (mUseIndirect) {
        f32 scale = mIndirect.calcValue(layer, mNumLayers);
        f32 mtx[2][3];
        mtx[0][0] = scale;
        mtx[0][1] = 0.0f;
        mtx[0][2] = 0.0f;
        mtx[1][0] = 0.0f;
        mtx[1][1] = scale;
        mtx[1][2] = 0.0f;
        GXSetIndTexMtx(GX_ITM_0, mtx, 0);
    }
#endif
}

void FurDrawer::createFurMap() {
    u32 count;
    u16* pPixels;
    u32 width = mFurTexture->getTexInfo()->mWidth;
    u32 height = mFurTexture->getTexInfo()->mHeight;
    pPixels = reinterpret_cast< u16* >(mFurTexture->mImage);
    count = height * width;

    for (u32 i = 0; i < count; i++) {
        pPixels[i] = 255;
    }

    for (u32 type = 0; type < 4; type++) {
        for (u32 i = 0; i < f32(height) * f32(width) * mDensity[type]; i++) {
            f32 randY = MR::getRandom(0.0f, 1.0f);
            f32 randX = MR::getRandom(0.0f, 1.0f);
            s32 y = f32(height) * randY;
            s32 x = f32(width) * randX;

            if (y == s32(height)) {
                y = s32(height) - 1;
            }

            if (x == s32(width)) {
                x = s32(width) - 1;
            }

            pPixels[x + width * y] = (u16(255.0f * mThickness[type]) << 8) | u16(255 - mLengthMap[type]);
        }
    }

#ifdef __3DS__
    // GX IA8 texels are alpha then intensity, as the Wii's big-endian u16 stores them; the 3DS
    // stores the low byte first, which would make the intensity the alpha (every texel opaque).
    for (u32 i = 0; i < count; i++) {
        pPixels[i] = static_cast< u16 >((pPixels[i] >> 8) | (pPixels[i] << 8));
    }
#endif
    DCStoreRange(pPixels, count * sizeof(u16));
#ifdef __3DS__
    rvl3DSInvalidateJUTTexture(mFurTexture);  // the Citro3D copy is made again from the new texels
#endif
}
