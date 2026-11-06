// Native J3D draws (block N): the GPU transform of a draw of .n3d vertices (native.cpp), built
// from the J3D material's channels and coordinate generations, the model's position matrices and
// the camera, then submitted with the shader shaders/gx.v.pica. Also the frame end of the 3DS
// renderer: its counters, the frame phase and category timings, and the periodic log.
//
// Moved from draw.cpp and processor.cpp when the GX command interpreter left the 3DS target
// (block 8): no GX register is read here.

#include "gx_internal.h"
#include "gx_state.h"
#include "host.h"
#include "GXDelta3DS.hpp"
#include "GXToCitro3D.hpp"
#include "JPADraw3DS.hpp"
#include <Game/Map/LightDataHolder.hpp>
#include <Game/Util/CameraUtil.hpp>

#include <string.h>

RvlGXProfile gGXProfile;
bool gGXProfileOn;

namespace {

    // One light of a lighting channel, as the shader takes it.
    struct LightTerm {
        f32 pos[3], dir[3];
        f32 angular[3], distance[3];  // attenuation coefficients
        f32 color[4];                 // 0..255
    };

    struct LightList {
        u32 count;
        u32 diffuse;
        bool attn;
        LightTerm lights[8];
    };

    // One lighting channel: material and ambient sources, the lights.
    struct ChannelPlan {
        bool colorLit, alphaLit;
        bool colorFromVertex, alphaFromVertex;        // material from the vertex colour
        bool ambColorFromVertex, ambAlphaFromVertex;  // ambient from the vertex colour
        u8 material[4], ambient[4];
        LightList colorLights, alphaLights;
    };

    // Lights the shader holds (five uniform rows each).
    const u32 kGpuLightSlots = 6;
    const f32 kTexIdentity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

    void unpackColor(u32 rgba, u8* out) {
        out[0] = rgba >> 24;
        out[1] = rgba >> 16;
        out[2] = rgba >> 8;
        out[3] = rgba;
    }

    void setVec4(f32* out, f32 x, f32 y, f32 z, f32 w) {
        out[0] = x;
        out[1] = y;
        out[2] = z;
        out[3] = w;
    }

    // The channel of XF channel control words (0x100E-0x1011 layout), without lights.
    void planChannel(u32 colorCtrl, u32 alphaCtrl, const u8* material, const u8* ambient, ChannelPlan& p) {
        p.colorLit = (colorCtrl >> 1) & 1;
        p.alphaLit = (alphaCtrl >> 1) & 1;
        p.colorFromVertex = colorCtrl & 1;
        p.alphaFromVertex = alphaCtrl & 1;
        p.ambColorFromVertex = (colorCtrl >> 6) & 1;
        p.ambAlphaFromVertex = (alphaCtrl >> 6) & 1;
        for (u32 i = 0; i < 4; i++) {
            p.material[i] = material[i];
            p.ambient[i] = ambient[i];
        }
        p.colorLights.count = p.alphaLights.count = 0;
    }

    // The lights of an ActorLightInfo the control word enables (lights 0 and 1, and light 2's
    // alpha), in view space, and GX_LIGHT4, the point light (rvlNativePointLight), with its
    // distance attenuation.
    void setLightList(LightList& list, u32 control, const ActorLightInfo& info, bool alpha) {
        // Only the lights used are cleared: the whole list is about 0.5 KB, twice a draw.
        list.count = 0;
        const u32 mask = ((control >> 2) & 0xF) | (((control >> 11) & 0xF) << 4);
        list.diffuse = (control >> 7) & 3;
        list.attn = true;
        for (u32 id = 0; id < 3; id++) {
            if ((mask & (1u << id)) == 0) {
                continue;
            }
            LightTerm& light = list.lights[list.count++];
            memset(&light, 0, sizeof(light));
            light.angular[0] = light.distance[0] = 1.0f;
            if (id < 2) {
                const LightInfo& source = id == 0 ? info.mInfo0 : info.mInfo1;
                TVec3f position(source.mPos);
                if (!source.mIsFollowCamera) {
                    MR::getCameraViewMtx().mult(position, position);
                }
                light.pos[0] = position.x;
                light.pos[1] = position.y;
                light.pos[2] = position.z;
                const u8* color = reinterpret_cast< const u8* >(&source.mColor);
                for (u32 c = 0; c < 4; c++) {
                    light.color[c] = color[c];
                }
            } else if (alpha) {
                light.color[3] = info.mAlpha2;
            }
        }
        const RvlNativePointLight* point = rvlNativePointLight();
        if ((mask & (1u << 4)) != 0 && point != nullptr) {
            LightTerm& light = list.lights[list.count++];
            memset(&light, 0, sizeof(light));
            light.angular[0] = 1.0f;  // GX_SP_OFF
            for (u32 c = 0; c < 3; c++) {
                light.pos[c] = point->pos[c];
                light.distance[c] = point->k[c];
            }
            for (u32 c = 0; c < 4; c++) {
                light.color[c] = point->color[c];
            }
        }
    }

