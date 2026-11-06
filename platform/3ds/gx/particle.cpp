// JPA particles on the 3DS (JPADraw3DS.hpp). The JPA draws keep the Wii's code: where it sends
// GX its vertex arrays, display lists, matrices and TEV state, the 3DS branches hand the same
// values to this file, which makes the vertices and texture coordinates GX would (positions
// through the position matrix, coordinate 0 through the texture coordinate generation, the second
// texture's from the vertex's own) and draws them with the combiners of particle_tev.inc
// (tools/3ds/particles/jpatev.py, checked against the Wii TEV by jpacheck.py).

#include "JPADraw3DS.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "JSystem/JParticle/JPAResourceManager.hpp"
#include "gx_state.h"
#include "host.h"
#include <math.h>
#include <string.h>

struct ParticleStage3DS {
    u8 colorSrc[3];
    u8 colorFunc;
    u8 alphaSrc[3];
    u8 alphaFunc;
};

namespace {
#include "particle_tev.inc"

    const u8* sPositions;
    const u8* sTexCoords;
    u32 sTextureMap;
    bool sSecond;
    bool sIndirect;
    f32 sIndirectRows[2];  // the matrix rows' sums times 2^exponent: the texels per unit of the value
    u32 sTexGen = JPA3DS::TEXGEN_IDENTITY;
    Mtx sTexMtx;
    Mtx sPosMtx;
    GXColor sPrm = {0xFF, 0xFF, 0xFF, 0xFF};
    GXColor sEnv = {0x00, 0x00, 0x00, 0x00};
    u32 sColorMode = 1, sAlphaMode;
    // The highest alpha passing when GX's two comparisons pass a range inside 0-255 (the swimming
    // bubbles' GEQUAL 60 XNOR LEQUAL 39: 40-59), 255 otherwise (setAlphaCompare).
    u32 sAlphaHigh = 255;
    f32 sPointSize = 6.0f, sLineWidth = 6.0f;

    // A camera-space vertex with its texture coordinates (0: the generated one, 1: the vertex's).
    struct Vertex {
        TVec3f position;
        TVec2f uv0, uv1;
    };

    // Triangles waiting for one draw (TDDraw::drawParticleTriangles takes up to 192 vertices): the
    // particles of an emitter, each with its prm colour, until a texture or a mode changes. A draw
    // per particle cost about 29 ms a frame at 268 MHz in the file selection (CIA 0.16.0).
    // Clip space when they join the batch: a quad of particles, nearly all of them, is 4 vertices and
    // 6 indices, and none at all when it lies wholly beyond one side of the screen (the 3D particles
    // took about 3 ms of each loop in the CIA 0.18.9 log, 9 at the worst tenth of it).
    const u32 kBatch = 192;
    const u32 kBatchIndices = kBatch / 4 * 6;
    HostGfxVertex sBatchVertices[kBatch];
    TVec2f sBatchUv0[kBatch], sBatchUv1[kBatch];
    u16 sBatchIndices[kBatchIndices];
    u32 sBatchCount, sBatchIndexCount;
    u32 sStatBatches, sStatQuads, sStatCulled;  // for the periodic log (takeStats)
    // The blending, depth and alpha test the direct state has (setShapeState, setAlphaCompare),
    // valid from beginDraw.
    bool sShapeStateKnown, sAlphaKnown;
    u8 sShapeState[6], sAppliedFunc, sAppliedRef;
    u32 sAppliedHigh;
    JUTTexture* sBatchTextures[4];  // the textures loaded when the batch began

    // The primitive between begin and end, and its vertices still waiting for a triangle.
    GXPrimitive sPrimitive;
    Vertex sPending[4];
    u32 sPendingCount, sStripCount;

    u32 packColor(GXColor color) {
        return (static_cast< u32 >(color.r) << 24) | (static_cast< u32 >(color.g) << 16) | (static_cast< u32 >(color.b) << 8) | color.a;
    }

