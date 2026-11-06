// ##############################################################################################
// # Clipping native draws before the GPU                                                       #
// #                                                                                            #
// # The PICA clips only against the near and far planes. A model whose triangles cross the     #
// # camera plane far off to the side (a sky or backdrop around the camera) leaves the clipper  #
// # with triangles thousands of screens wide, and rasterizing them stalls the GPU for good: on #
// # hardware the title screen's backdrop did (CIA 0.10.5). The Wii's GX clips every side of    #
// # the view volume, so those triangles are clipped here instead (host clip.c), and the result #
// # on screen is the same.                                                                     #
// #                                                                                            #
// # Every draw is first tested with the bounding spheres of its matrix slots (computed when    #
// # the group is uploaded): a draw wholly inside the near plane and the guard band goes to the #
// # GPU as before. The others are processed triangle by triangle: triangles inside are drawn   #
// # with their cached vertices, triangles outside one plane are dropped (the GPU would drop    #
// # them), and the rest are clipped into new vertices in the frame's transient vertices.       #
// #                                                                                            #
// # Each vertex is classified once (a triangle list reads a vertex about six times), with the  #
// # clip and position matrices folded together: the large ground models of a scene go this way #
// # every frame, and transforming every triangle's three vertices took most of the frame (CIA  #
// # 0.11.11, about 10 FPS). A new vertex interpolates the model-space attributes of its edge,  #
// # which the shader then transforms, lights and maps as it does any vertex. When a triangle's #
// # vertices use different position matrices, the vertices are first brought into the space of #
// # the first one's matrix (same view-space position; normals through the matrices' inverses); #
// # their texture generation inputs are interpolated as they are.                              #
// ##############################################################################################
#include "gx_state.h"
#include "host.h"
#include <revolution/mtx.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

    // ##############################################################################################
    // # Clip vertices and matrix spaces                                                            #
    // #                                                                                            #
    // # A vertex being clipped carries its clip-space position (which clip.c reads) and the arena  #
    // # vertex. The helpers move a position and a normal between a matrix row's model space, view  #
    // # space and clip space, and flushOut draws the clipped triangles from the transient          #
    // # vertices.                                                                                  #
    // ##############################################################################################
    struct ClipVertex {
        f32 clip[4];  // first: clip.c reads the clip-space position here
        HostGfxModelVertex v;
    };

    u32 sClipDraws, sClipTriangles, sInvalidTriangles, sInvalidReports, sCulledDraws;
    u32 sSmallDraws;  // draws under the lowered detail's size (rvlNativeClipNeeded)
    u32 sInvalidMatrixDraws, sMatrixReports;  // draws dropped whole for their matrices

    // Floats of a report as their bits (NaN and infinity stay visible; no %g in the log).
    void logFloatBits(const char* label, const f32* values, u32 count) {
        char line[16 * 9 + 1];
        u32 used = 0;
        for (u32 i = 0; i < count && i < 16; i++) {
            u32 bits;
            memcpy(&bits, &values[i], sizeof(bits));
            used += snprintf(line + used, sizeof(line) - used, " %08lx", static_cast< unsigned long >(bits));
        }
        line[used] = '\0';
        hostLog("%s:%s\n", label, line);
    }
    u64 sClipTicks;
    u8 sCodes[65536];  // outcode of each vertex of the arena window, for the draw being clipped
    bool sReportedOverflow;

    // Clipped triangles waiting to be drawn (one triangle list per flush).
    const u32 kOutVertices = 3072;
    HostGfxModelVertex sOut[kOutVertices];

    void lerpClipVertex(void* pOut, const void* pA, const void* pB, float t) {
        const ClipVertex& a = *static_cast< const ClipVertex* >(pA);
        const ClipVertex& b = *static_cast< const ClipVertex* >(pB);
        ClipVertex& o = *static_cast< ClipVertex* >(pOut);
        for (u32 i = 0; i < 4; i++) {
            o.clip[i] = a.clip[i] + (b.clip[i] - a.clip[i]) * t;
        }
        const f32* fa = &a.v.x;
        const f32* fb = &b.v.x;
        f32* fo = &o.v.x;
        for (u32 i = 0; i < 6; i++) {
            fo[i] = fa[i] + (fb[i] - fa[i]) * t;
        }
        for (u32 u = 0; u < 3; u++) {
            for (u32 i = 0; i < 3; i++) {
                o.v.tex[u][i] = a.v.tex[u][i] + (b.v.tex[u][i] - a.v.tex[u][i]) * t;
            }
        }
        for (u32 i = 0; i < 4; i++) {
            o.v.rgba[i] = static_cast< u8 >(a.v.rgba[i] + (b.v.rgba[i] - a.v.rgba[i]) * t + 0.5f);
        }
        o.v.matrix = a.v.matrix;
        memcpy(o.v.texRow, a.v.texRow, sizeof(o.v.texRow));
    }

    // Position matrix row r of the transform (3 palette rows) applied to model position p.
    inline void toView(const HostGfxTransform& t, u32 row, const f32* p, f32* out) {
        for (u32 i = 0; i < 3; i++) {
            const f32* m = t.palette[row + i];
            out[i] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
        }
    }

    inline void toClip(const HostGfxTransform& t, const f32* view, f32* out) {
        for (u32 i = 0; i < 4; i++) {
            const f32* c = t.clip[i];
            out[i] = c[0] * view[0] + c[1] * view[1] + c[2] * view[2] + c[3];
        }
    }

    void setClip(const HostGfxTransform& t, ClipVertex& cv) {
        f32 view[3];
        u32 row = cv.v.matrix < 28 ? cv.v.matrix : 0;
        toView(t, row, &cv.v.x, view);
        toClip(t, view, cv.clip);
    }

    // The normal transform the shader applies with a position matrix (rows m): its cofactor rows
    // (inverse transpose up to a scale) with nrmcofactor, else the matrix itself.
    void normalToView(const HostGfxTransform& t, const Mtx m, const f32* n, f32* out) {
        if (t.nrmcofactor) {
            f32 c[3][3];
            for (u32 i = 0; i < 3; i++) {
                const f32* r1 = m[(i + 1) % 3];
                const f32* r2 = m[(i + 2) % 3];
                c[i][0] = r1[1] * r2[2] - r1[2] * r2[1];
                c[i][1] = r1[2] * r2[0] - r1[0] * r2[2];
                c[i][2] = r1[0] * r2[1] - r1[1] * r2[0];
            }
            for (u32 i = 0; i < 3; i++) {
                out[i] = c[i][0] * n[0] + c[i][1] * n[1] + c[i][2] * n[2];
            }
        } else {
            for (u32 i = 0; i < 3; i++) {
                out[i] = m[i][0] * n[0] + m[i][1] * n[1] + m[i][2] * n[2];
            }
        }
    }

    // Brings vertex v (its own matrix row) into the space of matrix row `row`, keeping its view-space
    // position and normal.
    void moveToRow(const HostGfxTransform& t, u32 row, const Mtx inverse, HostGfxModelVertex& v) {
        const Mtx& own = *reinterpret_cast< const Mtx* >(t.palette[v.matrix < 28 ? v.matrix : 0]);
        const Mtx& target = *reinterpret_cast< const Mtx* >(t.palette[row]);
        f32 view[3];
        toView(t, v.matrix < 28 ? v.matrix : 0, &v.x, view);
        f32 p[3];
        for (u32 i = 0; i < 3; i++) {
            p[i] = inverse[i][0] * view[0] + inverse[i][1] * view[1] + inverse[i][2] * view[2] + inverse[i][3];
        }
        if (t.nrmpalette) {
            f32 nView[3], n[3];
            normalToView(t, own, &v.nx, nView);
            if (t.nrmcofactor) {
                // Undo the cofactor transform: its inverse is the transpose up to a scale.
                for (u32 i = 0; i < 3; i++) {
                    n[i] = target[0][i] * nView[0] + target[1][i] * nView[1] + target[2][i] * nView[2];
                }
            } else {
                for (u32 i = 0; i < 3; i++) {
                    n[i] = inverse[i][0] * nView[0] + inverse[i][1] * nView[1] + inverse[i][2] * nView[2];
                }
            }
            v.nx = n[0];
            v.ny = n[1];
            v.nz = n[2];
        }
        v.x = p[0];
        v.y = p[1];
        v.z = p[2];
        v.matrix = static_cast< u8 >(row);
    }

    bool flushOut(u32& used, const HostGfxState* pState, const HostGfxTransform& t, u32 version) {
        if (used == 0) {
            return true;
        }
        HostGfxModelVertex* out;
        unsigned long first = hostGfxArenaAlloc(used, 0, &out);
        if (first == HOST_GFX_NO_ARENA) {
            if (!sReportedOverflow) {
                hostLog("native clip: no transient vertices left, clipped triangles dropped\n");
                sReportedOverflow = true;
            }
            used = 0;
            return true;
        }
        memcpy(out, sOut, used * sizeof(HostGfxModelVertex));
        hostGfxDrawArena(first, nullptr, used, HOST_GFX_TRIANGLES, pState, &t, version);
        used = 0;
        return true;
    }

}  // namespace