    // rvlNativeSetViewport: a projection already folded with its viewport, and a scissor.
    bool sViewportSet;
    f32 sViewportProjection[4][4];
    u16 sViewportScissor[4];  // x0, y0, x1, y1

    // View space to the 3DS clip space: the camera projection (or rvlNativeSetViewport's), turned
    // for the top screen's portrait frame buffer (rows y, -x, z, w).
    void setViewToClip(HostGfxTransform& t) {
        const f32(*projection)[4] = sViewportSet ? sViewportProjection : *MR::getCameraProjectionMtx().toCMtx44();
        for (u32 k = 0; k < 4; k++) {
            t.clip[0][k] = projection[1][k];
            t.clip[1][k] = -projection[0][k];
            t.clip[2][k] = projection[2][k];
            t.clip[3][k] = projection[3][k];
        }
    }

    // Fills t but its view-to-clip matrix and palette; false when the shader cannot hold the lights.
    bool buildTransform(const RvlNativeDraw& d, const ChannelPlan& c, HostGfxTransform& t) {
        if (c.colorLights.count + c.alphaLights.count > kGpuLightSlots) {
            gGXProfile.cpuLights++;
            return false;
        }

        for (u32 u = 0; u < 3; u++) {
            f32(*m)[4] = &t.texmtx[u * 3];
            f32(*post)[4] = &t.texpost[u * 3];
            f32(*fin)[4] = &t.texfinal[u * 2];
            t.normalize[u] = 0;
            t.divide[u] = 0;
            t.vtxmtx[u] = 0;
            t.srtg[u] = 0.0f;
            setVec4(post[0], 1.0f, 0.0f, 0.0f, 0.0f);
            setVec4(post[1], 0.0f, 1.0f, 0.0f, 0.0f);
            setVec4(post[2], 0.0f, 0.0f, 1.0f, 0.0f);
            if (u >= d.unitCount) {
                for (u32 r = 0; r < 3; r++) {
                    setVec4(m[r], 0.0f, 0.0f, 0.0f, 0.0f);
                }
                setVec4(fin[0], 0.0f, 0.0f, 0.0f, 0.0f);
                setVec4(fin[1], 0.0f, 0.0f, 0.0f, 0.0f);
                continue;
            }

            const RvlNativeTexGen& g = d.gen[u];
            // GX_TG_SRTG: the lit colour's red and green, of the channel this pass rasterizes (the
            // shader has no other); its matrix rows stay zero below.
            t.srtg[u] = g.srtg >= 0 && static_cast< u32 >(g.srtg) == d.rasChannel ? 1.0f : 0.0f;
            // A deferred J3D texture matrix was applied while decoding the vertices: identity.
            const bool generated = g.direct || g.generated;
            const bool stq = !g.direct && g.stq;
            const f32* x = g.direct ? &kTexIdentity[0][0] : g.matrix;
            if (generated) {
                setVec4(m[0], x[0], x[1], x[2], x[3]);
                setVec4(m[1], x[4], x[5], x[6], x[7]);
                if (stq) {
                    setVec4(m[2], x[8], x[9], x[10], x[11]);
                } else {
                    setVec4(m[2], 0.0f, 0.0f, 0.0f, 1.0f);
                }
                t.divide[u] = stq;
            } else {
                setVec4(m[0], 0.0f, 0.0f, 0.0f, 0.0f);
                setVec4(m[1], 0.0f, 0.0f, 0.0f, 0.0f);
                setVec4(m[2], 0.0f, 0.0f, 0.0f, 1.0f);
            }
            if (d.captureMask & (1 << u)) {
                // A screen capture is stored on its side (rvl3DSMapJUTTextureCoord).
                const f32* k = d.captureMap[u];
                setVec4(fin[0], 0.0f, k[1], k[0], 0.0f);
                setVec4(fin[1], k[3], 0.0f, k[2], 0.0f);
            } else {
                setVec4(fin[0], 1.0f, 0.0f, 0.0f, 0.0f);
                setVec4(fin[1], 0.0f, 1.0f, 0.0f, 0.0f);
            }
            f32 side = 0.0f;
            bool displaced = false;
            if (d.distort == RVL_DISTORT_ONE_DRAW && u < 2) {
                side = u == 0 ? -0.5f : 0.5f;
                displaced = true;
            } else if (d.distort != RVL_DISTORT_NONE && d.distort != RVL_DISTORT_ONE_DRAW && (d.distortUnits & (1 << u))) {
                side = d.distort == RVL_DISTORT_MEAN ? 0.0f : static_cast< f32 >(d.distortDraw) - 0.5f * static_cast< f32 >(d.distortSamples - 1);
                displaced = true;
            }
            if (displaced) {
                const u32 at = d.distort == RVL_DISTORT_ONE_DRAW ? 0 : u;
                const f32 ds = d.distortMean[at][0] + side * d.distortStep[at][0], dt = d.distortMean[at][1] + side * d.distortStep[at][1];
                if (d.captureMask & (1 << u)) {
                    fin[0][2] += d.captureMap[u][1] * dt;
                    fin[1][2] += d.captureMap[u][3] * ds;
                } else {
                    fin[0][2] += ds;
                    fin[1][2] += dt;
                }
            }
        }

        // Lighting: colours in 0..1, sources chosen with masks.
        const f32 k255 = 1.0f / 255.0f;
        setVec4(t.material, c.material[0] * k255, c.material[1] * k255, c.material[2] * k255, c.material[3] * k255);
        setVec4(t.ambient, c.ambient[0] * k255, c.ambient[1] * k255, c.ambient[2] * k255, c.ambient[3] * k255);
        f32 mc = c.colorFromVertex ? 1.0f : 0.0f, ma = c.alphaFromVertex ? 1.0f : 0.0f;
        setVec4(t.maskmat, mc, mc, mc, ma);
        mc = c.ambColorFromVertex ? 1.0f : 0.0f, ma = c.ambAlphaFromVertex ? 1.0f : 0.0f;
        setVec4(t.maskamb, mc, mc, mc, ma);
        mc = c.colorLit ? 1.0f : 0.0f, ma = c.alphaLit ? 1.0f : 0.0f;
        setVec4(t.masklit, mc, mc, mc, ma);
        t.lit = c.colorLit || c.alphaLit;

        // Each light of each list takes a slot: position, direction (w: 1 - k), angular
        // attenuation (w: lowest diffuse factor), distance attenuation (w: k), colour. The
        // diffuse factor is max(n.l * k + 1 - k, lowest).
        u32 slot = 0;
        for (u32 list = 0; list < 2; list++) {
            const LightList& lights = list == 0 ? c.colorLights : c.alphaLights;
            f32 k = lights.diffuse != 0 ? 1.0f : 0.0f;
            f32 lowest = lights.diffuse == 2 ? 0.0f : -1e10f;
            for (u32 i = 0; i < lights.count; i++, slot++) {
                const LightTerm& l = lights.lights[i];
                f32(*s)[4] = &t.lights[slot * 5];
                setVec4(s[0], l.pos[0], l.pos[1], l.pos[2], 0.0f);
                setVec4(s[1], l.dir[0], l.dir[1], l.dir[2], 1.0f - k);
                if (lights.attn) {
                    setVec4(s[2], l.angular[0], l.angular[1], l.angular[2], lowest);
                    setVec4(s[3], l.distance[0], l.distance[1], l.distance[2], k);
                } else {
                    setVec4(s[2], 1.0f, 0.0f, 0.0f, lowest);
                    setVec4(s[3], 1.0f, 0.0f, 0.0f, k);
                }
                if (list == 0) {
                    setVec4(s[4], l.color[0] * k255, l.color[1] * k255, l.color[2] * k255, 0.0f);
                } else {
                    setVec4(s[4], 0.0f, 0.0f, 0.0f, l.color[3] * k255);
                }
            }
        }
        t.lightCount = static_cast< u8 >(slot);
        return true;
    }