    bool readsSource(const ParticleStage3DS& stage, u8 source) {
        for (u32 i = 0; i < 3; i++) {
            if (stage.colorSrc[i] == source || stage.alphaSrc[i] == source) {
                return true;
            }
        }
        return false;
    }

    void drawBatch() {
        if (sBatchCount == 0) {
            return;
        }
        const u32 count = sBatchCount, indexCount = sBatchIndexCount;
        sBatchCount = sBatchIndexCount = 0;
        sStatBatches++;
        JUTTexture* texture = sBatchTextures[sTextureMap & 1];
        JUTTexture* second = sSecond ? sBatchTextures[3] : nullptr;
        const u32 mode = sColorMode < 6 ? sColorMode : 1;
        const ParticleStage3DS* table[2] = {&kParticleColorStages[mode][sAlphaMode & 1], &kParticleSecondStage};
        u32 stageCount = second != nullptr ? 2 : 1;
        if (texture == nullptr && readsSource(*table[0], 0x03)) {
            return;  // GX would sample a texture loaded for something else
        }
        HostGfxStage stages[6] = {};
        for (u32 i = 0; i < stageCount; i++) {
            for (u32 k = 0; k < 3; k++) {
                stages[i].colorSrc[k] = table[i]->colorSrc[k];
                stages[i].alphaSrc[k] = table[i]->alphaSrc[k];
            }
            stages[i].colorFunc = table[i]->colorFunc;
            stages[i].alphaFunc = table[i]->alphaFunc;
        }
        u32 constant = kParticleConstantWhite[mode] ? 0xFFFFFFFFu : packColor(sEnv);
        // GX indirect texturing, which the PICA lacks (RvlDistortion): the first texture displaced by
        // the value of the distortion as the blend of two samples, as native_submit.cpp does. With a
        // second texture or the alpha range's stages, no units or stages left: the displacement of
        // the middle of the values.
        JUTTexture* distortion = sIndirect && texture != nullptr && texture->getWidth() != 0 && texture->getHeight() != 0 ? sBatchTextures[2] : nullptr;
        RvlDistortion plan = {};
        if (distortion != nullptr && distortion->getTexInfo() != nullptr) {
            rvlDistortionPlan(distortion->getTexInfo(), sIndirectRows[0], sIndirectRows[1], texture->getWidth(), texture->getHeight(), 0, 2, 0, plan);
            if (second != nullptr || sAlphaHigh != 255 || !readsSource(*table[0], 0x03)) {
                for (u32 i = 0; i < count; i++) {
                    sBatchUv0[i].x += plan.mean[0];
                    sBatchUv0[i].y += plan.mean[1];
                }
                distortion = nullptr;
            }
        } else {
            distortion = nullptr;
        }
        if (distortion != nullptr) {
            HostGfxStage distorted[3] = {};
            distorted[0].colorSrc[0] = 5;     // GPU_TEXTURE2
            distorted[0].colorSrc[1] = 0x0E;  // GPU_CONSTANT
            distorted[0].colorFunc = 5;       // GPU_SUBTRACT
            distorted[0].colorScale = plan.scale;
            distorted[0].constant = plan.low * 0x01010101u;
            distorted[1].colorSrc[0] = 4;     // GPU_TEXTURE1
            distorted[1].colorSrc[1] = 3;     // GPU_TEXTURE0
            distorted[1].colorSrc[2] = 0x0F;  // GPU_PREVIOUS
            distorted[1].colorFunc = 4;       // GPU_INTERPOLATE
            distorted[1].alphaSrc[0] = 3;
            distorted[2] = stages[0];
            for (u32 k = 0; k < 3; k++) {
                distorted[2].colorSrc[k] = distorted[2].colorSrc[k] == 3 ? 0x0F : distorted[2].colorSrc[k];
                distorted[2].alphaSrc[k] = distorted[2].alphaSrc[k] == 3 ? 0x0F : distorted[2].alphaSrc[k];
            }
            const f32 halfStep[2] = {0.5f * plan.step[0], 0.5f * plan.step[1]};
            TDDraw::drawParticleDistortedTriangles(sBatchVertices, sBatchUv0, sBatchUv1, count, sBatchIndices, indexCount, texture, distortion,
                                                   plan.mean, halfStep, constant, distorted, 3);
            return;
        }
        u8 bufferAlpha = 0;
        if (sAlphaHigh < 255) {
            // The alpha test passes A >= low; the upper end is made in the combiners, which keep the
            // colour: a step, (A - high) * 4, * 8, * 8, saturates to 255 for A > high and 0 below,
            // and the last combiner takes it off A (the buffer keeps A from the particle's last
            // stage): A where A <= high, 0 above. The particle combiners never read the constant's
            // alpha, which holds high.
            const u32 last = stageCount - 1;
            for (u32 i = stageCount; i < stageCount + 4; i++) {
                HostGfxStage& h = stages[i];
                h.colorSrc[0] = h.colorSrc[1] = h.colorSrc[2] = 0x0F;  // GPU_PREVIOUS
                h.colorFunc = 0;  // GPU_REPLACE
                h.alphaSrc[0] = h.alphaSrc[1] = h.alphaSrc[2] = 0x0F;
                h.alphaScale = 2;  // GPU_TEVSCALE_4
            }
            stages[stageCount].alphaSrc[1] = 0x0E;  // GPU_CONSTANT
            stages[stageCount].alphaFunc = 5;  // GPU_SUBTRACT: PREVIOUS - high
            stages[stageCount + 1].alphaFunc = 2;  // GPU_ADD: PREVIOUS + PREVIOUS
            stages[stageCount + 2].alphaFunc = 2;
            HostGfxStage& out = stages[stageCount + 3];
            out.alphaSrc[0] = 0x0D;  // GPU_PREVIOUS_BUFFER: A
            out.alphaFunc = 5;  // A - step
            out.alphaScale = 0;
            bufferAlpha = static_cast< u8 >(1u << last);
            constant = (constant & 0xFFFFFF00u) | sAlphaHigh;
            stageCount += 4;
        }
        TDDraw::drawParticleTriangles(sBatchVertices, sBatchUv0, second != nullptr ? sBatchUv1 : nullptr, count, sBatchIndices, indexCount, texture,
                                      second, constant, stages, stageCount, bufferAlpha);
    }