namespace {

    // ##############################################################################################
    // # Is a draw inside, outside, or across?                                                      #
    // #                                                                                            #
    // # rvlNativeClipNeeded tests each slot's bounding sphere against the planes in view space:    #
    // # every sphere outside one screen edge culls the draw, a sphere across the near plane or the #
    // # guard band sends it to the clipper, and draws too small on screen at the lowered detail    #
    // # level are dropped. A draw with NaN or infinite matrices is dropped whole.                  #
    // ##############################################################################################
    // The clip planes of clip.c as planes of view space (clip-space plane p becomes p * clip),
    // normalized: [0] with the guard band, [1] with the screen's edges instead (a draw whose every
    // sphere is wholly outside one of them is not seen at all). They depend on the clip matrix
    // alone, which most draws of a frame share.
    const f32 (*viewPlanes(const HostGfxTransform& t))[HOST_CLIP_PLANES][4] {
        static const f32 kPlanes[2][HOST_CLIP_PLANES][4] = {
            {{0, 0, 1, 1}, {-1, 0, 0, HOST_CLIP_GUARD}, {1, 0, 0, HOST_CLIP_GUARD}, {0, -1, 0, HOST_CLIP_GUARD}, {0, 1, 0, HOST_CLIP_GUARD}},
            {{0, 0, 1, 1}, {-1, 0, 0, 1}, {1, 0, 0, 1}, {0, -1, 0, 1}, {0, 1, 0, 1}},
        };
        static f32 planes[2][HOST_CLIP_PLANES][4];
        static f32 planesClip[4][4];
        static bool planesValid;
        if (planesValid && memcmp(planesClip, t.clip, sizeof(planesClip)) == 0) {
            return planes;
        }
        for (u32 k = 0; k < 2; k++) {
            for (u32 p = 0; p < HOST_CLIP_PLANES; p++) {
                f32* q = planes[k][p];
                const f32* c = kPlanes[k][p];
                for (u32 j = 0; j < 4; j++) {
                    q[j] = c[0] * t.clip[0][j] + c[1] * t.clip[1][j] + c[2] * t.clip[2][j] + c[3] * t.clip[3][j];
                }
                f32 length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
                if (length > 0.0f) {
                    for (u32 j = 0; j < 4; j++) {
                        q[j] /= length;
                    }
                }
            }
        }
        memcpy(planesClip, t.clip, sizeof(planesClip));
        planesValid = true;
        return planes;
    }