    // A displaced sample as a blend of two samples (RvlDistortion), the distortion texture's value
    // choosing between them. The material's own stages then take the result in place of its
    // texture, from the previous stage when only the first reads it, else from the combiner buffer
    // (setupDistortion checked them).
    void addDistortionStages(HostGfxState& st, const RvlNativeDraw& d) {
        const u32 kTexture0 = 3, kTexture1 = 4, kTexture2 = 5, kBuffer = 0x0D, kConstant = 0x0E, kPrevious = 0x0F;
        if (st.stageCount + 2 > HOST_GFX_STAGES) {
            return;
        }
        bool laterReads = false;
        for (u32 i = 1; i < st.stageCount; i++) {
            for (u32 k = 0; k < 3; k++) {
                laterReads |= st.stages[i].colorSrc[k] == kTexture0 || st.stages[i].alphaSrc[k] == kTexture0;
            }
        }
        const u32 result = laterReads ? kBuffer : kPrevious;
        for (u32 i = st.stageCount; i-- > 0;) {
            st.stages[i + 2] = st.stages[i];
            HostGfxStage& moved = st.stages[i + 2];
            for (u32 k = 0; k < 3; k++) {
                moved.colorSrc[k] = moved.colorSrc[k] == kTexture0 ? result : moved.colorSrc[k];
                moved.alphaSrc[k] = moved.alphaSrc[k] == kTexture0 ? result : moved.alphaSrc[k];
            }
        }
        if (laterReads) {
            st.bufferUpdateRgb = st.bufferUpdateAlpha = 1 << 1;
        }
        memset(&st.stages[0], 0, sizeof(st.stages[0]) * 2);
        HostGfxStage& choice = st.stages[0];
        choice.colorSrc[0] = kTexture2;
        choice.colorSrc[1] = kConstant;
        choice.colorFunc = 5;   // GPU_SUBTRACT
        choice.colorScale = d.distortScale;
        choice.constant = d.distortLow * 0x01010101u;
        HostGfxStage& blend = st.stages[1];
        blend.colorSrc[0] = kTexture1;
        blend.colorSrc[1] = kTexture0;
        blend.colorSrc[2] = kPrevious;
        blend.colorFunc = 4;  // GPU_INTERPOLATE
        blend.alphaSrc[0] = kTexture0;
        st.stageCount += 2;
    }