    // Why batches were drawn (the periodic log, takeStats): a full batch, other textures, another
    // array or texture map, second or indirect texture, indirect matrix, env colour, modes, alpha
    // test, blending or depth, the end of the particles.
    enum { FLUSH_FULL, FLUSH_TEXTURE, FLUSH_ARRAYS, FLUSH_EXTEX, FLUSH_INDIRECT, FLUSH_ENV, FLUSH_MODES, FLUSH_ALPHA, FLUSH_SHAPE, FLUSH_END, FLUSH_REASONS };
    u32 sFlushReasons[FLUSH_REASONS];

    void drawBatchFor(u32 reason) {
        if (sBatchCount != 0) {
            sFlushReasons[reason]++;
        }
        drawBatch();
    }

    // Room for vertices and indices in the batch, with the textures loaded now.
    void prepareBatch(u32 vertices, u32 indices) {
        JUTTexture* const* loaded = JPAResourceManager::sLoadedTexture;
        if (sBatchCount + vertices > kBatch || sBatchIndexCount + indices > kBatchIndices ||
            (sBatchCount != 0 && (loaded[0] != sBatchTextures[0] || loaded[1] != sBatchTextures[1] || loaded[2] != sBatchTextures[2] ||
                                  loaded[3] != sBatchTextures[3]))) {
            drawBatchFor(sBatchCount + vertices > kBatch || sBatchIndexCount + indices > kBatchIndices ? FLUSH_FULL : FLUSH_TEXTURE);
        }
        if (sBatchCount == 0) {
            for (u32 i = 0; i < 4; i++) {
                sBatchTextures[i] = loaded[i];
            }
        }
    }