    // Where a model-space sphere (centre, radius) of position matrix row `row` lies against the
    // guard-band planes: inside all, wholly outside one, or crossing; *pSeen (optional) whether it
    // reaches the screen. Invalid with NaN or infinite values.
    enum { SPHERE_INSIDE, SPHERE_OUTSIDE, SPHERE_CROSSING, SPHERE_INVALID };
    int classifySphere(const HostGfxTransform& t, const f32 (*planes)[HOST_CLIP_PLANES][4], u32 row, const f32* s, bool* pSeen,
                       f32* pPixels = nullptr) {
        f32 centre[3];
        toView(t, row, s, centre);
        // The matrix may scale: its Frobenius norm bounds the radius it can grow to.
        f32 scale = 0.0f;
        for (u32 i = 0; i < 3; i++) {
            const f32* m = t.palette[row + i];
            scale += m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
        }
        const f32 radius = s[3] * std::sqrt(scale);
        if (pPixels != nullptr) {
            // The radius in pixels of the 240-pixel screen height, for a perspective projection
            // (clip row 0 is the Wii's y row, row 3 gives w); a huge size when the centre is not
            // in front of the camera.
            const f32 w = t.clip[3][0] * centre[0] + t.clip[3][1] * centre[1] + t.clip[3][2] * centre[2] + t.clip[3][3];
            *pPixels = w > radius ? radius * std::fabs(t.clip[0][1]) * 120.0f / w : 1.0e30f;
        }
        bool outside = false, crossing = false;
        for (u32 p = 0; p < HOST_CLIP_PLANES; p++) {
            const f32* q = planes[0][p];
            const f32 distance = q[0] * centre[0] + q[1] * centre[1] + q[2] * centre[2] + q[3];
            if (!std::isfinite(distance) || !std::isfinite(radius)) {
                return SPHERE_INVALID;
            }
            if (distance < -radius) {
                outside = true;
            } else if (distance < radius) {
                crossing = true;
            }
        }
        if (pSeen != nullptr) {
            bool seen = !outside;
            for (u32 p = 0; p < HOST_CLIP_PLANES && seen; p++) {
                const f32* q = planes[1][p];
                seen = q[0] * centre[0] + q[1] * centre[1] + q[2] * centre[2] + q[3] >= -radius;
            }
            *pSeen = seen;
        }
        return outside ? SPHERE_OUTSIDE : crossing ? SPHERE_CROSSING : SPHERE_INSIDE;
    }

    // Vertex outcodes for the draw being clipped, computed on first use: sStamp[i] == sStampNow
    // marks sCodes[i] as this draw's (bit 7 of a code: invalid).
    u16 sStamp[65536];
    u16 sStampNow;

}  // namespace

int rvlNativeClipNeeded(const RvlNativeDraw& d, const HostGfxTransform& t) {
    // A draw whose every position matrix holds NaN or infinite values has no shape to draw: it
    // went to the clipper, which tested and dropped its triangles one by one (up to 7000 a frame
    // in Good Egg's play, about 10 ms: CIA 0.12.6).
    const u32 matrixCount = d.matrixCount < 10 ? d.matrixCount : 10;
    const u32 slots = d.slotMask != 0 ? d.slotMask : (1u << matrixCount) - 1;
    bool finiteSlot = false;
    for (u32 slot = 0; slot < 10 && !finiteSlot; slot++) {
        if (!(slots & (1u << slot))) {
            continue;
        }
        const f32* m = t.palette[slot * 3];
        finiteSlot = true;
        for (u32 k = 0; k < 12 && finiteSlot; k++) {
            finiteSlot = std::isfinite(m[k]);
        }
    }
    // So does a draw under a NaN or infinite view-to-clip matrix (every vertex of it is invalid).
    bool finiteClip = true;
    for (u32 k = 0; k < 16 && finiteClip; k++) {
        finiteClip = std::isfinite(t.clip[k / 4][k % 4]);
    }
    if (!finiteClip || (slots != 0 && !finiteSlot)) {
        if (sMatrixReports < 3) {
            sMatrixReports++;
            hostLog("native clip: draw with NaN or infinite %s matrices (first %lu, %lu indices, %lu matrices), dropped\n",
                    finiteClip ? "position" : "view-to-clip", static_cast< unsigned long >(d.first),
                    static_cast< unsigned long >(d.indexCount), static_cast< unsigned long >(d.matrixCount));
            logFloatBits("native clip:   view-to-clip rows", t.clip[0], 16);
            logFloatBits("native clip:   palette slot 0 rows", t.palette[0], 12);
        }
        sInvalidTriangles += d.indexCount / 3;
        sInvalidMatrixDraws++;
        return NATIVE_CLIP_CULL;
    }
    if (d.spheres == nullptr || d.slotMask == 0) {
        return NATIVE_CLIP_CPU;
    }
    const f32(*planes)[HOST_CLIP_PLANES][4] = viewPlanes(t);
    int needed = NATIVE_CLIP_NONE;
    bool visible = false;
    // With the 96 MB of an Old 3DS, while the game runs behind the clock (hostPerfDetail under 1), a perspective draw whose every
    // sphere is under minPixels in radius on screen is not drawn: up to 2 pixels (4 wide) at the
    // lowest detail. Far small objects are most of a dense scene's draws (the Comet Observatory:
    // about 230 a frame at 38 us each, CIA 0.14.5).
    const f32 detail = hostPerfDetail();
    const f32 minPixels = detail < 1.0f && t.clip[3][3] == 0.0f && hostLowMemory() ? 2.0f * (1.0f - detail) / 0.6f : 0.0f;
    bool large = minPixels == 0.0f;
    for (u32 slot = 0; slot < 10; slot++) {
        if (!(d.slotMask & (1u << slot))) {
            continue;
        }
        bool seen;
        f32 pixels;
        const int where = classifySphere(t, planes, slot * 3, d.spheres[slot], &seen, large ? nullptr : &pixels);
        if (!large && pixels >= minPixels) {
            large = true;
        }
        if (where == SPHERE_INVALID) {
            return NATIVE_CLIP_CPU;  // NaN or infinite bounds or matrices: only the triangle test can tell
        }
        // The PICA clips against the near plane itself. The CPU clipped crossing draws because huge
        // near-clipped triangles were thought to stall it (Oct 2026); that stall was the shader's
        // unwritten outputs (CIA 0.10.14), and the clipper never clipped a triangle until CIA
        // 0.12.9, when it cost up to 18 ms a frame on Good Egg's planet. Crossing draws go to the
        // GPU as they are; debug option cpuclip=1 clips them on the CPU again.
        if (where == SPHERE_CROSSING && hostDebugCpuClip()) {
            needed = NATIVE_CLIP_CPU;
        }
        visible |= seen;
    }
    if (!visible) {
        sCulledDraws++;
        return NATIVE_CLIP_CULL;
    }
    if (!large) {
        sSmallDraws++;
        return NATIVE_CLIP_CULL;
    }
    return needed;
}

