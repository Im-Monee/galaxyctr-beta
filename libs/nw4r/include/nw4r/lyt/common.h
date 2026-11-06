#pragma once

#include "nw4r/db/assert.h"
#include "nw4r/lyt/resources.h"
#include "nw4r/math/types.h"
#include "nw4r/ut/Color.h"
#include "nw4r/ut/inlines.h"
// 3DS: host.h, the port's host interface.
#ifdef __3DS__
#include "host.h"
#endif

namespace nw4r {
    namespace lyt {
        class Material;
        // 3DS: layout textures are drawn through Citro3D from their TexMap.
#ifdef __3DS__
        class TexMap;
#endif

        namespace detail {
            inline bool IsCITexelFormat(_GXTexFmt fmt) {
                return fmt == GX_TF_C4 || fmt == GX_TF_C8 || fmt == GX_TF_C14X2;
            }

            inline u8 GetHorizontalPosition(u8 packed) {
                return packed % 3;
            }

            inline u8 GetVerticalPosition(u8 packed) {
                return packed / 3;
            }

            bool IsModulateVertexColor(ut::Color*, u8);
            const ut::Color MultipleAlpha(const ut::Color, u8);
            void MultipleAlpha(ut::Color*, const ut::Color*, u8);
            void SetVertexFormat(bool, u8);
            void DrawQuad(const math::VEC2&, const Size&, u8, const math::VEC2 (*)[4], const ut::Color*);
            void DrawQuad(const math::VEC2&, const Size&, u8, const math::VEC2 (*)[4], const ut::Color*, u8);
#ifdef __3DS__
            // 3DS: a material as Material::SetupGX compiles it for DrawQuad's Citro3D draw: the
            // TEV as combiner stages with its textures, each unit's texture coordinate set and
            // SRT matrix, and where the rasterized colour comes from (GXSetChanCtrl).
            struct NativeQuadMaterial {
                HostGfxState state;
                u8 unitCount;
                u8 unitSet[3];        // DrawQuad's texture coordinate set, 0xFF: none
                f32 unitMtx[3][2][3]; // s' = m[0][0] s + m[0][1] t + m[0][2]
                bool colorFromVertex, alphaFromVertex;
                ut::Color matColor;   // the register colour, global alpha applied
            };
            // Null: DrawQuad draws vertex colours only.
            void SetNativeMaterial(const NativeQuadMaterial*);
#endif

            class TexCoordAry {
            public:
                typedef const math::VEC2 (*ConstArrayType)[4];
                TexCoordAry();

                void Free();
                void Reserve(u8);
                void SetSize(u8);
                void Copy(const void*, u8);

                bool IsEmpty() const {
                    return mCap == 0;
                }
                u8 GetSize() const {
                    return mNum;
                }
                ConstArrayType GetArray() const {
                    return mData;
                }

                u8 mCap;
                u8 mNum;
                math::VEC2 (*mData)[4];
            };

            inline const char* GetStrTableStr(const void* pTable, int idx) {
                const u32* offs = static_cast< const u32* >(pTable);
                const char* pool = static_cast< const char* >(pTable);
                return &pool[offs[idx]];
            }
        };  // namespace detail

        class ResourceAccessor;

        struct ResBlockSet {
            const res::TextureList* pTextureList;
            const res::FontList* pFontList;
            const res::MaterialList* pMaterialList;
            ResourceAccessor* pResAccessor;
        };

        namespace detail {
            bool EqualsResName(const char*, const char*);
            bool EqualsMaterialName(const char*, const char*);
            bool TestFileHeader(const res::BinaryFileHeader&, u32);

            inline bool TestFileVersion(const res::BinaryFileHeader& fileHeader) {
#if (VERSION == RMGJ01 || VERSION == RMGE01 || VERSION == RMGP01)
                u8 upper = ut::BitExtract(fileHeader.version, 8, 8);
                u8 lower = ut::BitExtract(fileHeader.version, 0, 8);
                return upper == 0 && lower >= 8 && lower <= 10;
#else
                return (ut::BitExtract(fileHeader.version, 8, 8) == 0 && ut::BitExtract(fileHeader.version, 0, 8) >= 9);
#endif
            }

            inline s32 GetSignatureInt(const char sig[4]) {
                return *reinterpret_cast< const s32* >(sig);
            }

            inline u8 GetVtxColorElement(const ut::Color* pColors, u32 idx) {
                return reinterpret_cast< const u8* >(&pColors[idx / 4])[idx % 4];
            }
            inline void SetVtxColorElement(ut::Color* pColors, u32 idx, u8 value) {
                reinterpret_cast< u8* >(&pColors[idx / 4])[idx % 4] = value;
            }

            const Size GetTextureSize(Material* pMaterial, u8 texMapIdx);

            inline void SetHorizontalPosition(u8* pVar, u8 newVal) {
#if (VERSION == RMGJ01 || VERSION == RMGE01 || VERSION == RMGP01)
                NW4R_ASSERT_AT(256, newVal < HORIZONTALPOSITION_MAX);
#else
                NW4R_ASSERT_AT(252, newVal < HORIZONTALPOSITION_MAX);
#endif
                *pVar = u8(GetVerticalPosition(*pVar) * 3 + newVal);
            }

            inline void SetVerticalPosition(u8* pVar, u8 newVal) {
#if (VERSION == RMGJ01 || VERSION == RMGE01 || VERSION == RMGP01)
                NW4R_ASSERT_AT(265, newVal < VERTICALPOSITION_MAX);
#else
                NW4R_ASSERT_AT(261, newVal < VERTICALPOSITION_MAX);
#endif
                *pVar = u8(newVal * 3 + GetHorizontalPosition(*pVar));
            }

        };  // namespace detail
    };  // namespace lyt
};  // namespace nw4r
