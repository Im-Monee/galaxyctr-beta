#include "nw4r/lyt/common.h"
#include "nw4r/lyt/texMap.h"
#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXGet.h>
#include <revolution/gx/GXStruct.h>
#include <revolution/tpl.h>
// 3DS: JUTTexture3DS.hpp, GPU textures for JUTTextures.
#ifdef __3DS__
#include "JUTTexture3DS.hpp"
#endif

namespace nw4r {
    namespace lyt {
        void TexMap::Get(_GXTexObj* pTexObj) const {
            // 3DS: the texture object kept as data for the Citro3D draws (rvl/jut_texture.cpp).
#ifdef __3DS__
            rvl3DSInitTexObj(pTexObj, mImage, mWidth, mHeight, GetTexelFormat(), GetWrapModeS(), GetWrapModeT(), IsMipMap(),
                             detail::IsCITexelFormat(GetTexelFormat()) ? GX_TLUT0 : 0xFF);
            rvl3DSInitTexObjLOD(pTexObj, GetMinFilter(), GetMagFilter(), GetMinLOD(), GetMaxLOD(), GetLODBias(), IsBiasClampEnable(),
                                IsEdgeLODEnable(), GetAnisotropy());
            return;
#else
            if (detail::IsCITexelFormat(GetTexelFormat())) {
                u32 tlutName = GXGetTexObjTlut(pTexObj);
                GXInitTexObjCI(pTexObj, mImage, mWidth, mHeight, GXCITexFmt(GetTexelFormat()), GetWrapModeS(), GetWrapModeT(), IsMipMap(), tlutName);
            } else {
                GXInitTexObj(pTexObj, mImage, mWidth, mHeight, GetTexelFormat(), GetWrapModeS(), GetWrapModeT(), IsMipMap());
            }

            GXInitTexObjLOD(pTexObj, GetMinFilter(), GetMagFilter(), GetMinLOD(), GetMaxLOD(), GetLODBias(), IsBiasClampEnable(), IsEdgeLODEnable(),
                            GetAnisotropy());
#endif
        }

        void TexMap::Get(_GXTlutObj* pTlutObj) const {
#ifndef __3DS__  // 3DS: palettes are expanded when the texture is converted
            GXInitTlutObj(pTlutObj, GetPalette(), GetPaletteFormat(), GetPaletteEntryNum());
#endif
        }

        void TexMap::Set(const GXTexObj& texObj) {
            void* image;
            u16 width, height;
            GXTexFmt format;
            GXTexWrapMode wrapS, wrapT;
            GXBool mipmap;

            // 3DS: the texture object read back from its data (rvl/jut_texture.cpp).
#ifdef __3DS__
            rvl3DSGetTexObjAll(&texObj, &image, &width, &height, &format, &wrapS, &wrapT, &mipmap);
#else
            GXGetTexObjAll(&texObj, &image, &width, &height, &format, &wrapS, &wrapT, &mipmap);
#endif

            mImage = image;
            SetSize(width, height);
            mBits.textureFormat = format;
            SetWrapMode(wrapS, wrapT);
            SetMipMap(mipmap);

            GXTexFilter minFilter, magFilter;
            f32 minLOD, maxLOD, lodBias;
            GXBool biasCLampEnable, edgeLODEnable;
            GXAnisotropy aniso;
            // 3DS: the texture object read back from its data (rvl/jut_texture.cpp).
#ifdef __3DS__
            rvl3DSGetTexObjLODAll(&texObj, &minFilter, &magFilter, &minLOD, &maxLOD, &lodBias, &biasCLampEnable, &edgeLODEnable, &aniso);
#else
            GXGetTexObjLODAll(&texObj, &minFilter, &magFilter, &minLOD, &maxLOD, &lodBias, &biasCLampEnable, &edgeLODEnable, &aniso);
#endif

            SetFilter(minFilter, magFilter);
            SetLOD(minLOD, maxLOD);
            SetLODBias(lodBias);
            SetBiasClampEnable(biasCLampEnable);
            SetEdgeLODEnable(edgeLODEnable);
            mBits.anisotropy = aniso;
        }

        void TexMap::ReplaceImage(const TPLDescriptor* pTPLDesc) {
            const TPLHeader& header = *pTPLDesc->textureHeader;
            mImage = header.data;
            SetSize(header.width, header.height);
            SetTexelFormat(GXTexFmt(header.format));

            if (const TPLClutHeader* const pClut = pTPLDesc->CLUTHeader) {
                SetPalette(pClut->data);
                SetPaletteFormat(pClut->format);
                SetPaletteEntryNum(pClut->numEntries);
            } else {
                SetPalette(nullptr);
                SetPaletteFormat(GXTlutFmt(0));
                SetPaletteEntryNum(0);
            }
        }

        void TexMap::ReplaceImage(TPLPalette* p, u32 id) {
            if (reinterpret_cast< u32 >(p->descriptorArray) < 0x80000000) {
                TPLBind(p);
            }

            ReplaceImage(TPLGet(p, id));
        }
    };  // namespace lyt
};  // namespace nw4r