// ##############################################################################################
// # Subdividing triangles of projected textures                                                #
// #                                                                                            #
// # Draws with a 3x4 (s/q, t/q) texture coordinate on unit 1 or 2. GX divides it per pixel;    #
// # the shader divides at the vertex (unit 0 alone has the PICA's projection mode, gfx.c), so  #
// # across a triangle whose q changes a lot the coordinates bend, in steps along the edges:    #
// # the title's sea (CometNearOrbitSky's EarthFar_v, clouds on unit 2, q = 7000 - y).          #
// #                                                                                            #
// # Such triangles are cut into an n x n grid of smaller ones, n growing with the ratio of the #
// # vertices' q: the error of the division at the vertex falls as 1 / n^2, and colours and     #
// # other coordinates stay exact (they are linear in model space, where the new vertices are   #
// # interpolated). Triangles whose q hardly changes go as they are. (Scaling the position by 1 #
// # / q instead, CIA 0.20.2-0.20.4, weighted the vertex colours by q: the sea lost its dark    #
// # lower half.) A budget per frame, a minimum size on screen and the memory of draws that     #
// # needed no cut keep the cost down.                                                          #
// ##############################################################################################
namespace {
    const u32 kSubMaxN = 8;
    // Grid vertices a frame may make (it has 16384 transient ones). Without this limit and the
    // screen sizes below, the Comet Observatory's draws made about 200000 triangles a frame
    // (CIA 0.20.5, 3 FPS).
    const u32 kSubBudget = 4000;
    const f32 kSubMinDrawPixels = 40.0f;  // a draw's largest sphere radius worth testing
    const f32 kSubPiecePixels = 32.0f;    // grid triangles no smaller than this on screen
    const u32 kSubQCache = 16384;         // vertices whose q the triangle pass computes once
    unsigned long sSubFrame;
    u32 sSubFrameMade;

    // Draws whose last pass cut nothing skip the pass for kSubRecheckFrames: in the Comet
    // Observatory's busy places the pass over large draws with nothing to cut took up to 7 ms a
    // drawn frame (CIA 0.20.7, 'Native clip' 250 ms per 100 loops for the same new vertices).
    const unsigned long kSubRecheckFrames = 30;
    struct SubHistory {
        const u16* indices;
        u32 first;
        unsigned long checked;
        unsigned long scanned;  // the frame of the last pass over every triangle
        bool cut;
    };
    SubHistory sSubHistory[128];
    // Between whole passes, every kSubScanFrames drawn frames, only the runs of kSubRunTriangles
    // triangles that had one to cut at the last whole pass are looked at; the others go whole.
    // One large draw cut every frame took about 2.8 ms a drawn frame in the paused Comet
    // Observatory (CIA 0.20.12) for about 200 triangles cut. A triangle that comes to need cutting
    // in between is cut at most kSubScanFrames - 1 frames late.
    const unsigned long kSubScanFrames = 4;
    const u32 kSubRunTriangles = 32;
    const u32 kSubRunMaskBytes = 512;  // runs remembered per draw (131072 triangles)
    u8 sSubRunMask[128][kSubRunMaskBytes];

    SubHistory& subHistory(const RvlNativeDraw& d) {
        const u32 key = (reinterpret_cast< uintptr_t >(d.indices) >> 2) ^ d.first;
        return sSubHistory[(key ^ (key >> 7)) & 127];
    }
    const u32 kSubBatchIndices = 9216;
    u16 sSubIndices[kSubBatchIndices];
    u32 sSubDraws, sSubTriangles, sSubMade;
    // For the log: the pass's parts (setup, triangles, the draws), its triangles looked at one by one
    // and copied by runs, and its whole passes.
    u64 sSubSetupTicks, sSubLoopTicks, sSubSubmitTicks;
    u32 sSubLooked, sSubCopied, sSubWholePasses;

    // The vertex's q for unit u, as the shader computes it (matrix, then the dual transform's row).
    f32 vertexQ(const HostGfxTransform& t, u32 u, const HostGfxModelVertex& v) {
        const f32* in = v.tex[u];
        f32 r[3];
        for (u32 i = 0; i < 3; i++) {
            const f32* m = t.texmtx[u * 3 + i];
            r[i] = m[0] * in[0] + m[1] * in[1] + m[2] * in[2] + m[3];
        }
        const f32* p = t.texpost[u * 3 + 2];
        return p[0] * r[0] + p[1] * r[1] + p[2] * r[2] + p[3];
    }

    HostGfxModelVertex gridVertex(const HostGfxModelVertex& a, const HostGfxModelVertex& b, const HostGfxModelVertex& c, f32 fb,
                                  f32 fc) {
        HostGfxModelVertex o = a;
        const f32* pa = &a.x;
        const f32* pb = &b.x;
        const f32* pc = &c.x;
        f32* po = &o.x;
        for (u32 i = 0; i < 6; i++) {
            po[i] = pa[i] + (pb[i] - pa[i]) * fb + (pc[i] - pa[i]) * fc;
        }
        for (u32 u = 0; u < 3; u++) {
            for (u32 i = 0; i < 3; i++) {
                o.tex[u][i] = a.tex[u][i] + (b.tex[u][i] - a.tex[u][i]) * fb + (c.tex[u][i] - a.tex[u][i]) * fc;
            }
        }
        for (u32 i = 0; i < 4; i++) {
            o.rgba[i] = static_cast< u8 >(a.rgba[i] + (b.rgba[i] - a.rgba[i]) * fb + (c.rgba[i] - a.rgba[i]) * fc + 0.5f);
        }
        return o;
    }

    void flushGrid(u32& vertices, u32& indices, const HostGfxState* pState, const HostGfxTransform& t, u32 version) {
        if (indices != 0) {
            HostGfxModelVertex* out;
            const unsigned long first = hostGfxArenaAlloc(vertices, 0, &out);
            u16* gpuIndices = first != HOST_GFX_NO_ARENA ? hostGfxIndexAllocTransient(indices) : nullptr;
            if (gpuIndices != nullptr) {
                memcpy(out, sOut, vertices * sizeof(HostGfxModelVertex));
                const u16 base = static_cast< u16 >(first % 65536);
                for (u32 i = 0; i < indices; i++) {
                    gpuIndices[i] = static_cast< u16 >(base + sSubIndices[i]);
                }
                hostGfxDrawNativeIndexed(first, gpuIndices, indices, pState, &t, version);
            } else if (!sReportedOverflow) {
                hostLog("native subdivide: no transient vertices or indices left, triangles dropped\n");
                sReportedOverflow = true;
            }
        }
        vertices = indices = 0;
    }
}  // namespace