    // A next draw of RVL_DISTORT_TWO_DRAWS: over the pixels of the draw before, by the same choice as
    // addDistortionStages for that pair of samples, so that the result is their blend. The material replaces what
    // is behind it, so nothing reads its alpha: the last stage's alpha becomes the choice, with the
    // constant's alpha (setupDistortion checked that its colour does not read that).
    void setSecondDistortionDraw(HostGfxState& st, const RvlNativeDraw& d) {
        HostGfxStage& last = st.stages[st.stageCount - 1];
        last.alphaSrc[0] = 5;     // GPU_TEXTURE2
        last.alphaOp[0] = 2;      // GPU_TEVOP_A_SRC_R
        last.alphaSrc[1] = 0x0E;  // GPU_CONSTANT
        last.alphaOp[1] = 0;      // GPU_TEVOP_A_SRC_ALPHA
        last.alphaFunc = 5;       // GPU_SUBTRACT
        last.alphaScale = d.distortScale;
        const u32 low = d.distortLow + (d.distortDraw - 1) * 255 / (1u << d.distortScale);
        last.constant = (last.constant & 0x00FFFFFFu) | (low << 24);
        int logic;
        GXToCitro3D::blendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET, st.blendEquation, st.blendSrc, st.blendDst, logic);
        st.alphaTestFunc = 1;  // GPU_ALWAYS
        st.alphaRef = 0;
        st.depthFunc = 2;      // GPU_EQUAL
        st.writeMask = GXToCitro3D::WRITE_RGB;
    }

    HostGfxState sState;
    HostGfxTransform sTransform;
    HostGfxTransform sPreviousTransform;
    bool sPreviousTransformValid;
    u32 sPreviousTransformVersion;
    u32 sSerial = 0x80000000u;  // transform versions
    u32 sFrameDraws;
    u64 sVisibilityTicks;  // rvlNativeClipNeeded, since the last log
    // rvlNativeDrawSubmit before the visibility test (state copy, matrices) and after it (lights,
    // transform, version), since the last log.
    u64 sPrepareTicks, sBuildTicks;

}  // namespace