    void addVertex(const Vertex& v, const HostGfxVertex& clip) {
        sBatchVertices[sBatchCount] = clip;
        sBatchUv0[sBatchCount] = v.uv0;
        sBatchUv1[sBatchCount] = v.uv1;
        sBatchCount++;
    }

    void triangle(const Vertex& a, const Vertex& b, const Vertex& c) {
        prepareBatch(3, 3);
        const u32 color = packColor(sPrm);
        const Vertex* v[3] = {&a, &b, &c};
        for (u32 i = 0; i < 3; i++) {
            HostGfxVertex clip;
            TDDraw::particleVertexToClip(v[i]->position, clip, color);
            sBatchIndices[sBatchIndexCount++] = static_cast< u16 >(sBatchCount);
            addVertex(*v[i], clip);
        }
    }

    // The triangles (a, b, c) and (a, c, d). Beyond one side of the screen, all four corners beyond
    // the same side, the GPU would draw nothing of it.
    void quad(const Vertex* v) {
        prepareBatch(4, 6);
        const u32 color = packColor(sPrm);
        HostGfxVertex clip[4];
        u32 outside = 15;
        for (u32 i = 0; i < 4; i++) {
            TDDraw::particleVertexToClip(v[i].position, clip[i], color);
            const f32 w = clip[i].w;
            outside &= (clip[i].x > w ? 1u : 0u) | (clip[i].x < -w ? 2u : 0u) | (clip[i].y > w ? 4u : 0u) | (clip[i].y < -w ? 8u : 0u);
        }
        if (outside != 0) {
            sStatCulled++;
            return;
        }
        sStatQuads++;
        const u16 first = static_cast< u16 >(sBatchCount);
        for (u32 i = 0; i < 4; i++) {
            addVertex(v[i], clip[i]);
        }
        static const u8 kQuad[6] = {0, 1, 2, 0, 2, 3};
        for (u32 i = 0; i < 6; i++) {
            sBatchIndices[sBatchIndexCount++] = static_cast< u16 >(first + kQuad[i]);
        }
    }

    // GX's vertex: the position through GX_PNMTX0, coordinate 0 through the texture coordinate
    // generation (GX_TG_TEX0 with (s, t, 1, 1), or GX_TG_POS with the model-space position).
    Vertex makeVertex(f32 x, f32 y, f32 z, f32 s, f32 t) {
        Vertex v;
        const TVec3f model(x, y, z);
        PSMTXMultVec(sPosMtx, &model, &v.position);
        v.uv1 = TVec2f(s, t);
        if (sTexGen == JPA3DS::TEXGEN_MATRIX) {
            v.uv0 = TVec2f(sTexMtx[0][0] * s + sTexMtx[0][1] * t + sTexMtx[0][2] + sTexMtx[0][3],
                           sTexMtx[1][0] * s + sTexMtx[1][1] * t + sTexMtx[1][2] + sTexMtx[1][3]);
        } else if (sTexGen == JPA3DS::TEXGEN_POSITION) {
            const f32 ps = sTexMtx[0][0] * x + sTexMtx[0][1] * y + sTexMtx[0][2] * z + sTexMtx[0][3];
            const f32 pt = sTexMtx[1][0] * x + sTexMtx[1][1] * y + sTexMtx[1][2] * z + sTexMtx[1][3];
            const f32 q = sTexMtx[2][0] * x + sTexMtx[2][1] * y + sTexMtx[2][2] * z + sTexMtx[2][3];
            // Divided at the vertex: exact when q is the same at every vertex (a billboard facing
            // the camera of a projection from it); the GPU has no projective coordinate here.
            v.uv0 = q != 0.0f ? TVec2f(ps / q, pt / q) : TVec2f(0.0f, 0.0f);
        } else {
            v.uv0 = TVec2f(s, t);
        }
        return v;
    }