bool rvlNativeSubdivideNeeded(const HostGfxTransform& t) {
    for (u32 u = 1; u < 3; u++) {
        if (t.divide[u] && !t.normalize[u] && !t.vtxmtx[u]) {
            return true;
        }
    }
    return false;
}

bool rvlNativeSubdivideSubmit(const RvlNativeDraw& d, const HostGfxState* pState, const HostGfxTransform& t, u32 version) {
    if (hostGfxFrameId() != sSubFrame) {
        sSubFrame = hostGfxFrameId();
        sSubFrameMade = 0;
    }
    // Small draws, or none of the frame's budget left: as they are, without the triangle pass.
    bool large = d.spheres == nullptr || d.slotMask == 0;
    if (!large) {
        const f32(*planes)[HOST_CLIP_PLANES][4] = viewPlanes(t);
        for (u32 slot = 0; slot < 10 && !large; slot++) {
            if (d.slotMask & (1u << slot)) {
                f32 pixels = 0.0f;
                classifySphere(t, planes, slot * 3, d.spheres[slot], nullptr, &pixels);
                large = pixels >= kSubMinDrawPixels;
            }
        }
    }
    SubHistory& history = subHistory(d);
    const bool known = history.indices == d.indices && history.first == d.first;
    if (!large || sSubFrameMade >= kSubBudget || (known && !history.cut && hostGfxFrameId() - history.checked < kSubRecheckFrames)) {
        return hostGfxDrawNativeIndexed(d.first, d.indices, d.indexCount, pState, &t, version) != 0;
    }
    const u64 start = hostGetTick();
    sSubDraws++;
    const u32 windowStart = d.first - d.first % 65536;
    const HostGfxModelVertex* vertices = hostGfxArenaVertex(windowStart);
    u32 units[2], unitCount = 0;
    for (u32 u = 1; u < 3; u++) {
        if (t.divide[u] && !t.normalize[u] && !t.vtxmtx[u]) {
            units[unitCount++] = u;
        }
    }
    u32 made = 0, batchVertices = 0, batchIndices = 0;
    // Triangles left whole keep their own vertices.
    u16* plain = hostGfxIndexAllocTransient(d.indexCount);
    if (plain == nullptr) {
        return false;
    }
    u32 plainCount = 0;
    Mtx inverses[10];
    u32 inverseMask = 0;
    // Each vertex's q once: a vertex serves about six triangles, and the scan computed it for
    // every one (about 3.3 ms a drawn frame for one large draw in the paused Comet Observatory,
    // CIA 0.20.11). Only when the draw's vertices span a range the cache holds.
    const unsigned long frame = hostGfxFrameId();
    const bool wholePass = !known || frame - history.scanned >= kSubScanFrames;
    u8* runMask = sSubRunMask[&history - sSubHistory];
    if (wholePass) {
        memset(runMask, 0, kSubRunMaskBytes);
        history.scanned = frame;
    }
    static f32 sVertexQ[2][kSubQCache];
    u32 lowest = 0xFFFF, highest = 0;
    if (wholePass) {
        for (u32 k = 0; k < d.indexCount; k++) {
            const u32 index = d.indices[k];
            lowest = index < lowest ? index : lowest;
            highest = index > highest ? index : highest;
        }
    }
    const bool qCached = wholePass && d.indexCount != 0 && highest - lowest < kSubQCache;
    if (qCached) {
        for (u32 i = 0; i < unitCount; i++) {
            for (u32 index = lowest; index <= highest; index++) {
                sVertexQ[i][index - lowest] = vertexQ(t, units[i], vertices[index]);
            }
        }
    }
    const u32 wholeIndices = d.indexCount - d.indexCount % 3;
    sSubWholePasses += wholePass ? 1 : 0;
    const u64 loopStart = hostGetTick();
    sSubSetupTicks += loopStart - start;
    for (u32 k = 0; k + 2 < d.indexCount; k += 3) {
        const u32 run = k / 3 / kSubRunTriangles;
        const bool remembered = run < kSubRunMaskBytes * 8;
        if (!wholePass && remembered && (k / 3) % kSubRunTriangles == 0 && !(runMask[run >> 3] & (1u << (run & 7)))) {
            // A run with nothing to cut at the last whole pass: whole, in one copy.
            const u32 end = k + kSubRunTriangles * 3 < wholeIndices ? k + kSubRunTriangles * 3 : wholeIndices;
            memcpy(plain + plainCount, d.indices + k, (end - k) * sizeof(u16));
            plainCount += end - k;
            sSubCopied += (end - k) / 3;
            k = end - 3;
            continue;
        }
        if (sSubFrameMade + 6 > kSubBudget) {
            // The frame's budget is spent: the rest of the draw as it is, in one copy (and looked
            // at again until the next whole pass).
            memcpy(plain + plainCount, d.indices + k, (d.indexCount - k) * sizeof(u16));
            plainCount += d.indexCount - k;
            if (wholePass && remembered) {
                memset(runMask + run / 8, 0xFF, kSubRunMaskBytes - run / 8);
            }
            break;
        }
        sSubLooked++;
        // First the vertices' q, which is cheap: most triangles change it too little to be cut and
        // go whole without their screen positions (about 8 ms a drawn frame in the Comet
        // Observatory, CIA 0.20.6, when those came first).
        const HostGfxModelVertex* p[3] = {&vertices[d.indices[k]], &vertices[d.indices[k + 1]], &vertices[d.indices[k + 2]]};
        // n from the largest ratio of the vertices' q over the divided units: 1.1 gives 2, 1.5 gives
        // 5, 1.8 and more the most; none when q changes sign.
        f32 ratio = 1.0f;
        for (u32 i = 0; i < unitCount; i++) {
            f32 q[3];
            for (u32 j = 0; j < 3; j++) {
                q[j] = qCached ? sVertexQ[i][d.indices[k + j] - lowest] : vertexQ(t, units[i], *p[j]);
            }
            f32 low = q[0] < q[1] ? (q[0] < q[2] ? q[0] : q[2]) : (q[1] < q[2] ? q[1] : q[2]);
            f32 high = q[0] > q[1] ? (q[0] > q[2] ? q[0] : q[2]) : (q[1] > q[2] ? q[1] : q[2]);
            if (low <= 0.0f && high >= 0.0f) {
                low = high = -1.0f;
                ratio = 0.0f;  // q changes sign (behind the projector): nothing exact to come close to
                break;
            } else if (low < 0.0f) {
                const f32 swap = -low;
                low = -high;
                high = swap;
            }
            if (low > 0.0f && high / low > ratio) {
                ratio = high / low;
            }
        }
        u32 n = ratio > 1.0f ? static_cast< u32 >((ratio - 1.0f) * 10.0f) + 1 : 1;
        if (n > kSubMaxN) {
            n = kSubMaxN;
        }
        HostGfxModelVertex v[3];
        ClipVertex cv[3];
        if (n > 1) {
            unsigned codes[3];
            for (u32 j = 0; j < 3; j++) {
                v[j] = *p[j];
                cv[j].v = v[j];
                setClip(t, cv[j]);
                codes[j] = hostClipOutcode(cv[j].clip);
            }
            if (((codes[0] | codes[1] | codes[2]) & HOST_CLIP_INVALID_BIT) || (codes[0] & codes[1] & codes[2])) {
                continue;  // no shape, or wholly outside one plane (the GPU would drop it)
            }
        }
        // No smaller than kSubPiecePixels on screen (the 400 x 240 screen spans 2 units of x / w
        // and y / w); a triangle reaching behind the camera stays whole.
        if (n > 1) {
            if (cv[0].clip[3] <= 0.0f || cv[1].clip[3] <= 0.0f || cv[2].clip[3] <= 0.0f) {
                n = 1;
            } else {
                f32 sx[3], sy[3];
                for (u32 j = 0; j < 3; j++) {
                    sx[j] = cv[j].clip[0] / cv[j].clip[3];
                    sy[j] = cv[j].clip[1] / cv[j].clip[3];
                }
                f32 extent = 0.0f;
                for (u32 j = 0; j < 3; j++) {
                    const u32 o = (j + 1) % 3;
                    const f32 dx = std::fabs(sx[j] - sx[o]) * 200.0f, dy = std::fabs(sy[j] - sy[o]) * 200.0f;
                    extent = dx > extent ? dx : extent;
                    extent = dy > extent ? dy : extent;
                }
                const u32 bySize = static_cast< u32 >(extent / kSubPiecePixels);
                n = bySize < n ? (bySize > 1 ? bySize : 1) : n;
            }
        }
        const u32 gridVertices = (n + 1) * (n + 2) / 2;
        // Runs are remembered for a triangle cut (or left whole for the budget), not for every one
        // whose q changes enough: most of those stay whole for their size on screen, which is the
        // costly test (CIA 0.20.13: the pass still took about 3 ms a drawn frame).
        if (n > 1 && remembered) {
            runMask[run >> 3] |= static_cast< u8 >(1u << (run & 7));
        }
        if (n == 1 || sSubFrameMade + gridVertices > kSubBudget) {
            plain[plainCount++] = d.indices[k];
            plain[plainCount++] = d.indices[k + 1];
            plain[plainCount++] = d.indices[k + 2];
            continue;
        }
        const u32 row = v[0].matrix < 28 ? v[0].matrix : 0;
        for (u32 j = 1; j < 3; j++) {
            if (v[j].matrix == v[0].matrix) {
                continue;
            }
            const u32 slot = row / 3;
            if (!(inverseMask & (1u << slot))) {
                if (!PSMTXInverse(*reinterpret_cast< const Mtx* >(t.palette[row]), inverses[slot])) {
                    PSMTXIdentity(inverses[slot]);
                }
                inverseMask |= 1u << slot;
            }
            moveToRow(t, row, inverses[slot], v[j]);
        }
        const u32 nv = (n + 1) * (n + 2) / 2, ni = 3 * n * n;
        if (batchVertices + nv > kOutVertices || batchIndices + ni > kSubBatchIndices) {
            flushGrid(batchVertices, batchIndices, pState, t, version);
        }
        // Grid points (i along a->b, j along a->c), row by row: point (i, j) at offset[i] + j. The
        // same values as gridVertex, with the edges' differences and 1 / n once a triangle (the
        // grid's vertices were about 1.6 us each, about 3 ms a drawn frame in the Comet Observatory
        // for one draw of about 100 triangles, all cut: CIA 0.20.15).
        u32 offset[kSubMaxN + 2];
        u32 at = 0;
        {
            const f32* pa = &v[0].x;
            const f32* pb = &v[1].x;
            const f32* pc = &v[2].x;
            f32 base[15], toB[15], toC[15], colorBase[4], colorB[4], colorC[4];
            for (u32 e = 0; e < 6; e++) {
                base[e] = pa[e], toB[e] = pb[e] - pa[e], toC[e] = pc[e] - pa[e];
            }
            for (u32 u = 0; u < 3; u++) {
                for (u32 e = 0; e < 3; e++) {
                    base[6 + u * 3 + e] = v[0].tex[u][e];
                    toB[6 + u * 3 + e] = v[1].tex[u][e] - v[0].tex[u][e];
                    toC[6 + u * 3 + e] = v[2].tex[u][e] - v[0].tex[u][e];
                }
            }
            for (u32 e = 0; e < 4; e++) {
                colorBase[e] = v[0].rgba[e] + 0.5f;
                colorB[e] = static_cast< f32 >(v[1].rgba[e] - v[0].rgba[e]);
                colorC[e] = static_cast< f32 >(v[2].rgba[e] - v[0].rgba[e]);
            }
            const f32 step = 1.0f / static_cast< f32 >(n);
            for (u32 i = 0; i <= n; i++) {
                offset[i] = at;
                const f32 fb = static_cast< f32 >(i) * step;
                for (u32 j = 0; j + i <= n; j++) {
                    const f32 fc = static_cast< f32 >(j) * step;
                    HostGfxModelVertex& o = sOut[batchVertices + at++];
                    o.matrix = v[0].matrix;
                    o.texRow[0] = v[0].texRow[0], o.texRow[1] = v[0].texRow[1], o.texRow[2] = v[0].texRow[2];
                    f32* po = &o.x;
                    for (u32 e = 0; e < 6; e++) {
                        po[e] = base[e] + toB[e] * fb + toC[e] * fc;
                    }
                    for (u32 u = 0; u < 3; u++) {
                        for (u32 e = 0; e < 3; e++) {
                            o.tex[u][e] = base[6 + u * 3 + e] + toB[6 + u * 3 + e] * fb + toC[6 + u * 3 + e] * fc;
                        }
                    }
                    for (u32 e = 0; e < 4; e++) {
                        o.rgba[e] = static_cast< u8 >(colorBase[e] + colorB[e] * fb + colorC[e] * fc);
                    }
                }
            }
        }
        for (u32 i = 0; i < n; i++) {
            for (u32 j = 0; j + i < n; j++) {
                const u16 p = static_cast< u16 >(batchVertices + offset[i] + j);
                const u16 pi = static_cast< u16 >(batchVertices + offset[i + 1] + j);
                sSubIndices[batchIndices++] = p;
                sSubIndices[batchIndices++] = pi;
                sSubIndices[batchIndices++] = static_cast< u16 >(p + 1);
                if (j + i + 1 < n) {
                    sSubIndices[batchIndices++] = pi;
                    sSubIndices[batchIndices++] = static_cast< u16 >(pi + 1);
                    sSubIndices[batchIndices++] = static_cast< u16 >(p + 1);
                }
            }
        }
        batchVertices += nv;
        made += nv;
        sSubFrameMade += nv;
        sSubTriangles += n * n;
    }
    const u64 submitStart = hostGetTick();
    sSubLoopTicks += submitStart - loopStart;
    flushGrid(batchVertices, batchIndices, pState, t, version);
    if (plainCount != 0) {
        hostGfxDrawNativeIndexed(d.first, plain, plainCount, pState, &t, version);
    }
    sSubMade += made;
    history.indices = d.indices;
    history.first = d.first;
    history.checked = hostGfxFrameId();
    history.cut = made != 0;
    const u64 end = hostGetTick();
    sSubSubmitTicks += end - submitStart;
    sClipTicks += end - start;
    return true;
}

