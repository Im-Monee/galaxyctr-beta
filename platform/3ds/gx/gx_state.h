#pragma once

// ##############################################################################################
// # The native draw                                                                            #
// #                                                                                            #
// # The game's J3D models are drawn natively: their vertices come pre-converted from the .n3d  #
// # files the converter wrote (native.cpp), and the state of each draw is built from the J3D   #
// # material, never from GX commands (DETAILS.md, section 5). RvlNativeDraw is one such draw,  #
// # handed from native.cpp to native_submit.cpp, which turns it into the uniforms and state of #
// # host.h; native_clip.cpp cuts the triangles the GPU cannot draw as they are.                #
// ##############################################################################################
#include "host.h"
#include <revolution/types.h>

// ##############################################################################################
// # Texture coordinate generation and the point light                                          #
// #                                                                                            #
// # How each unit's coordinates are made (GX's texgen: a matrix applied to the position, the   #
// # normal or a texture coordinate), and the player's point light (GX_LIGHT4).                 #
// ##############################################################################################
struct RvlNativeTexGen {
    bool generated;    // a matrix generation (else the coordinate is (0, 0))
    bool direct;       // vertex.tex already contains the CPU-computed J3D deferred coordinate
    u8 row;            // source: 0 position, 1 normal, 5 + n texture coordinate n
    bool stq, abc1;    // 3x4 matrix (s, t, q); input (a, b, c, 1) instead of (a, b, 1, 1)
    const f32* matrix; // 3x4 rows
    s8 srtg = -1;      // GX_TG_SRTG: the lit red and green of colour channel 0 or 1 (else -1)
};

struct ActorLightInfo;
struct PointLightInfo;

// GX_LIGHT4, the point light LightFunction::loadPointLightInfo loads before the player's draws
// (native.cpp rvlNativeLoadPointLight): view-space position, colour, distance attenuation k0-k2.
struct RvlNativePointLight {
    bool loaded;
    f32 pos[3];
    f32 k[3];
    u8 color[4];
};
void rvlNativeLoadPointLight(const PointLightInfo* pInfo);
const RvlNativePointLight* rvlNativePointLight();  // null until a point light was loaded

// ##############################################################################################
// # Runs of a large group                                                                      #
// #                                                                                            #
// # For the clipper (native_clip.cpp): a large group's triangles in runs of 32, each with a    #
// # bounding sphere, so most runs can be judged inside or outside at once.                     #
// ##############################################################################################
// A run of RVL_NATIVE_CHUNK_TRIANGLES consecutive triangles of a large group: the bounding sphere
// (centre, radius) of its vertices in the space of their position matrix row, or row 0xFF when
// they use several rows or the sphere is unknown (the run is then tested triangle by triangle).
#define RVL_NATIVE_CHUNK_TRIANGLES 32
struct RvlNativeChunk {
    f32 sphere[4];
    u32 row;
};

// ##############################################################################################
// # Indirect texturing, approximated                                                           #
// #                                                                                            #
// # The PICA has no indirect texturing (a texture that moves another texture's coordinates:    #
// # water, heat haze). The displaced sample is approximated as a blend of a few samples at     #
// # fixed offsets, weighted by the distortion texture (rvlDistortionPlan, native.cpp;          #
// # platform/3ds/HARDWARE_LIMITS.md).                                                          #
// ##############################################################################################
enum { RVL_DISTORT_NONE, RVL_DISTORT_ONE_DRAW, RVL_DISTORT_TWO_DRAWS, RVL_DISTORT_MEAN };

// A displaced sample as a piecewise linear blend of samples, GX indirect texturing being absent from
// the PICA. The Wii moves it by (value - 128) * row texels of the file's texture. The samples sit
// around the displacement of the middle of the values the distortion texture holds, step apart
// (mean, step: texture coordinates); as far as those values make them, up to a limit that keeps
// them from showing as a double image. Between samples j and j + 1 the blend is
// (value - low - j * 255 / 2^scale) * 2^scale.
struct RvlDistortion {
    f32 mean[2], step[2];
    u8 low, scale, samples;
};
struct ResTIMG;
// rowS, rowT: the indirect matrix rows' sums times 2^exponent. width, height: the texture's on the
// 3DS, halvings: down from the file's. maxSamples: 2-4; samples, when not 0: as many as another
// texture of the material has.
void rvlDistortionPlan(const ResTIMG* indirect, f32 rowS, f32 rowT, u32 width, u32 height, u32 halvings, u32 maxSamples, u32 samples, RvlDistortion& plan);