bool rvlNativeDrawSubmit(const RvlNativeDraw& d) {
    const u64 prepareStart = gGXProfileOn ? hostGetTick() : 0;
    sState = *d.state;
    if (d.distort == RVL_DISTORT_ONE_DRAW) {
        addDistortionStages(sState, d);
    } else if (d.distort == RVL_DISTORT_TWO_DRAWS && d.distortDraw != 0) {
        setSecondDistortionDraw(sState, d);
    }
    sState.scissor[0] = sViewportSet ? sViewportScissor[0] : 0;
    sState.scissor[1] = sViewportSet ? sViewportScissor[1] : 0;
    sState.scissor[2] = sViewportSet ? sViewportScissor[2] : 640;
    sState.scissor[3] = sViewportSet ? sViewportScissor[3] : 456;

    // The view-to-clip matrix and the palette first: draws wholly outside the view (or too small,
    // native_clip.cpp) are dropped before their lights and material are built.
    HostGfxTransform& t = sTransform;
    memset(&t, 0, sizeof(t));
    setViewToClip(t);
    const u32 matrices = d.matrixCount < 10 ? d.matrixCount : 10;
    for (u32 m = 0; m < matrices; m++) {
        const f32(*rows)[4] = d.matrices[m];
        for (u32 r = 0; r < 3; r++) {
            setVec4(t.palette[m * 3 + r], rows[r][0], rows[r][1], rows[r][2], rows[r][3]);
        }
    }
    // Only the rows the draw's vertices use are compared and sent (gfx.c uploadTransform).
    t.paletteRows = static_cast< u8 >(matrices != 0 ? matrices * 3 : 3);

    u64 start = gGXProfileOn ? hostGetTick() : 0;
    if (gGXProfileOn) {
        sPrepareTicks += start - prepareStart;
    }
    // Draws that may reach behind the camera or far off screen are clipped first (native_clip.cpp):
    // the GPU clips only against near and far, and such triangles stall it.
    // Draws wholly outside the view are not sent at all.
    const int clip = rvlNativeClipNeeded(d, t);
    if (gGXProfileOn) {
        sVisibilityTicks += hostGetTick() - start;
    }
    if (clip == NATIVE_CLIP_CULL) {
        return true;
    }

    const u64 buildStart = gGXProfileOn ? hostGetTick() : 0;
    const u32 ch = d.rasChannel & 1;
    u8 material[4], ambient[4];
    unpackColor(d.matColor[ch], material);
    unpackColor(d.ambColor[ch], ambient);
    if (d.actorLight != nullptr && ch == 0 && !d.ownAmbient) {
        memcpy(ambient, &d.actorLight->mColor, sizeof(ambient));
    }
    ChannelPlan c;
    planChannel(d.colorCtrl[ch], d.alphaCtrl[ch], material, ambient, c);
    if (d.actorLight != nullptr) {
        setLightList(c.colorLights, d.colorCtrl[ch], *d.actorLight, false);
        setLightList(c.alphaLights, d.alphaCtrl[ch], *d.actorLight, true);
    }
    if (!buildTransform(d, c, t)) {
        return false;
    }
    t.nrmpalette = d.normals && t.lit;
    t.nrmcofactor = t.nrmpalette;

    u32 version;
    if (sPreviousTransformValid && memcmp(&t, &sPreviousTransform, sizeof(t)) == 0) {
        version = sPreviousTransformVersion;
    } else {
        version = sSerial++;
        sPreviousTransform = t;
        sPreviousTransformVersion = version;
        sPreviousTransformValid = true;
    }
    start = gGXProfileOn ? hostGetTick() : 0;
    if (gGXProfileOn) {
        sBuildTicks += start - buildStart;
    }
    bool submitted = clip == NATIVE_CLIP_CPU       ? rvlNativeClipSubmit(d, &sState, t, version)
                     : rvlNativeSubdivideNeeded(t) ? rvlNativeSubdivideSubmit(d, &sState, t, version)
                                                   : hostGfxDrawNativeIndexed(d.first, d.indices, d.indexCount, &sState, &t, version) != 0;
    if (gGXProfileOn) {
        gGXProfile.host += hostGetTick() - start;
    }
    if (submitted) {
        gGXProfile.nativeDraws++;
        sFrameDraws++;
    }
    return submitted;
}

f32 rvlNativeProjectionScaleY() {
    if (sViewportSet) {
        return 0.0f;
    }
    return (*MR::getCameraProjectionMtx().toCMtx44())[1][1];
}

void rvlNativeSetViewport(const f32 (*pProjection)[4], const f32* pViewport, const u16* pScissor) {
    sViewportSet = pProjection != nullptr;
    if (!sViewportSet) {
        return;
    }
    if (pViewport != nullptr) {
        rvlViewportProjection(sViewportProjection, pProjection, pViewport[0], pViewport[1], pViewport[2], pViewport[3]);
    } else {
        memcpy(sViewportProjection, pProjection, sizeof(sViewportProjection));
    }
    sViewportScissor[0] = pScissor != nullptr ? pScissor[0] : 0;
    sViewportScissor[1] = pScissor != nullptr ? pScissor[1] : 0;
    sViewportScissor[2] = pScissor != nullptr ? pScissor[0] + pScissor[2] : 640;
    sViewportScissor[3] = pScissor != nullptr ? pScissor[1] + pScissor[3] : 456;
}

