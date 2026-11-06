#include "nw4r/lyt/animation.h"
#include "nw4r/lyt/layout.h"
#include "nw4r/lyt/material.h"
#include "nw4r/lyt/resourceAccessor.h"
#include "nw4r/lyt/texMap.h"
#include "nw4r/lyt/types.h"
#include "nw4r/math/triangular.h"
#include "nw4r/math/types.h"
#include "nw4r/ut/inlines.h"
#include <cstdio>
// 3DS: DirectDraw, TDDraw's Citro3D draws; GXToCitro3D.hpp, GX values as 3DS GPU values;
// JUTTexture3DS.hpp, GPU textures for JUTTextures; TevCompile3DS.hpp, TEV settings compiled into
// combiner stages.
#ifdef __3DS__
#include "Game/Util/DirectDraw.hpp"
#include "GXToCitro3D.hpp"
#include "JUTTexture3DS.hpp"
#include "TevCompile3DS.hpp"
#endif

const GXColorS10 DefaultBlackColor = {0, 0, 0, 0};

namespace nw4r {
    namespace lyt {
        namespace {
            bool IsWhiteColor(const GXColorS10& color) {
                return color.r == 255 && color.g == 255 && color.b == 255 && color.a == 255;
            }

            bool operator==(const GXColorS10& rLhs, const GXColorS10& rRhs) {
                return rLhs.r == rRhs.r && rLhs.g == rRhs.g && rLhs.b == rRhs.b && rLhs.a == rRhs.a;
            }

            bool operator!=(const GXColorS10& rLhs, const GXColorS10& rRhs) {
                return !(rLhs == rRhs);
            }

            void CalcTextureMtx(nw4r::math::MTX34* pMtx, const nw4r::lyt::TexSRT& rSrt) {
                nw4r::math::VEC2 center(0.5f, 0.5f);

                f32 cr = nw4r::math::CosDeg(rSrt.rotate);
                f32 sr = nw4r::math::SinDeg(rSrt.rotate);

                f32 a0, a1;

                a0 = cr * rSrt.scale.x;
                a1 = -sr * rSrt.scale.y;

                pMtx->_00 = a0;
                pMtx->_01 = a1;
                pMtx->_02 = 0.0f;
                pMtx->_03 = rSrt.translate.x + center.x + a0 * -center.x + a1 * -center.y;

                a0 = sr * rSrt.scale.x;
                a1 = cr * rSrt.scale.y;

                pMtx->_10 = a0;
                pMtx->_11 = a1;
                pMtx->_12 = 0.0f;
                pMtx->_13 = rSrt.translate.y + center.y + a0 * -center.x + a1 * -center.y;

                pMtx->_20 = 0.0f;
                pMtx->_21 = 0.0f;
                pMtx->_22 = 1.0f;
                pMtx->_23 = 0.0f;
            }

            void SetIndTexMtx(GXIndTexMtxID id, const f32 mtx[2][3]) {
#ifndef __3DS__  // 3DS: only the Wii SetupGX uses it (no indirect textures)
                f32 m00, m01, m02, m10, m11, m12;
                f32 a00, a01, a02, a10, a11, a12;

                s8 scaleExp = 0;

                m00 = mtx[0][0];
                m01 = mtx[0][1];
                m02 = mtx[0][2];
                m10 = mtx[1][0];
                m11 = mtx[1][1];
                m12 = mtx[1][2];

                a00 = nw4r::math::FAbs(m00);
                a01 = nw4r::math::FAbs(m01);
                a02 = nw4r::math::FAbs(m02);
                a10 = nw4r::math::FAbs(m10);
                a11 = nw4r::math::FAbs(m11);
                a12 = nw4r::math::FAbs(m12);

                if (a00 >= 1.0f || a01 >= 1.0f || a02 >= 1.0f || a10 >= 1.0f || a11 >= 1.0f || a12 >= 1.0f) {
                    do {
                        if (scaleExp >= 46) {
                            break;
                        }

                        scaleExp++;

                        m00 /= 2.0f;
                        m01 /= 2.0f;
                        m02 /= 2.0f;
                        m10 /= 2.0f;
                        m11 /= 2.0f;
                        m12 /= 2.0f;

                        a00 /= 2.0f;
                        a01 /= 2.0f;
                        a02 /= 2.0f;
                        a10 /= 2.0f;
                        a11 /= 2.0f;
                        a12 /= 2.0f;
                    } while (a00 >= 1.0f || a01 >= 1.0f || a02 >= 1.0f || a10 >= 1.0f || a11 >= 1.0f || a12 >= 1.0f);

                } else if (a00 < 0.5f && a01 < 0.5f && a02 < 0.5f && a10 < 0.5f && a11 < 0.5f && a12 < 0.5f) {
                    do {
                        scaleExp--;

                        m00 *= 2.0f;
                        m01 *= 2.0f;
                        m02 *= 2.0f;
                        m10 *= 2.0f;
                        m11 *= 2.0f;
                        m12 *= 2.0f;

                        a00 *= 2.0f;
                        a01 *= 2.0f;
                        a02 *= 2.0f;
                        a10 *= 2.0f;
                        a11 *= 2.0f;
                        a12 *= 2.0f;

                    } while (a00 < 0.5f && a01 < 0.5f && a02 < 0.5f && a10 < 0.5f && a11 < 0.5f && a12 < 0.5f && scaleExp > -17);
                }

                f32 outMtx[2][3] = {m00, m01, m02, m10, m11, m12};
                GXSetIndTexMtx(id, outMtx, scaleExp);
#endif
            }

            void CalcIndTexMtx(f32 mtx[2][3], const nw4r::lyt::TexSRT& rSrt) {
                f32 cr = nw4r::math::CosDeg(rSrt.rotate);
                f32 sr = nw4r::math::SinDeg(rSrt.rotate);

                mtx[0][0] = cr * rSrt.scale.x;
                mtx[0][1] = -sr * rSrt.scale.y;
                mtx[0][2] = rSrt.translate.x;

                mtx[1][0] = sr * rSrt.scale.x;
                mtx[1][1] = cr * rSrt.scale.y;
                mtx[1][2] = rSrt.translate.y;
            }

            inline u32 GetTexMtxIdx(u32 mtx) {
                return (mtx - GX_TEXMTX0) / 3;
            }

            inline u32 GetTexMtx(u32 idx) {
                return GX_TEXMTX0 + idx * 3;
            }

            void SetColorComponentValue(nw4r::ut::Color* pColor, u32 idx, s16 value) {
                u8 bvalue = nw4r::ut::Min< s16 >(nw4r::ut::Max< s16 >(value, 0), 255);

                switch (idx % 4) {
                case 0: {
                    pColor->r = bvalue;
                    break;
                }

                case 1: {
                    pColor->g = bvalue;
                    break;
                }

                case 2: {
                    pColor->b = bvalue;
                    break;
                }

                case 3: {
                    pColor->a = bvalue;
                    break;
                }

                default: {
                    break;
                }
                }
            }

            u32 CalcOffsetTexSRTAry(const nw4r::lyt::detail::BitGXNums& rNums) {
                return rNums.texMap * ROUND_UP(sizeof(nw4r::lyt::TexMap), 4);
            }

            u32 CalcOffsetTexCoordGenAry(const nw4r::lyt::detail::BitGXNums& rNums) {
                return rNums.texSRT * ROUND_UP(sizeof(nw4r::lyt::TexSRT), 4) + CalcOffsetTexSRTAry(rNums);
            }

            u32 CalcOffsetChanCtrlAry(const nw4r::lyt::detail::BitGXNums& rNums) {
                return rNums.texCoordGen * ROUND_UP(sizeof(nw4r::lyt::TexCoordGen), 4) + CalcOffsetTexCoordGenAry(rNums);
            }

            u32 CalcOffsetMatColAry(const nw4r::lyt::detail::BitGXNums& rNums) {
                return rNums.chanCtrl * ROUND_UP(sizeof(nw4r::lyt::ChanCtrl), 4) + CalcOffsetChanCtrlAry(rNums);
            }

            u32 CalcOffsetTevSwapAry(const nw4r::lyt::detail::BitGXNums& rNums) {
                return rNums.matCol * ROUND_UP(sizeof(nw4r::ut::Color), 4) + CalcOffsetMatColAry(rNums);
            }