    // GX_POINTS: a square of the point size (pixels / 6) facing the camera, its coordinates
    // offset from 0 to 1 across it (GXEnableTexOffsets, GX_TO_ONE).
    void point(const Vertex& v) {
        const f32 half = 0.5f * (sPointSize / 6.0f) * TDDraw::directViewUnitsPerPixel(v.position);
        Vertex c[4] = {v, v, v, v};
        const f32 dx[4] = {-half, half, half, -half}, dy[4] = {half, half, -half, -half};
        const f32 ds[4] = {0.0f, 1.0f, 1.0f, 0.0f}, dt[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        for (u32 i = 0; i < 4; i++) {
            c[i].position.x += dx[i];
            c[i].position.y += dy[i];
            c[i].uv0.x += ds[i];
            c[i].uv0.y += dt[i];
            c[i].uv1.x += ds[i];
            c[i].uv1.y += dt[i];
        }
        quad(c);
    }

    // GX_LINES: a band of the line width (pixels / 6) across the line on the screen, its s offset
    // from 0 to 1 across it (GX_TO_ONE).
    void line(const Vertex& a, const Vertex& b) {
        f32 dx = b.position.x - a.position.x, dy = b.position.y - a.position.y;
        const f32 length = sqrtf(dx * dx + dy * dy);
        if (length == 0.0f) {
            dx = 1.0f, dy = 0.0f;
        } else {
            dx /= length, dy /= length;
        }
        Vertex c[4] = {a, b, b, a};
        for (u32 i = 0; i < 4; i++) {
            const f32 half = 0.5f * (sLineWidth / 6.0f) * TDDraw::directViewUnitsPerPixel(c[i].position);
            const f32 side = i == 0 || i == 1 ? -1.0f : 1.0f;
            c[i].position.x += -dy * half * side;
            c[i].position.y += dx * half * side;
            const f32 offset = side > 0.0f ? 1.0f : 0.0f;
            c[i].uv0.x += offset;
            c[i].uv1.x += offset;
        }
        quad(c);
    }

    void add(const Vertex& v) {
        switch (sPrimitive) {
        case GX_POINTS:
            point(v);
            break;
        case GX_LINES:
            sPending[sPendingCount++] = v;
            if (sPendingCount == 2) {
                line(sPending[0], sPending[1]);
                sPendingCount = 0;
            }
            break;
        case GX_TRIANGLESTRIP:
            if (sStripCount >= 2) {
                // GX alternates the winding; particles are drawn without culling.
                triangle(sPending[0], sPending[1], v);
            }
            sPending[0] = sPending[1];
            sPending[1] = v;
            sStripCount++;
            break;
        case GX_QUADS:
            sPending[sPendingCount++] = v;
            if (sPendingCount == 4) {
                quad(sPending);
                sPendingCount = 0;
            }
            break;
        default:
            break;
        }
    }

    // GX comparison of an alpha value (GXCompare).
    bool compare(GXCompare comp, u32 a, u32 ref) {
        switch (comp) {
        case GX_NEVER:
            return false;
        case GX_LESS:
            return a < ref;
        case GX_EQUAL:
            return a == ref;
        case GX_LEQUAL:
            return a <= ref;
        case GX_GREATER:
            return a > ref;
        case GX_NEQUAL:
            return a != ref;
        case GX_GEQUAL:
            return a >= ref;
        default:
            return true;
        }
    }
    // What setAlphaCompare applies for GX's two comparisons: the PICA test and the highest alpha
    // passing (sAlphaHigh).
    void computeAlphaTest(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1, u8& outFunc, u8& outRef, u32& outHigh) {
        enum { NEVER = 0, ALWAYS = 1, EQUAL = 2, NOTEQUAL = 3, LEQUAL = 5, GEQUAL = 7 };
        u32 count = 0, first = 256, last = 0, missing = 0;
        for (u32 a = 0; a < 256; a++) {
            const bool x = compare(comp0, a, ref0), y = compare(comp1, a, ref1);
            bool pass;
            switch (op) {
            case GX_AOP_AND:
                pass = x && y;
                break;
            case GX_AOP_OR:
                pass = x || y;
                break;
            case GX_AOP_XOR:
                pass = x != y;
                break;
            default:
                pass = x == y;
                break;
            }
            if (pass) {
                count++;
                first = first < a ? first : a;
                last = a;
            } else {
                missing = a;
            }
        }
        u8 func = GEQUAL, ref = static_cast< u8 >(first);
        u32 high = 255;
        if (count == 0) {
            func = NEVER, ref = 0;
        } else if (count == 256) {
            func = ALWAYS, ref = 0;
        } else if (count == last - first + 1 && last == 255) {
            func = GEQUAL;
        } else if (count == last - first + 1 && first == 0) {
            func = LEQUAL, ref = static_cast< u8 >(last);
        } else if (count == 1) {
            func = EQUAL;
        } else if (count == 255) {
            func = NOTEQUAL, ref = static_cast< u8 >(missing);
        } else if (count == last - first + 1) {
            high = last;  // first > 0, last < 255
        }
        outFunc = func, outRef = ref, outHigh = high;
    }
}  // namespace

namespace JPA3DS {
    void setArrays(const u8* pPositions, const u8* pTexCoords, u32 textureMap) {
        if (textureMap != sTextureMap) {
            drawBatchFor(FLUSH_ARRAYS);
        }
        sPositions = pPositions;
        sTexCoords = pTexCoords;
        sTextureMap = textureMap;
    }