// Frame phases and categories -------------------------------------------------------------------

namespace {
    volatile int sDebugFramePhase;

    // Game frame phases (GameSystem::frameLoop marks them): wall ticks of each.
    const int kFramePhases = 6;
    u64 sPhaseTicks[kFramePhases], sPhaseStart;
    u64 sLongestDrawTicks;

    // Scene category timing (profile=1; CategoryList in SceneFunction.cpp): time per kind
    // (movement, calcAnim, draw, drawOpa, drawXlu, entry) and type, inclusive of nested calls.
    // Kind "sys" times parts of the update outside the categories (SceneExecutor.cpp,
    // GameSystem.cpp), inclusive of the categories they run.
    const int kCategoryKinds = 7, kCategoryTypes = 128;
    u64 sCategoryTicks[kCategoryKinds][kCategoryTypes];

    // Each kind's total and the sixteen costliest categories since the last log.
    void logCategories(u64 ms) {
        static const char* const kKinds[kCategoryKinds] = {"move", "anim", "draw", "opa", "xlu", "entry", "sys"};
        hostLog("Game category totals ms:");
        for (int k = 0; k < kCategoryKinds; k++) {
            u64 total = 0;
            for (int t = 0; t < kCategoryTypes; t++) {
                total += sCategoryTicks[k][t];
            }
            hostLog(" %s=%lu", kKinds[k], static_cast< u32 >(total / ms));
        }
        hostLog("%s", "\n");
        // Mario's parts (SysTimer3DS.hpp), always: in the list below they fall past its end.
        static const int kMarioParts[] = {34, 57, 33, 50, 51, 52, 53, 54, 30, 59, 60, 31, 32, 61, 62, 63, 55, 56, 58};
        // os_thread.cpp rvlInterruptsEnabled, on the game thread: handlers, then the woken threads.
        hostLog("Interrupts ms: handlers %lu, woken threads %lu\n", static_cast< u32 >(sCategoryTicks[6][64] / ms),
                static_cast< u32 >(sCategoryTicks[6][65] / ms));
        hostLog("Mario parts ms:");
        for (int t : kMarioParts) {
            hostLog(" %d=%lu", t, static_cast< u32 >(sCategoryTicks[6][t] / ms));
        }
        hostLog("%s", "\n");
        hostLog("Game categories ms:");
        for (int n = 0; n < 26; n++) {
            int bestK = -1, bestT = 0;
            for (int k = 0; k < kCategoryKinds; k++) {
                for (int t = 0; t < kCategoryTypes; t++) {
                    if (sCategoryTicks[k][t] != 0 && (bestK < 0 || sCategoryTicks[k][t] > sCategoryTicks[bestK][bestT])) {
                        bestK = k;
                        bestT = t;
                    }
                }
            }
            if (bestK < 0) {
                break;
            }
            hostLog(" %s:%d=%lu", kKinds[bestK], bestT, static_cast< u32 >(sCategoryTicks[bestK][bestT] / ms));
            sCategoryTicks[bestK][bestT] = 0;
        }
        hostLog("%s", "\n");
        memset(sCategoryTicks, 0, sizeof(sCategoryTicks));
    }
}  // namespace

extern "C" void rvlGXDebugSetFramePhase(int phase) {
    sDebugFramePhase = phase;
    hostDebugTrace("frame phase", phase, 0, 0);
}

extern "C" int rvlGXDebugGetFramePhase(void) {
    return sDebugFramePhase;
}

extern "C" void rvlGXFramePhase(int phase) {
    u64 now = hostGetTick();
    if (phase > 0 && phase < kFramePhases) {
        sPhaseTicks[phase] += now - sPhaseStart;
        if (phase == 1 && now - sPhaseStart > sLongestDrawTicks) {
            sLongestDrawTicks = now - sPhaseStart;
        }
    }
    sPhaseStart = now;
}

namespace {
    // The category running now (packed kind << 8 | type, -1 for none), for the object timing.
    int sCurrentCategory = -1;