// ##############################################################################################
// # Clipping a draw                                                                            #
// #                                                                                            #
// # The triangles wholly inside go into one index list drawn with the cached vertices; the     #
// # others are clipped (clip.c) into transient vertices and drawn after. A large group's runs  #
// # (gx_state.h, RvlNativeChunk) are judged by their spheres first, so a run wholly inside or  #
// # outside needs no per-triangle test.                                                        #
// ##############################################################################################
bool rvlNativeClipSubmit(const RvlNativeDraw& d, const HostGfxState* pState, const HostGfxTransform& t, u32 version) {
    const u64 start = hostGetTick();
    sClipDraws++;
    u32 windowStart = d.first - d.first % 65536;
    const HostGfxModelVertex* vertices = hostGfxArenaVertex(windowStart);
    u16* inside = hostGfxIndexAllocTransient(d.indexCount);
    if (inside == nullptr) {
        return false;
    }
    if (++sStampNow == 0) {
        memset(sStamp, 0, sizeof(sStamp));
        sStampNow = 1;
    }
    // A vertex's outcode, computed once per draw with the clip matrix times its position rows.
    f32 folded[28][4][4];
    u32 foldedMask = 0;
    auto outcode = [&](u32 i) -> unsigned {
        if (sStamp[i] != sStampNow) {
            const HostGfxModelVertex& v = vertices[i];
            const u32 row = v.matrix < 28 ? v.matrix : 0;
            f32(*m)[4] = folded[row];
            if (!(foldedMask & (1u << row))) {
                for (u32 r = 0; r < 4; r++) {
                    const f32* c = t.clip[r];
                    for (u32 k = 0; k < 4; k++) {
                        m[r][k] = c[0] * t.palette[row][k] + c[1] * t.palette[row + 1][k] + c[2] * t.palette[row + 2][k] +
                                  (k == 3 ? c[3] : 0.0f);
                    }
                }
                foldedMask |= 1u << row;
            }
            f32 clip[4];
            for (u32 r = 0; r < 4; r++) {
                clip[r] = m[r][0] * v.x + m[r][1] * v.y + m[r][2] * v.z + m[r][3];
            }
            const unsigned code = hostClipOutcode(clip);
            sCodes[i] = static_cast< u8 >(code == HOST_CLIP_INVALID ? 0x80 : code);
            sStamp[i] = sStampNow;
        }
        const u8 code = sCodes[i];
        return code & 0x80 ? HOST_CLIP_INVALID : code;
    };
    u32 insideCount = 0, used = 0;
    // Inverses of the position matrices, made when a triangle mixes matrices.
    Mtx inverses[10];
    u32 inverseMask = 0;
    // Triangles [k0, k1) (index positions) one by one.
    auto clipTriangles = [&](u32 k0, u32 k1) {
        for (u32 k = k0; k + 2 < k1; k += 3) {
            ClipVertex polygon[HOST_CLIP_MAX_VERTICES], scratch[HOST_CLIP_MAX_VERTICES];
            const unsigned codes[3] = {outcode(d.indices[k]), outcode(d.indices[k + 1]), outcode(d.indices[k + 2])};
            if ((codes[0] | codes[1] | codes[2]) == 0) {
                inside[insideCount++] = d.indices[k];
                inside[insideCount++] = d.indices[k + 1];
                inside[insideCount++] = d.indices[k + 2];
                continue;
            }
            if ((codes[0] | codes[1] | codes[2]) & HOST_CLIP_INVALID_BIT) {
                // A NaN or infinite vertex (skinning, matrices): the triangle has no shape to draw.
                if (sInvalidReports < 3) {
                    sInvalidReports++;
                    // The first invalid vertex, its position matrix rows and the clip matrix's w row.
                    const u32 j = codes[0] & HOST_CLIP_INVALID_BIT ? 0 : codes[1] & HOST_CLIP_INVALID_BIT ? 1 : 2;
                    const HostGfxModelVertex& v = vertices[d.indices[k + j]];
                    const u32 row = v.matrix < 28 ? v.matrix : 0;
                    hostLog("native clip: draw with NaN or infinite vertices (first %lu, %lu indices), its triangles dropped; vertex %u, "
                            "matrix row %u\n",
                            static_cast< unsigned long >(d.first), static_cast< unsigned long >(d.indexCount), d.indices[k + j], v.matrix);
                    logFloatBits("native clip:   vertex position", &v.x, 3);
                    logFloatBits("native clip:   its matrix rows", t.palette[row], 12);
                    logFloatBits("native clip:   view-to-clip rows", t.clip[0], 16);
                }
                sInvalidTriangles++;
                continue;
            }
            if (codes[0] & codes[1] & codes[2]) {
                continue;
            }
            for (u32 j = 0; j < 3; j++) {
                polygon[j].v = vertices[d.indices[k + j]];
                setClip(t, polygon[j]);
            }
            u32 row = polygon[0].v.matrix < 28 ? polygon[0].v.matrix : 0;
            for (u32 j = 1; j < 3; j++) {
                if (polygon[j].v.matrix == polygon[0].v.matrix) {
                    continue;
                }
                u32 slot = row / 3;
                if (!(inverseMask & (1u << slot))) {
                    if (!PSMTXInverse(*reinterpret_cast< const Mtx* >(t.palette[row]), inverses[slot])) {
                        PSMTXIdentity(inverses[slot]);
                    }
                    inverseMask |= 1u << slot;
                }
                moveToRow(t, row, inverses[slot], polygon[j].v);
            }
            int n = hostClipPolygon(reinterpret_cast< unsigned char* >(polygon), 3, sizeof(ClipVertex), lerpClipVertex,
                                    reinterpret_cast< unsigned char* >(scratch));
            if (n < 3) {
                continue;
            }
            sClipTriangles++;
            if (used + 3 * static_cast< u32 >(n - 2) > kOutVertices) {
                flushOut(used, pState, t, version);
            }
            for (int j = 1; j + 1 < n; j++) {
                sOut[used++] = polygon[0].v;
                sOut[used++] = polygon[j].v;
                sOut[used++] = polygon[j + 1].v;
            }
        }
    };
    if (d.chunks != nullptr && d.chunkCount != 0) {
        // Large groups come in runs of triangles with their own spheres: runs inside go to the GPU
        // as they are, runs outside are dropped, and only the runs crossing a plane are tested
        // triangle by triangle.
        const f32(*planes)[HOST_CLIP_PLANES][4] = viewPlanes(t);
        const u32 runIndices = RVL_NATIVE_CHUNK_TRIANGLES * 3;
        for (u32 c = 0; c < d.chunkCount; c++) {
            const RvlNativeChunk& chunk = d.chunks[c];
            const u32 k0 = c * runIndices;
            const u32 k1 = k0 + runIndices < d.indexCount ? k0 + runIndices : d.indexCount;
            const int where = chunk.row < 28 ? classifySphere(t, planes, chunk.row, chunk.sphere, nullptr) : SPHERE_CROSSING;
            if (where == SPHERE_INSIDE) {
                memcpy(inside + insideCount, d.indices + k0, (k1 - k0) * sizeof(u16));
                insideCount += k1 - k0;
            } else if (where != SPHERE_OUTSIDE) {
                clipTriangles(k0, k1);
            }
        }
    } else {
        clipTriangles(0, d.indexCount);
    }
    bool submitted = true;
    if (insideCount != 0) {
        submitted = hostGfxDrawNativeIndexed(d.first, inside, insideCount, pState, &t, version) != 0;
    }
    flushOut(used, pState, t, version);
    sClipTicks += hostGetTick() - start;
    return submitted;
}

