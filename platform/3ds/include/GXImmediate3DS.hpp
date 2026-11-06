#pragma once

// 3DS: GX immediate geometry as DirectDraw (Citro3D) submits. Included after a file's other
// headers, it turns the GXBegin ... GXEnd vertices of that file into TDDraw immediate
// primitives (DirectDraw.cpp), so the Wii's geometry code stays one copy; the file's 3DS branch
// sets the state (TDDraw::setupDirect, setDirectTexture, setDirectCombine) instead of the GX
// material calls. Only the forms the converted files use are mapped.

#include "Game/Util/DirectDraw.hpp"

#undef GXBegin
#undef GXEnd
#undef GXPosition3f32
#undef GXColor1u32
#undef GXTexCoord2f32
#undef GXNormal3f32
#undef GXCmd1f32
#undef GXLoadPosMtxImm
#define GXBegin(primitive, format, count) TDDraw::beginDirectPrimitive((primitive), (count))
#define GXEnd() TDDraw::endDirectPrimitive()
#define GXPosition3f32(x, y, z) TDDraw::directPosition((x), (y), (z))
#define GXColor1u32(color) TDDraw::directColor((color))
#define GXTexCoord2f32(s, t) TDDraw::directTexCoord((s), (t))
#define GXNormal3f32(x, y, z) TDDraw::directNormal((x), (y), (z))
#define GXCmd1f32(value) TDDraw::directTexComponent((value))
#define GXLoadPosMtxImm(mtx, id) TDDraw::loadDirectPosMtx((mtx), (id))
