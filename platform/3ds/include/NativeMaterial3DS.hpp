#pragma once

// ##############################################################################################
// # A model's material for immediate geometry                                                  #
// #                                                                                            #
// # A J3D material as the native renderer translates it (platform/3ds/gx/native.cpp), for      #
// # geometry drawn with a model's material: the Wii loads the material's display list          #
// # (J3DMatPacket::callDL) and sends the vertices with GXBegin (ElectricRailHolder).           #
// ##############################################################################################

#include "host.h"
#include <revolution/types.h>

class J3DMaterial;
class J3DShapePacket;
class J3DModelData;

// The material's combiners, textures, blend, alpha compare, Z mode and cull; its texture
// coordinate generation matrices of units 0-2 (3x4 rows, animated values included); and the
// rasterized colour of its channel when unlit (the material colour register, or white for vertex
// colours). False when the material has no native translation.
bool rvlNativeMaterialState(J3DMaterial* pMaterial, J3DShapePacket* pPacket, HostGfxState& rState, f32 (*pTexMtx)[3][4],
                            u32& rRasterColor);

// MR::drawSimpleModel: every shape of the model (its .n3d), positions only, through one view
// matrix (the Wii's loaded position matrix) and the camera projection, unlit with colour as the
// material colour, with rState's combiners and fragment state. False without a .n3d.
bool rvlNativeDrawSimpleModel(J3DModelData* pModelData, const f32 (*pViewMtx)[4], const HostGfxState& rState, u32 color);
// The same with a view matrix per position matrix slot (the Wii's GXLoadPosMtxImm into
// GX_PNMTX0 + 3 * slot); a null entry takes pViewMtx.
bool rvlNativeDrawSimpleModelSlots(J3DModelData* pModelData, const f32 (*pViewMtx)[4], const f32 (*const* pSlots)[4],
                                   const HostGfxState& rState, u32 color);

// Every texture coordinate of the next native draws through this 3x4 matrix (the Wii's
// GXLoadTexMtxImm plus GXSetTexCoordGen2 from a shape callback); null restores the material's.
void rvlNativeSetTexMtxOverride(const f32 (*pTexMtx)[4]);