    void setExTex(bool second, bool indirect) {
        if (second != sSecond || indirect != sIndirect) {
            drawBatchFor(FLUSH_EXTEX);
        }
        sSecond = second;
        sIndirect = indirect;
    }

    void setIndirect(const f32* pMatrix, s8 exponent) {
        drawBatchFor(FLUSH_INDIRECT);
        sIndirectRows[0] = ldexpf(pMatrix[0] + pMatrix[1] + pMatrix[2], exponent);
        sIndirectRows[1] = ldexpf(pMatrix[3] + pMatrix[4] + pMatrix[5], exponent);
    }

    void setTexGen(u32 mode) {
        sTexGen = mode;
    }

    void loadTexMtx(const Mtx matrix) {
        PSMTXCopy(matrix, sTexMtx);
    }

    void loadPosMtx(const Mtx matrix) {
        PSMTXCopy(matrix, sPosMtx);
    }

    void setTevColor(GXTevRegID id, GXColor color) {
        if (id == GX_TEVREG0) {
            sPrm = color;
        } else if (id == GX_TEVREG1) {
            if (packColor(color) != packColor(sEnv)) {
                drawBatchFor(FLUSH_ENV);
            }
            sEnv = color;
        }
    }

    void setModes(u32 colorMode, u32 alphaMode) {
        if (colorMode != sColorMode || alphaMode != sAlphaMode) {
            drawBatchFor(FLUSH_MODES);
        }
        sColorMode = colorMode;
        sAlphaMode = alphaMode;
    }

    // The one PICA comparison passing the alpha values the two GX ones do (tools/3ds/particles/
    // jpacheck.py reduced_alpha_test): the values passing make no comparison or all of them, the
    // values from one up, up to one, one or all but one. A range inside 0-255 takes its lower
    // bound here and its upper one in the combiners (flush, sAlphaHigh).
    void setAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
        // The same as the last emitter's, nearly always: the 256 comparisons below once.
        static bool sKnown;
        static GXCompare sComp0, sComp1;
        static GXAlphaOp sOp;
        static u8 sRef0, sRef1, sFunc, sRef;
        static u32 sHigh;
        if (!(sKnown && comp0 == sComp0 && ref0 == sRef0 && op == sOp && comp1 == sComp1 && ref1 == sRef1)) {
            computeAlphaTest(comp0, ref0, op, comp1, ref1, sFunc, sRef, sHigh);
            sKnown = true;
            sComp0 = comp0, sRef0 = ref0, sOp = op, sComp1 = comp1, sRef1 = ref1;
        }
        // The kept triangles were made for the test applied: drawn first only when it changes.
        if (sAlphaKnown && sFunc == sAppliedFunc && sRef == sAppliedRef && sHigh == sAppliedHigh) {
            return;
        }
        drawBatchFor(FLUSH_ALPHA);
        sAlphaHigh = sHigh;
        sAppliedFunc = sFunc, sAppliedRef = sRef, sAppliedHigh = sHigh;
        sAlphaKnown = true;
        TDDraw::setDirectAlphaTest(sFunc, sRef);
    }