    // Object timing (profile=1; NameObjCategoryList::execute): time per category and object
    // name, inclusive, so the costliest objects of a category show up in the log.
    struct ObjectTime {
        const char* name;
        int category;
        u64 ticks;
    };
    const int kObjectTimes = 256;
    // Timing every object of every frame cost the game about a millisecond a frame (two system
    // tick reads per object, about a thousand objects): one frame in kObjectSampleFrames is timed.
    const u32 kObjectSampleFrames = 8;
    u32 sObjectSampleFrame;
    ObjectTime sObjectTimes[kObjectTimes];
    u32 sObjectTimesLost;

    // The twelve costliest objects since the last log; names compare by pointer (objects of one
    // class share their name string, as the game's own tables do).
    void logObjects(u64 ms) {
        hostLog("Game objects ms:");
        for (int n = 0; n < 12; n++) {
            int best = -1;
            for (int i = 0; i < kObjectTimes; i++) {
                if (sObjectTimes[i].ticks != 0 && (best < 0 || sObjectTimes[i].ticks > sObjectTimes[best].ticks)) {
                    best = i;
                }
            }
            if (best < 0) {
                break;
            }
            const ObjectTime& time = sObjectTimes[best];
            static const char* const kKinds[kCategoryKinds] = {"move", "anim", "draw", "opa", "xlu", "entry", "sys"};
            // Objects are timed one frame in kObjectSampleFrames: the time is scaled back.
            hostLog(" %s:%d %s=%lu", kKinds[time.category >> 8], time.category & 0xFF, time.name != nullptr ? time.name : "?",
                    static_cast< u32 >(time.ticks * kObjectSampleFrames / ms));
            sObjectTimes[best].ticks = 0;
        }
        if (sObjectTimesLost != 0) {
            hostLog(" (%lu not counted, table full)", sObjectTimesLost);
        }
        hostLog("%s", "\n");
        memset(sObjectTimes, 0, sizeof(sObjectTimes));
        sObjectTimesLost = 0;
    }
}  // namespace

extern "C" void rvlGXCategoryMark(int kind, int type, unsigned long long* pMark) {
    if (gGXProfileOn) {
        pMark[0] = hostGetTick();
        pMark[1] = static_cast< unsigned long long >(static_cast< long long >(sCurrentCategory));
        sCurrentCategory = (kind << 8) | (type & 0xFF);
    }
}

extern "C" void rvlGXCategoryEnd(int kind, int type, const unsigned long long* pMark) {
    if (gGXProfileOn && kind >= 0 && kind < kCategoryKinds && type >= 0 && type < kCategoryTypes) {
        sCategoryTicks[kind][type] += hostGetTick() - pMark[0];
        sCurrentCategory = static_cast< int >(static_cast< long long >(pMark[1]));
    }
}

extern "C" bool rvlGXObjectProfileOn(void) {
    return gGXProfileOn && sCurrentCategory >= 0 && sObjectSampleFrame % kObjectSampleFrames == 0;
}

extern "C" unsigned long long rvlGXObjectMark(void) {
    return hostGetTick();
}

extern "C" void rvlGXObjectEnd(const char* pName, unsigned long long start) {
    const u64 ticks = hostGetTick() - start;
    const int category = sCurrentCategory;
    u32 slot = ((reinterpret_cast< uintptr_t >(pName) >> 2) ^ static_cast< u32 >(category) * 0x9E3779B1u) % kObjectTimes;
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) % kObjectTimes) {
        ObjectTime& time = sObjectTimes[slot];
        if (time.ticks == 0 && time.name == nullptr) {
            time.name = pName;
            time.category = category;
        }
        if (time.name == pName && time.category == category) {
            time.ticks += ticks;
            return;
        }
    }
    sObjectTimesLost++;
}