// ##############################################################################################
// # One native draw                                                                            #
// #                                                                                            #
// # Everything native_submit.cpp needs: the pixel state, the lighting channels in the Wii's    #
// # own words, the texture coordinate generation, the position matrices of the draw's slots,   #
// # the light, the vertices and indices, the bounds for culling and clipping, the units that   #
// # sample a screen capture, and the indirect texturing plan.                                  #
// ##############################################################################################
struct RvlNativeDraw {
    const HostGfxState* state;  // state built from the J3D material
    u32 unitCount, rasChannel;
    u32 colorCtrl[2], alphaCtrl[2];  // XF channel control words (0x100E-0x1011 layout)
    u32 matColor[2], ambColor[2];    // 0xRRGGBBAA
    RvlNativeTexGen gen[3];
    const f32 (*matrices[10])[4];    // view-space 3x4 position matrix of each slot
    u32 matrixCount;
    bool normals;                    // the vertices carry model-space normals
    const ActorLightInfo* actorLight; // J3D actor light selected by ActorLightCtrl
    bool ownAmbient;                 // keep ambColor[0] with an actor light (no loadLight since the material; fur)
    u32 first;                       // arena index of the vertices
    const u16* indices;              // triangle list, relative to the start of the arena window
    u32 indexCount;
    const f32 (*spheres)[4];         // model-space bounding sphere (centre, radius) of each slot's vertices
    u32 slotMask;                    // slots with vertices (bit n: spheres[n] is set); 0: bounds unknown
    const RvlNativeChunk* chunks;    // the runs of a large group (native_clip.cpp), or null
    u32 chunkCount;
    u32 captureMask;                 // units sampling a screen capture (bit u), stored on its side:
    f32 captureMap[3][4];            // their mapping u = c0 + c1 t, v = c2 + c3 s
    // GX indirect texturing (RvlDistortion), by unit 2's texture (native_submit.cpp).
    // RVL_DISTORT_ONE_DRAW: units 0 and 1 are the two samples of the material's only texture.
    // RVL_DISTORT_TWO_DRAWS: units in distortUnits take each sample in a draw of its own, the first
    // the material as it is, each next one over the last with unit 2 as its alpha (distortDraw).
    // RVL_DISTORT_MEAN: no room for two samples (a third texture, blending): units in distortUnits
    // only get the displacement of the middle of the values.
    u8 distort;
    u8 distortUnits;
    u8 distortDraw, distortSamples;
    u8 distortLow, distortScale;
    f32 distortMean[3][2], distortStep[3][2];
};

// ##############################################################################################
// # Submitting, clipping and the frame end                                                     #
// #                                                                                            #
// # The functions of native_submit.cpp, native_clip.cpp and native.cpp that the rest of the    #
// # native renderer calls.                                                                     #
// ##############################################################################################
// Draws with the GPU shader (shaders/gx.v.pica); false when the host rejects the submission.
bool rvlNativeDrawSubmit(const RvlNativeDraw& draw);
// What a native draw needs (native_clip.cpp): nothing (straight to the GPU), the CPU clipper
// (some slot's bounding sphere may reach behind the near plane or past the guard band: the GPU
// cannot clip it), or nothing at all (every slot's sphere wholly outside the view: culled).
enum { NATIVE_CLIP_NONE, NATIVE_CLIP_CPU, NATIVE_CLIP_CULL };
int rvlNativeClipNeeded(const RvlNativeDraw& draw, const HostGfxTransform& transform);
// Draws the triangles wholly inside as they are and the others clipped on the CPU; false when the
// host rejects the submission.
// Draws with a projected texture coordinate on unit 1 or 2 (divided at the vertex): their triangles
// whose q changes a lot are cut into smaller ones (native_clip.cpp).
bool rvlNativeSubdivideNeeded(const HostGfxTransform& transform);
bool rvlNativeSubdivideSubmit(const RvlNativeDraw& draw, const HostGfxState* pState, const HostGfxTransform& transform, u32 version);
bool rvlNativeClipSubmit(const RvlNativeDraw& draw, const HostGfxState* pState, const HostGfxTransform& transform,
                         u32 transformVersion);
// Counters for the periodic log: draws that went through the clipper, triangles clipped.
void rvlNativeClipTakeStats(u32& draws, u32& triangles, u32& invalid, u32& invalidMatrixDraws, u32& culled, u64& ticks);

// The camera projection's vertical scale (cot(fovy / 2)) for the screen size of far models
// (native.cpp rvlNativeFarPacket); 0 while another projection is set (MR::drawInitFor2DModel).
f32 rvlNativeProjectionScaleY();

// Frame end of the native renderer (native.cpp): cache ageing, and its profile log.
void rvlNativeEndFrame();
void rvlNativeLogFrame();
