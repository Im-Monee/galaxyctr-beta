#pragma once

#include <revolution/types.h>
#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXStruct.h>

class JUTTexture;
struct ResTIMG;

// ##############################################################################################
// # Textures for the GPU                                                                       #
// #                                                                                            #
// # The interface of platform/3ds/rvl/jut_texture.cpp: GPU textures for JUTTextures, native    #
// # model textures and images, screen captures, and GXTexObj kept as data.                     #
// ##############################################################################################
// Direct Citro3D conversion of a JUTTexture, and shared CPU scratch space for texture decode.
void* rvl3DSGetJUTTexture(const JUTTexture* texture);
// The texture of a J3D ResTIMG, for the native model renderer (platform/3ds/gx/native.cpp).
void* rvl3DSGetNativeTIMGTexture(const ResTIMG* timg);
// The screen capture whose JUTTexture image the ResTIMG names (a model texture replaced with a
// capture's, as ShadowProjDummy with Mario's shadow), with its coordinate mapping (u = c0 + c1 t,
// v = c2 + c3 s, rvl3DSMapJUTTextureCoord); null when it is not one.
void* rvl3DSGetNativeTIMGCapture(const ResTIMG* timg, f32* coeffs);
void* rvl3DSGetNativeTexture(const void* image, u16 width, u16 height, u32 format, const void* palette, u32 paletteFormat,
                             u16 paletteEntries, u8 wrapS, u8 wrapT, u8 magFilter);
bool rvl3DSIsEfbCopy(const JUTTexture* texture);
// Forgets the textures made from this image (its texels were rewritten in place, as a Mii icon's).
void rvl3DSInvalidateTextureImage(const void* image);
void rvl3DSCaptureJUTTexture(JUTTexture* texture);
// Captures the Wii frame buffer rectangle [x0, x1) x [y0, y1) (640x456) into the texture.
void rvl3DSCaptureJUTTextureRegion(JUTTexture* texture, f32 x0, f32 y0, f32 x1, f32 y1);
// Whether the last capture above really copied the screen. In a frame the renderer skips
// (host/gfx.c, sSkipFrame) nothing is copied and the texture keeps its old image: a one-time
// capture request (CaptureScreenDirector, GalaxyMapController) must then stay pending.
bool rvl3DSLastCaptureDone();
// The texture coordinate to sample for (s, t) over the texture: a capture is stored on its side
// (the top screen's frame buffer is portrait), other textures as they are.
void rvl3DSMapJUTTextureCoord(const JUTTexture* texture, f32 s, f32 t, f32& u, f32& v);
void rvl3DSInvalidateJUTTexture(const JUTTexture* texture);
u32* rvl3DSTextureScratch();
// GXGetTexBufferSize(width, height, format, GX_FALSE, 1).
u32 rvl3DSTexBufferSize(u16 width, u16 height, u32 format);

// A GXTexObj as a plain record of its texture (the 3DS has no GX texture registers): what
// GXInitTexObj(CI)/GXInitTexObjLOD store and GXGetTexObjAll/GXGetTexObjLODAll/GXGetTexObjTlut
// read back. tlut is 0xFF for a non-indexed texture.
void rvl3DSInitTexObj(GXTexObj* obj, void* image, u16 width, u16 height, u32 format, u32 wrapS, u32 wrapT, bool mipmap, u32 tlut);
void rvl3DSInitTexObjLOD(GXTexObj* obj, u32 minFilter, u32 magFilter, f32 minLod, f32 maxLod, f32 lodBias, bool biasClamp, bool edgeLod,
                         u32 anisotropy);
void rvl3DSGetTexObjAll(const GXTexObj* obj, void** image, u16* width, u16* height, GXTexFmt* format, GXTexWrapMode* wrapS,
                        GXTexWrapMode* wrapT, GXBool* mipmap);
void rvl3DSGetTexObjLODAll(const GXTexObj* obj, GXTexFilter* minFilter, GXTexFilter* magFilter, f32* minLod, f32* maxLod, f32* lodBias,
                           GXBool* biasClamp, GXBool* edgeLod, GXAnisotropy* anisotropy);
u32 rvl3DSGetTexObjTlut(const GXTexObj* obj);