// The 3DS frame end (MainLoopFramework::endRender, before hostGfxEndFrame).
void rvl3DSEndFrame() {
    static u32 sFrameCount, sLastLogFrame;
    static u64 sLastLogTick;
    sFrameCount++;
    sObjectSampleFrame++;
    rvlNativeEndFrame();
    hostDebugFrameDrawn();
    if (sFrameDraws >= 100) {
        hostDebugTevSeen();  // a scene with models is up (debug.txt capture=tev / shot=tev)
    }
    u64 now = hostGetTick();
    u64 ms = hostGetTickFrequency() / 1000;
    // Every 100 frames, or every 5 seconds in slow scenes. The "GX frame" line keeps the form
    // tools read (draws are native draws; nothing is spent in GX any more).
    if (sFrameCount % 100 == 1 || now - sLastLogTick >= ms * 5000) {
        hostLog("GX frame %lu: %lu draws, 0 vertices, %lu ms since last log (%lu frames), 0 ms in GX, %lu ms waiting for the GPU\n", sFrameCount,
                sFrameDraws, static_cast< u32 >((now - sLastLogTick) / ms), sFrameCount - sLastLogFrame,
                static_cast< u32 >(hostGfxTakeGpuWait() / ms));
        float gpuAverage, gpuMax;
        hostGfxTakeGpuTime(&gpuAverage, &gpuMax);
        hostLog("GPU time from frame end to finish: %lu.%lu ms average, %lu.%lu ms longest\n", static_cast< u32 >(gpuAverage),
                static_cast< u32 >(gpuAverage * 10.0f) % 10, static_cast< u32 >(gpuMax), static_cast< u32 >(gpuMax * 10.0f) % 10);
        hostLog("Native draws: %lu since last log\n", gGXProfile.nativeDraws);
        unsigned long submitCalls, submitVectors, submitBinds;
        unsigned long long stateTicks, drawTicks;
        hostGfxTakeSubmitStats(&submitCalls, &stateTicks, &drawTicks, &submitVectors, &submitBinds);
        if (gGXProfileOn) {
            hostLog("Native submit detail: %lu calls, state and uniforms %lu ms (%lu vectors, %lu texture binds), "
                    "C3D_DrawElements %lu ms, visibility test %lu ms, before it %lu ms, lights and transform %lu ms\n",
                    submitCalls, static_cast< u32 >(stateTicks / ms), submitVectors, submitBinds, static_cast< u32 >(drawTicks / ms),
                    static_cast< u32 >(sVisibilityTicks / ms), static_cast< u32 >(sPrepareTicks / ms), static_cast< u32 >(sBuildTicks / ms));
        }
        sVisibilityTicks = sPrepareTicks = sBuildTicks = 0;
        u32 particleBatches, particleQuads, particleCulled;
        JPA3DS::takeStats(particleBatches, particleQuads, particleCulled);
        hostLog("Particles: %lu draws, %lu quads drawn, %lu off screen\n", static_cast< unsigned long >(particleBatches),
                static_cast< unsigned long >(particleQuads), static_cast< unsigned long >(particleCulled));
        JPA3DS::logFlushReasons();
        u32 clipDraws, clipTriangles, clipInvalid, clipInvalidMatrixDraws, clipCulled;
        u64 clipTicks;
        rvlNativeClipTakeStats(clipDraws, clipTriangles, clipInvalid, clipInvalidMatrixDraws, clipCulled, clipTicks);
        if (clipDraws != 0 || clipCulled != 0 || clipInvalidMatrixDraws != 0) {
            hostLog("Native clip: %lu draws culled outside the view, %lu tested triangle by triangle (%lu ms), %lu triangles clipped, "
                    "%lu dropped for NaN or infinite vertices (%lu draws dropped whole for their matrices)\n",
                    clipCulled, clipDraws, static_cast< u32 >(clipTicks / ms), clipTriangles, clipInvalid, clipInvalidMatrixDraws);
        }
        sLastLogFrame = sFrameCount;
        hostLog("Longest draw phase: %lu ms\n", static_cast< u32 >(sLongestDrawTicks / ms));
        sLongestDrawTicks = 0;
        hostLog("Frame phases ms: draw %lu, endRender %lu, update %lu, calcAnim %lu, endFrame+wait %lu\n",
                static_cast< u32 >(sPhaseTicks[1] / ms), static_cast< u32 >(sPhaseTicks[2] / ms), static_cast< u32 >(sPhaseTicks[3] / ms),
                static_cast< u32 >(sPhaseTicks[4] / ms), static_cast< u32 >(sPhaseTicks[5] / ms));
        hostGfxLogPacing();
        if (gGXProfileOn) {
            logCategories(ms);
            logObjects(ms);
            hostLog("Native submit: %lu ms, %lu draws refused for their lights\n", static_cast< u32 >(gGXProfile.host / ms), gGXProfile.cpuLights);
            if (hostDebugNative()) {
                rvlNativeLogFrame();
            }
        }
        for (int i = 0; i < kFramePhases; i++) {
            sPhaseTicks[i] = 0;
        }
        gGXProfile = RvlGXProfile();
        sLastLogTick = now;
        // The timers cost about a dozen system calls a draw: on for one log in four. The pacing
        // lines of the others show the game without them.
        static u32 sLogCount;
        sLogCount++;
        gGXProfileOn = hostDebugProfile() != 0 && sLogCount % 4 == 0;
        hostGfxSetProfile(gGXProfileOn);
    }
    sFrameDraws = 0;
}