            u32 CalcOffsetGetAlphaCompare(const nw4r::lyt::detail::BitGXNums& rNums) {
                return rNums.tevSwap * ROUND_UP(sizeof(nw4r::lyt::TevSwapMode), 4) + CalcOffsetTevSwapAry(rNums);
            }

            u32 CalcOffsetBlendMode(const nw4r::lyt::detail::BitGXNums& rNums) {
                return rNums.alpComp * ROUND_UP(sizeof(nw4r::lyt::AlphaCompare), 4) + CalcOffsetGetAlphaCompare(rNums);
            }

            u32 CalcOffsetIndirectStageAry(const nw4r::lyt::detail::BitGXNums& rNums) {
                return rNums.blendMode * ROUND_UP(sizeof(nw4r::lyt::BlendMode), 4) + CalcOffsetBlendMode(rNums);
            }

            u32 CalcOffsetIndTexSRTAry(const nw4r::lyt::detail::BitGXNums& rNums) {
                return rNums.indStage * ROUND_UP(sizeof(nw4r::lyt::IndirectStage), 4) + CalcOffsetIndirectStageAry(rNums);
            }

            u32 CalcOffsetTevStageAry(const nw4r::lyt::detail::BitGXNums& rNums) {
                return rNums.indSRT * ROUND_UP(sizeof(nw4r::lyt::TexSRT), 4) + CalcOffsetIndTexSRTAry(rNums);
            }

            void InitTexSRT(nw4r::lyt::TexSRT* texSRTs, u32 num) {
                for (u32 i = 0; i < num; ++i) {
                    texSRTs[i].translate = nw4r::math::VEC2(0.f, 0.f);
                    texSRTs[i].rotate = 0.f;
                    texSRTs[i].scale = nw4r::math::VEC2(1.f, 1.f);
                }
            }

        };  // namespace

        Material::Material() {
            Init();
            memset(mName, 0, sizeof(mName));
        }

#pragma dont_inline on

        // inlining is off the charts here. No idea why the compiler will not inline so much of these calls
        Material::Material(const res::Material* pRes, const ResBlockSet& resBlockSet) {
            Init();

            SetName(pRes->name);

            for (int i = 0; i < 3; ++i) {
                mTevCols[i] = pRes->tevCols[i];
            }

            for (int i = 0; i < 4; ++i) {
                mTevKCols[i] = pRes->tevKCols[i];
            }

            u32 resOffs = sizeof(res::Material);
            const res::TexMap* const pResTexMap = detail::ConvertOffsToPtr< res::TexMap >(pRes, resOffs);
            resOffs += sizeof(res::TexMap) * pRes->resNum.GetTexMapNum();
            const TexSRT* const resTexSRTs = detail::ConvertOffsToPtr< TexSRT >(pRes, resOffs);
            resOffs += sizeof(TexSRT) * pRes->resNum.GetTexSRTNum();
            const TexCoordGen* const resTexCoordGens = detail::ConvertOffsToPtr< TexCoordGen >(pRes, resOffs);
            resOffs += sizeof(TexCoordGen) * pRes->resNum.GetTexCoordGenNum();

            const u8 texMapNum = ut::Min(pRes->resNum.GetTexMapNum(), u8(GX_MAX_TEXMAP));
            const u8 texSRTNum = ut::Min(pRes->resNum.GetTexSRTNum(), u8(10));
            const u8 texCoordGenNum = ut::Min(pRes->resNum.GetTexCoordGenNum(), u8(GX_MAX_TEXCOORD));
            const bool allocChanCtrl = 0 != pRes->resNum.GetChanCtrlNum();
            const bool allocMatCol = 0 != pRes->resNum.GetMatColNum();
            const bool allocTevSwap = pRes->resNum.HasTevSwapTable();
            const bool allocAlpComp = pRes->resNum.HasAlphaCompare();
            const bool allocBlendMode = pRes->resNum.HasBlendMode();
            u8 indTexSRTNum = ut::Min(pRes->resNum.GetIndTexSRTNum(), u8(3));
            u8 indStageNum = ut::Min(pRes->resNum.GetIndTexStageNum(), u8(GX_MAX_INDTEXSTAGE));
            u8 tevStageNum = ut::Min(pRes->resNum.GetTevStageNum(), u8(GX_MAX_TEVSTAGE));

            ReserveGXMem(texMapNum, texSRTNum, texCoordGenNum, tevStageNum, allocTevSwap, indStageNum, indTexSRTNum, allocChanCtrl, allocMatCol,
                         allocAlpComp, allocBlendMode);

            if (mpGXMem) {
                SetTextureNum(texMapNum);
                if (texMapNum > 0) {
                    const res::Texture* const textures =
                        detail::ConvertOffsToPtr< res::Texture >(resBlockSet.pTextureList, sizeof(*resBlockSet.pTextureList));
                    TexMap* const texMaps = GetTexMapAry();

                    u8 di = 0;
                    for (u8 si = 0; si < texMapNum; ++si) {
                        const char* const fileName = detail::ConvertOffsToPtr< char >(textures, textures[pResTexMap[si].texIdx].nameStrOffset);

                        void* const pTplRes = resBlockSet.pResAccessor->GetResource(0x74696D67, fileName, nullptr);
                        texMaps[di].ReplaceImage(static_cast< TPLPalettePtr >(pTplRes), 0);
                        texMaps[di].SetWrapMode(pResTexMap[si].GetWarpModeS(), pResTexMap[si].GetWarpModeT());
                        texMaps[di].SetFilter(pResTexMap[si].GetMinFilter(), pResTexMap[si].GetMagFilter());
                        ++di;
                    }
                    SetTextureNum(di);
                }

                TexSRT* texSRTs = GetTexSRTAry();
                for (int i = 0; i < texSRTNum; ++i) {
                    texSRTs[i].translate = resTexSRTs[i].translate;
                    texSRTs[i].rotate = resTexSRTs[i].rotate;
                    texSRTs[i].scale = resTexSRTs[i].scale;
                }

                TexCoordGen* texCoordGens = GetTexCoordGenAry();
                SetTexCoordGenNum(texCoordGenNum);
                for (int i = 0; i < mGXMemNum.texCoordGen; ++i) {
                    texCoordGens[i] = resTexCoordGens[i];
                }

                if (allocChanCtrl) {
                    const ChanCtrl* const pResChanCtrl = detail::ConvertOffsToPtr< ChanCtrl >(pRes, resOffs);
                    GetChanCtrlAry()[0] = *pResChanCtrl;
                    resOffs += sizeof(*pResChanCtrl);
                }

                if (allocMatCol) {
                    const ut::Color* const pResMatCol = detail::ConvertOffsToPtr< ut::Color >(pRes, resOffs);
                    GetMatColAry()[0] = *pResMatCol;
                    resOffs += sizeof(*pResMatCol);
                }

                if (allocTevSwap) {
                    const TevSwapMode* const pResTevSwap = detail::ConvertOffsToPtr< TevSwapMode >(pRes, resOffs);
                    TevSwapMode* const tevSwaps = GetTevSwapAry();
                    for (int i = 0; i < GX_MAX_TEVSWAP; ++i) {
                        tevSwaps[i] = pResTevSwap[i];
                    }
                    resOffs += sizeof(*tevSwaps) * GX_MAX_TEVSWAP;
                }

                if (indTexSRTNum > 0) {
                    TexSRT* const indTexSRTs = GetIndTexSRTAry();
                    const TexSRT* const pResIndMtx = detail::ConvertOffsToPtr< TexSRT >(pRes, resOffs);
                    for (int i = 0; i < indTexSRTNum; ++i) {
                        indTexSRTs[i] = pResIndMtx[i];
                    }
                }
                resOffs += sizeof(TexSRT) * pRes->resNum.GetIndTexSRTNum();

                if (indStageNum > 0) {
                    SetIndStageNum(indStageNum);
                    IndirectStage* indirectStages = GetIndirectStageAry();
                    const IndirectStage* const pResIndStg = detail::ConvertOffsToPtr< IndirectStage >(pRes, resOffs);
                    for (int i = 0; i < indStageNum; ++i) {
                        indirectStages[i] = pResIndStg[i];
                    }
                }
                resOffs += sizeof(IndirectStage) * pRes->resNum.GetIndTexStageNum();

                if (tevStageNum > 0) {
                    SetTevStageNum(tevStageNum);
                    TevStage* const tevStages = GetTevStageAry();
                    const TevStage* const pResTevStg = detail::ConvertOffsToPtr< TevStage >(pRes, resOffs);
                    for (int i = 0; i < tevStageNum; ++i) {
                        tevStages[i] = pResTevStg[i];
                    }
                }
                resOffs += sizeof(TevStage) * pRes->resNum.GetTevStageNum();

                if (allocAlpComp) {
                    const AlphaCompare* const pResAlphaCompare = detail::ConvertOffsToPtr< AlphaCompare >(pRes, resOffs);
                    *GetAlphaComparePtr() = *pResAlphaCompare;
                    resOffs += sizeof(*pResAlphaCompare);
                }

                if (allocBlendMode) {
                    const BlendMode* const pResBlendMode = detail::ConvertOffsToPtr< BlendMode >(pRes, resOffs);
                    *GetBlendModePtr() = *pResBlendMode;
                    resOffs += sizeof(*pResBlendMode);
                }
            }
        }

#pragma dont_inline reset