    void beginDraw() {
        sShapeStateKnown = sAlphaKnown = false;
    }

    void setShapeState(u8 equation, u8 source, u8 destination, bool zEnable, u8 zTest, bool zUpdate) {
        const u8 state[6] = {equation, source, destination, zEnable, zTest, zUpdate};
        if (sShapeStateKnown && memcmp(state, sShapeState, sizeof(state)) == 0) {
            return;
        }
        drawBatchFor(FLUSH_SHAPE);
        memcpy(sShapeState, state, sizeof(state));
        sShapeStateKnown = true;
        TDDraw::setDirectBlendState(equation, source, destination);
        TDDraw::setDirectDepthMode(zEnable, zTest, zUpdate);
    }

    void takeStats(u32& batches, u32& quads, u32& culled) {
        batches = sStatBatches, quads = sStatQuads, culled = sStatCulled;
        sStatBatches = sStatQuads = sStatCulled = 0;
    }

    void logFlushReasons() {
        hostLog("Particle draws by reason: full %lu, textures %lu, arrays %lu, second texture %lu, indirect %lu, env %lu, modes %lu, "
                "alpha test %lu, blend/depth %lu, end %lu\n",
                static_cast< unsigned long >(sFlushReasons[FLUSH_FULL]), static_cast< unsigned long >(sFlushReasons[FLUSH_TEXTURE]),
                static_cast< unsigned long >(sFlushReasons[FLUSH_ARRAYS]), static_cast< unsigned long >(sFlushReasons[FLUSH_EXTEX]),
                static_cast< unsigned long >(sFlushReasons[FLUSH_INDIRECT]), static_cast< unsigned long >(sFlushReasons[FLUSH_ENV]),
                static_cast< unsigned long >(sFlushReasons[FLUSH_MODES]), static_cast< unsigned long >(sFlushReasons[FLUSH_ALPHA]),
                static_cast< unsigned long >(sFlushReasons[FLUSH_SHAPE]), static_cast< unsigned long >(sFlushReasons[FLUSH_END]));
        memset(sFlushReasons, 0, sizeof(sFlushReasons));
    }

    void setPointSize(f32 sixths) {
        sPointSize = sixths;
    }

    void setLineWidth(f32 sixths) {
        sLineWidth = sixths;
    }

    void callDisplayList(const u8* pList) {
        if (sPositions == nullptr || sTexCoords == nullptr) {
            return;
        }
        // GX_QUADS (0x80, vertex format 0), a big-endian vertex count, then per vertex the
        // position and coordinate indices (GX_INDEX8: GX_S8 positions, GX_S8 coordinates).
        u32 i = 0;
        while (i + 3 <= 32 && pList[i] == 0x80) {
            const u32 count = (static_cast< u32 >(pList[i + 1]) << 8) | pList[i + 2];
            i += 3;
            begin(GX_QUADS);
            for (u32 k = 0; k < count && i + 2 <= 32; k++, i += 2) {
                const s8* p = reinterpret_cast< const s8* >(sPositions + pList[i] * 3);
                const s8* c = reinterpret_cast< const s8* >(sTexCoords + pList[i + 1] * 2);
                vertex(p[0], p[1], p[2], c[0], c[1]);
            }
            end();
        }
    }

    void begin(GXPrimitive primitive) {
        sPrimitive = primitive;
        sPendingCount = 0;
        sStripCount = 0;
    }

    void vertex(f32 x, f32 y, f32 z, f32 s, f32 t) {
        add(makeVertex(x, y, z, s, t));
    }

    void end() {
        sPendingCount = 0;
        sStripCount = 0;
    }

    void flush() {
        drawBatchFor(FLUSH_END);
    }
}  // namespace JPA3DS