// ##############################################################################################
// # Counters for the log                                                                       #
// #                                                                                            #
// # Taken and reset by native_submit.cpp's periodic log.                                       #
// ##############################################################################################
void rvlNativeClipTakeStats(u32& draws, u32& triangles, u32& invalid, u32& invalidMatrixDraws, u32& culled, u64& ticks) {
    invalidMatrixDraws = sInvalidMatrixDraws;
    sInvalidReports = sMatrixReports = 0;  // up to three reports of each kind per log
    sInvalidMatrixDraws = 0;
    ticks = sClipTicks;
    sClipTicks = 0;
    culled = sCulledDraws;
    sCulledDraws = 0;
    hostLog("native clip: %lu draws too small to draw at the lowered detail\n", static_cast< unsigned long >(sSmallDraws));
    sSmallDraws = 0;
    if (sSubDraws != 0) {
        hostLog("native subdivide: %lu draws with projected coordinates beyond unit 0, %lu triangles from %lu new vertices\n",
                static_cast< unsigned long >(sSubDraws), static_cast< unsigned long >(sSubTriangles), static_cast< unsigned long >(sSubMade));
        const u64 ms = hostGetTickFrequency() / 1000;
        hostLog("native subdivide pass: setup %lu ms, triangles %lu ms, draws %lu ms; %lu triangles looked at, %lu copied by runs, %lu whole passes\n",
                static_cast< unsigned long >(sSubSetupTicks / ms), static_cast< unsigned long >(sSubLoopTicks / ms),
                static_cast< unsigned long >(sSubSubmitTicks / ms), static_cast< unsigned long >(sSubLooked),
                static_cast< unsigned long >(sSubCopied), static_cast< unsigned long >(sSubWholePasses));
    }
    sSubDraws = sSubTriangles = sSubMade = 0;
    sSubSetupTicks = sSubLoopTicks = sSubSubmitTicks = 0;
    sSubLooked = sSubCopied = sSubWholePasses = 0;
    invalid = sInvalidTriangles;
    sInvalidTriangles = 0;
    draws = sClipDraws;
    triangles = sClipTriangles;
    sClipDraws = 0;
    sClipTriangles = 0;
}