        void Material::Init() {
            mTevCols[0] = DefaultBlackColor;
            mTevCols[1].a = mTevCols[1].b = mTevCols[1].g = mTevCols[1].r = 255;
            mTevCols[2].a = mTevCols[2].b = mTevCols[2].g = mTevCols[2].r = 255;

            InitBitGXNums(&mGXMemCap);
            InitBitGXNums(&mGXMemNum);

            mbUserAllocated = false;

            mpGXMem = 0;
        }

        Material::~Material() {
            UnbindAllAnimation();

            if (mpGXMem != NULL) {
                Layout::FreeMemory(mpGXMem);
                mpGXMem = NULL;
            }
        }

        void Material::InitBitGXNums(detail::BitGXNums* ptr) {
            ptr->texMap = 0;
            ptr->texSRT = 0;
            ptr->texCoordGen = 0;
            ptr->indSRT = 0;
            ptr->indStage = 0;
            ptr->tevSwap = 0;
            ptr->tevStage = 0;
            ptr->chanCtrl = 0;
            ptr->matCol = 0;
            ptr->alpComp = 0;
            ptr->blendMode = 0;
        }

        void Material::ReserveGXMem(u8 texMapNum, u8 texSRTNum, u8 texCoordGenNum, u8 tevStageNum, bool allocTevSwap, u8 indStageNum, u8 indSRTNum,
                                    bool allocChanCtrl, bool allocMatCol, bool allocAlpComp, bool allocBlendMode) {
            const int tevSwapNum = allocTevSwap ? 1 : 0;
            const int chanCtrlNum = allocChanCtrl ? 1 : 0;
            const int matColNum = allocMatCol ? 1 : 0;
            const int alpCompNum = allocAlpComp ? 1 : 0;
            const int blendModeNum = allocBlendMode ? 1 : 0;

            if (mGXMemCap.texMap < texMapNum || mGXMemCap.texSRT < texSRTNum || mGXMemCap.texCoordGen < texCoordGenNum ||
                mGXMemCap.tevStage < tevStageNum || mGXMemCap.tevSwap < tevSwapNum || mGXMemCap.indStage < indStageNum ||
                mGXMemCap.indSRT < indSRTNum || mGXMemCap.chanCtrl < chanCtrlNum || mGXMemCap.matCol < matColNum || mGXMemCap.alpComp < alpCompNum ||
                mGXMemCap.blendMode < blendModeNum) {
                if (mpGXMem) {
                    Layout::FreeMemory(mpGXMem);
                    mpGXMem = 0;

                    InitBitGXNums(&mGXMemCap);
                    InitBitGXNums(&mGXMemNum);
                }

                mpGXMem = Layout::AllocMemory(sizeof(TexMap) * texMapNum + sizeof(TexSRT) * texSRTNum +
                                              4 * (texCoordGenNum + chanCtrlNum + matColNum + tevSwapNum + alpCompNum + blendModeNum + indStageNum) +
                                              sizeof(TexSRT) * indSRTNum + sizeof(TevStage) * tevStageNum);
                if (mpGXMem) {
                    mGXMemCap.texMap = texMapNum;
                    mGXMemCap.texSRT = texSRTNum;
                    mGXMemCap.texCoordGen = texCoordGenNum;
                    mGXMemCap.indSRT = indSRTNum;
                    mGXMemCap.indStage = indStageNum;
                    mGXMemCap.tevSwap = tevSwapNum;
                    mGXMemCap.tevStage = tevStageNum;
                    mGXMemCap.chanCtrl = chanCtrlNum;
                    mGXMemCap.matCol = matColNum;
                    mGXMemCap.alpComp = alpCompNum;
                    mGXMemCap.blendMode = blendModeNum;

                    mGXMemNum.texSRT = mGXMemCap.texSRT;
                    InitTexSRT(GetTexSRTAry(), mGXMemNum.texSRT);
                    mGXMemNum.indSRT = mGXMemCap.indSRT;
                    InitTexSRT(GetIndTexSRTAry(), mGXMemNum.indSRT);

                    mGXMemNum.chanCtrl = mGXMemCap.chanCtrl;
                    if (mGXMemNum.chanCtrl) {
                        GetChanCtrlAry()[0] = ChanCtrl();
                    }

                    mGXMemNum.matCol = mGXMemCap.matCol;
                    if (mGXMemNum.matCol) {
                        GetMatColAry()[0] = ut::Color::WHITE;
                    }

                    mGXMemNum.tevSwap = mGXMemCap.tevSwap;
                    if (mGXMemNum.tevSwap) {
                        TevSwapMode* const tevSwaps = GetTevSwapAry();
                        tevSwaps[0].Set(GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
                        tevSwaps[1].Set(GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
                        tevSwaps[2].Set(GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
                        tevSwaps[3].Set(GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
                    }

                    mGXMemNum.alpComp = mGXMemCap.alpComp;
                    if (mGXMemNum.alpComp) {
                        *GetAlphaComparePtr() = AlphaCompare();
                    }

                    mGXMemNum.blendMode = mGXMemCap.blendMode;
                    if (mGXMemNum.blendMode) {
                        *GetBlendModePtr() = BlendMode();
                    }
                }
            }
        }

        const TexMap* Material::GetTexMapAry() const {
            return detail::ConvertOffsToPtr< TexMap >(mpGXMem, 0);
        }

        TexMap* Material::GetTexMapAry() {
            return detail::ConvertOffsToPtr< TexMap >(mpGXMem, 0);
        }

        TexSRT* Material::GetTexSRTAry() {
            return detail::ConvertOffsToPtr< TexSRT >(mpGXMem, CalcOffsetTexSRTAry(mGXMemCap));
        }

        TexCoordGen* Material::GetTexCoordGenAry() {
            return detail::ConvertOffsToPtr< TexCoordGen >(mpGXMem, CalcOffsetTexCoordGenAry(mGXMemCap));
        }

        ChanCtrl* Material::GetChanCtrlAry() {
            return detail::ConvertOffsToPtr< ChanCtrl >(mpGXMem, CalcOffsetChanCtrlAry(mGXMemCap));
        }

        ut::Color* Material::GetMatColAry() {
            return detail::ConvertOffsToPtr< ut::Color >(mpGXMem, CalcOffsetMatColAry(mGXMemCap));
        }

        TevSwapMode* Material::GetTevSwapAry() {
            return detail::ConvertOffsToPtr< TevSwapMode >(mpGXMem, CalcOffsetTevSwapAry(mGXMemCap));
        }

        AlphaCompare* Material::GetAlphaComparePtr() {
            return detail::ConvertOffsToPtr< AlphaCompare >(mpGXMem, CalcOffsetGetAlphaCompare(mGXMemCap));
        }

        BlendMode* Material::GetBlendModePtr() {
            return detail::ConvertOffsToPtr< BlendMode >(mpGXMem, CalcOffsetBlendMode(mGXMemCap));
        }

        IndirectStage* Material::GetIndirectStageAry() {
            return detail::ConvertOffsToPtr< IndirectStage >(mpGXMem, CalcOffsetIndirectStageAry(mGXMemCap));
        }

        TexSRT* Material::GetIndTexSRTAry() {
            return detail::ConvertOffsToPtr< TexSRT >(mpGXMem, CalcOffsetIndTexSRTAry(mGXMemCap));
        }

        TevStage* Material::GetTevStageAry() {
            return detail::ConvertOffsToPtr< TevStage >(mpGXMem, CalcOffsetTevStageAry(mGXMemCap));
        }

        void Material::SetName(const char* pName) {
            strncpy(mName, pName, sizeof(mName) - 1);
            mName[sizeof(mName) - 1] = '\0';
        }

        void Material::SetTextureNum(u8 num) {
            if (num > 0) {
                TexMap* const pTexMap = GetTexMapAry();

                for (u32 i = mGXMemNum.texMap; i < num; i++) {
                    new (&pTexMap[i]) TexMap();
                }
            }

            mGXMemNum.texMap = num;
        }

        void Material::SetTexCoordGenNum(u8 num) {
            if (num > 0) {
                TexCoordGen* const pTexCoordGen = GetTexCoordGenAry();

                for (u32 i = mGXMemNum.texCoordGen; i < num; i++) {
                    new (&pTexCoordGen[i]) TexCoordGen();
                }
            }

            mGXMemNum.texCoordGen = num;
        }

        void Material::SetTevStageNum(u8 num) {
            if (num > 0) {
                TevStage* const pTevStage = GetTevStageAry();

                for (u32 i = mGXMemNum.tevStage; i < num; i++) {
                    new (&pTevStage[i]) TevStage();
                }
            }

            mGXMemNum.tevStage = num;
        }

        void Material::SetIndStageNum(u8 num) {
            if (num > 0) {
                IndirectStage* const pIndStage = GetIndirectStageAry();

                for (u32 i = mGXMemNum.indStage; i < num; i++) {
                    new (&pIndStage[i]) IndirectStage();
                }
            }

            mGXMemNum.indStage = num;
        }

        void Material::SetColorElement(u32 idx, s16 value) {
            switch (idx) {
            case detail::ANIMTARGET_MATCOLOR_MATCOLOR0_R:
            case detail::ANIMTARGET_MATCOLOR_MATCOLOR0_G:
            case detail::ANIMTARGET_MATCOLOR_MATCOLOR0_B:
            case detail::ANIMTARGET_MATCOLOR_MATCOLOR0_A:
                if (mGXMemNum.matCol >= 1) {
                    ut::Color* matCols = GetMatColAry();
                    SetColorComponentValue(&matCols[0], (idx - detail::ANIMTARGET_MATCOLOR_MATCOLOR0_R) % 4, value);
                }
                break;

            case detail::ANIMTARGET_MATCOLOR_TEVCOLOR0_R:
            case detail::ANIMTARGET_MATCOLOR_TEVCOLOR0_G:
            case detail::ANIMTARGET_MATCOLOR_TEVCOLOR0_B:
            case detail::ANIMTARGET_MATCOLOR_TEVCOLOR0_A:
            case detail::ANIMTARGET_MATCOLOR_TEVCOLOR1_R:
            case detail::ANIMTARGET_MATCOLOR_TEVCOLOR1_G:
            case detail::ANIMTARGET_MATCOLOR_TEVCOLOR1_B:
            case detail::ANIMTARGET_MATCOLOR_TEVCOLOR1_A:
            case detail::ANIMTARGET_MATCOLOR_TEVCOLOR2_R:
            case detail::ANIMTARGET_MATCOLOR_TEVCOLOR2_G:
            case detail::ANIMTARGET_MATCOLOR_TEVCOLOR2_B:
            case detail::ANIMTARGET_MATCOLOR_TEVCOLOR2_A: {
                const u32 regIdx = (idx - detail::ANIMTARGET_MATCOLOR_TEVCOLOR0_R) / 4;
                switch ((idx - detail::ANIMTARGET_MATCOLOR_TEVCOLOR0_R) % 4) {
                case 0:
                    mTevCols[regIdx].r = value;
                    break;
                case 1:
                    mTevCols[regIdx].g = value;
                    break;
                case 2:
                    mTevCols[regIdx].b = value;
                    break;
                case 3:
                    mTevCols[regIdx].a = value;
                    break;
                }
            } break;

            case detail::ANIMTARGET_MATCOLOR_TEVKONST0_R:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST0_G:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST0_B:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST0_A:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST1_R:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST1_G:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST1_B:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST1_A:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST2_R:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST2_G:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST2_B:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST2_A:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST3_R:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST3_G:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST3_B:
            case detail::ANIMTARGET_MATCOLOR_TEVKONST3_A: {
                const u32 regIdx = (idx - detail::ANIMTARGET_MATCOLOR_TEVKONST0_R) / 4;
                SetColorComponentValue(&mTevKCols[regIdx], (idx - detail::ANIMTARGET_MATCOLOR_TEVKONST0_R) % 4, value);
            } break;
            }
        }

#ifdef __3DS__
        // The Wii branch of SetupGX below as data: the channel colour, the texture coordinate
        // generators and matrices, the material's TEV stages (or nw4r's defaults for its texture
        // count), registers, konst colours and swap tables, compiled into Citro3D combiner stages
        // for DrawQuad. Returns SetupGX's value (useRasStage && useVtxColor).
        bool Material::SetupNativeTev(bool modulate, u8 alpha) {
            static detail::NativeQuadMaterial sQuad;
            detail::NativeQuadMaterial& q = sQuad;
            q = detail::NativeQuadMaterial();

            bool useVtxColor = true;
            bool useMatColor = false;
            q.colorFromVertex = q.alphaFromVertex = true;
            if (IsChanCtrlCap()) {
                const ChanCtrl* const pChanCtrl = GetChanCtrlAry();
                q.colorFromVertex = pChanCtrl->GetColorSrc() == GX_SRC_VTX;
                q.alphaFromVertex = pChanCtrl->GetAlphaSrc() == GX_SRC_VTX;
                useVtxColor = q.colorFromVertex || q.alphaFromVertex;
                useMatColor = !q.colorFromVertex || !q.alphaFromVertex;
            }
            bool useRasStage = useVtxColor && modulate;
            q.matColor = ut::Color::WHITE;
            if (useMatColor) {
                if (IsMatColorCap()) {
                    q.matColor = *GetMatColAry();
                }
                q.matColor = detail::MultipleAlpha(q.matColor, alpha);
                useRasStage = useRasStage || q.matColor != ut::Color::WHITE;
            }

            RvlTevDesc tev = {};
            if (IsTevSwapCap()) {
                const TevSwapMode* const pTevSwap = GetTevSwapAry();
                for (int i = 0; i < GX_MAX_TEVSWAP; i++) {
                    tev.swapTables[i][0] = pTevSwap[i].GetR();
                    tev.swapTables[i][1] = pTevSwap[i].GetG();
                    tev.swapTables[i][2] = pTevSwap[i].GetB();
                    tev.swapTables[i][3] = pTevSwap[i].GetA();
                }
            } else {
                rvlTevDefaultSwapTables(tev);
            }
            for (int i = 0; i < 3; i++) {
                tev.registers[i + 1][0] = mTevCols[i].r;
                tev.registers[i + 1][1] = mTevCols[i].g;
                tev.registers[i + 1][2] = mTevCols[i].b;
                tev.registers[i + 1][3] = mTevCols[i].a;
            }
            for (int i = 0; i < 4; i++) {
                tev.konst[i][0] = mTevKCols[i].r;
                tev.konst[i][1] = mTevKCols[i].g;
                tev.konst[i][2] = mTevKCols[i].b;
                tev.konst[i][3] = mTevKCols[i].a;
            }

            // A stage with GX's defaults (GXSetTevOp-like ADD, no bias, x1, into PREV).
            struct Stage {
                static RvlTevStageDesc& add(RvlTevDesc& rTev, u8 texCoord, u8 texMap, u8 channel) {
                    RvlTevStageDesc& st = rTev.stages[rTev.stageCount++];
                    st = RvlTevStageDesc();
                    st.texCoord = texCoord;
                    st.texMap = texMap;
                    st.channel = channel;
                    st.kcsel = GX_TEV_KCSEL_1;
                    st.kasel = GX_TEV_KASEL_1;
                    return st;
                }
                static void color(RvlTevStageDesc& st, u8 a, u8 b, u8 c, u8 d) {
                    st.colorIn[0] = a;
                    st.colorIn[1] = b;
                    st.colorIn[2] = c;
                    st.colorIn[3] = d;
                }
                static void alpha(RvlTevStageDesc& st, u8 a, u8 b, u8 c, u8 d) {
                    st.alphaIn[0] = a;
                    st.alphaIn[1] = b;
                    st.alphaIn[2] = c;
                    st.alphaIn[3] = d;
                }
            };
            static const u8 kColSels[GX_MAX_TEXMAP] = {GX_TEV_KCSEL_K3_A, GX_TEV_KCSEL_K3_B, GX_TEV_KCSEL_K3_G, GX_TEV_KCSEL_K3_R,
                                                       GX_TEV_KCSEL_K2_A, GX_TEV_KCSEL_K2_B, GX_TEV_KCSEL_K2_G, GX_TEV_KCSEL_K2_R};
            static const u8 kAlpSels[GX_MAX_TEXMAP] = {GX_TEV_KASEL_K3_A, GX_TEV_KASEL_K3_B, GX_TEV_KASEL_K3_G, GX_TEV_KASEL_K3_R,
                                                       GX_TEV_KASEL_K2_A, GX_TEV_KASEL_K2_B, GX_TEV_KASEL_K2_G, GX_TEV_KASEL_K2_R};
            const u8 kNull = 0xFF;

            if (mGXMemNum.tevStage > 0) {
                const TevStage* const pTevStage = GetTevStageAry();
                for (int i = 0; i < mGXMemNum.tevStage && i < 16; i++) {
                    const u32 map = pTevStage[i].GetTexMap();
                    RvlTevStageDesc& st = Stage::add(tev, pTevStage[i].GetTexCoordGen(), map < GX_MAX_TEXMAP ? map : kNull,
                                                     pTevStage[i].GetColorChan());
                    st.rasSwap = pTevStage[i].GetRasSwapSel();
                    st.texSwap = pTevStage[i].GetTexSwapSel();
                    Stage::color(st, pTevStage[i].GetColorInA(), pTevStage[i].GetColorInB(), pTevStage[i].GetColorInC(),
                                 pTevStage[i].GetColorInD());
                    st.colorOp = pTevStage[i].GetColorOp();
                    st.colorBias = pTevStage[i].GetColorBias();
                    st.colorScale = pTevStage[i].GetColorScale();
                    st.colorOut = pTevStage[i].GetColorOutReg();
                    st.kcsel = pTevStage[i].GetKColorSel();
                    Stage::alpha(st, pTevStage[i].GetAlphaInA(), pTevStage[i].GetAlphaInB(), pTevStage[i].GetAlphaInC(),
                                 pTevStage[i].GetAlphaInD());
                    st.alphaOp = pTevStage[i].GetAlphaOp();
                    st.alphaBias = pTevStage[i].GetAlphaBias();
                    st.alphaScale = pTevStage[i].GetAlphaScale();
                    st.alphaOut = pTevStage[i].GetAlphaOutReg();
                    st.kasel = pTevStage[i].GetKAlphaSel();
                }
                useRasStage = true;
            } else if (mGXMemNum.texMap == 0) {
                RvlTevStageDesc& st = Stage::add(tev, kNull, kNull, GX_COLOR0A0);
                Stage::color(st, GX_CC_ZERO, GX_CC_C1, GX_CC_RASC, GX_CC_ZERO);
                Stage::alpha(st, GX_CA_ZERO, GX_CA_A1, GX_CA_RASA, GX_CA_ZERO);
                useRasStage = true;
            } else {
                if (mGXMemNum.texMap == 1) {
                    RvlTevStageDesc& st = Stage::add(tev, GX_TEXCOORD0, GX_TEXMAP0, kNull);
                    Stage::color(st, GX_CC_C0, GX_CC_C1, GX_CC_TEXC, GX_CC_ZERO);
                    Stage::alpha(st, GX_CA_A0, GX_CA_A1, GX_CA_TEXA, GX_CA_ZERO);
                } else if (mGXMemNum.texMap == 2) {
                    RvlTevStageDesc& st0 = Stage::add(tev, GX_TEXCOORD0, GX_TEXMAP0, kNull);
                    Stage::color(st0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
                    Stage::alpha(st0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
                    RvlTevStageDesc& st1 = Stage::add(tev, GX_TEXCOORD1, GX_TEXMAP1, kNull);
                    Stage::color(st1, GX_CC_TEXC, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
                    Stage::alpha(st1, GX_CA_TEXA, GX_CA_APREV, GX_CA_KONST, GX_CA_ZERO);
                    st1.kcsel = kColSels[GX_TEXMAP0];
                    st1.kasel = kAlpSels[GX_TEXMAP0];
                } else {
                    for (int i = 0; i < mGXMemNum.texMap && i < GX_MAX_TEXMAP; i++) {
                        RvlTevStageDesc& st = Stage::add(tev, i, i, kNull);
                        Stage::color(st, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, i == 0 ? GX_CC_ZERO : GX_CC_CPREV);
                        Stage::alpha(st, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, i == 0 ? GX_CA_ZERO : GX_CA_APREV);
                        st.kcsel = kColSels[i];
                        st.kasel = kAlpSels[i];
                    }
                }
                if (mTevCols[0] != DefaultBlackColor || !IsWhiteColor(mTevCols[1])) {
                    RvlTevStageDesc& st = Stage::add(tev, kNull, kNull, kNull);
                    Stage::color(st, GX_CC_C0, GX_CC_C1, GX_CC_CPREV, GX_CC_ZERO);
                    Stage::alpha(st, GX_CA_A0, GX_CA_A1, GX_CA_APREV, GX_CA_ZERO);
                }
                if (useRasStage) {
                    RvlTevStageDesc& st = Stage::add(tev, kNull, kNull, GX_COLOR0A0);
                    Stage::color(st, GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASC, GX_CC_ZERO);
                    Stage::alpha(st, GX_CA_ZERO, GX_CA_APREV, GX_CA_RASA, GX_CA_ZERO);
                }
            }

            RvlTevProgram program;
            if (!rvlTevCompile(tev, program)) {
                static u32 sReports;
                if (sReports < 4) {
                    sReports++;
                    hostLog("lyt: material %s exceeds the 3DS combiners (%u TEV stages)\n", GetName(), tev.stageCount);
                }
            }
            q.state.stageCount = program.stageCount;
            for (u32 i = 0; i < program.stageCount; i++) {
                q.state.stages[i] = program.stages[i];
            }

            // Each unit's texture and coordinates: the generator unitCoord[u] reads DrawQuad's set
            // (GX_TG_TEX0 + n) through its SRT matrix (GX_TEXMTX0 + 3 k), or the identity.
            const TexMap* const pTexMap = GetTexMapAry();
            const TexCoordGen* const pTexCoordGen = GetTexCoordGenAry();
            const TexSRT* const pTexSrt = GetTexSRTAry();
            q.unitCount = program.unitCount;
            for (u32 u = 0; u < program.unitCount; u++) {
                q.unitSet[u] = 0xFF;
                q.unitMtx[u][0][0] = q.unitMtx[u][1][1] = 1.0f;
                const u32 map = program.unitMap[u];
                if (map < mGXMemNum.texMap) {
                    const TexMap& rTexMap = pTexMap[map];
                    q.state.textures[u] = rvl3DSGetNativeTexture(rTexMap.mImage, rTexMap.GetSize().width, rTexMap.GetSize().height,
                                                                rTexMap.GetTexelFormat(), rTexMap.GetPalette(), rTexMap.GetPaletteFormat(),
                                                                rTexMap.GetPaletteEntryNum(), rTexMap.GetWrapModeS(), rTexMap.GetWrapModeT(),
                                                                rTexMap.GetMagFilter());
                }
                const u32 coord = program.unitCoord[u];
                if (coord >= mGXMemNum.texCoordGen) {
                    continue;
                }
                const u32 src = pTexCoordGen[coord].GetTexGenSrc();
                if (src >= GX_TG_TEX0 && src <= GX_TG_TEX7) {
                    q.unitSet[u] = static_cast< u8 >(src - GX_TG_TEX0);
                }
                const u32 texMtx = pTexCoordGen[coord].GetTexMtx();
                if (pTexCoordGen[coord].GetTexGenType() == GX_TG_MTX2x4 && texMtx != GX_IDENTITY && GetTexMtxIdx(texMtx) < mGXMemNum.texSRT) {
                    math::MTX34 mtx;
                    CalcTextureMtx(&mtx, pTexSrt[GetTexMtxIdx(texMtx)]);
                    for (int r = 0; r < 2; r++) {
                        q.unitMtx[u][r][0] = mtx.m[r][0];
                        q.unitMtx[u][r][1] = mtx.m[r][1];
                        q.unitMtx[u][r][2] = mtx.m[r][2] + mtx.m[r][3];
                    }
                }
            }
            // GX indirect texturing, which the PICA lacks (PicSmoke): the only texture displaced by the
            // value of the distortion as the blend of two samples, as platform/3ds/gx/native_submit.cpp
            // does for models.
            if (mGXMemNum.indStage == 1 && mGXMemNum.indSRT != 0 && program.unitCount == 1 && program.stageCount + 2 <= HOST_GFX_STAGES &&
                q.state.textures[0] != nullptr) {
                const TevStage* const pStages = GetTevStageAry();
                const IndirectStage& rIndStage = GetIndirectStageAry()[0];
                u32 selected = GX_ITM_OFF;
                for (int i = 0; i < mGXMemNum.tevStage && i < 16; i++) {
                    if (pStages[i].GetIndMtxSel() != GX_ITM_OFF && pStages[i].GetIndFormat() == GX_ITF_8 && pStages[i].GetIndBiasSel() == GX_ITB_STU &&
                        pStages[i].GetTexMap() == program.unitMap[0]) {
                        selected = pStages[i].GetIndMtxSel();
                        break;
                    }
                }
                bool firstOnly = true;
                for (u32 i = 0; i < program.stageCount; i++) {
                    for (u32 k = 0; k < 3; k++) {
                        firstOnly &= q.state.stages[i].colorSrc[k] != (i == 0 ? 0x0F : 0x03) && q.state.stages[i].alphaSrc[k] != (i == 0 ? 0x0F : 0x03);
                    }
                }
                const u32 indMap = rIndStage.GetTexMap();
                if (selected >= GX_ITM_0 && selected <= GX_ITM_2 && selected - GX_ITM_0 < mGXMemNum.indSRT && firstOnly && indMap < mGXMemNum.texMap &&
                    rIndStage.GetTexCoordGen() < mGXMemNum.texCoordGen) {
                    const TexMap& rIndMap = pTexMap[indMap];
                    const TexMap& rMap = pTexMap[program.unitMap[0]];
                    void* indirect = rvl3DSGetNativeTexture(rIndMap.mImage, rIndMap.GetSize().width, rIndMap.GetSize().height, rIndMap.GetTexelFormat(),
                                                            rIndMap.GetPalette(), rIndMap.GetPaletteFormat(), rIndMap.GetPaletteEntryNum(),
                                                            rIndMap.GetWrapModeS(), rIndMap.GetWrapModeT(), rIndMap.GetMagFilter());
                    if (indirect != nullptr && rMap.GetSize().width != 0 && rMap.GetSize().height != 0) {
                        f32 ind[2][3];
                        CalcIndTexMtx(ind, GetIndTexSRTAry()[selected - GX_ITM_0]);
                        const f32 stepS = 64.0f * (ind[0][0] + ind[0][1] + ind[0][2]) / rMap.GetSize().width;
                        const f32 stepT = 64.0f * (ind[1][0] + ind[1][1] + ind[1][2]) / rMap.GetSize().height;
                        q.unitSet[1] = q.unitSet[0];
                        memcpy(q.unitMtx[1], q.unitMtx[0], sizeof(q.unitMtx[0]));
                        q.unitMtx[0][0][2] -= stepS;
                        q.unitMtx[0][1][2] -= stepT;
                        q.unitMtx[1][0][2] += stepS;
                        q.unitMtx[1][1][2] += stepT;
                        const TexCoordGen& rIndGen = pTexCoordGen[rIndStage.GetTexCoordGen()];
                        const u32 indSrc = rIndGen.GetTexGenSrc();
                        q.unitSet[2] = indSrc >= GX_TG_TEX0 && indSrc <= GX_TG_TEX7 ? static_cast< u8 >(indSrc - GX_TG_TEX0) : 0xFF;
                        memset(q.unitMtx[2], 0, sizeof(q.unitMtx[2]));
                        q.unitMtx[2][0][0] = q.unitMtx[2][1][1] = 1.0f;
                        const u32 indTexMtx = rIndGen.GetTexMtx();
                        if (rIndGen.GetTexGenType() == GX_TG_MTX2x4 && indTexMtx != GX_IDENTITY && GetTexMtxIdx(indTexMtx) < mGXMemNum.texSRT) {
                            math::MTX34 mtx;
                            CalcTextureMtx(&mtx, pTexSrt[GetTexMtxIdx(indTexMtx)]);
                            for (int r = 0; r < 2; r++) {
                                q.unitMtx[2][r][0] = mtx.m[r][0];
                                q.unitMtx[2][r][1] = mtx.m[r][1];
                                q.unitMtx[2][r][2] = mtx.m[r][2] + mtx.m[r][3];
                            }
                        }
                        for (u32 i = program.stageCount; i-- > 0;) {
                            q.state.stages[i + 2] = q.state.stages[i];
                        }
                        HostGfxStage& first = q.state.stages[2];
                        for (u32 k = 0; k < 3; k++) {
                            first.colorSrc[k] = first.colorSrc[k] == 0x03 ? 0x0F : first.colorSrc[k];
                            first.alphaSrc[k] = first.alphaSrc[k] == 0x03 ? 0x0F : first.alphaSrc[k];
                        }
                        memset(&q.state.stages[0], 0, sizeof(q.state.stages[0]) * 2);
                        q.state.stages[0].colorSrc[0] = 5;     // GPU_TEXTURE2
                        q.state.stages[0].colorSrc[1] = 0x0E;  // GPU_CONSTANT
                        q.state.stages[0].colorFunc = 5;       // GPU_SUBTRACT
                        q.state.stages[0].colorScale = 1;      // GPU_TEVSCALE_2
                        q.state.stages[0].constant = 0x40404040u;
                        q.state.stages[1].colorSrc[0] = 4;     // GPU_TEXTURE1
                        q.state.stages[1].colorSrc[1] = 3;     // GPU_TEXTURE0
                        q.state.stages[1].colorSrc[2] = 0x0F;  // GPU_PREVIOUS
                        q.state.stages[1].colorFunc = 4;       // GPU_INTERPOLATE
                        q.state.stages[1].alphaSrc[0] = 3;
                        q.state.stageCount = static_cast< u8 >(program.stageCount + 2);
                        q.state.textures[1] = q.state.textures[0];
                        q.state.textures[2] = indirect;
                        q.unitCount = 3;
                    }
                }
            }
            for (u32 u = 0; u < q.unitCount; u++) {
                if (q.state.textures[u] == nullptr) {
                    detail::SetNativeMaterial(nullptr);  // a texture the 3DS cannot load: vertex colours only
                    return useRasStage && useVtxColor;
                }
            }
            detail::SetNativeMaterial(&q);
            return useRasStage && useVtxColor;
        }
#endif
        bool Material::SetupGX(bool modulate, u8 alpha) {
#ifdef __3DS__
            const bool useVtxColor = SetupNativeTev(modulate, alpha);
            // Alpha compare: the 3DS has one comparison. Two joined by AND or OR reduce to one exactly
            // when the other side always passes (AND ALWAYS) or never does (OR NEVER); otherwise the
            // first is kept.
            u32 compare = GX_ALWAYS;
            u8 reference = 0;
            if (IsAlphaCompareCap()) {
                const AlphaCompare* const pAlphaComp = GetAlphaComparePtr();
                const u32 op = pAlphaComp->GetOp();
                const bool useSecond = (op == GX_AOP_AND && pAlphaComp->GetComp0() == GX_ALWAYS) ||
                                       (op == GX_AOP_OR && pAlphaComp->GetComp0() == GX_NEVER);
                compare = useSecond ? pAlphaComp->GetComp1() : pAlphaComp->GetComp0();
                reference = useSecond ? pAlphaComp->GetRef1() : pAlphaComp->GetRef0();
            }
            TDDraw::setDirectAlphaTest(GXToCitro3D::alphaTest(compare), reference);
            u8 equation, source, destination;
            int logic;
            if (IsBlendModeCap()) {
                const BlendMode* const pBlendMode = GetBlendModePtr();
                GXToCitro3D::blendMode(pBlendMode->GetType(), pBlendMode->GetSrcFactor(), pBlendMode->GetDstFactor(),
                                       pBlendMode->GetOp(), equation, source, destination, logic);
            } else {
                GXToCitro3D::blendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET, equation, source, destination,
                                       logic);
            }
            TDDraw::setDirectBlendState(equation, source, destination);
            return useVtxColor;
#else
            static GXTevKColorSel kColSels[GX_MAX_TEXMAP] = {GX_TEV_KCSEL_K3_A, GX_TEV_KCSEL_K3_B, GX_TEV_KCSEL_K3_G, GX_TEV_KCSEL_K3_R,
                                                             GX_TEV_KCSEL_K2_A, GX_TEV_KCSEL_K2_B, GX_TEV_KCSEL_K2_G, GX_TEV_KCSEL_K2_R};

            static GXTevKAlphaSel kAlpSels[GX_MAX_TEXMAP] = {GX_TEV_KASEL_K3_A, GX_TEV_KASEL_K3_B, GX_TEV_KASEL_K3_G, GX_TEV_KASEL_K3_R,
                                                             GX_TEV_KASEL_K2_A, GX_TEV_KASEL_K2_B, GX_TEV_KASEL_K2_G, GX_TEV_KASEL_K2_R};

            bool useVtxColor = true;
            bool useMatColor = false;

            GXSetNumChans(1);

            if (IsChanCtrlCap()) {
                const ChanCtrl* const pChanCtrl = GetChanCtrlAry();

                GXSetChanCtrl(GX_COLOR0, GX_FALSE, GX_SRC_REG, pChanCtrl->GetColorSrc(), GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);

                GXSetChanCtrl(GX_ALPHA0, GX_FALSE, GX_SRC_REG, pChanCtrl->GetAlphaSrc(), GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);

                useVtxColor = pChanCtrl->GetColorSrc() == GX_SRC_VTX || pChanCtrl->GetAlphaSrc() == GX_SRC_VTX;

                useMatColor = pChanCtrl->GetColorSrc() == GX_SRC_REG || pChanCtrl->GetAlphaSrc() == GX_SRC_REG;
            } else {
                GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
            }

            bool useRasStage = false;

            if (useVtxColor) {
                useRasStage = useRasStage || modulate;
            }

            if (useMatColor) {
                ut::Color matColor = ut::Color::WHITE;

                if (IsMatColorCap()) {
                    matColor = *GetMatColAry();
                }

                matColor = detail::MultipleAlpha(matColor, alpha);
                GXSetChanMatColor(GX_COLOR0A0, matColor);

                useRasStage = useRasStage || matColor != ut::Color::WHITE;
            }

            bool setTexMtx = false;
            bool useTexMtx[10];

            for (int i = 0; i < 10; i++) {
                useTexMtx[i] = false;
            }

            GXSetNumTexGens(mGXMemNum.texCoordGen);
            if (mGXMemNum.texCoordGen > 0) {
                const TexCoordGen* const pTexCoordGen = GetTexCoordGenAry();

                for (int i = 0; i < mGXMemNum.texCoordGen; i++) {
                    u32 texMtx = pTexCoordGen[i].GetTexMtx();

                    if (pTexCoordGen[i].GetTexGenType() == GX_TG_MTX2x4 && texMtx != GX_IDENTITY) {
                        useTexMtx[GetTexMtxIdx(texMtx)] = true;
                        setTexMtx = true;
                    }

                    GXSetTexCoordGen2(static_cast< GXTexCoordID >(i), pTexCoordGen[i].GetTexGenType(), pTexCoordGen[i].GetTexGenSrc(), texMtx,
                                      GX_FALSE, 0x7D);
                }
            }

            if (setTexMtx) {
                const TexSRT* const pTexSrt = GetTexSRTAry();

                for (u8 i = 0; i < mGXMemNum.texSRT; i++) {
                    if (!useTexMtx[i]) {
                        continue;
                    }

                    math::MTX34 mtx;
                    CalcTextureMtx(&mtx, pTexSrt[i]);
                    GXLoadTexMtxImm(mtx, GetTexMtx(i), GX_MTX2x4);
                }
            }

            if (mGXMemNum.texMap > 0) {
                u32 tlutID = GX_TLUT0;
                u32 bigTlutID = GX_BIGTLUT0;

                const TexMap* const pTexMap = GetTexMapAry();

                for (int i = 0; i < mGXMemNum.texMap; i++) {
                    const TexMap& rTexMap = pTexMap[i];

                    GXTexObj texObj;
                    rTexMap.Get(&texObj);

                    if (detail::IsCITexelFormat(rTexMap.GetTexelFormat())) {
                        u32 tlutName;
                        if (static_cast< int >(rTexMap.GetTexelFormat()) == GX_TF_C14X2) {
                            tlutName = bigTlutID++;
                        } else {
                            tlutName = tlutID++;
                        }

                        GXInitTexObjTlut(&texObj, tlutName);

                        GXTlutObj tlutObj;
                        rTexMap.Get(&tlutObj);

                        GXLoadTlut(&tlutObj, tlutName);
                    }

                    GXLoadTexObj(&texObj, static_cast< GXTexMapID >(i));
                }
            }

            GXSetTevColorS10(GX_TEVREG0, mTevCols[0]);
            GXSetTevColorS10(GX_TEVREG1, mTevCols[1]);
            GXSetTevColorS10(GX_TEVREG2, mTevCols[2]);

            GXSetTevKColor(GX_KCOLOR0, mTevKCols[GX_KCOLOR0]);
            GXSetTevKColor(GX_KCOLOR1, mTevKCols[GX_KCOLOR1]);
            GXSetTevKColor(GX_KCOLOR2, mTevKCols[GX_KCOLOR2]);
            GXSetTevKColor(GX_KCOLOR3, mTevKCols[GX_KCOLOR3]);

            if (IsTevSwapCap()) {
                const TevSwapMode* const pTevSwap = GetTevSwapAry();

                for (int i = 0; i < GX_MAX_TEVSWAP; i++) {
                    GXSetTevSwapModeTable(static_cast< GXTevSwapSel >(i), pTevSwap[i].GetR(), pTevSwap[i].GetG(), pTevSwap[i].GetB(),
                                          pTevSwap[i].GetA());
                }
            } else {
                GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);

                GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);

                GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);

                GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
            }

            bool setIndTexMtx = false;
            bool useIndTexMtx[3];

            for (int i = 0; i < 3; i++) {
                useIndTexMtx[i] = false;
            }

            if (mGXMemNum.tevStage > 0) {
                GXSetNumTevStages(mGXMemNum.tevStage);

                const TevStage* const pTevStage = GetTevStageAry();

                for (int i = 0; i < mGXMemNum.tevStage; i++) {
                    GXTevStageID tevStage = static_cast< GXTevStageID >(i);

                    GXSetTevOrder(tevStage, pTevStage[i].GetTexCoordGen(), pTevStage[i].GetTexMap(), pTevStage[i].GetColorChan());

                    GXSetTevSwapMode(tevStage, pTevStage[i].GetRasSwapSel(), pTevStage[i].GetTexSwapSel());

                    GXSetTevColorIn(tevStage, pTevStage[i].GetColorInA(), pTevStage[i].GetColorInB(), pTevStage[i].GetColorInC(),
                                    pTevStage[i].GetColorInD());

                    GXSetTevColorOp(tevStage, pTevStage[i].GetColorOp(), pTevStage[i].GetColorBias(), pTevStage[i].GetColorScale(),
                                    pTevStage[i].IsColorClamp(), pTevStage[i].GetColorOutReg());

                    GXSetTevKColorSel(tevStage, pTevStage[i].GetKColorSel());

                    GXSetTevAlphaIn(tevStage, pTevStage[i].GetAlphaInA(), pTevStage[i].GetAlphaInB(), pTevStage[i].GetAlphaInC(),
                                    pTevStage[i].GetAlphaInD());

                    GXSetTevAlphaOp(tevStage, pTevStage[i].GetAlphaOp(), pTevStage[i].GetAlphaBias(), pTevStage[i].GetAlphaScale(),
                                    pTevStage[i].IsAlphaClamp(), pTevStage[i].GetAlphaOutReg());

                    GXSetTevKAlphaSel(tevStage, pTevStage[i].GetKAlphaSel());
                    GXIndTexMtxID indMtx = pTevStage[i].GetIndMtxSel();

                    GXSetTevIndirect(tevStage, pTevStage[i].GetIndStage(), pTevStage[i].GetIndFormat(), pTevStage[i].GetIndBiasSel(), indMtx,
                                     pTevStage[i].GetIndWrapS(), pTevStage[i].GetIndWrapT(), pTevStage[i].IsIndAddPrev(), pTevStage[i].IsIndUtcLod(),
                                     pTevStage[i].GetIndAlphaSel());
                    if (GX_ITM_0 <= indMtx && indMtx <= GX_ITM_2) {
                        useIndTexMtx[indMtx - 1] = true;
                        setIndTexMtx = true;
                    }
                }

                useRasStage = true;
            } else {
                u8 tevStageID = GX_TEVSTAGE0;

                if (mGXMemNum.texMap == 0) {
                    GXTevStageID tevStage = GX_TEVSTAGE0;

                    GXSetTevOrder(tevStage, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
                    GXSetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_C1, GX_CC_RASC, GX_CC_ZERO);
                    GXSetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_A1, GX_CA_RASA, GX_CA_ZERO);

                    useRasStage = true;
                    tevStageID = GX_TEVSTAGE1;
                } else {
                    if (mGXMemNum.texMap == 1) {
                        GXTevStageID tevStage = GX_TEVSTAGE0;

                        GXSetTevOrder(tevStage, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
                        GXSetTevColorIn(tevStage, GX_CC_C0, GX_CC_C1, GX_CC_TEXC, GX_CC_ZERO);
                        GXSetTevAlphaIn(tevStage, GX_CA_A0, GX_CA_A1, GX_CA_TEXA, GX_CA_ZERO);

                        tevStageID = GX_TEVSTAGE1;
                    } else {
                        if (mGXMemNum.texMap == 2) {
                            GXTevStageID tevStage = GX_TEVSTAGE0;

                            GXSetTevOrder(tevStage, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
                            GXSetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
                            GXSetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);

                            tevStage = GX_TEVSTAGE1;

                            GXSetTevOrder(tevStage, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
                            GXSetTevColorIn(tevStage, GX_CC_TEXC, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
                            GXSetTevAlphaIn(tevStage, GX_CA_TEXA, GX_CA_APREV, GX_CA_KONST, GX_CA_ZERO);

                            GXSetTevKColorSel(tevStage, kColSels[GX_TEXMAP0]);
                            GXSetTevKAlphaSel(tevStage, kAlpSels[GX_TEXMAP0]);

                            tevStageID = GX_TEVSTAGE2;
                        } else {
                            for (int i = 0; i < mGXMemNum.texMap; i++) {
                                GXTevStageID tevStage = static_cast< GXTevStageID >(tevStageID);

                                GXSetTevOrder(tevStage, static_cast< GXTexCoordID >(i), static_cast< GXTexMapID >(i), GX_COLOR_NULL);

                                GXTevColorArg colorD = i == GX_TEXMAP0 ? GX_CC_ZERO : GX_CC_CPREV;
                                GXTevAlphaArg alphaD = i == GX_TEXMAP0 ? GX_CA_ZERO : GX_CA_APREV;

                                GXSetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, colorD);
                                GXSetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, alphaD);

                                GXSetTevKColorSel(tevStage, kColSels[i]);
                                GXSetTevKAlphaSel(tevStage, kAlpSels[i]);

                                tevStageID++;
                            }
                        }

                        if (mTevCols[0] != DefaultBlackColor || !IsWhiteColor(mTevCols[1])) {
                            GXTevStageID tevStage = static_cast< GXTevStageID >(tevStageID);

                            GXSetTevOrder(tevStage, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
                            GXSetTevColorIn(tevStage, GX_CC_C0, GX_CC_C1, GX_CC_CPREV, GX_CC_ZERO);
                            GXSetTevAlphaIn(tevStage, GX_CA_A0, GX_CA_A1, GX_CA_APREV, GX_CA_ZERO);

                            tevStageID++;
                        }
                    }

                    if (useRasStage) {
                        GXTevStageID tevStage = static_cast< GXTevStageID >(tevStageID);

                        GXSetTevOrder(tevStage, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
                        GXSetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASC, GX_CC_ZERO);
                        GXSetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_APREV, GX_CA_RASA, GX_CA_ZERO);

                        tevStageID++;
                    }
                }

                const u8 tevStageNum = tevStageID;
                for (u8 id = 0; id < tevStageNum; id++) {
                    GXTevStageID tevStage = static_cast< GXTevStageID >(id);

                    GXSetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, TRUE, GX_TEVPREV);
                    GXSetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, TRUE, GX_TEVPREV);

                    GXSetTevDirect(tevStage);
                    GXSetTevSwapMode(tevStage, GX_TEV_SWAP0, GX_TEV_SWAP0);
                }

                GXSetNumTevStages(tevStageNum);
            }

            if (setIndTexMtx) {
                const TexSRT* const pIndTexSrt = GetIndTexSRTAry();

                for (int i = 0; i < mGXMemNum.indSRT; i++) {
                    if (!useIndTexMtx[i]) {
                        continue;
                    }

                    f32 mtx[2][3];
                    CalcIndTexMtx(mtx, pIndTexSrt[i]);
                    SetIndTexMtx(static_cast< GXIndTexMtxID >(GX_ITM_0 + i), mtx);
                }
            }

            GXSetNumIndStages(mGXMemNum.indStage);
            if (mGXMemNum.indStage > 0) {
                const IndirectStage* const pIndStage = GetIndirectStageAry();

                for (int i = 0; i < mGXMemNum.indStage; i++) {
                    GXIndTexStageID indStage = static_cast< GXIndTexStageID >(i);

                    GXSetIndTexOrder(indStage, pIndStage[i].GetTexCoordGen(), pIndStage[i].GetTexMap());

                    GXSetIndTexCoordScale(indStage, pIndStage[i].GetScaleS(), pIndStage[i].GetScaleT());
                }
            }

            if (IsAlphaCompareCap()) {
                const AlphaCompare* const pAlphaComp = GetAlphaComparePtr();

                GXSetAlphaCompare(pAlphaComp->GetComp0(), pAlphaComp->GetRef0(), pAlphaComp->GetOp(), pAlphaComp->GetComp1(), pAlphaComp->GetRef1());
            } else {
                GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
            }

            if (IsBlendModeCap()) {
                const BlendMode* const pBlendMode = GetBlendModePtr();

                GXSetBlendMode(pBlendMode->GetType(), pBlendMode->GetSrcFactor(), pBlendMode->GetDstFactor(), pBlendMode->GetOp());

            } else {
                GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
            }

            return useRasStage && useVtxColor;
#endif
        }

        void Material::BindAnimation(AnimTransform* pAnimTrans) {
            pAnimTrans->Bind(this);
        }

        void Material::UnbindAnimation(AnimTransform* pAnimTrans) {
            detail::UnbindAnimationLink(&mAnimList, pAnimTrans);
        }

        void Material::UnbindAllAnimation() {
            UnbindAnimation(nullptr);
        }

        void Material::Animate() {
            for (AnimationList::Iterator it = mAnimList.GetBeginIter(); it != mAnimList.GetEndIter(); ++it) {
                if (it->IsEnable()) {
                    AnimTransform* animTrans = it->GetAnimTransform();
                    animTrans->Animate(it->GetIndex(), this);
                }
            }
        }

        void Material::AddAnimationLink(AnimationLink* pAnimLink) {
            mAnimList.PushBack(pAnimLink);
        }

        AnimationLink* Material::FindAnimationLink(AnimTransform* pAnimTrans) {
            return detail::FindAnimationLink(&mAnimList, pAnimTrans);
        }

        void Material::SetAnimationEnable(AnimTransform* pAnimTrans, bool enable) {
            AnimationLink* pAnimLink = FindAnimationLink(pAnimTrans);

            if (pAnimLink != NULL) {
                pAnimLink->SetEnable(enable);
            }
        }

        namespace detail {
            const Size GetTextureSize(Material* pMaterial, u8 idx) {
                if (idx >= pMaterial->GetTextureNum()) {
                    return Size(0.0f, 0.0f);
                }

                const TexMap& rTexMap = pMaterial->GetTexture(idx);
                return rTexMap.GetSize();
            }
        };  // namespace detail

    };  // namespace lyt
};  // namespace nw4r
