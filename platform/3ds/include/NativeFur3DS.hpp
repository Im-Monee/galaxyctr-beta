#pragma once

// ##############################################################################################
// # Fur on the 3DS                                                                             #
// #                                                                                            #
// # FurCtrl and FurDrawer: each layer model's shape is drawn by the native J3D renderer        #
// # (platform/3ds/gx/native.cpp) with the layer's CPU-made positions (CShader) and the fur's   #
// # own material instead of the model's, as FurDrawer::setupMaterial sets it on the Wii.       #
// ##############################################################################################

#include "host.h"
#include <revolution/mtx.h>
#include <revolution/types.h>

class J3DModel;
struct J3DFogInfo;
struct ActorLightInfo;

struct RvlNativeFurMaterial {
    HostGfxState state;      // combiners (body x lighting + konst, then fur), textures, blend, Z, cull
    u32 colorCtrl, alphaCtrl;  // channel 0, XF layout (0x100E): enable, sources, light mask, functions
    u32 matColor, ambColor;    // 0xRRGGBBAA
    Mtx texMtx[2];             // texture matrices of the body (unit 0) and fur (unit 1) coordinates
    const J3DFogInfo* fog;     // the fog control's, or null for none
};

// The fur material being set up (FurDrawer.cpp).
RvlNativeFurMaterial& rvl3DSFurMaterial();
// Draws shape shapeIndex of a fur layer model with the fur material; false when it cannot.
bool rvlNativeDrawFurLayer(J3DModel* pLayer, u32 shapeIndex, const RvlNativeFurMaterial& rMaterial, const ActorLightInfo* pActorLight);
