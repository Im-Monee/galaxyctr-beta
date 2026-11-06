// Native J3D drawing (block N).
//
// The PC converter (tools/3ds/native) writes, next to every J3D model of the game, an .n3d file:
// its shapes' vertices already decoded, triangle lists, matrix tables and its materials compiled
// into 3DS texture combiner stages (tools/3ds/native/n3d.py and mat.py describe the format).
// ResourceHolder registers each model's .n3d when it creates the model, and
// DrawBufferShapeDrawer draws the model's packets from it: no display list and no GX command
// of the model is read. Colours, texture numbers, texture matrices and position
// matrices come from the J3D objects at draw time. Deferred per-joint texture matrices are
// calculated on the CPU and uploaded as transient Citro3D vertices. CPU skinning remains
// dependent on source vertex mappings being present in the file; unsupported packets are
// counted and reported instead of being silently discarded.

#include "gx_internal.h"
#include "gx_state.h"
#include "host.h"
#include "JUTTexture3DS.hpp"
#include <Game/Map/LightPointCtrl.hpp>
#include <Game/Util/CameraUtil.hpp>
#include <Game/Util/ModelUtil.hpp>
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3DGraphBase/J3DShapeMtx.hpp>
#include <JSystem/J3DGraphBase/J3DTransform.hpp>
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/J3DGraphBase/J3DVertex.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <revolution/mtx.h>
#include "NativeFur3DS.hpp"
#include "NativeMaterial3DS.hpp"
#include "GXDelta3DS.hpp"
#include "GXToCitro3D.hpp"
#include "TevCompile3DS.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

extern "C" int rvlDVDEntryRelPath(s32 entry, char* pDst, u32 size);  // dvd.cpp

namespace {

    struct N3DShape {
        u8 mode, pad;
        u16 groupCount;
        u32 mask;
        u16 stride, pad2;
        f32 radius, box[6];
        u32 groupTable;
        f32 posScale, uvScale;
    };

    struct N3DGroup {
        u16 matrixCount, pad;
        u16 matrices[10];
        u32 vertexCount, vertexOffset, indexCount, indexOffset;
        u32 pad2;  // groups are 0x2C bytes apart in the file
    };

    const u32 kVersion = 16;  // n3d.py VERSION
    const u32 kMaterialSize = 0x14C;
    const u32 kMaxPasses = 6;  // passes of a material after the first (mat.py MAX_PASSES)
    const u32 kStageSize = 28;
    enum {
        ATTR_NRM = 1,
        ATTR_CLR0 = 2,
        ATTR_CLR1 = 4,
        ATTR_TEX0 = 8,
        ATTR_POS_F32 = 1 << 11,
        ATTR_TEXMTX = 1 << 12,
        ATTR_SOURCE_POS = 1 << 13,
        ATTR_NRM_X2 = 1 << 14,  // normals at half scale: the shape has normals longer than 1 (n3d.py)
    };
    enum { REF_FIXED, REF_TEVREG, REF_KONST };
    enum { PART_RGB, PART_ALPHA, PART_R, PART_G, PART_B };

    // Reasons a packet cannot be submitted natively (GXProfile::nativeFallbacks).
    enum {
        FB_NO_FILE,      // no .n3d for the model (or it does not match)
        FB_USER_DATA,    // deferred matrices whose shape data cannot be represented by N3D v4
        FB_MODEL_FLAGS,  // CPU skinning or LOD
        FB_TEXTURE,      // palette texture, or no texture
        FB_SHAPE,        // billboard or matrixless shape
        FB_UPLOAD,       // no room in the vertex arena
        FB_LIGHTS,       // actor-light configuration cannot be represented
        FB_MATERIAL,     // material exceeds the native stage/unit representation
        FB_SUBMIT,       // host rejected a native draw submission
    };

    struct Upload {
        u32 epoch;
        u32 key;  // what the vertices depend on besides the file: sources of the units, channel
        u32 first;
        u16* indices;
        u32 bounds;  // its GroupBounds (boundsOf), kNoBounds when there was no room
        // What the vertices were decoded with (decodeVertex), for draws that accept another key's
        // vertices (uploadCompatible): colour set, and each unit's source and coordinate set.
        u8 ras;
        u8 row[3];
        u8 coord[3];
        u8 exact;  // a fixed key (fur, simple draws): only that key matches
    };
    // Uploads kept per group and pass slot: one per key. A model drawn in one frame both plainly
    // and with a TEV delta (another colour channel or texture sources) has two sets of vertices;
    // with one entry each draw replaced the other's and uploaded again, every frame, until the
    // arena filled (about 500000 vertices per 100 frames in Rosalina's talk, CIA 0.14.13). Mario's
    // mode draws (J3DModelX) have more keys than entries; most use neither the vertex colours nor
    // texture coordinates, and take the vertices of any upload that matches in what they use
    // (CIA 0.14.15).
    const u32 kUploadWays = 2;
    // A fur layer's vertices (uploadGroup): by the model and the layer's positions, which clones share.
    struct FurUpload {
        const void* model;
        const void* positions;
        u32 shape, group;
        Upload up;
    };
    FurUpload sFurUploads[256];

    // Model-space bounding spheres of an uploaded group's vertices, one per matrix slot, for the
    // clipping test of native_clip.cpp. Persistent uploads keep theirs until the arena is emptied;
    // transient ones (skinned, deferred texture matrices) for the frame.
    struct GroupBounds {
        u32 slotMask;
        f32 spheres[10][4];
        u32 chunkFirst, chunkCount;  // its runs in sChunks (computeChunks), for large persistent groups
    };
    const u32 kNoBounds = 0xFFFFFFFFu;
    const u32 kTransientBounds = 0x80000000u;
    const u32 kMaxPersistentBounds = 8192;
    const u32 kTransientBoundCount = 512;
    GroupBounds* sPersistentBounds;
    u32 sPersistentBoundCount, sPersistentBoundCapacity, sPersistentBoundEpoch;
    GroupBounds sTransientBounds[kTransientBoundCount];
    u32 sTransientBoundCount;

    const GroupBounds* boundsOf(u32 index) {
        if (index == kNoBounds) {
            return nullptr;
        }
        if (index & kTransientBounds) {
            index &= ~kTransientBounds;
            return index < sTransientBoundCount ? &sTransientBounds[index] : nullptr;
        }
        return index < sPersistentBoundCount ? &sPersistentBounds[index] : nullptr;
    }

    // A bounds entry for an upload made now; kNoBounds when there is no room (the draw is then
    // always tested triangle by triangle).
    u32 newBounds(bool persistent, u32 epoch, GroupBounds*& pOut) {
        if (!persistent) {
            if (sTransientBoundCount == kTransientBoundCount) {
                return kNoBounds;
            }
            pOut = &sTransientBounds[sTransientBoundCount];
            return kTransientBounds | sTransientBoundCount++;
        }
        if (sPersistentBoundEpoch != epoch) {
            sPersistentBoundEpoch = epoch;
            sPersistentBoundCount = 0;
        }
        if (sPersistentBoundCount == sPersistentBoundCapacity) {
            u32 capacity = sPersistentBoundCapacity == 0 ? 256 : sPersistentBoundCapacity * 2;
            if (capacity > kMaxPersistentBounds) {
                return kNoBounds;
            }
            GroupBounds* grown = static_cast< GroupBounds* >(realloc(sPersistentBounds, capacity * sizeof(GroupBounds)));
            if (grown == nullptr) {
                return kNoBounds;
            }
            sPersistentBounds = grown;
            sPersistentBoundCapacity = capacity;
        }
        pOut = &sPersistentBounds[sPersistentBoundCount];
        return sPersistentBoundCount++;
    }

    // Runs of triangles of large persistent groups with their own spheres (RvlNativeChunk): the
    // ground of a scene is one group that crosses the near plane or the guard band in every frame,
    // and testing its runs instead of all its vertices keeps the CPU clipper to the few triangles
    // near the edges. Emptied with the persistent arena, as the bounds.
    const u32 kChunkMinTriangles = 256;
    const u32 kMaxChunks = 16384;
    RvlNativeChunk* sChunks;
    u32 sChunkCount, sChunkCapacity, sChunkEpoch;

    void computeChunks(const HostGfxModelVertex* vertices, u32 base, const u16* indices, u32 indexCount, u32 epoch, GroupBounds& b) {
        const u32 triangles = indexCount / 3;
        if (triangles < kChunkMinTriangles) {
            return;
        }
        if (sChunkEpoch != epoch) {
            sChunkEpoch = epoch;
            sChunkCount = 0;
        }
        const u32 count = (triangles + RVL_NATIVE_CHUNK_TRIANGLES - 1) / RVL_NATIVE_CHUNK_TRIANGLES;
        if (sChunkCount + count > sChunkCapacity) {
            u32 capacity = sChunkCapacity == 0 ? 1024 : sChunkCapacity;
            while (capacity < sChunkCount + count) {
                capacity *= 2;
            }
            if (capacity > kMaxChunks) {
                return;
            }
            RvlNativeChunk* grown = static_cast< RvlNativeChunk* >(realloc(sChunks, capacity * sizeof(RvlNativeChunk)));
            if (grown == nullptr) {
                return;
            }
            sChunks = grown;
            sChunkCapacity = capacity;
        }
        for (u32 c = 0; c < count; c++) {
            RvlNativeChunk& chunk = sChunks[sChunkCount + c];
            const u32 k0 = c * RVL_NATIVE_CHUNK_TRIANGLES * 3;
            const u32 k1 = k0 + RVL_NATIVE_CHUNK_TRIANGLES * 3 < triangles * 3 ? k0 + RVL_NATIVE_CHUNK_TRIANGLES * 3 : triangles * 3;
            f32 lo[3], hi[3];
            u32 row = vertices[indices[k0] - base].matrix;
            bool known = row < 28;
            for (u32 k = k0; k < k1 && known; k++) {
                const HostGfxModelVertex& v = vertices[indices[k] - base];
                const f32 p[3] = {v.x, v.y, v.z};
                known = v.matrix == row && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
                for (u32 i = 0; i < 3 && known; i++) {
                    lo[i] = k == k0 || p[i] < lo[i] ? p[i] : lo[i];
                    hi[i] = k == k0 || p[i] > hi[i] ? p[i] : hi[i];
                }
            }
            chunk.row = known ? row : 0xFF;
            f32 r2 = 0.0f;
            for (u32 i = 0; i < 3 && known; i++) {
                chunk.sphere[i] = (lo[i] + hi[i]) * 0.5f;
                const f32 half = (hi[i] - lo[i]) * 0.5f;
                r2 += half * half;
            }
            chunk.sphere[3] = known ? std::sqrt(r2) : 0.0f;
        }
        b.chunkFirst = sChunkCount;
        b.chunkCount = count;
        sChunkCount += count;
    }

    void computeBounds(const HostGfxModelVertex* vertices, u32 count, GroupBounds& b) {
        f32 lo[10][3], hi[10][3];
        b.slotMask = 0;
        b.chunkFirst = b.chunkCount = 0;
        for (u32 i = 0; i < count; i++) {
            const HostGfxModelVertex& v = vertices[i];
            u32 slot = v.matrix / 3 < 10 ? v.matrix / 3 : 0;
            const f32 p[3] = {v.x, v.y, v.z};
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
                // NaN compares false and would be left out of the sphere: with such a vertex the
                // bounds are unknown, and native_clip.cpp tests (and drops) its triangles.
                b.slotMask = 0;
                return;
            }
            if (!(b.slotMask & (1u << slot))) {
                b.slotMask |= 1u << slot;
                for (u32 k = 0; k < 3; k++) {
                    lo[slot][k] = hi[slot][k] = p[k];
                }
                continue;
            }
            for (u32 k = 0; k < 3; k++) {
                lo[slot][k] = p[k] < lo[slot][k] ? p[k] : lo[slot][k];
                hi[slot][k] = p[k] > hi[slot][k] ? p[k] : hi[slot][k];
            }
        }
        for (u32 slot = 0; slot < 10; slot++) {
            if (!(b.slotMask & (1u << slot))) {
                continue;
            }
            f32 r2 = 0.0f;
            for (u32 k = 0; k < 3; k++) {
                b.spheres[slot][k] = (lo[slot][k] + hi[slot][k]) * 0.5f;
                f32 half = (hi[slot][k] - lo[slot][k]) * 0.5f;
                r2 += half * half;
            }
            b.spheres[slot][3] = std::sqrt(r2);
        }
    }

    bool sSkinning;
    // Positions (and with sSkinNormals normals) read from the model's vertex buffer, as CPU
    // skinning makes them, without skinning's matrices: fur layers (CShader).
    bool sCpuPositions;
    bool sSkinNormals;
    const J3DVertexBuffer* sSkinVertexBuffer;

    // A registered model. Its tables live in one block of the heap the model was created in
    // (freed with it); the block starts with the model's address, checked on every lookup so
    // that a freed block is never used. Vertices and indices stay in the file until uploaded.
    struct NativeModel {
        J3DModelData* model;
        u8* block;
        const N3DShape* shapes;
        u32 shapeCount;
        const N3DGroup* groups;  // all groups, shape by shape
        u32* firstGroup;         // per shape: index of its first group
        Upload* uploads;         // per group two: the first pass's vertices, a later pass's (own units)
        const u8* materials;
        u32 materialCount;
        const char* path;
        // The whole .n3d in memory, for models whose vertices are decoded again every frame
        // (skinned, CPU positions, deferred texture matrices): null until first needed.
        u8* raw;
        u32 rawSize;
        // The far model (.far next to the .n3d, tools/3ds/native/far.py): the whole model as one
        // draw, for when an instance is small on screen. None when farVertexCount is 0.
        u16 farDrw, farMaterial;
        u32 farVertexCount, farIndexCount;
        f32 farPosScale;
        f32 farSphere[1][4];
        bool farCullNone;
        u8 farPrepared;  // 0: state not built yet, 1: built, 2: unusable
        u32 farColorCtrl, farAmbColor;
        Upload farUpload;
        HostGfxState farState;
    };

    // Memory the raw .n3d copies may take (malloc). Past it, transient uploads read the card.
    const u32 kRawBudget = 1536 * 1024;
    u32 sRawBytes;

    void releaseRaw(NativeModel& m) {
        if (m.raw != nullptr) {
            free(m.raw);
            sRawBytes -= m.rawSize;
            m.raw = nullptr;
            m.rawSize = 0;
        }
    }

    // The model's .n3d in memory, read on first use; null when it does not fit the budget.
    const u8* rawFile(NativeModel& m) {
        if (m.raw != nullptr) {
            return m.raw;
        }
        void* file = hostFileOpen(m.path);
        if (file == nullptr) {
            return nullptr;
        }
        long size = hostFileSize(file);
        if (size > 0 && sRawBytes + static_cast< u32 >(size) <= kRawBudget) {
            m.raw = static_cast< u8* >(malloc(size));
            if (m.raw != nullptr && hostFileRead(file, m.raw, size, 0) == size) {
                m.rawSize = static_cast< u32 >(size);
                sRawBytes += m.rawSize;
            } else {
                free(m.raw);
                m.raw = nullptr;
            }
        }
        hostFileClose(file);
        return m.raw;
    }

    // Whole .n3d files kept for a while after an upload of their groups (uploadGroup). The groups
    // of a model upload one after the other, in each pass that draws them; reading each group's
    // vertices and indices from the pack took about 15 ms a group whatever its size (a seek far
    // into a 450 MB file on the card), and a scene's first frames upload hundreds of groups
    // (CIA 0.14.7: 247 groups in 4 s entering the Comet Observatory). A whole file is one read.
    // Files over kUploadFileMax read their groups from the card as before. The cache empties
    // after kUploadIdleFrames frames without uploads, giving the memory back.
    const u32 kUploadCacheBudget = 1024 * 1024;
    const u32 kUploadFileMax = 512 * 1024;
    const u32 kUploadIdleFrames = 120;
    struct UploadFile {
        char path[256];
        u8* data;
        u32 size;
        u32 lastUse;
    };
    UploadFile sUploadFiles[16];
    u32 sUploadCacheBytes, sUploadCacheClock, sUploadIdle;
    u32 sUploadFileReads, sUploadFileHits;

    void releaseUploadFiles() {
        for (UploadFile& f : sUploadFiles) {
            free(f.data);
            f.data = nullptr;
            f.size = 0;
        }
        sUploadCacheBytes = 0;
    }

    // The model's file from the cache, read whole on a miss; null when it is too large or there is
    // no memory for it.
    const u8* uploadFile(const NativeModel& m, u32& size) {
        sUploadIdle = 0;
        sUploadCacheClock++;
        for (UploadFile& f : sUploadFiles) {
            if (f.data != nullptr && strcmp(f.path, m.path) == 0) {
                f.lastUse = sUploadCacheClock;
                size = f.size;
                sUploadFileHits++;
                return f.data;
            }
        }
        if (strlen(m.path) >= sizeof(sUploadFiles[0].path)) {
            return nullptr;
        }
        void* file = hostFileOpen(m.path);
        if (file == nullptr) {
            return nullptr;
        }
        const long fileSize = hostFileSize(file);
        u8* data = nullptr;
        UploadFile* slot = nullptr;
        if (fileSize > 0 && static_cast< u32 >(fileSize) <= kUploadFileMax) {
            // The least recently used files leave until the new one fits the budget and a slot.
            for (;;) {
                UploadFile* lru = nullptr;
                slot = nullptr;
                for (UploadFile& f : sUploadFiles) {
                    if (f.data == nullptr) {
                        slot = &f;
                    } else if (lru == nullptr || f.lastUse < lru->lastUse) {
                        lru = &f;
                    }
                }
                if (slot != nullptr && sUploadCacheBytes + fileSize <= kUploadCacheBudget) {
                    break;
                }
                free(lru->data);
                sUploadCacheBytes -= lru->size;
                lru->data = nullptr;
                lru->size = 0;
            }
            data = static_cast< u8* >(malloc(fileSize));
            if (data != nullptr && hostFileRead(file, data, fileSize, 0) != fileSize) {
                free(data);
                data = nullptr;
            }
        }
        hostFileClose(file);
        if (data == nullptr) {
            return nullptr;
        }
        strcpy(slot->path, m.path);
        slot->data = data;
        slot->size = static_cast< u32 >(fileSize);
        slot->lastUse = sUploadCacheClock;
        sUploadCacheBytes += slot->size;
        sUploadFileReads++;
        size = slot->size;
        return data;
    }

    NativeModel* sModels;
    u32 sModelCount, sModelCapacity;
    u32 sLoaded, sMissing;

    inline u16 rd16(const u8* p) {
        u16 v;
        memcpy(&v, p, 2);
        return v;
    }

    inline u32 rd32(const u8* p) {
        u32 v;
        memcpy(&v, p, 4);
        return v;
    }

    bool isLive(const NativeModel& n) {
        return n.block != nullptr && rd32(n.block) == reinterpret_cast< u32 >(n.model);
    }

    // Model numbers (plus one) by model data, checked against the model: findModel scanned every
    // native model (163 in the Comet Observatory) for each material drawn and each material
    // snapshot (rvlNativeSnapshot, every object's calcAnim). A stale slot falls back to the scan.
    u16 sModelIndex[256];

    NativeModel* findModel(const J3DModelData* model) {
        const u32 h = reinterpret_cast< u32 >(model) * 0x9E3779B1u >> 24;
        const u32 known = sModelIndex[h];
        if (known != 0 && known - 1 < sModelCount && sModels[known - 1].model == model && isLive(sModels[known - 1])) {
            return &sModels[known - 1];
        }
        for (u32 i = 0; i < sModelCount; i++) {
            if (sModels[i].model == model && isLive(sModels[i])) {
                sModelIndex[h] = static_cast< u16 >(i + 1);
                return &sModels[i];
            }
        }
        return nullptr;
    }

    // The model whose material table holds pMat. A model may draw a material another one lends:
    // MarioActor::copyMaterial gives the face model (MarioFace) Mario's "Face" material, and the
    // face model's own materials name a placeholder texture ("Dummy").
    NativeModel* findMaterialOwner(const J3DMaterial* pMat) {
        for (u32 i = 0; i < sModelCount; i++) {
            const J3DModelData* data = sModels[i].model;
            if (isLive(sModels[i]) && pMat->getIndex() < data->getMaterialNum() &&
                data->getMaterialNodePointer(pMat->getIndex()) == pMat) {
                return &sModels[i];
            }
        }
        return nullptr;
    }

    bool readAt(void* file, void* dst, u32 size, u32 offset) {
        return hostFileRead(file, dst, size, offset) == static_cast< long >(size);
    }

    u32 alignUp(u32 v) {
        return (v + 3) & ~3u;
    }

    u16 nativeStride(u32 mask) {
        u32 size = (mask & ATTR_POS_F32) ? 16 : 8;
        size += 4;
        if (mask & ATTR_NRM) size += 4;
        if (mask & ATTR_CLR0) size += 4;
        if (mask & ATTR_CLR1) size += 4;
        for (u32 t = 0; t < 8; t++) {
            if (mask & (ATTR_TEX0 << t)) size += 4;
        }
        if (mask & ATTR_TEXMTX) size += 8;
        return size <= 0xFFFF ? static_cast<u16>(size) : 0;
    }

    bool supportedComponentType(GXCompType type) {
        return type == GX_F32 || type == GX_U8 || type == GX_S8 || type == GX_U16 || type == GX_S16;
    }

    // Far models (far.py): the header, a vertex.
    struct FarHeader {
        char magic[4];
        u32 version;
        u16 drw, material;
        u32 vertexCount, indexCount;
        f32 posScale;
        f32 sphere[4];
        u8 cullNone, pad[3];
        u32 sourceTriangles;
    };
    struct FarVertex {
        s16 pos[3];
        s8 nrm[3];
        u8 pad;
        u8 rgba[4];
        u8 pad2[2];
    };
    const u32 kFarVersion = 1;
    static_assert(sizeof(FarHeader) == 0x30 && sizeof(FarVertex) == 16, "far.py HEADER and VERTEX");

    // The model's .far path: its .n3d's with the other extension.
    bool farPath(const NativeModel& n, char (&out)[384]) {
        const size_t length = strlen(n.path);
        if (length < 4 || length >= sizeof(out) || strcmp(n.path + length - 4, ".n3d") != 0) {
            return false;
        }
        memcpy(out, n.path, length - 4);
        strcpy(out + length - 4, ".far");
        return true;
    }

    // Reads the far model's header, checked against the model; no far model when it does not match.
    void loadFarHeader(NativeModel& n, J3DModelData* pModel) {
        n.farVertexCount = n.farIndexCount = 0;
        n.farPrepared = 0;
        memset(&n.farUpload, 0, sizeof(n.farUpload));
        char path[384];
        if (!farPath(n, path)) {
            return;
        }
        void* file = hostFileOpen(path);
        if (file == nullptr) {
            return;
        }
        FarHeader h;
        const long size = hostFileSize(file);
        const bool ok = size >= static_cast< long >(sizeof(h)) && readAt(file, &h, sizeof(h), 0);
        hostFileClose(file);
        if (!ok || memcmp(h.magic, "N3DF", 4) != 0 || h.version != kFarVersion || h.vertexCount == 0 || h.vertexCount > 65536 ||
            h.indexCount == 0 || h.indexCount % 3 != 0 || h.indexCount > 0x100000 || h.drw >= pModel->getDrawMtxNum() ||
            pModel->getDrawMtxFlag(h.drw) != 0 || h.material >= pModel->getMaterialNum() ||
            static_cast< u32 >(size) != sizeof(h) + h.vertexCount * sizeof(FarVertex) + h.indexCount * 2) {
            return;
        }
        n.farDrw = h.drw;
        n.farMaterial = h.material;
        n.farVertexCount = h.vertexCount;
        n.farIndexCount = h.indexCount;
        n.farPosScale = h.posScale;
        memcpy(n.farSphere[0], h.sphere, sizeof(h.sphere));
        n.farCullNone = h.cullNone != 0;
    }

}  // namespace

// Registration ---------------------------------------------------------------------------------

namespace {
    const char* sArchivePath;  // set by ResourceHolderManager::createResourceHolder
}

void rvlNativeSetArchive(const char* pPath) {
    sArchivePath = pPath;
}

void rvlNativeRegister(J3DModelData* pModel, s32 entry, const char* pName) {
    if (pModel == nullptr || pName == nullptr) {
        return;
    }
    char rel[256];
    if (sArchivePath != nullptr) {
        snprintf(rel, sizeof(rel), "%s%s", sArchivePath[0] == '/' ? "" : "/", sArchivePath);
    } else if (entry < 0 || rvlDVDEntryRelPath(entry, rel, sizeof(rel)) < 0) {
        if (sMissing++ < 5) {
            hostLog("native: no archive path for %s\n", pName);
        }
        return;
    }
    const char* dot = strrchr(pName, '.');
    int baseLen = dot != nullptr ? static_cast< int >(dot - pName) : static_cast< int >(strlen(pName));
    char path[384];
    snprintf(path, sizeof(path), "sdmc:/smg/native%s/%.*s.n3d", rel, baseLen, pName);

    void* file = hostFileOpen(path);
    if (file == nullptr) {
        if (sMissing++ < 5) {
            hostLog("native: no %s\n", path);
        }
        return;
    }
    const long fileSizeValue = hostFileSize(file);
    if (fileSizeValue < 0 || static_cast< unsigned long >(fileSizeValue) > 0xFFFFFFFFul) {
        hostFileClose(file);
        return;
    }
    const u32 fileSize = static_cast< u32 >(fileSizeValue);
    const auto inFile = [fileSize](u32 offset, u32 size) {
        return offset <= fileSize && size <= fileSize - offset;
    };
    u8 header[0x1C];
    bool ok = readAt(file, header, sizeof(header), 0);
    u32 shapeCount = ok ? rd32(header + 8) : 0, materialCount = ok ? rd32(header + 16) : 0;
    // Records: the materials, then the records of their later passes.
    u32 recordCount = ok ? rd32(header + 24) : 0;
    if (!ok || memcmp(header, "N3D1", 4) != 0 || rd32(header + 4) != kVersion || shapeCount != pModel->getShapeNum() ||
        materialCount != pModel->getMaterialNum()) {
        hostLog("native: %s does not match its model (file: version %lu, %lu shapes, %lu materials; expected: version %lu, %u shapes, %u "
                "materials)\n",
                path, ok ? rd32(header + 4) : 0, shapeCount, materialCount, kVersion, pModel->getShapeNum(), pModel->getMaterialNum());
        hostFileClose(file);
        return;
    }
    const u32 shapeOffset = rd32(header + 12);
    const u32 materialOffset = rd32(header + 20);
    if (shapeOffset < sizeof(header) || materialOffset < shapeOffset || (shapeOffset & 3) != 0 || (materialOffset & 3) != 0 ||
        shapeCount > (0xFFFFFFFFu / sizeof(N3DShape)) || recordCount < materialCount || recordCount > (0xFFFFFFFFu / kMaterialSize) ||
        !inFile(shapeOffset, shapeCount * sizeof(N3DShape)) || !inFile(materialOffset, recordCount * kMaterialSize)) {
        hostLog("native: %s has invalid table offsets\n", path);
        hostFileClose(file);
        return;
    }

    // Shape table first, to size the block.
    N3DShape shapes[64];
    if (shapeCount > 64 || !readAt(file, shapes, shapeCount * sizeof(N3DShape), shapeOffset)) {
        hostLog("native: %s: %lu shapes, unsupported\n", path, shapeCount);
        hostFileClose(file);
        return;
    }
    u32 groupCount = 0;
    for (u32 s = 0; s < shapeCount; s++) {
        const u32 knownMask = ATTR_NRM | ATTR_CLR0 | ATTR_CLR1 | ATTR_POS_F32 | ATTR_TEXMTX | ATTR_SOURCE_POS | ATTR_NRM_X2 |
                              ((ATTR_TEX0 << 8) - ATTR_TEX0);
        if (shapes[s].mode > 3 || shapes[s].mask & ~knownMask || shapes[s].stride != nativeStride(shapes[s].mask) ||
            shapes[s].groupTable < shapeOffset ||
            shapes[s].groupCount > (0xFFFFFFFFu - groupCount) / sizeof(N3DGroup) ||
            !inFile(shapes[s].groupTable, shapes[s].groupCount * sizeof(N3DGroup))) {
            hostLog("native: %s has an invalid shape table\n", path);
            hostFileClose(file);
            return;
        }
        groupCount += shapes[s].groupCount;
    }
    if (groupCount > 0xFFFFFFFFu / (2 * sizeof(Upload)) ||
        shapeCount > (0xFFFFFFFFu - 4 - groupCount * sizeof(N3DGroup)) / 4 ||
        recordCount > (0xFFFFFFFFu - (4 + shapeCount * sizeof(N3DShape) + groupCount * sizeof(N3DGroup) +
                                      (shapeCount + 1) * 4 + groupCount * 2 * kUploadWays * sizeof(Upload))) /
                           kMaterialSize) {
        hostLog("native: %s is too large for native registration\n", path);
        hostFileClose(file);
        return;
    }
    u32 pathSize = alignUp(strlen(path) + 1);
    u32 size = 4 + shapeCount * sizeof(N3DShape) + groupCount * sizeof(N3DGroup) + (shapeCount + 1) * 4 + groupCount * 2 * kUploadWays * sizeof(Upload) +
               recordCount * kMaterialSize + pathSize;
    u8* block = static_cast< u8* >(JKRHeap::alloc(size, 4, nullptr));
    if (block == nullptr) {
        hostLog("native: no memory for %s (%lu bytes)\n", path, size);
        hostFileClose(file);
        return;
    }
    memcpy(block, &pModel, 4);
    u8* p = block + 4;
    N3DShape* outShapes = reinterpret_cast< N3DShape* >(p);
    memcpy(outShapes, shapes, shapeCount * sizeof(N3DShape));
    p += shapeCount * sizeof(N3DShape);
    N3DGroup* groups = reinterpret_cast< N3DGroup* >(p);
    p += groupCount * sizeof(N3DGroup);
    u32* firstGroup = reinterpret_cast< u32* >(p);
    p += (shapeCount + 1) * 4;
    Upload* uploads = reinterpret_cast< Upload* >(p);
    memset(uploads, 0, groupCount * 2 * kUploadWays * sizeof(Upload));
    p += groupCount * 2 * kUploadWays * sizeof(Upload);
    u8* materials = p;
    p += recordCount * kMaterialSize;
    char* storedPath = reinterpret_cast< char* >(p);
    strcpy(storedPath, path);

    u32 g = 0;
    for (u32 s = 0; s < shapeCount && ok; s++) {
        firstGroup[s] = g;
        ok = readAt(file, groups + g, shapes[s].groupCount * sizeof(N3DGroup), shapes[s].groupTable);
        g += shapes[s].groupCount;
    }
    firstGroup[shapeCount] = g;
    ok = ok && readAt(file, materials, recordCount * kMaterialSize, materialOffset);
    hostFileClose(file);
    J3DTexture* modelTextures = pModel->getTexture();
    for (u32 s = 0; ok && s < shapeCount; s++) {
        const N3DShape& shape = outShapes[s];
        for (u32 i = 0; i < shape.groupCount; i++) {
            const N3DGroup& group = groups[firstGroup[s] + i];
            if (group.matrixCount > 10 || group.vertexCount > 65536 || group.indexCount > 0x7FFFFFFFu ||
                (group.indexOffset & 1) != 0 || (group.indexCount != 0 && group.indexCount % 3 != 0) ||
                group.vertexCount > 0xFFFFFFFFu / shape.stride ||
                !inFile(group.vertexOffset, group.vertexCount * static_cast< u32 >(shape.stride)) ||
                !inFile(group.indexOffset, group.indexCount * sizeof(u16))) {
                ok = false;
                break;
            }
            for (u32 m = 0; m < group.matrixCount; m++) {
                if (m == 0 && group.matrices[m] == 0xFFFF) {
                    ok = false;
                    break;
                }
                if (group.matrices[m] != 0xFFFF && group.matrices[m] >= pModel->getDrawMtxNum()) {
                    ok = false;
                    break;
                }
            }
        }
    }
    for (u32 i = 0; ok && i < recordCount; i++) {
        const u8* material = materials + i * kMaterialSize;
        ok = material[0] <= HOST_GFX_STAGES && material[1] <= 3;
        if (ok && material[0x0F] != 0) {
            // A material's later passes: records after the materials (passes have none).
            const u32 first = rd16(material + 0xC6);
            ok = i < materialCount && material[0x0F] <= kMaxPasses && first >= materialCount && first + material[0x0F] <= recordCount;
        }
        for (u32 u = 0; ok && u < 3; u++) {
            const u8* unit = material + 0x10 + u * 4;
            if (u < material[1]) {
                ok = unit[2] < 8 && unit[3] < 8 && rd16(unit) != 0xFFFF &&
                     modelTextures != nullptr && rd16(unit) < modelTextures->getNum();
            } else {
                ok = rd16(unit) == 0xFFFF && unit[2] == 0xFF && unit[3] == 0xFF;
            }
        }
    }
    if (!ok) {
        hostLog("native: %s could not be read\n", path);
        memset(block, 0, 4);
        JKRHeap::free(block, nullptr);
        return;
    }

    // Models whose block was freed (their scene ended) give back their in-memory .n3d.
    for (u32 i = 0; i < sModelCount; i++) {
        if (sModels[i].model == pModel || !isLive(sModels[i])) {
            releaseRaw(sModels[i]);
        }
    }
    NativeModel* n = nullptr;
    for (u32 i = 0; i < sModelCount && n == nullptr; i++) {
        if (sModels[i].model == pModel || !isLive(sModels[i])) {
            n = &sModels[i];  // the same model again, or a slot whose block was freed
        }
    }
    if (n == nullptr) {
        if (sModelCount == sModelCapacity) {
            u32 capacity = sModelCapacity ? sModelCapacity * 2 : 256;
            NativeModel* grown = static_cast< NativeModel* >(realloc(sModels, capacity * sizeof(NativeModel)));
            if (grown == nullptr) {
                JKRHeap::free(block, nullptr);
                return;
            }
            sModels = grown;
            sModelCapacity = capacity;
        }
        n = &sModels[sModelCount++];
        n->raw = nullptr;
        n->rawSize = 0;
    }
    n->model = pModel;
    for (FurUpload& entry : sFurUploads) {
        if (entry.model == n) {
            entry.model = nullptr;  // of the model that had this memory
        }
    }
    n->block = block;
    n->shapes = outShapes;
    n->shapeCount = shapeCount;
    n->groups = groups;
    n->firstGroup = firstGroup;
    n->uploads = uploads;
    n->materials = materials;
    n->materialCount = materialCount;
    n->path = storedPath;
    loadFarHeader(*n, pModel);
    sLoaded++;
}

// Drawing --------------------------------------------------------------------------------------

namespace {

    // The values of a material that change from one model instance to another (colour, texture
    // pattern and texture matrix animations). J3D applies each instance's animations to the
    // shared J3DMaterial just before writing that instance's display list (DisplayListMaker::diff):
    // rvlNativeSnapshot keeps a copy then, and the packets of that instance draw with it.
    struct MatValues {
        s16 tev[3][4];      // TEV registers C0-C2 (RGBA, signed)
        s16 konst[4][4];    // konst colours
        u32 matColor[2], ambColor[2];
        u16 texNo[8];
        f32 texMtx[8][3][4];  // J3D's eight material texture matrices (TEXMTX0-7)
        u16 tevValid, konstValid, colorValid, texMtxValid;
    };

#ifdef __3DS__
    // J3D exposes eight texture-matrix slots.  The native material format already
    // carries all eight; keeping the snapshot in sync lets animated slots 4-7 use
    // the same Citro3D path instead of silently reverting to identity.
    static const u32 kNativeTexMtxSlots = 8;
#else
    static const u32 kNativeTexMtxSlots = 4;
#endif

    void fillValues(J3DMaterial* mat, MatValues& v) {
        v.tevValid = v.konstValid = v.colorValid = v.texMtxValid = 0;
        J3DTevBlock* tev = mat->getTevBlock();
        if (tev == nullptr) {
            return;
        }
        for (u32 i = 0; i < 3; i++) {
            const J3DGXColorS10* c = tev->getTevColor(i);
            if (c != nullptr) {
                v.tev[i][0] = c->r, v.tev[i][1] = c->g, v.tev[i][2] = c->b, v.tev[i][3] = c->a;
                v.tevValid |= 1 << i;
            }
        }
        for (u32 i = 0; i < 8; i++) {
            const J3DGXColor* c = tev->getTevKColor(i);
            if (c != nullptr) {
                v.konst[i][0] = c->r, v.konst[i][1] = c->g, v.konst[i][2] = c->b, v.konst[i][3] = c->a;
                v.konstValid |= 1 << i;
            }
        }
        J3DColorBlock* colors = mat->getColorBlock();
        if (colors == nullptr) {
            return;
        }
        for (u32 c = 0; c < 2; c++) {
            const J3DGXColor* mc = colors->getMatColor(c);
            const J3DGXColor* ac = colors->getAmbColor(c);
            if (mc != nullptr) {
                v.matColor[c] = (static_cast< u32 >(mc->r) << 24) | (mc->g << 16) | (mc->b << 8) | mc->a;
                v.colorValid |= 1 << c;
            }
            if (ac != nullptr) {
                v.ambColor[c] = (static_cast< u32 >(ac->r) << 24) | (ac->g << 16) | (ac->b << 8) | ac->a;
                v.colorValid |= 4 << c;
            }
        }
        for (u32 i = 0; i < 8; i++) {
            v.texNo[i] = mat->getTexNo(i);
        }
        for (u32 i = 0; i < kNativeTexMtxSlots; i++) {
            J3DTexMtx* tm = mat->getTexMtx(i);
            if (tm != nullptr) {
                memcpy(v.texMtx[i], tm->getMtx(), sizeof(v.texMtx[i]));
                v.texMtxValid |= 1 << i;
            }
        }
    }

    // Snapshots by (model instance, material), open addressing; used for two frames after
    // they were taken (an instance not animated any more falls back to the shared values).
    struct Snapshot {
        const J3DModel* model;
        u16 material;
        u32 frame;
        MatValues values;
    };
    // Instances x animated materials of a scene: the Observatory's Lumas, Toads and the rest passed 256,
    // and an instance whose snapshot found no room drew with the shared material's values, another
    // instance's colours (Lumas and the Star Festival's Toads changing colour, CIA 0.19.12).
    const u32 kSnapshots = 1024;  // power of two
    const u32 kSnapshotProbes = 16;
    Snapshot* sSnapshots;
    u32 sFrame;
    u32 sSnapshotMisses;

    u32 snapshotSlot(const J3DModel* model, u16 material) {
        u32 h = reinterpret_cast< u32 >(model) * 2654435761u ^ material * 40503u;
        return (h >> 8) & (kSnapshots - 1);
    }

    const MatValues* findSnapshot(const J3DModel* model, u16 material) {
        if (sSnapshots == nullptr) {
            return nullptr;
        }
        u32 slot = snapshotSlot(model, material);
        for (u32 k = 0; k < kSnapshotProbes; k++) {
            const Snapshot& e = sSnapshots[(slot + k) & (kSnapshots - 1)];
            if (e.model == model && e.material == material) {
                return sFrame - e.frame <= 1 ? &e.values : nullptr;
            }
            if (e.model == nullptr) {
                return nullptr;
            }
        }
        return nullptr;
    }

}  // namespace

namespace {
    // MR::syncMaterialAnimation: a lower detail model draws with the display lists of the model it
    // follows, here with its snapshots. Their own values are the file's (RosettaMiddle's A1 is 0).
    struct SnapshotSource {
        const J3DModel* model;
        const J3DModel* source;
    };
    SnapshotSource sSnapshotSources[256];  // power of two

    const J3DModel* snapshotOwner(const J3DModel* model) {
        const u32 h = reinterpret_cast< u32 >(model) * 2654435761u >> 24;
        for (u32 k = 0; k < 8; k++) {
            const SnapshotSource& e = sSnapshotSources[(h + k) & 255];
            if (e.model == model) {
                return e.source;
            }
        }
        return model;
    }
}  // namespace

namespace {
    // The last colours of each material of a model that lower detail models follow, kept while
    // the snapshots expire: such a model takes a snapshot only while an animation changes it, and
    // is not drawn (nor snapshot) while its lower detail model is. On the Wii that model keeps
    // the instance's own display list, with the colours it was last given: each Luma's colour, a
    // colour animation stopped at its own frame (MR::startBrkAndSetFrame), came back with the
    // shared material's after two frames and changed as Mario moved (CIA 0.19.10).
    struct OwnerColors {
        const J3DModel* model;
        u16 material;
        u16 tevValid, konstValid, colorValid;
        s16 tev[3][4];
        s16 konst[4][4];
        u32 matColor[2], ambColor[2];
    };
    const u32 kOwnerColors = 1024;  // power of two
    OwnerColors* sOwnerColors;
    const J3DModel* sOwnerModels[256];  // the models lower detail models follow (power of two)

    u32 ownerSlot(const J3DModel* model, u16 material) {
        return ((reinterpret_cast< u32 >(model) * 2654435761u ^ material * 40503u) >> 6) & (kOwnerColors - 1);
    }

    bool isOwnerModel(const J3DModel* model) {
        const u32 h = reinterpret_cast< u32 >(model) * 2654435761u >> 24;
        for (u32 k = 0; k < 8; k++) {
            const J3DModel* e = sOwnerModels[(h + k) & 255];
            if (e == model) {
                return true;
            }
            if (e == nullptr) {
                return false;
            }
        }
        return false;
    }

    void keepOwnerColors(const J3DModel* model, u16 material, const MatValues& v) {
        if (sOwnerColors == nullptr) {
            sOwnerColors = static_cast< OwnerColors* >(calloc(kOwnerColors, sizeof(OwnerColors)));
            if (sOwnerColors == nullptr) {
                return;
            }
        }
        const u32 slot = ownerSlot(model, material);
        OwnerColors* target = &sOwnerColors[(slot + 7) & (kOwnerColors - 1)];
        for (u32 k = 0; k < 8; k++) {
            OwnerColors& e = sOwnerColors[(slot + k) & (kOwnerColors - 1)];
            if ((e.model == model && e.material == material) || e.model == nullptr) {
                target = &e;
                break;
            }
        }
        target->model = model;
        target->material = material;
        target->tevValid = v.tevValid;
        target->konstValid = v.konstValid;
        target->colorValid = v.colorValid;
        memcpy(target->tev, v.tev, sizeof(target->tev));
        memcpy(target->konst, v.konst, sizeof(target->konst));
        memcpy(target->matColor, v.matColor, sizeof(target->matColor));
        memcpy(target->ambColor, v.ambColor, sizeof(target->ambColor));
    }

    const OwnerColors* findOwnerColors(const J3DModel* model, u16 material) {
        if (sOwnerColors == nullptr) {
            return nullptr;
        }
        const u32 slot = ownerSlot(model, material);
        for (u32 k = 0; k < 8; k++) {
            const OwnerColors& e = sOwnerColors[(slot + k) & (kOwnerColors - 1)];
            if (e.model == model && e.material == material) {
                return &e;
            }
            if (e.model == nullptr) {
                return nullptr;
            }
        }
        return nullptr;
    }
}  // namespace

void rvlNativeSyncMaterials(const J3DModel* pModel, const J3DModel* pSource) {
    const u32 h = reinterpret_cast< u32 >(pModel) * 2654435761u >> 24;
    for (u32 k = 0; k < 8; k++) {
        SnapshotSource& e = sSnapshotSources[(h + k) & 255];
        if (e.model == nullptr || e.model == pModel || k == 7) {
            e.model = pModel;
            e.source = pSource;
            break;
        }
    }
    const u32 s = reinterpret_cast< u32 >(pSource) * 2654435761u >> 24;
    for (u32 k = 0; k < 8; k++) {
        const J3DModel*& e = sOwnerModels[(s + k) & 255];
        if (e == nullptr || e == pSource || k == 7) {
            e = pSource;
            break;
        }
    }
}

void rvlNativeSnapshot(J3DModel* pModel, u16 material) {
    if (pModel == nullptr || pModel->getModelData() == nullptr) {
        return;
    }
    NativeModel* native = findModel(pModel->getModelData());
    if (native == nullptr || material >= native->materialCount) {
        return;
    }
    if (sSnapshots == nullptr) {
        sSnapshots = static_cast< Snapshot* >(calloc(kSnapshots, sizeof(Snapshot)));
        if (sSnapshots == nullptr) {
            return;
        }
    }
    u32 slot = snapshotSlot(pModel, material);
    Snapshot* target = nullptr;
    for (u32 k = 0; k < kSnapshotProbes && target == nullptr; k++) {
        Snapshot& e = sSnapshots[(slot + k) & (kSnapshots - 1)];
        if ((e.model == pModel && e.material == material) || e.model == nullptr || sFrame - e.frame > 1) {
            target = &e;
        }
    }
    if (target == nullptr) {
        sSnapshotMisses++;
        // The colours lower detail models take are kept even so (keepOwnerColors).
        J3DMaterial* materialNode = pModel->getModelData()->getMaterialNodePointer(material);
        if (materialNode != nullptr && isOwnerModel(pModel)) {
            static MatValues sMissed;
            fillValues(materialNode, sMissed);
            keepOwnerColors(pModel, material, sMissed);
        }
        return;
    }
    target->model = pModel;
    target->material = material;
    target->frame = sFrame;
    J3DMaterial* materialNode = pModel->getModelData()->getMaterialNodePointer(material);
    if (materialNode == nullptr) {
        target->model = nullptr;
        target->material = 0;
        target->frame = 0;
        return;
    }
    fillValues(materialNode, target->values);
    if (isOwnerModel(pModel)) {
        keepOwnerColors(pModel, material, target->values);
    }
}

// A shape of a model drawn with one view matrix for all its matrix slots: the Wii's
// GXLoadPosMtxImm before calling the shape's display list (Plant's leaves).
namespace {
    const f32 (*sOverrideMatrix)[4];
    const f32 (*sTexMtxOverride)[4];
}

void rvlNativeSetTexMtxOverride(const f32 (*pTexMtx)[4]) {
    sTexMtxOverride = pTexMtx;
}


bool rvlNativeBegin(J3DMaterial* pMat, J3DShapePacket* pFirst, bool userData);
bool rvlNativePacket(J3DShapePacket* pPacket, const ActorLightInfo* pActorLight, bool materialAmbient);
bool rvlNativePacket(J3DShapePacket* pPacket, const ActorLightInfo* pActorLight) {
    return rvlNativePacket(pPacket, pActorLight, false);
}
const ActorLightInfo* rvlNativeLoadedLight();

bool rvlNativeDrawShape(J3DModel* pModel, u32 shapeIndex, const f32 (*pViewMtx)[4]) {
    if (pModel == nullptr || shapeIndex >= pModel->getModelData()->getShapeNum()) {
        return false;
    }
    J3DShapePacket* packet = pModel->getShapePacket(static_cast< u16 >(shapeIndex));
    if (packet == nullptr || packet->mpShape == nullptr || packet->mpShape->getMaterial() == nullptr) {
        return false;
    }
    Mtx* base = packet->getBaseMtxPtr();
    packet->setBaseMtxPtr(const_cast< Mtx* >(reinterpret_cast< const Mtx* >(pViewMtx)));
    sOverrideMatrix = pViewMtx;
    bool submitted = rvlNativeBegin(packet->mpShape->getMaterial(), packet, false) &&
                     rvlNativePacket(packet, rvlNativeLoadedLight());  // the light loaded last, as GX
    sOverrideMatrix = nullptr;
    packet->setBaseMtxPtr(base);
    return submitted;
}

void rvlNativeEndFrame() {
    rvlNativeSetViewport(nullptr, nullptr, nullptr);  // MR::drawInitFor2DModel's lasts for the frame
    sFrame++;
    sTransientBoundCount = 0;
    if (sUploadCacheBytes != 0 && ++sUploadIdle >= kUploadIdleFrames) {
        releaseUploadFiles();
    }
}

namespace {

    // GXSetFog's register values (RVL_SDK GXPixel.c): the form HostGfxState carries fog in
    // (gfx.c applyFog decodes it). The range adjustment table is not represented.
    void encodeFog(const J3DFogInfo& f, HostGfxState& st) {
        const u32 type = f.mType;
        if ((type & 7) == 0) {
            return;  // GX_FOG_NONE
        }
        const u32 fsel = type & 7, proj = (type >> 3) & 1;
        f32 a, c;
        u32 bMantissa = 0, bShift = 0;
        if (proj) {
            if (f.mFarZ == f.mNearZ || f.mEndZ == f.mStartZ) {
                a = c = 0.0f;
            } else {
                const f32 range = 1.0f / (f.mEndZ - f.mStartZ);
                a = (f.mFarZ - f.mNearZ) * range;
                c = (f.mStartZ - f.mNearZ) * range;
            }
        } else {
            f32 A, B, C;
            if (f.mFarZ == f.mNearZ || f.mEndZ == f.mStartZ) {
                A = 0.0f;
                B = 0.5f;
                C = 0.0f;
            } else {
                A = (f.mFarZ * f.mNearZ) / ((f.mFarZ - f.mNearZ) * (f.mEndZ - f.mStartZ));
                B = f.mFarZ / (f.mFarZ - f.mNearZ);
                C = f.mStartZ / (f.mEndZ - f.mStartZ);
            }
            f32 mantissa = B;
            s32 exponent = 0;
            while (mantissa > 1.0f) {
                mantissa /= 2;
                exponent++;
            }
            while (mantissa > 0 && mantissa < 0.5f) {
                mantissa *= 2;
                exponent--;
            }
            a = A / static_cast< f32 >(1 << (exponent + 1));
            bMantissa = static_cast< u32 >(mantissa * 8388638);
            bShift = static_cast< u32 >(exponent + 1);
            c = C;
        }
        u32 aBits, cBits;
        memcpy(&aBits, &a, sizeof(aBits));
        memcpy(&cBits, &c, sizeof(cBits));
        st.fogParamA = ((aBits >> 12) & 0x7FF) | (((aBits >> 23) & 0xFF) << 11) | ((aBits >> 31) << 19);
        st.fogParamB = bMantissa & 0xFFFFFF;
        st.fogParamShift = bShift & 0x1F;
        st.fogParamC = ((cBits >> 12) & 0x7FF) | (((cBits >> 23) & 0xFF) << 11) | ((cBits >> 31) << 19) | (proj << 20) | (fsel << 21);
        st.fogColor = (static_cast< u32 >(f.mColor.r) << 16) | (static_cast< u32 >(f.mColor.g) << 8) | f.mColor.b;
    }

    // The material being drawn (rvlNativeBegin), shared by its packets.
    NativeModel* sModel;
    const u8* sMat;
    bool sCullAll;
    HostGfxState sState;
    // Its later passes (mat.py compile_material): their records and states.
    u32 sPassCount;
    const u8* sPassRecords[kMaxPasses];
    u8 sPassChannels[kMaxPasses];  // rasterized channel of each pass
    // A pass with units of its own (mat.py own_unit: a fourth texture in one of them): 1, or 2 when
    // its texture is missing (the pass is left out); 0 the first pass's units.
    u8 sPassOwnUnits[kMaxPasses];
    u8 sPassCaptureMask[kMaxPasses];        // captureMask and captureMap of a pass with units of its own
    f32 sPassCaptureMap[kMaxPasses][3][4];
    HostGfxState sPassStates[kMaxPasses];
    // GXSetZCompLoc(GX_TRUE) of the material (record byte 0x0D): with an alpha test and depth
    // writes, the Wii writes the depth of the pixels the test then discards. Such a draw goes as a
    // depth-only draw without the test, then the material on the depths it left (depthFirst).
    bool sZCompBefore;
    HostGfxState sDepthOnlyState, sDepthEqualState;
    // mat.FLAG_ALPHA_SPLIT: GX's two alpha comparisons pass both ends of the range ("A <= a or
    // A >= b"); the group is drawn again with the record's second test (0x14A, 0x14B). The two
    // tests pass different values, so each pixel is drawn once.
    bool sAlphaSplit;
    u8 sAlphaSplitFunc, sAlphaSplitRef;
    HostGfxState sAlphaSplitState;
    RvlNativeDraw sDraw;
    u32 sUploadKey;
    bool sUploadKeyExact;  // the key is a fixed one (fur, simple draws), not from setupUnits
    bool sHasUserData;
    u8 sUnitRow[3];  // generation source of each unit (0 position, 1 normal, 5 + n texture set n)
    u8 sUnitCoord[3];  // source texture-coordinate set used by each unit
    u8 sUnitMap[3];    // texture map (material texture slot) each unit samples
    // GX changes over the material (rvlNativeSetDelta): the draw's combiners replaced by the
    // delta's stage, and a constant final alpha (-1: none).
    const RvlGXDelta* sDelta;
    bool sDeltaStages;
    s32 sDeltaAlpha = -1;
    u8 sUnitStq[3], sUnitAbc1[3];
    bool sDeferredTexMtx, sDeferredTexInvalid;
    u32 sDeferredMatrixCount, sDeferredTexGenCount;
    Mtx sDeferredTexMatrices[10][8];
    // Optional profiling for the direct J3D path; reset and reported every GX profile window.
    u64 sNativeSetupTicks, sNativeSubmitTicks;
    u32 sNativeSubmitCalls;
    // Group uploads (uploadGroup) since the last log: persistent and transient ones, their
    // vertices, and the time spent in them (cache hits included).
    u32 sPersistentUploads, sTransientUploads, sUploadedVertices;
    // Persistent uploads for a second key beside a current one, and those that replaced a current one.
    u32 sSecondKeyUploads, sReplacedKeyUploads;
    // Draws that used another key's vertices (uploadCompatible).
    u32 sCompatibleUploads;
    u64 sUploadTicks;
    u64 sUploadFileTicks;  // reading whole .n3d files, part of the uploads
    u32 sSubmittedTriangles;
    const f32 kIdentity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

    u32 packColor(const GXColor& c) {
        return (static_cast< u32 >(c.r) << 24) | (c.g << 16) | (c.b << 8) | c.a;
    }

    s32 clampByte(s32 v) {
        return v < 0 ? 0 : v > 255 ? 255 : v;
    }

    s32 refComponent(s32 value, u8 negate) {
        switch (negate) {
        case 1:
            return clampByte(-value);
        case 2:
            return value & 0xFF;
        case 3:
            return clampByte(value + 128);
        case 4:
            return clampByte(value - 255);
        case 5:
            return clampByte((value - 255) >> 1);
        case 6:
            return clampByte(-value >> 1);
        default:
            return clampByte(value);
        }
    }

    // A combiner constant half from its reference (mat.py): the material's current colours.
    bool refValue(const MatValues& v, const u8* ref, s32* rgba) {
        u8 kind = ref[0], index = ref[1], part = ref[2], negate = ref[3];
        const s16* c;
        if (kind == REF_TEVREG && index >= 1 && index <= 3 && (v.tevValid & (1 << (index - 1)))) {
            c = v.tev[index - 1];
        } else if (kind == REF_KONST && index <= 3 && (v.konstValid & (1 << index))) {
            c = v.konst[index];
        } else {
            return false;
        }
        // negate 2: a TEV register read as input a, b or c, whose low 8 bits the TEV uses;
        // negate 3: the value plus 128, added with GPU_ADD_SIGNED; negate 4: its part over 255,
        // negate 5: half of it, added after the clamped value; negate 6: half the value negated
        // (mat.py).
        if (part == PART_RGB) {
            for (u32 i = 0; i < 3; i++) {
                rgba[i] = refComponent(c[i], negate);
            }
        } else {
            rgba[0] = rgba[1] = rgba[2] = refComponent(c[part == PART_ALPHA ? 3 : part - PART_R], negate);
        }
        return true;
    }

    // XF channel control word (GXSetChanCtrl) from a J3DColorChanInfo.
    u32 channelControl(const u8* info) {
        u32 enable = info[0] & 1, matSrc = info[1] & 1, mask = info[2], diffuse = info[3] & 3, attn = info[4], ambSrc = info[5] & 1;
        if (attn == 0) {  // GX_AF_SPEC
            diffuse = 0;
        }
        u32 attnEnable = attn != 2 ? 1 : 0, attnSelect = attn != 0 ? 1 : 0;
        return matSrc | (enable << 1) | ((mask & 0xF) << 2) | (ambSrc << 6) | (diffuse << 7) | (attnEnable << 9) | (attnSelect << 10) |
               (((mask >> 4) & 0xF) << 11);
    }

    // Decodes one stored vertex (see n3d.py) into the GPU path's vertex.
    void decodeVertex(const N3DShape& s, const u8* v, u32 ras, HostGfxModelVertex& o) {
        u32 mask = s.mask;
        u32 off;
        u8 slot;
        if (mask & ATTR_POS_F32) {
            memcpy(&o.x, v, 12);
            slot = v[12];
            off = 16;
        } else {
            s16 p[3];
            memcpy(p, v, 6);
            o.x = p[0] * s.posScale;
            o.y = p[1] * s.posScale;
            o.z = p[2] * s.posScale;
            slot = v[6];
            off = 8;
        }
        u32 sourcePos = 0;
        if (mask & ATTR_SOURCE_POS) {
            memcpy(&sourcePos, v + off, sizeof(sourcePos));
            off += sizeof(sourcePos);
        }
        f32 nrm[3] = {0.0f, 0.0f, 1.0f};
        if (mask & ATTR_NRM) {
            const s8* q = reinterpret_cast< const s8* >(v + off);
            const f32 step = (mask & ATTR_NRM_X2) ? 2.0f / 127.0f : 1.0f / 127.0f;
            nrm[0] = q[0] * step;
            nrm[1] = q[1] * step;
            nrm[2] = q[2] * step;
            off += 4;
        }
        o.nx = nrm[0];
        o.ny = nrm[1];
        o.nz = nrm[2];
        if (sSkinning || sCpuPositions) {
            if (sSkinVertexBuffer == nullptr) {
                sDeferredTexInvalid = true;
                return;
            }
            const J3DVertexData* vd = sSkinVertexBuffer->getVertexData();
            const void* positions = sSkinVertexBuffer->getCurrentVtxPos();
            if (vd == nullptr || positions == nullptr || sourcePos >= vd->getVtxNum()) {
                sDeferredTexInvalid = true;
                return;
            }
            if (sSkinNormals) {
                const void* normals = sSkinVertexBuffer->getCurrentVtxNrm();
                if (normals == nullptr) {
                    sDeferredTexInvalid = true;
                    return;
                }
                const f32* n = static_cast<const f32*>(normals) + sourcePos * 3;
                o.nx = n[0];
                o.ny = n[1];
                o.nz = n[2];
            }
            if (sCpuPositions) {
                // CShader writes the layers big-endian, as the model's arrays (FurShader.cpp). They
                // are s16 or f32 (FurCtrl::setupFur).
                const u8* b = static_cast<const u8*>(positions);
                f32 p[3];
                if (vd->getVtxPosType() == GX_F32) {
                    b += sourcePos * 12;
                    for (u32 i = 0; i < 3; i++, b += 4) {
                        const u32 bits = (static_cast<u32>(b[0]) << 24) | (static_cast<u32>(b[1]) << 16) | (static_cast<u32>(b[2]) << 8) | b[3];
                        memcpy(&p[i], &bits, sizeof(bits));
                    }
                } else {
                    const f32 scale = 1.0f / static_cast<f32>(1 << vd->getVtxPosFrac());
                    b += sourcePos * 6;
                    for (u32 i = 0; i < 3; i++, b += 2) {
                        p[i] = static_cast<s16>((b[0] << 8) | b[1]) * scale;
                    }
                }
                o.x = p[0];
                o.y = p[1];
                o.z = p[2];
            } else if (vd->getVtxPosType() == GX_F32) {
                const f32* p = static_cast<const f32*>(positions) + sourcePos * 3;
                o.x = p[0];
                o.y = p[1];
                o.z = p[2];
            } else {
                const f32 scale = 1.0f / static_cast<f32>(1 << vd->getVtxPosFrac());
                switch (vd->getVtxPosType()) {
                case GX_U8: {
                    const u8* p = static_cast<const u8*>(positions) + sourcePos * 3;
                    o.x = p[0] * scale;
                    o.y = p[1] * scale;
                    o.z = p[2] * scale;
                    break;
                }
                case GX_S8: {
                    const s8* p = static_cast<const s8*>(positions) + sourcePos * 3;
                    o.x = p[0] * scale;
                    o.y = p[1] * scale;
                    o.z = p[2] * scale;
                    break;
                }
                case GX_U16: {
                    const u16* p = static_cast<const u16*>(positions) + sourcePos * 3;
                    o.x = p[0] * scale;
                    o.y = p[1] * scale;
                    o.z = p[2] * scale;
                    break;
                }
                default: {
                    const s16* p = static_cast<const s16*>(positions) + sourcePos * 3;
                    o.x = p[0] * scale;
                    o.y = p[1] * scale;
                    o.z = p[2] * scale;
                    break;
                }
                }
            }
            if (sSkinNormals && vd->getVtxNrmType() != GX_F32) {
                const f32 scale = 1.0f / static_cast<f32>(1 << vd->getVtxNrmFrac());
                const void* normalData = sSkinVertexBuffer->getCurrentVtxNrm();
                switch (vd->getVtxNrmType()) {
                case GX_U8: {
                    const u8* n = static_cast<const u8*>(normalData) + sourcePos * 3;
                    o.nx = n[0] * scale;
                    o.ny = n[1] * scale;
                    o.nz = n[2] * scale;
                    break;
                }
                case GX_S8: {
                    const s8* n = static_cast<const s8*>(normalData) + sourcePos * 3;
                    o.nx = n[0] * scale;
                    o.ny = n[1] * scale;
                    o.nz = n[2] * scale;
                    break;
                }
                case GX_U16: {
                    const u16* n = static_cast<const u16*>(normalData) + sourcePos * 3;
                    o.nx = n[0] * scale;
                    o.ny = n[1] * scale;
                    o.nz = n[2] * scale;
                    break;
                }
                default: {
                    const s16* n = static_cast<const s16*>(normalData) + sourcePos * 3;
                    o.nx = n[0] * scale;
                    o.ny = n[1] * scale;
                    o.nz = n[2] * scale;
                    break;
                }
                }
                const f32 length = std::sqrt(o.nx * o.nx + o.ny * o.ny + o.nz * o.nz);
                if (length > 0.000001f) {
                    o.nx /= length;
                    o.ny /= length;
                    o.nz /= length;
                }
            }
            // Skinned positions take one matrix; a fur layer's stay on their joints.
            if (sSkinning) {
                slot = 0;
            }
        }
        static const u8 kWhite[4] = {255, 255, 255, 255};
        const u8* clr0 = kWhite;
        const u8* clr1 = kWhite;
        if (mask & ATTR_CLR0) {
            clr0 = v + off;
            off += 4;
        }
        if (mask & ATTR_CLR1) {
            clr1 = v + off;
            off += 4;
        }
        memcpy(o.rgba, ras == 1 ? clr1 : clr0, 4);
        f32 tex[8][2] = {};
        for (u32 t = 0; t < 8; t++) {
            if (mask & (ATTR_TEX0 << t)) {
                s16 q[2];
                memcpy(q, v + off, 4);
                tex[t][0] = q[0] * s.uvScale;
                tex[t][1] = q[1] * s.uvScale;
                off += 4;
            }
        }
        u8 texMtx[8] = {};
        if (mask & ATTR_TEXMTX) {
            memcpy(texMtx, v + off, sizeof(texMtx));
        }
        o.matrix = static_cast< u8 >(slot * 3);
        o.texRow[0] = o.texRow[1] = o.texRow[2] = 0;
        for (u32 u = 0; u < 3; u++) {
            o.texRow[u] = (mask & ATTR_TEXMTX) ? texMtx[sUnitCoord[u]] : 0;
            u8 row = sUnitRow[u];
            if (row == 0) {
                o.tex[u][0] = o.x, o.tex[u][1] = o.y, o.tex[u][2] = o.z;
            } else if (row == 1) {
                o.tex[u][0] = nrm[0], o.tex[u][1] = nrm[1], o.tex[u][2] = nrm[2];
            } else if (row >= 5 && row < 13) {
                o.tex[u][0] = tex[row - 5][0], o.tex[u][1] = tex[row - 5][1], o.tex[u][2] = 1.0f;
            } else {
                o.tex[u][0] = o.tex[u][1] = 0.0f, o.tex[u][2] = 1.0f;
            }
            if (sDeferredTexMtx && u < sDraw.unitCount) {
                u8 matrixIndex = texMtx[sUnitCoord[u]];
                if (matrixIndex < 30 || matrixIndex >= 60 || (matrixIndex - 30) % 3 != 0 || sUnitCoord[u] >= sDeferredTexGenCount) {
                    sDeferredTexInvalid = true;
                    o.tex[u][0] = o.tex[u][1] = 0.0f;
                    o.tex[u][2] = 1.0f;
                    continue;
                }
                u32 matrixSlot = (matrixIndex - 30) / 3;
                if (matrixSlot >= sDeferredMatrixCount) {
                    sDeferredTexInvalid = true;
                    o.tex[u][0] = o.tex[u][1] = 0.0f;
                    o.tex[u][2] = 1.0f;
                    continue;
                }
                f32 input[4] = {o.tex[u][0], o.tex[u][1], o.tex[u][2], 1.0f};
                if (!sUnitAbc1[u]) {
                    input[2] = 1.0f;
                }
                const Mtx& m = sDeferredTexMatrices[matrixSlot][sUnitCoord[u]];
                f32 ss = m[0][0] * input[0] + m[0][1] * input[1] + m[0][2] * input[2] + m[0][3];
                f32 tt = m[1][0] * input[0] + m[1][1] * input[1] + m[1][2] * input[2] + m[1][3];
                if (sUnitStq[u]) {
                    f32 q = m[2][0] * input[0] + m[2][1] * input[1] + m[2][2] * input[2] + m[2][3];
                    if (q != 0.0f) {
                        ss /= q;
                        tt /= q;
                    } else {
                        ss = tt = 0.0f;
                    }
                }
                o.tex[u][0] = ss;
                o.tex[u][1] = tt;
                o.tex[u][2] = 1.0f;
            }
        }
    }

    // Whether an upload's vertices serve the current draw although its key differs: they differ
    // only in the colour set and the units' sources (decodeVertex), so the draw must not read
    // the vertex colours of another set, and each of its units must have the same source.
    bool uploadCompatible(const Upload& up, const N3DShape& s) {
        if (sUploadKeyExact || up.exact) {
            return false;
        }
        const RvlNativeDraw& d = sDraw;
        if (d.rasChannel <= 1 && up.ras != d.rasChannel) {
            // Colour from the vertex as material, or as ambient of a lit channel.
            const u32 c = d.colorCtrl[d.rasChannel], a = d.alphaCtrl[d.rasChannel];
            const bool colour = (c & 1) || ((c & 2) && (c & 0x40)) || (a & 1) || ((a & 2) && (a & 0x40));
            if (colour) {
                return false;
            }
        }
        for (u32 u = 0; u < d.unitCount && u < 3; u++) {
            if (up.row[u] != sUnitRow[u] || ((s.mask & ATTR_TEXMTX) && up.coord[u] != sUnitCoord[u])) {
                return false;
            }
        }
        return true;
    }

    // Static J3D groups stay in the persistent arena. Deferred texture matrices vary by packet
    // and frame, so those vertices and indices use the current frame's transient buffers.
    // Slot 1 holds the vertices of a later pass whose units generate coordinates from other vertex
    // data than the first pass's (sPassOwnUnits).
    bool uploadGroup(NativeModel& n, u32 shapeIndex, u32 g, Upload*& pOut, u32 slot = 0) {
        Upload* ways = &n.uploads[((n.firstGroup[shapeIndex] + g) * 2 + slot) * kUploadWays];
        u32 epoch = hostGfxArenaEpoch();
        auto current = [&](const Upload& w) { return w.indices != nullptr && w.epoch == epoch; };
        // A fur layer's positions are made once (FurCtrl::calcLayerForm): it stays with the static
        // groups, under its own entry, as a model's groups have room for one set of vertices. Ten
        // layers a frame in the transient vertices (11130 on Dino Piranha's planet) left the HUD,
        // drawn last, without any (CIA 0.17.1).
        FurUpload* fur = nullptr;
        if (sCpuPositions && !sSkinning && !sDeferredTexMtx) {
            const void* positions = sSkinVertexBuffer != nullptr ? sSkinVertexBuffer->getCurrentVtxPos() : nullptr;
            FurUpload* stale = nullptr;
            for (FurUpload& entry : sFurUploads) {
                if (entry.model == &n && entry.positions == positions && entry.shape == shapeIndex && entry.group == g && current(entry.up)) {
                    pOut = &entry.up;
                    return true;
                }
                if (stale == nullptr && !current(entry.up)) {
                    stale = &entry;
                }
            }
            fur = stale;
            if (fur != nullptr) {
                fur->model = &n;
                fur->positions = positions;
                fur->shape = shapeIndex;
                fur->group = g;
            }
        }
        const bool cpuVertices = (sSkinning || sCpuPositions) && fur == nullptr;
        const bool persistent = !sDeferredTexMtx && !cpuVertices && fur == nullptr;
        if (persistent) {
            for (u32 w = 0; w < kUploadWays; w++) {
                if (current(ways[w]) && ways[w].key == sUploadKey) {
                    pOut = &ways[w];
                    return true;
                }
            }
            for (u32 w = 0; w < kUploadWays; w++) {
                if (current(ways[w]) && uploadCompatible(ways[w], n.shapes[shapeIndex])) {
                    pOut = &ways[w];
                    sCompatibleUploads++;
                    return true;
                }
            }
        }
        // The entry to fill: a free or stale one, else the last (the first keys stay).
        u32 way = kUploadWays - 1;
        if (persistent) {
            for (u32 w = 0; w < kUploadWays; w++) {
                if (!current(ways[w])) {
                    way = w;
                    break;
                }
            }
            u32 others = 0;
            for (u32 w = 0; w < kUploadWays; w++) {
                others += w != way && current(ways[w]) ? 1 : 0;
            }
            if (current(ways[way])) {
                sReplacedKeyUploads++;
            } else if (others != 0) {
                sSecondKeyUploads++;
            }
        }
        Upload& up = fur != nullptr ? fur->up : ways[way];
        pOut = &up;
        up.epoch = 0;
        up.key = 0;
        up.first = 0;
        up.indices = nullptr;
        up.bounds = kNoBounds;
        const N3DShape& s = n.shapes[shapeIndex];
        const N3DGroup& grp = n.groups[n.firstGroup[shapeIndex] + g];
        static u8 sChunk[16384];
        if (s.stride == 0 || s.stride > sizeof(sChunk)) {
            return false;
        }
        // Transient uploads come back every frame: they read the .n3d from memory (rawFile), not
        // the card (an open and reads per draw and frame before CIA 0.11.5).
        // A model's groups upload one after the other: its file stays open between them.
        const u8* raw = sDeferredTexMtx || cpuVertices ? rawFile(n) : nullptr;
        u32 rawSize = raw != nullptr ? n.rawSize : 0;
        if (raw == nullptr) {
            const u64 fileStart = hostGetTick();
            raw = uploadFile(n, rawSize);
            sUploadFileTicks += hostGetTick() - fileStart;
        }
        static void* sOpenFile;
        static char sOpenPath[256];  // a copy: a freed model's path memory may hold another's
        if (raw == nullptr && (sOpenFile == nullptr || strcmp(sOpenPath, n.path) != 0)) {
            if (sOpenFile != nullptr) {
                hostFileClose(sOpenFile);
            }
            sOpenFile = hostFileOpen(n.path);
            strncpy(sOpenPath, n.path, sizeof(sOpenPath) - 1);
        }
        void* file = raw == nullptr ? sOpenFile : nullptr;
        if (raw == nullptr && file == nullptr) {
            return false;
        }
        auto read = [&](void* dst, u32 size, u32 offset) {
            if (raw == nullptr) {
                return readAt(file, dst, size, offset);
            }
            if (offset > rawSize || size > rawSize - offset) {
                return false;
            }
            memcpy(dst, raw + offset, size);
            return true;
        };
        HostGfxModelVertex* out;
        u32 first = hostGfxArenaAlloc(grp.vertexCount, sDeferredTexMtx || cpuVertices ? 0 : 1, &out);
        if (first == HOST_GFX_NO_ARENA) {
            return false;
        }
        u16* indices = sDeferredTexMtx || cpuVertices ? hostGfxIndexAllocTransient(grp.indexCount) : hostGfxIndexAlloc(grp.indexCount);
        if (indices == nullptr) {
            return false;
        }
        u32 perChunk = sizeof(sChunk) / s.stride;
        bool ok = true;
        for (u32 i = 0; i < grp.vertexCount && ok; i += perChunk) {
            u32 count = grp.vertexCount - i < perChunk ? grp.vertexCount - i : perChunk;
            ok = read(sChunk, count * s.stride, grp.vertexOffset + i * s.stride);
            for (u32 k = 0; k < count && ok; k++) {
                decodeVertex(s, sChunk + k * s.stride, sDraw.rasChannel, out[i + k]);
            }
        }
        // Indices are relative to the start of the arena window the vertices are in (the draw binds
        // that window); an allocation never straddles two windows.
        u32 base = first % 65536;
        if (grp.vertexCount > 65536 - base) {
            return false;
        }
        u32 perIndexChunk = sizeof(sChunk) / 2;
        for (u32 i = 0; i < grp.indexCount && ok; i += perIndexChunk) {
            u32 count = grp.indexCount - i < perIndexChunk ? grp.indexCount - i : perIndexChunk;
            ok = read(sChunk, count * 2, grp.indexOffset + i * 2);
            const u16* src = reinterpret_cast< const u16* >(sChunk);
            for (u32 k = 0; k < count; k++) {
                if (src[k] >= grp.vertexCount) {
                    ok = false;
                    break;
                }
                indices[i + k] = static_cast< u16 >(base + src[k]);
            }
        }
        if (!ok || sDeferredTexInvalid) {
            return false;
        }
        GroupBounds* bounds = nullptr;
        up.bounds = newBounds(!(sDeferredTexMtx || cpuVertices), epoch, bounds);
        if (bounds != nullptr) {
            computeBounds(out, grp.vertexCount, *bounds);
            if (!(sDeferredTexMtx || cpuVertices)) {
                computeChunks(out, base, indices, grp.indexCount, epoch, *bounds);
            }
        }
        up.epoch = epoch;
        up.key = sUploadKey;
        up.ras = static_cast< u8 >(sDraw.rasChannel);
        memcpy(up.row, sUnitRow, 3);
        memcpy(up.coord, sUnitCoord, 3);
        up.exact = sUploadKeyExact ? 1 : 0;
        up.first = first;
        up.indices = indices;
        (sDeferredTexMtx || cpuVertices ? sTransientUploads : sPersistentUploads)++;
        sUploadedVertices += grp.vertexCount;
        return true;
    }

    enum {
        SHAPE_BAD_INDEX,
        SHAPE_NO_MATRIX,
        SHAPE_MISSING_MATRIX_TABLE,
    };

    bool fallback(u32 reason) {
        gGXProfile.nativeFallbacks[reason]++;
        return false;
    }

    bool fallbackShape(u32 reason) {
        gGXProfile.nativeShapeFallbacks[reason]++;
        return fallback(FB_SHAPE);
    }

}  // namespace

namespace {

    MatValues sLive;               // the shared material's values (rvlNativeBegin)
    const J3DMaterial* sBegunMaterial;  // that material
    const MatValues* sApplied;     // the values the state holds now
    J3DTexture* sTextures;
    s8 sGenTexMtx[3];              // per unit: texture matrix 0-3, -1 identity

    // The combiner stages of a material record (mat.py), without their constants.
    void loadStages(const u8* m, HostGfxState& st) {
        st.stageCount = m[0] > HOST_GFX_STAGES ? HOST_GFX_STAGES : m[0];
        for (u32 i = 0; i < st.stageCount; i++) {
            const u8* s = m + 0x1C + i * kStageSize;
            HostGfxStage& h = st.stages[i];
            for (u32 k = 0; k < 3; k++) {
                h.colorSrc[k] = s[k];
                h.colorOp[k] = s[3 + k];
                h.alphaSrc[k] = s[6 + k];
                h.alphaOp[k] = s[9 + k];
            }
            h.colorFunc = s[12];
            h.alphaFunc = s[13];
            h.colorScale = s[14];
            h.alphaScale = s[15];
        }
        st.bufferUpdateRgb = m[0x148] & 0xF;
        st.bufferUpdateAlpha = m[0x149] & 0xF;
    }

    // The stage constants of a material record from a set of values.
    void loadConstants(const MatValues& v, const u8* m, HostGfxState& st) {
        for (u32 i = 0; i < st.stageCount; i++) {
            const u8* s = m + 0x1C + i * kStageSize;
            s32 rgba[4] = {s[24], s[25], s[26], s[27]};
            refValue(v, s + 16, rgba);
            s32 alpha[4];
            if (refValue(v, s + 20, alpha)) {
                rgba[3] = alpha[0];
            }
            st.stages[i].constant = static_cast< u32 >(rgba[0]) | (rgba[1] << 8) | (rgba[2] << 16) | (static_cast< u32 >(rgba[3]) << 24);
        }
    }

    // The texture matrices of the units setupUnits set up, from a set of values.
    void unitMatrices(const MatValues& v) {
        RvlNativeDraw& d = sDraw;
        for (u32 u = 0; u < d.unitCount; u++) {
            s8 t = sGenTexMtx[u];
            d.gen[u].matrix = t >= 0 && (v.texMtxValid & (1 << t)) ? &v.texMtx[t][0][0] : &kIdentity[0][0];
            if (sTexMtxOverride != nullptr) {
                d.gen[u].matrix = &sTexMtxOverride[0][0];
            }
        }
    }

    // Textures, combiner constants, channel colours and texture matrices from a set of values.
    bool applyValues(const MatValues& v) {
        const u8* m = sMat;
        HostGfxState& st = sState;
        RvlNativeDraw& d = sDraw;
        for (u32 u = 0; u < d.unitCount; u++) {
            u16 texNo = v.texNo[sUnitMap[u]];
            if (sTextures == nullptr || texNo == 0xFFFF || texNo >= sTextures->getNum()) {
                return false;
            }
            ResTIMG* timg = sTextures->getResTIMG(texNo);
            // rvl3DSGetNativeTIMGTexture expands GX palette formats to an RGBA8
            // Citro3D texture, so indexed J3D textures do not need a GX fallback.
            if (sDelta != nullptr && (sDelta->mask & RvlGXDelta::TEXTURE) && sDelta->texture != nullptr &&
                sDelta->textureMap == sUnitMap[u]) {
                timg = const_cast< ResTIMG* >(sDelta->texture);  // the Wii's GXSetTexImgPtr of the map
            }
            d.captureMask &= ~(1u << u);
            st.textures[u] = rvl3DSGetNativeTIMGCapture(timg, d.captureMap[u]);
            if (st.textures[u] != nullptr) {
                d.captureMask |= 1u << u;  // a screen capture the image was replaced with
            } else {
                st.textures[u] = rvl3DSGetNativeTIMGTexture(timg);
            }
            if (st.textures[u] == nullptr) {
                return false;
            }
        }
        // The delta's TEV registers take the material's place in the combiner constants.
        const MatValues* constants = &v;
        static MatValues sOverridden;
        if (sDelta != nullptr && (sDelta->mask & RvlGXDelta::TEV_REGISTER)) {
            sOverridden = v;
            for (u32 r = 1; r < 4; r++) {
                if (sDelta->registerMask & (1 << r)) {
                    std::memcpy(sOverridden.tev[r - 1], sDelta->registers[r], sizeof(sOverridden.tev[r - 1]));
                    sOverridden.tevValid |= 1 << (r - 1);
                }
            }
            constants = &sOverridden;
        }
        if (hostDebugNative() < 2 && !sDeltaStages) {
            loadConstants(*constants, m, st);
        }
        if (sDeltaAlpha >= 0 && st.stageCount != 0) {
            HostGfxStage& h = st.stages[st.stageCount - 1];
            h.alphaSrc[0] = h.alphaSrc[1] = h.alphaSrc[2] = 0x0E;  // GPU_CONSTANT
            h.alphaOp[0] = h.alphaOp[1] = h.alphaOp[2] = 0;
            h.alphaFunc = 0;  // GPU_REPLACE
            h.alphaScale = 0;
            h.constant = (h.constant & 0x00FFFFFFu) | (static_cast< u32 >(sDeltaAlpha) << 24);
        }
        for (u32 p = 0; p < sPassCount; p++) {
            const u8* r = sPassRecords[p];
            loadConstants(*constants, r, sPassStates[p]);
            sPassCaptureMask[p] = 0;
            for (u32 u = 0; u < 3; u++) {
                sPassStates[p].textures[u] = st.textures[u];
                if (sPassOwnUnits[p] == 0 || u >= r[1]) {
                    continue;
                }
                if (memcmp(r + 0x10 + u * 4, m + 0x10 + u * 4, 4) == 0) {
                    if (d.captureMask & (1u << u)) {
                        sPassCaptureMask[p] |= 1u << u;
                        memcpy(sPassCaptureMap[p][u], d.captureMap[u], sizeof(d.captureMap[u]));
                    }
                    continue;
                }
                const u16 texNo = v.texNo[r[0x12 + u * 4] & 7];
                ResTIMG* timg = sTextures != nullptr && texNo < sTextures->getNum() ? sTextures->getResTIMG(texNo) : nullptr;
                // RosettaHairMat's fourth texture is the screen capture her dissolve fades to.
                void* own = timg != nullptr ? rvl3DSGetNativeTIMGCapture(timg, sPassCaptureMap[p][u]) : nullptr;
                if (own != nullptr) {
                    sPassCaptureMask[p] |= 1u << u;
                } else if (timg != nullptr) {
                    own = rvl3DSGetNativeTIMGTexture(timg);
                }
                sPassStates[p].textures[u] = own;
                if (own == nullptr) {
                    sPassOwnUnits[p] = 2;
                }
            }
        }
        for (u32 c = 0; c < 2; c++) {
            d.matColor[c] = (v.colorValid & (1 << c)) ? v.matColor[c] : rd32(m + 0xE8 + c * 4);
            d.ambColor[c] = (v.colorValid & (4 << c)) ? v.ambColor[c] : rd32(m + 0xF0 + c * 4);
        }
        if (sDelta != nullptr && (sDelta->mask & (RvlGXDelta::MAT_COLOR | RvlGXDelta::AMB_COLOR))) {
            // GXSetChanMatColor / GXSetChanAmbColor of COLOR0A0 (0xRRGGBBAA here).
            for (u32 i = 0; i < 4; i++) {
                const u32 shift = 24 - i * 8, part = i < 3 ? 1 : 2;
                if ((sDelta->mask & RvlGXDelta::MAT_COLOR) && (sDelta->matMask & part)) {
                    d.matColor[0] = (d.matColor[0] & ~(0xFFu << shift)) | (static_cast< u32 >(sDelta->matColor[i]) << shift);
                }
                if ((sDelta->mask & RvlGXDelta::AMB_COLOR) && (sDelta->ambMask & part)) {
                    d.ambColor[0] = (d.ambColor[0] & ~(0xFFu << shift)) | (static_cast< u32 >(sDelta->ambColor[i]) << shift);
                }
            }
        }
        unitMatrices(v);
        sApplied = &v;
        return true;
    }

    bool setupUnits(const u8* m, u32 count, const u8* coords);

    // Later pass p of group g with units of its own (sPassOwnUnits): their coordinates, and the
    // group's vertices again in upload slot 1 when the units generate them from other vertex data
    // than the first pass's (RosettaHairMat's fourth texture, from the position). A pass that cannot
    // be set up is left out; the first pass's units are back after.
    bool submitOwnUnits(NativeModel& n, u32 shapeIndex, u32 g, u32 p, const N3DShape& s) {
        if (sPassOwnUnits[p] != 1) {
            return true;
        }
        RvlNativeDraw& d = sDraw;
        const RvlNativeDraw saved = d;
        u8 row[3], coord[3], stq[3], abc1[3];
        s8 texMtx[3];
        memcpy(row, sUnitRow, 3);
        memcpy(coord, sUnitCoord, 3);
        memcpy(stq, sUnitStq, 3);
        memcpy(abc1, sUnitAbc1, 3);
        memcpy(texMtx, sGenTexMtx, 3);
        const u32 key = sUploadKey;
        const u8* r = sPassRecords[p];
        u8 coords[3];
        for (u32 u = 0; u < 3; u++) {
            coords[u] = r[0x13 + u * 4] & 7;
        }
        d.unitCount = r[1];
        d.captureMask = sPassCaptureMask[p];
        memcpy(d.captureMap, sPassCaptureMap[p], sizeof(d.captureMap));
        bool ok = d.unitCount <= 3 && setupUnits(r, d.unitCount, coords);
        if (ok) {
            unitMatrices(*sApplied);
            for (u32 u = 0; u < d.unitCount; u++) {
                d.gen[u].direct = sDeferredTexMtx;
                ok &= !sDeferredTexMtx || (d.gen[u].generated && sUnitCoord[u] < sDeferredTexGenCount);
            }
        }
        // The first pass's vertices hold each unit's source data, and its texture matrix row or
        // deferred coordinate of the coordinate set: the same when those match.
        const bool same = memcmp(row, sUnitRow, 3) == 0 &&
                          (memcmp(coord, sUnitCoord, 3) == 0 || (!sDeferredTexMtx && !(s.mask & ATTR_TEXMTX)));
        if (ok && !same) {
            Upload* up;
            ok = uploadGroup(n, shapeIndex, g, up, 1);
            if (ok) {
                d.first = up->first;
                d.indices = up->indices;
                const GroupBounds* bounds = boundsOf(up->bounds);
                d.spheres = bounds != nullptr ? bounds->spheres : nullptr;
                d.slotMask = bounds != nullptr ? bounds->slotMask : 0;
                d.chunks = bounds != nullptr && bounds->chunkCount != 0 ? &sChunks[bounds->chunkFirst] : nullptr;
                d.chunkCount = bounds != nullptr ? bounds->chunkCount : 0;
            }
        }
        const bool submitted = !ok || rvlNativeDrawSubmit(d);
        d = saved;
        memcpy(sUnitRow, row, 3);
        memcpy(sUnitCoord, coord, 3);
        memcpy(sUnitStq, stq, 3);
        memcpy(sUnitAbc1, abc1, 3);
        memcpy(sGenTexMtx, texMtx, 3);
        sUploadKey = key;
        sDeferredTexInvalid = false;
        return submitted;
    }

}  // namespace


void rvlNativeSetDelta(const RvlGXDelta* pDelta) {
    sDelta = pDelta != nullptr && pDelta->mask != 0 ? pDelta : nullptr;
}

// Texture units, GX changes ----------------------------------------------------------------------
namespace {
    // Texture coordinate generation of units 0..count-1 from the material record's texgen of each
    // unit's coordinate (coords), and the upload key their vertex inputs make.
    bool setupUnits(const u8* m, u32 count, const u8* coords) {
        RvlNativeDraw& d = sDraw;
        sUploadKey = d.rasChannel;
        sUploadKeyExact = false;
        for (u32 u = 0; u < 3; u++) {
            RvlNativeTexGen& g = d.gen[u];
            g = RvlNativeTexGen();
            sUnitRow[u] = 0xFF;
            sUnitCoord[u] = 0;
            sUnitStq[u] = sUnitAbc1[u] = 0;
            sGenTexMtx[u] = -1;
            if (u >= count) {
                continue;
            }
            u32 coord = coords[u] & 7;
            sUnitCoord[u] = static_cast< u8 >(coord);
            const u8* gen = m + 0xF8 + coord * 4;
            u32 type = gen[0], src = gen[1], mtx = gen[2];
            g.generated = type <= 1;
            g.stq = type == 0;
            if (type == 10 && (src == 0x13 || src == 0x14)) {
                // GX_TG_SRTG from GX_TG_COLOR0/1: no vertex input, the shader takes the channel's
                // lit colour (buildTransform, when the pass rasterizes that channel).
                g.srtg = static_cast< s8 >(src - 0x13);
            } else if (src == 0 || src == 1) {
                g.row = static_cast< u8 >(src);
                g.abc1 = true;
            } else if (src >= 4 && src <= 11) {
                g.row = static_cast< u8 >(5 + src - 4);
            } else {
                g.generated = false;
            }
            sUnitStq[u] = g.stq;
            sUnitAbc1[u] = g.abc1;
            g.matrix = &kIdentity[0][0];
            if (mtx >= 30 && mtx <= 57) {
                u32 t = (mtx - 30) / 3;
                if (t >= 8) {
                    return false;
                }
                sGenTexMtx[u] = static_cast< s8 >(t);
            }
            sUnitRow[u] = g.generated ? g.row : 0xFF;
            sUploadKey |= static_cast< u32 >(sUnitRow[u]) << (8 + u * 8);
            sUploadKey ^= 0x9E3779B9u + sUnitCoord[u] + (sUploadKey << 6) + (sUploadKey >> 2);
        }
        return true;
    }

    // GXSetAlphaCompare's two comparisons as the one the 3DS has: exact when one side is always
    // (or, with OR, never) true; otherwise the first comparison.
    void alphaCompare(const RvlGXDelta& x, u8& func, u8& ref) {
        const u32 kNever = 0, kAlways = 7;
        u32 c = x.alphaComp0;
        u8 r = x.alphaRef0;
        if (x.alphaCompareOp == 0) {  // GX_AOP_AND
            if (x.alphaComp0 == kAlways) {
                c = x.alphaComp1, r = x.alphaRef1;
            }
        } else if (x.alphaCompareOp == 1) {  // GX_AOP_OR
            if (x.alphaComp0 == kAlways || x.alphaComp1 == kAlways) {
                c = kAlways;
            } else if (x.alphaComp0 == kNever) {
                c = x.alphaComp1, r = x.alphaRef1;
            }
        }
        func = GXToCitro3D::alphaTest(c);
        ref = r;
    }

    // XF channel control word (channelControl's layout) from GXSetChanCtrl's values.
    u32 channelWord(bool enable, u32 ambSrc, u32 matSrc, u32 lights, u32 diffuse, u32 attn) {
        if (attn == 0) {  // GX_AF_SPEC
            diffuse = 0;
        }
        u32 attnEnable = attn != 2 ? 1 : 0, attnSelect = attn != 0 ? 1 : 0;
        return (matSrc & 1) | ((enable ? 1u : 0u) << 1) | ((lights & 0xF) << 2) | ((ambSrc & 1) << 6) | ((diffuse & 3) << 7) |
               (attnEnable << 9) | (attnSelect << 10) | (((lights >> 4) & 0xF) << 11);
    }

    // The fragment state a delta changes. The material's later passes rebuild its own colour:
    // they are dropped once the draw is something else (another blend, Z, stencil or combiner).
    void applyDeltaPixel(const RvlGXDelta& x, HostGfxState& st) {
        const u32 m = x.mask;
        if (m & RvlGXDelta::CULL) {
            sCullAll = x.cull == 3;
            st.cull = x.cull == 2 ? 1 : x.cull == 1 ? 2 : 0;
        }
        if (m & RvlGXDelta::ZMODE) {
            st.depthTest = x.zEnable;
            st.depthFunc = GXToCitro3D::depthTest(x.zFunc);
            st.writeMask = (st.writeMask & ~GXToCitro3D::WRITE_DEPTH) | (x.zEnable && x.zUpdate ? GXToCitro3D::WRITE_DEPTH : 0);
        }
        if (m & RvlGXDelta::ALPHA_COMPARE) {
            alphaCompare(x, st.alphaTestFunc, st.alphaRef);
            sAlphaSplit = false;
        }
        if (m & RvlGXDelta::BLEND) {
            int logic;
            GXToCitro3D::blendMode(x.blendType, x.blendSrc, x.blendDst, x.logicOp, st.blendEquation, st.blendSrc, st.blendDst, logic);
            st.logicOpEnable = logic >= 0;
            st.logicOp = logic >= 0 ? static_cast< u8 >(logic) : 0;
            st.writeMask = (st.writeMask & GXToCitro3D::WRITE_DEPTH) | (x.colorUpdate ? GXToCitro3D::WRITE_RGB : 0) |
                           (x.alphaUpdate ? GXToCitro3D::WRITE_ALPHA : 0);
        }
        if (m & RvlGXDelta::DST_ALPHA) {
            st.dstAlphaEnable = x.dstAlphaEnable;
            st.dstAlpha = x.dstAlpha;
        }
        if (m & RvlGXDelta::STENCIL) {
            st.stencilEnable = x.stencil[0];
            st.stencilFunc = x.stencil[1];
            st.stencilRef = x.stencil[2];
            st.stencilMask = x.stencil[3];
            st.stencilWriteMask = x.stencil[4];
            st.stencilFail = x.stencil[5];
            st.stencilZFail = x.stencil[6];
            st.stencilZPass = x.stencil[7];
        }
        if (m & RvlGXDelta::FOG) {
            st.fogParamA = st.fogParamB = st.fogParamShift = st.fogParamC = st.fogColor = 0;
            J3DFogInfo f;
            std::memset(&f, 0, sizeof(f));
            f.mType = x.fogType;
            f.mStartZ = x.fogStart;
            f.mEndZ = x.fogEnd;
            f.mNearZ = x.fogNear;
            f.mFarZ = x.fogFar;
            f.mColor.r = x.fogColor[0];
            f.mColor.g = x.fogColor[1];
            f.mColor.b = x.fogColor[2];
            f.mColor.a = x.fogColor[3];
            encodeFog(f, st);
        }
        if (m & (RvlGXDelta::BLEND | RvlGXDelta::ZMODE | RvlGXDelta::STENCIL | RvlGXDelta::ALPHA_COMPARE | RvlGXDelta::TEV_COLOR |
                 RvlGXDelta::GENMODE)) {
            sPassCount = 0;
        }
    }

    // The value of a TEV register (GX_TEVPREV + r) for the delta's combiners: the delta's own, else
    // the material's.
    void deltaRegister(const RvlGXDelta& x, u32 r, s16* rgba) {
        if ((x.mask & RvlGXDelta::TEV_REGISTER) && (x.registerMask & (1 << r))) {
            std::memcpy(rgba, x.registers[r], 4 * sizeof(s16));
        } else if (r >= 1 && (sLive.tevValid & (1 << (r - 1)))) {
            std::memcpy(rgba, sLive.tev[r - 1], 4 * sizeof(s16));
        } else {
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
        }
    }

    // A delta's own TEV stage (J3DModelX's mode lists, Mario's alpha list) replaces the material's
    // combiners; a stage-0 alpha that is only a register becomes the draw's final alpha.
    void applyDeltaStages(const RvlGXDelta& x, HostGfxState& st, RvlNativeDraw& d, u8* coords) {
        const u32 m = x.mask;
        if (m & RvlGXDelta::CHANNEL_COLOR0) {
            d.colorCtrl[0] = channelWord(x.chanEnable[0], x.chanAmbSrc[0], x.chanMatSrc[0], x.chanLights[0], x.chanDiffuse[0], x.chanAttn[0]);
        }
        if (m & RvlGXDelta::CHANNEL_ALPHA0) {
            d.alphaCtrl[0] = channelWord(x.chanEnable[1], x.chanAmbSrc[1], x.chanMatSrc[1], x.chanLights[1], x.chanDiffuse[1], x.chanAttn[1]);
        }
        if (m & (RvlGXDelta::GENMODE | RvlGXDelta::TEV_COLOR)) {
            RvlTevDesc desc;
            std::memset(&desc, 0, sizeof(desc));
            rvlTevDefaultSwapTables(desc);
            desc.stageCount = 1;
            RvlTevStageDesc& sd = desc.stages[0];
            const bool hasOrder = (m & RvlGXDelta::TEV_ORDER) != 0;
            sd.texCoord = hasOrder ? x.orderCoord : 0xFF;
            sd.texMap = hasOrder ? x.orderMap : 0xFF;
            sd.channel = hasOrder ? x.orderChannel : 4;  // GX_COLOR0A0
            if ((m & RvlGXDelta::GENMODE) && x.channels == 0) {
                sd.channel = 0xFF;
            }
            if ((m & RvlGXDelta::GENMODE) && x.texGens == 0) {
                sd.texMap = 0xFF;
            }
            // A calculation the lists leave alone keeps the material's stage 0: unknown here, so a
            // colour of zero (those lists write no colour) and the texture's alpha, or one.
            static const u8 kZeroColor[4] = {15, 15, 15, 15};  // GX_CC_ZERO
            static const u8 kTextureAlpha[4] = {7, 7, 7, 4};   // GX_CA_TEXA
            static const u8 kOneAlpha[4] = {7, 7, 7, 6};       // GX_CA_KONST, GX_TEV_KASEL_1
            const u8* ci = (m & RvlGXDelta::TEV_COLOR) ? x.colorIn : kZeroColor;
            const u8* ai = (m & RvlGXDelta::TEV_ALPHA) ? x.alphaIn : sd.texMap != 0xFF ? kTextureAlpha : kOneAlpha;
            for (u32 i = 0; i < 4; i++) {
                sd.colorIn[i] = ci[i];
                sd.alphaIn[i] = ai[i];
            }
            if (m & RvlGXDelta::TEV_COLOR) {
                sd.colorOp = x.colorOp, sd.colorBias = x.colorBias, sd.colorScale = x.colorScale;
            }
            if (m & RvlGXDelta::TEV_ALPHA) {
                sd.alphaOp = x.alphaOp, sd.alphaBias = x.alphaBias, sd.alphaScale = x.alphaScale;
            }
            sd.colorOut = sd.alphaOut = 0;  // GX_TEVPREV
            for (u32 r = 0; r < 4; r++) {
                deltaRegister(x, r, desc.registers[r]);
            }
            for (u32 k = 0; k < 4; k++) {
                for (u32 c = 0; c < 4; c++) {
                    desc.konst[k][c] = (sLive.konstValid & (1 << k)) ? static_cast< u8 >(clampByte(sLive.konst[k][c])) : 0xFF;
                }
            }
            RvlTevProgram prog;
            rvlTevCompile(desc, prog);
            st.stageCount = prog.stageCount;
            for (u32 i = 0; i < prog.stageCount && i < HOST_GFX_STAGES; i++) {
                st.stages[i] = prog.stages[i];
            }
            d.unitCount = prog.unitCount;
            d.rasChannel = prog.rasChannel;
            for (u32 u = 0; u < prog.unitCount && u < 3; u++) {
                sUnitMap[u] = prog.unitMap[u] & 7;
                coords[u] = prog.unitCoord[u] & 7;
            }
            sDeltaStages = true;
            sPassCount = 0;
        } else if (m & RvlGXDelta::TEV_ALPHA) {
            // GX_CA_APREV, A0, A1 or A2 alone (ADD, no bias, scale 1): a constant alpha.
            const u8* a = x.alphaIn;
            if (a[1] == 7 && a[2] == 7 && a[3] == 7 && x.alphaOp == 0 && x.alphaBias == 0 && x.alphaScale == 0 && a[0] <= 3) {
                s16 rgba[4];
                deltaRegister(x, a[0], rgba);
                sDeltaAlpha = clampByte(rgba[3]);
            }
        }
    }

    // A material whose stages displace its textures by an indirect texture (the game's 975 indirect
    // materials: water, crystals, ice, metal, lava), for native_submit.cpp: the indirect textures are
    // intensities, so the three inputs are one value. Two samples where they fit, else the
    // displacement of the middle of the values alone.
    void setupDistortion(J3DMaterial* pMat, u8* coords) {
        RvlNativeDraw& d = sDraw;
        d.distort = RVL_DISTORT_NONE;
        d.distortDraw = 0;
        J3DIndBlock* ind = pMat->getIndBlock();
        J3DTevBlock* tev = pMat->getTevBlock();
        const HostGfxState& st = sState;
        if (sDelta != nullptr || d.unitCount == 0 || ind == nullptr || ind->getIndTexStageNum() != 1 || st.stageCount == 0 ||
            sTextures == nullptr) {
            return;
        }
        const J3DIndTexOrder* order = ind->getIndTexOrder(0);
        const J3DIndTexCoordScale* scale = ind->getIndTexCoordScale(0);
        if (order == nullptr || scale == nullptr || order->mCoord >= 8 || order->mMap >= 8) {
            return;
        }
        // The displaced units: those of the stages with an indirect matrix, all with the same one.
        u32 mtxSel = 0, displaced = 0;
        for (u32 i = 0; i < tev->getTevStageNum(); i++) {
            const J3DIndTevStage* stage = tev->getIndTevStage(i);
            const J3DTevOrder* tevOrder = tev->getTevOrder(i);
            if (stage == nullptr || tevOrder == nullptr) {
                return;
            }
            const u32 info = stage->mInfo, sel = (info >> 9) & 0xF;
            if ((info & (1u << 20)) != 0 || ((info >> 13) & 0x3F) != 0) {
                return;  // adds the previous stage's offset, or wraps: not a plain displacement
            }
            if (sel == 0) {
                continue;
            }
            if (sel > 3 || (mtxSel != 0 && sel != mtxSel) || ((info >> 2) & 3) != 0 || ((info >> 4) & 7) != 7) {
                return;
            }
            mtxSel = sel;
            for (u32 u = 0; u < d.unitCount; u++) {
                if (sUnitMap[u] == tevOrder->mTexMap && coords[u] == tevOrder->mTexCoord) {
                    displaced |= 1u << u;
                }
            }
        }
        const J3DIndTexMtx* mtx = mtxSel != 0 ? ind->getIndTexMtx(mtxSel - 1) : nullptr;
        const u16 indNo = sLive.texNo[order->mMap];
        if (mtx == nullptr || displaced == 0 || indNo >= sTextures->getNum()) {
            return;
        }
        const f32 exponent = ldexpf(1.0f, static_cast< s8 >(mtx->field_0x18));
        const f32 rowS = (mtx->field_0x0[0][0] + mtx->field_0x0[0][1] + mtx->field_0x0[0][2]) * exponent;
        const f32 rowT = (mtx->field_0x0[1][0] + mtx->field_0x0[1][1] + mtx->field_0x0[1][2]) * exponent;
        if (rowS == 0.0f && rowT == 0.0f) {
            return;
        }
        const ResTIMG* indirect = sTextures->getResTIMG(indNo);
        const bool meanOnly = sPassCount != 0 || sAlphaSplit || d.unitCount > 2 || scale->mScaleS != 0 || scale->mScaleT != 0;
        // A draw for each sample: the next ones blend over the first, so the material must replace
        // what is behind it and write its depth. Two samples when the game is falling behind.
        const u8 kOne = 1, kZero = 0, kAlways = 1, kWriteDepth = 0x10;
        bool draws = !meanOnly && !(sZCompBefore && st.alphaTestFunc != kAlways) && st.blendSrc == kOne && st.blendDst == kZero && st.depthTest &&
                     (st.writeMask & kWriteDepth) != 0;
        const HostGfxStage& last = st.stages[st.stageCount - 1];
        for (u32 k = 0; k < 3; k++) {
            const u32 op = last.colorOp[k];
            if (last.colorSrc[k] == 0x0E && (op == 2 || op == 3)) {  // GPU_CONSTANT, its alpha
                draws = false;
            }
        }
        RvlDistortion plan = {};
        for (u32 u = 0; u < d.unitCount; u++) {
            if ((displaced & (1u << u)) == 0) {
                continue;
            }
            const u16 texNo = sLive.texNo[sUnitMap[u]];
            if (texNo >= sTextures->getNum()) {
                return;
            }
            const ResTIMG* timg = sTextures->getResTIMG(texNo);
            if (timg->mWidth == 0 || timg->mHeight == 0) {
                return;
            }
            rvlDistortionPlan(indirect, rowS, rowT, timg->mWidth, timg->mHeight, timg->_19 & 7, draws && !hostGfxFallingBehind() ? 4 : 2, plan.samples, plan);
            d.distortMean[u][0] = plan.mean[0];
            d.distortMean[u][1] = plan.mean[1];
            d.distortStep[u][0] = plan.step[0];
            d.distortStep[u][1] = plan.step[1];
        }
        d.distortLow = plan.low;
        d.distortScale = plan.scale;
        d.distortSamples = plan.samples;
        d.distortUnits = static_cast< u8 >(displaced);
        d.distort = RVL_DISTORT_MEAN;
        if (meanOnly) {
            return;
        }
        const u32 kTexture0 = 3, kBuffer = 0x0D, kPrevious = 0x0F;
        if (d.unitCount == 1 && (plan.samples == 2 || !draws)) {
            if (st.stageCount + 2 > HOST_GFX_STAGES) {
                return;
            }
            // The blend goes through PREVIOUS when only stage 0 reads the texture, else the buffer.
            bool later = false, reads = false, usesBuffer = st.bufferUpdateRgb != 0 || st.bufferUpdateAlpha != 0;
            for (u32 i = 0; i < st.stageCount; i++) {
                for (u32 k = 0; k < 3; k++) {
                    const u32 c = st.stages[i].colorSrc[k], a = st.stages[i].alphaSrc[k];
                    reads |= c == kTexture0 || a == kTexture0;
                    later |= i != 0 && (c == kTexture0 || a == kTexture0);
                    usesBuffer |= c == kBuffer || a == kBuffer;
                    if (i == 0 && (c == kPrevious || a == kPrevious)) {
                        return;
                    }
                }
            }
            if (!reads || (later && usesBuffer)) {
                return;
            }
            d.distort = RVL_DISTORT_ONE_DRAW;
        } else if (!draws) {
            return;
        } else {
            d.distort = RVL_DISTORT_TWO_DRAWS;
        }
        if (d.unitCount == 1) {
            sUnitMap[1] = sUnitMap[0];
            coords[1] = coords[0];
        }
        d.unitCount = 3;
        sUnitMap[2] = order->mMap;
        coords[2] = order->mCoord;
    }
}  // namespace

void rvlDistortionPlan(const ResTIMG* indirect, f32 rowS, f32 rowT, u32 width, u32 height, u32 halvings, u32 maxSamples, u32 samples, RvlDistortion& plan) {
    // Texels of the 3DS texture between two samples at most: near enough that the blend reads as a
    // shift (measured on the game's materials: tools/3ds/audit/u3_structure.py indirect_error).
    const f32 kMaxApart = 3.0f, kMaxApartOfMany = 4.0f;
    u32 low = 0, high = 255;
    if ((indirect->_19 & 0x80) != 0) {
        low = indirect->mPaletteDataOffset & 0xFF;
        high = (indirect->mPaletteDataOffset >> 8) & 0xFF;
    } else if (indirect->mFormat == GX_TF_I8 || indirect->mFormat == GX_TF_I4) {
        // Particle textures, as the file has them: their values, as texreduce._value_range counts them.
        static const ResTIMG* sCounted;  // the batches of an effect ask again and again
        static u8 sCountedLow, sCountedHigh;
        if (indirect != sCounted) {
            const u8* image = reinterpret_cast< const u8* >(indirect) + indirect->mImageDataOffset;
            const bool four = indirect->mFormat == GX_TF_I4;
            const u32 pixels = indirect->mWidth * indirect->mHeight;
            u32 count[256] = {};
            for (u32 i = 0; i < pixels; i++) {
                count[four ? ((i & 1) != 0 ? image[i / 2] & 0xF : image[i / 2] >> 4) * 17 : image[i]]++;
            }
            u32 below = 0;
            for (low = 0; low < 255 && (below += count[low]) <= pixels / 100;) {
                low++;
            }
            u32 above = 0;
            for (high = 255; high > low && (above += count[high]) <= pixels / 100;) {
                high--;
            }
            sCounted = indirect;
            sCountedLow = static_cast< u8 >(low);
            sCountedHigh = static_cast< u8 >(high);
        }
        low = sCountedLow;
        high = sCountedHigh;
    }
    // A texture stretched to the whole range (texreduce._stretched): the blend covers it with no bias.
    const bool stretched = (indirect->_19 & 0xC0) == 0xC0;
    const u32 span = high - low;
    const f32 perS = rowS / static_cast< f32 >(width << halvings), perT = rowT / static_cast< f32 >(height << halvings);
    const f32 texelsS = fabsf(perS) * width, texelsT = fabsf(perT) * height;
    const f32 texels = texelsS > texelsT ? texelsS : texelsT;  // of the 3DS texture, per unit of the value
    if (samples == 0) {
        samples = maxSamples <= 2 || texels * span <= kMaxApart ? 2 : stretched ? 3 : span * 4 <= 255 ? 2 : span * 4 <= 255 * 2 || span * 4 > 255 * 3 ? 3 : 4;
    }
    plan.samples = static_cast< u8 >(samples);
    const u32 bands = samples - 1;
    plan.scale = stretched ? bands - 1 : span * 4 <= 255 * bands ? 2 : span * 2 <= 255 * bands ? 1 : 0;
    const f32 band = stretched ? static_cast< f32 >(span) / bands : 255.0f / static_cast< f32 >(1 << plan.scale), range = band * bands;
    f32 first = 0.5f * static_cast< f32 >(low + high) - 0.5f * range;
    first = first < 0.0f ? 0.0f : first > 255.0f - range ? 255.0f - range : first;
    plan.low = stretched ? 0 : static_cast< u8 >(first);
    const f32 middle = first + 0.5f * range - 128.0f;
    plan.mean[0] = perS * middle;
    plan.mean[1] = perT * middle;
    plan.step[0] = perS * band;
    plan.step[1] = perT * band;
    const f32 apart = texels * band, limit = samples == 2 ? kMaxApart : kMaxApartOfMany;
    if (apart > limit) {
        plan.step[0] *= limit / apart;
        plan.step[1] *= limit / apart;
    }
}

bool rvlNativeBegin(J3DMaterial* pMat, J3DShapePacket* pFirst, bool userData) {
    if (pMat == nullptr || pFirst == nullptr || pFirst->mpModel == nullptr) {
        return fallback(FB_USER_DATA);
    }
    J3DModel* model = pFirst->mpModel;
    J3DModelData* data = model->getModelData();
    if (data == nullptr) {
        return fallback(FB_USER_DATA);
    }
    NativeModel* n = findModel(data);
    // The material record comes from the .n3d of the model that owns the material.
    NativeModel* owner = n;
    if (n != nullptr && (pMat->getIndex() >= data->getMaterialNum() || data->getMaterialNodePointer(pMat->getIndex()) != pMat)) {
        owner = findMaterialOwner(pMat);
    }
    if (n == nullptr || owner == nullptr || pMat->getIndex() >= owner->materialCount) {
        if (n == nullptr && gGXProfile.nativeFallbacks[FB_NO_FILE] < 8) {
            hostLog("native: draw skipped: no matching .n3d for model %p (material %u)\n", data, pMat->getIndex());
        }
        return fallback(FB_NO_FILE);
    }
    // LOD selection is resolved by the J3D model before the packet reaches this
    // renderer. The converted model contains the same shape/material tables, so
    // the active packet can be submitted without entering the Wii display-list path.
    sDeferredTexMtx = false;
    sDeferredTexInvalid = false;
    sHasUserData = userData;
    if (userData) {
        if (gGXProfileOn) {
            if (MR::isUseTexMtxProjMap(pMat)) {
                gGXProfile.nativeUserDataProj++;
            }
            if (MR::isUseTexMtxEnvMap(pMat)) {
                gGXProfile.nativeUserDataEnv++;
            }
            for (u32 i = 0; i < pMat->getTexGenNum(); i++) {
                u8 source = pMat->getTexCoord(i)->getTexGenSrc();
                u32 category = source == GX_TG_POS ? 0 : source == GX_TG_NRM ? 1 : 2;
                gGXProfile.nativeUserDataTexGenSrc[category]++;
            }
        }
    }
    // Converted groups carry their joint matrix indices, so these model flags do not
    // require entering the legacy J3D display-list path.
    const u8* m = owner->materials + pMat->getIndex() * kMaterialSize;
    // FLAG_INEXACT records a deliberate approximation (for example a constant combination
    // that PICA cannot encode exactly, or TEV stages left out of a material that cannot use
    // later passes), so these records remain renderable rather than disappearing as whole
    // objects.
    if (m[0] > HOST_GFX_STAGES || m[1] > 3 || m[0xC5] > 8) {
        if (gGXProfile.nativeFallbacks[FB_MATERIAL] < 8) {
            hostLog("native: draw skipped: material %u exceeds native stage/unit/texgen limits\n", pMat->getIndex());
        }
        return fallback(FB_MATERIAL);
    }
    if (pMat->getTevBlock() == nullptr || pMat->getColorBlock() == nullptr) {
        return fallback(FB_MATERIAL);
    }
    sSkinning = model->checkFlag(J3DMdlFlag_SkinPosCpu) || model->checkFlag(J3DMdlFlag_SkinNrmCpu);
    sCpuPositions = false;
    sSkinVertexBuffer = sSkinning ? model->getVertexBuffer() : nullptr;
    sSkinNormals = model->checkFlag(J3DMdlFlag_SkinNrmCpu);
    if (sSkinning && sSkinVertexBuffer == nullptr) {
        if (gGXProfile.nativeFallbacks[FB_MODEL_FLAGS] < 8) {
            hostLog("native: draw skipped: model %p lacks CPU skin vertex data\n", model);
        }
        return fallback(FB_MODEL_FLAGS);
    }
    if (sSkinning) {
        const J3DVertexData* vertexData = sSkinVertexBuffer->getVertexData();
        if (vertexData == nullptr || !supportedComponentType(vertexData->getVtxPosType()) ||
            vertexData->getVtxPosFrac() > 30 ||
            (sSkinNormals && (!supportedComponentType(vertexData->getVtxNrmType()) ||
                              vertexData->getVtxNrmFrac() > 30))) {
            return fallback(FB_MODEL_FLAGS);
        }
    }
    sMat = m;
    // A lent material's texture numbers index its owner's texture table: MarioActor::copyMaterial
    // also gives the packet the owner's (J3DMatPacket::mpTexture). With the drawn model's own
    // table, Mario's hands (a shorter table) were skipped and his eyes took the face model's
    // placeholder texture.
    sTextures = owner->model->getTexture();

    HostGfxState& st = sState;
    st = HostGfxState();
    u32 units = m[1] > 3 ? 3 : m[1];
    loadStages(m, st);
    if (hostDebugNative() >= 2 && units > 0) {
        // Check modes: the first texture alone, without the material's combiners.
        st.stageCount = 1;
        HostGfxStage& h = st.stages[0];
        h = HostGfxStage();
        h.colorSrc[0] = h.alphaSrc[0] = 0x03;  // texture 0
        h.colorSrc[1] = h.colorSrc[2] = h.alphaSrc[1] = h.alphaSrc[2] = 0x0F;
        h.colorFunc = h.alphaFunc = 0;  // replace
    }
    // The material's fog, as J3DFog::load gives it to GXSetFog.
    if (J3DPEBlock* pe = pMat->getPEBlock()) {
        if (const J3DFog* fog = pe->getFog()) {
            encodeFog(*fog, st);
        }
    }
    u32 cull = m[4];
    sCullAll = cull == 3;
    // GXCullMode (GX_CULL_FRONT 1, GX_CULL_BACK 2) reaches the hardware swapped (GXSetCullMode),
    // and the GX translation mapped the hardware bits: back faces culled is 1 (CULL_FRONT_CCW).
    st.cull = cull == 2 ? 1 : cull == 1 ? 2 : 0;
    st.blendEquation = m[5];
    st.blendSrc = m[6];
    st.blendDst = m[7];
    st.alphaTestFunc = m[8];
    st.alphaRef = m[9];
    if (hostDebugNative() == 3) {
        st.alphaTestFunc = 1;  // GPU_ALWAYS: isolate alpha-cutout materials.
    } else if (hostDebugNative() == 4) {
        st.alphaTestFunc = 5;  // GPU_LEQUAL: check the sampled alpha's polarity.
    }
    st.depthTest = m[10];
    st.depthFunc = m[11];
    st.writeMask = m[12];
    sZCompBefore = m[0x0D] != 0;
    sAlphaSplit = (m[3] & 8) != 0 && hostDebugNative() < 3;  // FLAG_ALPHA_SPLIT
    sAlphaSplitFunc = m[0x14A];
    sAlphaSplitRef = m[0x14B];
    st.dstAlphaEnable = false;
    st.dstAlpha = 0;
    // Later passes: the same textures and vertices, their own combiners, blend and depth test
    // (colour rebuilt in the frame buffer on the pixels the first pass wrote), without fog.
    sPassCount = hostDebugNative() >= 2 ? 0 : m[0x0F];
    for (u32 p = 0; p < sPassCount; p++) {
        const u8* r = owner->materials + (rd16(m + 0xC6) + p) * kMaterialSize;
        HostGfxState& ps = sPassStates[p];
        ps = st;
        loadStages(r, ps);
        ps.blendEquation = r[5];
        ps.blendSrc = r[6];
        ps.blendDst = r[7];
        ps.alphaTestFunc = r[8];
        ps.alphaRef = r[9];
        ps.depthTest = r[10];
        ps.depthFunc = r[11];
        ps.writeMask = r[12];
        ps.fogParamA = ps.fogParamB = ps.fogParamShift = ps.fogParamC = ps.fogColor = 0;
        sPassRecords[p] = r;
        sPassChannels[p] = r[2] & 1;  // lit by the GPU; its colours come from registers (mat.py)
        sPassOwnUnits[p] = memcmp(r + 0x10, m + 0x10, 12) != 0 ? 1 : 0;
    }

    RvlNativeDraw& d = sDraw;
    d.state = &st;
    d.unitCount = units;
    d.rasChannel = m[2] & 1;
    const u8* chans = m + 0xC8;  // colour 0, alpha 0, colour 1, alpha 1
    for (u32 c = 0; c < 2; c++) {
        d.colorCtrl[c] = channelControl(chans + (c * 2) * 8);
        d.alphaCtrl[c] = channelControl(chans + (c * 2 + 1) * 8);
    }
    u8 coords[3];
    for (u32 u = 0; u < 3; u++) {
        sUnitMap[u] = m[0x12 + u * 4] & 7;
        coords[u] = m[0x13 + u * 4] & 7;
    }
    fillValues(pMat, sLive);
    sBegunMaterial = pMat;
    sDeltaStages = false;
    sDeltaAlpha = -1;
    if (sDelta != nullptr) {
        applyDeltaPixel(*sDelta, st);
        applyDeltaStages(*sDelta, st, d, coords);
    }
    setupDistortion(pMat, coords);
    if (!setupUnits(m, d.unitCount, coords)) {
        return fallback(FB_TEXTURE);
    }
    if (!applyValues(sLive)) {
        return fallback(FB_TEXTURE);
    }
    sModel = n;
    return true;
}


// Fur layers (NativeFur3DS.hpp): the fur material replaces the model's once the packet is set up.
namespace {
    const RvlNativeFurMaterial* sFur;
    J3DModel* sFurLayer;  // the layer's packets are the model's, and name the model
    HostGfxState sFurState;

    void applyFur(RvlNativeDraw& d, J3DModel* pLayer) {
        const RvlNativeFurMaterial& f = *sFur;
        sFurState = f.state;
        if (f.fog != nullptr) {
            encodeFog(*f.fog, sFurState);
        }
        d.state = &sFurState;
        d.unitCount = 2;
        d.distort = RVL_DISTORT_NONE;
        d.captureMask = 0;
        d.rasChannel = 0;
        d.colorCtrl[0] = f.colorCtrl;
        d.alphaCtrl[0] = f.alphaCtrl;
        d.colorCtrl[1] = d.alphaCtrl[1] = 0;
        d.matColor[0] = f.matColor;
        d.ambColor[0] = f.ambColor;
        d.ownAmbient = true;
        for (u32 u = 0; u < 3; u++) {
            RvlNativeTexGen& g = d.gen[u];
            g = RvlNativeTexGen();
            sUnitRow[u] = 0xFF;
            sUnitCoord[u] = 0;
            sUnitStq[u] = sUnitAbc1[u] = 0;
            sGenTexMtx[u] = -1;
            if (u < 2) {
                // GX_TG_MTX2x4 of texture coordinate 0 with the body's or the fur's matrix.
                g.generated = true;
                g.row = 5;
                g.matrix = &f.texMtx[u][0][0];
                sUnitRow[u] = 5;
            }
        }
        sUploadKey = 0x46555200u;  // "FUR": the layer's own vertices, uploaded each frame
        sUploadKeyExact = true;
        sCpuPositions = true;
        sSkinVertexBuffer = pLayer->getVertexBuffer();
    }

    // The Wii's lights are GX state: LightFunction::loadActorLightInfo loads one, and every
    // packet drawn after it (a group's map parts, the packets sorted after the actor that loaded
    // it) is lit by it until the next load.
    const ActorLightInfo* sLoadedLight;
}  // namespace

void rvlNativeLoadLight(const ActorLightInfo* pInfo) {
    sLoadedLight = pInfo;
}

const ActorLightInfo* rvlNativeLoadedLight() {
    return sLoadedLight;
}

// GX_LIGHT4, the point light (LightFunction::loadPointLightInfo): its view-space position at load
// time, as GXInitLightPos took it, its colour, and GXInitLightDistAttn's coefficients (the GX SDK's
// formula, src/RVL_SDK/gx/GXLight.c); no spot (GXInitLightSpot GX_SP_OFF: angular 1, 0, 0).
namespace {
    RvlNativePointLight sPointLight;
}

void rvlNativeLoadPointLight(const PointLightInfo* pInfo) {
    RvlNativePointLight& l = sPointLight;
    TVec3f pos(pInfo->mPos);
    MR::getCameraViewMtx().mult(pos, pos);
    l.pos[0] = pos.x;
    l.pos[1] = pos.y;
    l.pos[2] = pos.z;
    l.color[0] = pInfo->mColor.r;
    l.color[1] = pInfo->mColor.g;
    l.color[2] = pInfo->mColor.b;
    l.color[3] = pInfo->mColor.a;
    f32 dist = pInfo->mRefDistance, br = pInfo->mRefBrightness;
    u32 fn = pInfo->mDistAttnFn;
    if (dist < 0.0f || br <= 0.0f || br >= 1.0f) {
        fn = GX_DA_OFF;
    }
    l.k[0] = 1.0f;
    l.k[1] = l.k[2] = 0.0f;
    if (fn == GX_DA_GENTLE) {
        l.k[1] = (1.0f - br) / (br * dist);
    } else if (fn == GX_DA_MEDIUM) {
        l.k[1] = 0.5f * (1.0f - br) / (br * dist);
        l.k[2] = 0.5f * (1.0f - br) / (br * dist * dist);
    } else if (fn == GX_DA_STEEP) {
        l.k[2] = (1.0f - br) / (br * dist * dist);
    }
    l.loaded = true;
}

const RvlNativePointLight* rvlNativePointLight() {
    return sPointLight.loaded ? &sPointLight : nullptr;
}

namespace {
    // sZCompBefore: the depth-only draw of a group, and d.state set to the material on the depths
    // it left (GPU_EQUAL, no depth write) for the draw after; nothing when the material has no
    // alpha test or writes no depth (the order does not matter then). False when the draw failed.
    bool depthFirst(RvlNativeDraw& d) {
        const HostGfxState& st = sState;
        const u8 kAlways = 1, kEqual = 2, kWriteDepth = 0x10;  // GPU_ALWAYS, GPU_EQUAL, GPU_WRITE_DEPTH
        if (!sZCompBefore || sFur != nullptr || d.state != &sState || st.alphaTestFunc == kAlways || !st.depthTest ||
            (st.writeMask & kWriteDepth) == 0) {
            return true;
        }
        sDepthOnlyState = st;
        sDepthOnlyState.writeMask = kWriteDepth;
        sDepthOnlyState.alphaTestFunc = kAlways;
        d.state = &sDepthOnlyState;
        const bool ok = rvlNativeDrawSubmit(d);
        sDepthEqualState = st;
        sDepthEqualState.depthFunc = kEqual;
        sDepthEqualState.writeMask = static_cast< u8 >(st.writeMask & ~kWriteDepth);
        d.state = ok ? &sDepthEqualState : &sState;
        return ok;
    }
}  // namespace

bool rvlNativePacket(J3DShapePacket* pPacket, const ActorLightInfo* pActorLight, bool materialAmbient) {
    if (pPacket == nullptr || pPacket->mpShape == nullptr || pPacket->mpModel == nullptr ||
        pPacket->getBaseMtxPtr() == nullptr || sModel == nullptr ||
        sModel->model != pPacket->mpModel->getModelData() || !isLive(*sModel)) {
        return fallback(FB_USER_DATA);
    }
    J3DShape* shape = pPacket->mpShape;
    if (shape->getMaterial() == nullptr) {
        return fallback(FB_USER_DATA);
    }
    NativeModel& n = *sModel;
    u32 index = shape->getIndex();
    if (index >= n.shapeCount) {
        return fallbackShape(SHAPE_BAD_INDEX);
    }
    const N3DShape& s = n.shapes[index];
    if ((sSkinning || sCpuPositions) && !(s.mask & ATTR_SOURCE_POS)) {
        return fallback(FB_MODEL_FLAGS);
    }
    if (s.mode > 3) {
        return fallbackShape(SHAPE_BAD_INDEX);
    }
    if (sCullAll || pPacket->checkFlag(J3DShpFlag_Hidden)) {
        return true;
    }
    bool billboard = s.mode == 1 || s.mode == 2;
    if (billboard) {
        gGXProfile.nativeBillboards++;
    }
    // A lent material (findMaterialOwner) has no snapshot under this model: its live values.
    const MatValues* values =
        shape->getMaterial() == sBegunMaterial ? findSnapshot(snapshotOwner(pPacket->mpModel), shape->getMaterial()->getIndex()) : nullptr;
    // A lower detail model (MR::syncMaterialAnimation) draws on the Wii with the whole display list
    // of the model it follows, not only its animated values: without a snapshot of that model's
    // material this frame (it takes one only while an animation changes it), the colours that
    // instance had last (keepOwnerColors), else its material's, with this model's own texture
    // numbers and matrices. Its own file's differ (RosettaMiddle's hair and face: A1 0, where
    // Rosetta's is 255; each Luma's colour; CIA 0.19.10).
    static MatValues sOwnerValues;
    const J3DModel* owner = snapshotOwner(pPacket->mpModel);
    const u16 materialIndex = shape->getMaterial()->getIndex();
    // The followed model itself, without a fresh snapshot: its own last colours too.
    if (values == nullptr && (owner != pPacket->mpModel || isOwnerModel(owner)) && shape->getMaterial() == sBegunMaterial &&
        owner->getModelData() != nullptr && materialIndex < owner->getModelData()->getMaterialNum()) {
        sOwnerValues = sLive;
        if (const OwnerColors* kept = findOwnerColors(owner, materialIndex)) {
            memcpy(sOwnerValues.tev, kept->tev, sizeof(kept->tev));
            memcpy(sOwnerValues.konst, kept->konst, sizeof(kept->konst));
            memcpy(sOwnerValues.matColor, kept->matColor, sizeof(kept->matColor));
            memcpy(sOwnerValues.ambColor, kept->ambColor, sizeof(kept->ambColor));
            sOwnerValues.tevValid = kept->tevValid;
            sOwnerValues.konstValid = kept->konstValid;
            sOwnerValues.colorValid = kept->colorValid;
        } else {
            MatValues own;
            fillValues(owner->getModelData()->getMaterialNodePointer(materialIndex), own);
            memcpy(sOwnerValues.tev, own.tev, sizeof(own.tev));
            memcpy(sOwnerValues.konst, own.konst, sizeof(own.konst));
            memcpy(sOwnerValues.matColor, own.matColor, sizeof(own.matColor));
            memcpy(sOwnerValues.ambColor, own.ambColor, sizeof(own.ambColor));
            sOwnerValues.tevValid = own.tevValid;
            sOwnerValues.konstValid = own.konstValid;
            sOwnerValues.colorValid = own.colorValid;
        }
        values = &sOwnerValues;
        sApplied = nullptr;  // the same record holds another material's values each time
    }
    if (values == nullptr) {
        values = &sLive;
    }
    if (values != sApplied && !applyValues(*values)) {
        applyValues(sLive);
        return fallback(FB_TEXTURE);
    }

    // This direct path does not invoke legacy display lists or load state through GX.
    u64 setupStart = gGXProfileOn ? hostGetTick() : 0;
    RvlNativeDraw& d = sDraw;
    d.normals = (s.mask & ATTR_NRM) != 0;
    d.actorLight = pActorLight;
    d.state = &sState;
    d.ownAmbient = materialAmbient;
    if (sFur != nullptr) {
        applyFur(d, sFurLayer);
    }
    // A channel may also name lights an ActorLightInfo does not load (3-7: the coin's specular
    // light, point lights): they are left out (setLightList) and the packet is drawn. Refusing it
    // made parts of the scene vanish and come back as the light state changed (CIA 0.11.5).
    if (pActorLight != nullptr && gGXProfileOn) {
        for (u32 c = 0; c < 2; c++) {
            const u32 colorMask = ((d.colorCtrl[c] >> 2) & 0xF) | (((d.colorCtrl[c] >> 11) & 0xF) << 4);
            const u32 alphaMask = ((d.alphaCtrl[c] >> 2) & 0xF) | (((d.alphaCtrl[c] >> 11) & 0xF) << 4);
            if ((colorMask & ~0x3u) != 0 || (alphaMask & ~0x7u) != 0) {
                gGXProfile.nativeFallbacks[FB_LIGHTS]++;  // still counted, for the log
                break;
            }
        }
    }
    J3DMtxBuffer* buffer = pPacket->mpMtxBuffer;
    J3DModel* model = pPacket->mpModel;
    J3DModelData* modelData = model->getModelData();
    if (modelData == nullptr) {
        return fallback(FB_USER_DATA);
    }
    // Models loaded with J3DMdlDataFlag_ConcatView (most of the game's) make their view-space
    // matrices at draw time: the packet's base (view) matrix times the joint's animation matrix
    // (J3DShapeMtxConcatView::loadMtxConcatView_PNGP); the others read the matrix buffer.
    bool concatView = (modelData->getFlag() & 0x10) != 0;
    J3DTexMtxObj* texMtxObj = pPacket->getTexMtxObj();
    // ShapePacketUserData replaces the per-joint texture-matrix setup that the Wii
    // display list performed. The same CPU calculation used by concat-view packets
    // represents its POS/NRM and environment-map generators when an object exists.
    Mtx userDataTexMtx[8];
    Mtx44 userDataEffectMtx[8];
    J3DTexMtxObj userDataTexMtxObj;
    if (sHasUserData && texMtxObj == nullptr) {
        u32 texGenCount = shape->getMaterial()->getTexGenNum();
        if (texGenCount > 8) {
            return fallback(FB_USER_DATA);
        }
        for (u32 i = 0; i < texGenCount; i++) {
            if (sLive.texMtxValid & (1 << i)) {
                memcpy(userDataTexMtx[i], sLive.texMtx[i], sizeof(Mtx));
            } else {
                PSMTXIdentity(userDataTexMtx[i]);
            }
            memset(userDataEffectMtx[i], 0, sizeof(Mtx44));
            userDataEffectMtx[i][0][0] = userDataEffectMtx[i][1][1] = userDataEffectMtx[i][2][2] = userDataEffectMtx[i][3][3] = 1.0f;
        }
        userDataTexMtxObj.mpTexMtx = userDataTexMtx;
        userDataTexMtxObj.mpEffectMtx = userDataEffectMtx;
        userDataTexMtxObj.mTexMtxNum = static_cast< u16 >(texGenCount);
        texMtxObj = &userDataTexMtxObj;
    }
    sDeferredTexMtx = texMtxObj != nullptr && (concatView || sHasUserData);
    sDeferredTexInvalid = false;
    for (u32 u = 0; u < d.unitCount; u++) {
        d.gen[u].direct = sDeferredTexMtx;
    }
    if (sDeferredTexMtx) {
        if (!(s.mask & ATTR_TEXMTX) || texMtxObj->getNumTexMtx() > 8) {
            return fallback(FB_USER_DATA);
        }
        sDeferredTexGenCount = texMtxObj->getNumTexMtx();
        for (u32 u = 0; u < d.unitCount; u++) {
            if (!d.gen[u].generated || sUnitCoord[u] >= sDeferredTexGenCount) {
                return fallback(FB_USER_DATA);
            }
        }
        J3DDifferedTexMtx::sTexGenBlock = shape->getMaterial()->getTexGenBlock();
        J3DDifferedTexMtx::sTexMtxObj = texMtxObj;
    }
    static Mtx sMatrices[10];
    const bool noMatrix = shape->checkFlag(J3DShpFlag_NoMtx);
    const N3DGroup* groups = n.groups + n.firstGroup[index];
    bool submitted = false;
    for (u32 g = 0; g < s.groupCount; g++) {
        const N3DGroup& grp = groups[g];
        if (grp.matrixCount > 10) {
            return !submitted ? fallbackShape(SHAPE_MISSING_MATRIX_TABLE) : true;
        }
        d.matrixCount = grp.matrixCount;
        for (u32 k = 0; k < d.matrixCount; k++) {
            u16 drw = grp.matrices[k] != 0xFFFF ? grp.matrices[k] : grp.matrices[0];
            if (sOverrideMatrix != nullptr) {
                d.matrices[k] = sOverrideMatrix;
            } else if (sSkinning) {
                d.matrices[k] = *pPacket->getBaseMtxPtr();
                PSMTXCopy(d.matrices[k], sMatrices[k]);
            } else if (noMatrix) {
                d.matrices[k] = *pPacket->getBaseMtxPtr();
                PSMTXCopy(d.matrices[k], sMatrices[k]);
            } else if (drw == 0xFFFF) {
                d.matrices[k] = kIdentity;
            } else if (concatView || billboard) {
                MtxPtr table = modelData->getDrawMtxFlag(drw) ? model->getWeightAnmMtx(0)
                                                               : buffer != nullptr ? buffer->getUserAnmMtx(0) : nullptr;
                if (table == nullptr) {
                    return fallback(FB_USER_DATA);
                }
                MtxPtr joint = reinterpret_cast< Mtx* >(table)[modelData->getDrawMtxIndex(drw)];
                if (billboard) {
                    PSMTXConcat(j3dSys.getViewMtx(), joint, sMatrices[k]);
                    if (s.mode == 1) J3DCalcBBoardMtx(sMatrices[k]);
                    else J3DCalcYBBoardMtx(sMatrices[k]);
                } else {
                    PSMTXConcat(*pPacket->getBaseMtxPtr(), joint, sMatrices[k]);
                }
                d.matrices[k] = sMatrices[k];
            } else {
                if (buffer == nullptr || buffer->getDrawMtx(drw) == nullptr) {
                    return fallback(FB_USER_DATA);
                }
                d.matrices[k] = *buffer->getDrawMtx(drw);
                PSMTXCopy(d.matrices[k], sMatrices[k]);
            }
        }
        if (sDeferredTexMtx) {
            sDeferredMatrixCount = d.matrixCount;
            for (u32 k = 0; k < d.matrixCount; k++) {
                J3DDifferedTexMtx::loadExecute(d.matrices[k], sDeferredTexMatrices[k]);
            }
        }
        Upload* up;
        sDeferredTexInvalid = false;
        const u64 uploadStart = gGXProfileOn ? hostGetTick() : 0;
        const bool uploaded = uploadGroup(n, index, g, up);
        if (gGXProfileOn) {
            sUploadTicks += hostGetTick() - uploadStart;
        }
        if (!uploaded) {
            return fallback(sDeferredTexInvalid ? FB_USER_DATA : FB_UPLOAD);
        }
        if (!submitted) {
            submitted = true;
        }
        d.first = up->first;
        d.indices = up->indices;
        d.indexCount = grp.indexCount;
        sSubmittedTriangles += grp.indexCount / 3;
        const GroupBounds* bounds = boundsOf(up->bounds);
        d.spheres = bounds != nullptr ? bounds->spheres : nullptr;
        d.slotMask = bounds != nullptr ? bounds->slotMask : 0;
        d.chunks = bounds != nullptr && bounds->chunkCount != 0 ? &sChunks[bounds->chunkFirst] : nullptr;
        d.chunkCount = bounds != nullptr ? bounds->chunkCount : 0;
        u64 submitStart = gGXProfileOn ? hostGetTick() : 0;
        if (!depthFirst(d)) {
            return fallback(FB_SUBMIT);
        }
        bool drawn = rvlNativeDrawSubmit(d);
        if (drawn && sAlphaSplit && sFur == nullptr) {
            sAlphaSplitState = *d.state;
            sAlphaSplitState.alphaTestFunc = sAlphaSplitFunc;
            sAlphaSplitState.alphaRef = sAlphaSplitRef;
            d.state = &sAlphaSplitState;
            drawn = rvlNativeDrawSubmit(d);
        }
        if (d.distort == RVL_DISTORT_TWO_DRAWS) {
            for (d.distortDraw = 1; drawn && d.distortDraw < d.distortSamples; d.distortDraw++) {
                drawn = rvlNativeDrawSubmit(d);
            }
            d.distortDraw = 0;
        }
        d.state = &sState;
        if (!drawn) {
            return fallback(FB_SUBMIT);
        }
        for (u32 p = 0; p < sPassCount && sFur == nullptr; p++) {
            const u32 channel = d.rasChannel;
            d.state = &sPassStates[p];
            d.rasChannel = sPassChannels[p];
            const bool passSubmitted = sPassOwnUnits[p] != 0 ? submitOwnUnits(n, index, g, p, s) : rvlNativeDrawSubmit(d);
            d.state = &sState;
            d.rasChannel = channel;
            if (!passSubmitted) {
                return fallback(FB_SUBMIT);
            }
        }
        if (gGXProfileOn) {
            sNativeSubmitTicks += hostGetTick() - submitStart;
            sNativeSubmitCalls++;
        }
    }
    if (gGXProfileOn) {
        sNativeSetupTicks += hostGetTick() - setupStart;
    }
    return true;
}

bool rvlNativeDrawFurLayer(J3DModel* pLayer, u32 shapeIndex, const RvlNativeFurMaterial& rMaterial, const ActorLightInfo* pActorLight) {
    if (pLayer == nullptr || pLayer->getModelData() == nullptr || shapeIndex >= pLayer->getModelData()->getShapeNum()) {
        return false;
    }
    J3DShapePacket* packet = pLayer->getShapePacket(static_cast< u16 >(shapeIndex));
    if (packet == nullptr || packet->mpShape == nullptr || packet->mpShape->getMaterial() == nullptr) {
        return false;
    }
    sFur = &rMaterial;
    sFurLayer = pLayer;
    bool submitted = rvlNativeBegin(packet->mpShape->getMaterial(), packet, false) && rvlNativePacket(packet, pActorLight);
    sFur = nullptr;
    sCpuPositions = false;
    return submitted;
}

bool rvlNativeMaterialState(J3DMaterial* pMaterial, J3DShapePacket* pPacket, HostGfxState& rState, f32 (*pTexMtx)[3][4],
                            u32& rRasterColor) {
    if (pMaterial == nullptr || pPacket == nullptr || !rvlNativeBegin(pMaterial, pPacket, false)) {
        return false;
    }
    rState = sState;
    const RvlNativeDraw& d = sDraw;
    for (u32 u = 0; u < 3; u++) {
        const f32* m = u < d.unitCount ? d.gen[u].matrix : nullptr;
        for (u32 r = 0; r < 3; r++) {
            for (u32 c = 0; c < 4; c++) {
                pTexMtx[u][r][c] = m != nullptr ? m[r * 4 + c] : (r == c ? 1.0f : 0.0f);
            }
        }
    }
    // Channel control bit 0: the material colour from the vertex (1) or the register (0).
    const u32 channel = d.rasChannel & 1;
    rRasterColor = (d.colorCtrl[channel] & 1) ? 0xFFFFFFFFu : d.matColor[channel];
    return true;
}

bool rvlNativeDrawSimpleModel(J3DModelData* pModelData, const f32 (*pViewMtx)[4], const HostGfxState& rState, u32 color) {
    return rvlNativeDrawSimpleModelSlots(pModelData, pViewMtx, nullptr, rState, color);
}

bool rvlNativeDrawSimpleModelSlots(J3DModelData* pModelData, const f32 (*pViewMtx)[4], const f32 (*const* pSlots)[4],
                                   const HostGfxState& rState, u32 color) {
    NativeModel* n = pModelData != nullptr ? findModel(pModelData) : nullptr;
    if (n == nullptr || !isLive(*n)) {
        return false;
    }
    sSkinning = false;
    sCpuPositions = false;
    sDeferredTexMtx = false;
    sDraw.rasChannel = 0;
    for (u32 u = 0; u < 3; u++) {
        sUnitRow[u] = 0xFF;
        sUnitCoord[u] = 0;
        sUnitStq[u] = sUnitAbc1[u] = 0;
        sGenTexMtx[u] = -1;
    }
    sUploadKey = 0x53494D00u;  // "SIM": positions only
    sUploadKeyExact = true;
    RvlNativeDraw d = RvlNativeDraw();
    d.state = &rState;
    d.matColor[0] = d.matColor[1] = color;
    bool submitted = false;
    for (u32 shape = 0; shape < n->shapeCount; shape++) {
        const N3DShape& s = n->shapes[shape];
        for (u32 g = 0; g < s.groupCount; g++) {
            const N3DGroup& grp = n->groups[n->firstGroup[shape] + g];
            Upload* up;
            sDeferredTexInvalid = false;
            if (grp.matrixCount > 10 || !uploadGroup(*n, shape, g, up)) {
                continue;
            }
            d.matrixCount = grp.matrixCount;
            for (u32 k = 0; k < d.matrixCount; k++) {
                d.matrices[k] = pSlots != nullptr && pSlots[k] != nullptr ? pSlots[k] : pViewMtx;
            }
            d.first = up->first;
            d.indices = up->indices;
            d.indexCount = grp.indexCount;
            const GroupBounds* bounds = boundsOf(up->bounds);
            d.spheres = bounds != nullptr ? bounds->spheres : nullptr;
            d.slotMask = bounds != nullptr ? bounds->slotMask : 0;
            d.chunks = bounds != nullptr && bounds->chunkCount != 0 ? &sChunks[bounds->chunkFirst] : nullptr;
            d.chunkCount = bounds != nullptr ? bounds->chunkCount : 0;
            submitted |= rvlNativeDrawSubmit(d);
        }
    }
    return submitted;
}

// Far models ------------------------------------------------------------------------------------
//
// An instance whose far model's sphere is under kFarPixels pixels of radius on screen draws as
// that one draw (far.py), in place of all its packets. The instance decides at its first packet of
// the frame (DrawBufferShapeDrawer::draw), lit by the light loaded for that packet, and its other
// packets then follow that decision. Back to the full model past kNearPixels.

namespace {
    const f32 kFarPixels = 16.0f;
    const f32 kNearPixels = 20.0f;

    struct FarInstance {
        const J3DModel* model;
        u32 pass;
        bool far;
    };
    FarInstance sFarInstances[512];  // power of two
    u32 sFarDrawn, sFarPackets, sFarUploads;
    // Draws of a DrawBuffer (rvlNativeFarPrepare): an instance decides once in each, so that a
    // buffer drawn twice in a frame draws its far instances both times.
    u32 sFarPass = 1;

    FarInstance& farInstance(const J3DModel* model) {
        return sFarInstances[(reinterpret_cast< u32 >(model) * 2654435761u >> 23) & 511];
    }

    // The far model's vertices in the persistent arena, read from its file once per arena epoch.
    bool uploadFar(NativeModel& n) {
        Upload& up = n.farUpload;
        const u32 epoch = hostGfxArenaEpoch();
        if (up.indices != nullptr && up.epoch == epoch) {
            return true;
        }
        up.indices = nullptr;
        char path[384];
        if (!farPath(n, path)) {
            return false;
        }
        void* file = hostFileOpen(path);
        if (file == nullptr) {
            return false;
        }
        const u32 vertexBytes = n.farVertexCount * sizeof(FarVertex);
        u8* data = static_cast< u8* >(malloc(vertexBytes + n.farIndexCount * 2));
        bool ok = data != nullptr && readAt(file, data, vertexBytes + n.farIndexCount * 2, sizeof(FarHeader));
        hostFileClose(file);
        HostGfxModelVertex* out = nullptr;
        const u32 first = ok ? hostGfxArenaAlloc(n.farVertexCount, 1, &out) : HOST_GFX_NO_ARENA;
        u16* indices = first != HOST_GFX_NO_ARENA ? hostGfxIndexAlloc(n.farIndexCount) : nullptr;
        const u32 base = first % 65536;
        ok = indices != nullptr && n.farVertexCount <= 65536 - base;
        for (u32 i = 0; ok && i < n.farVertexCount; i++) {
            FarVertex v;
            memcpy(&v, data + i * sizeof(FarVertex), sizeof(v));
            HostGfxModelVertex& o = out[i];
            memset(&o, 0, sizeof(o));
            o.x = v.pos[0] * n.farPosScale;
            o.y = v.pos[1] * n.farPosScale;
            o.z = v.pos[2] * n.farPosScale;
            o.nx = v.nrm[0] / 127.0f;
            o.ny = v.nrm[1] / 127.0f;
            o.nz = v.nrm[2] / 127.0f;
            memcpy(o.rgba, v.rgba, 4);
            for (u32 u = 0; u < 3; u++) {
                o.tex[u][2] = 1.0f;
            }
        }
        const u16* src = ok ? reinterpret_cast< const u16* >(data + vertexBytes) : nullptr;
        for (u32 i = 0; ok && i < n.farIndexCount; i++) {
            u16 k;
            memcpy(&k, src + i, 2);
            ok = k < n.farVertexCount;
            indices[i] = static_cast< u16 >(base + k);
        }
        free(data);
        if (!ok) {
            n.farVertexCount = 0;  // no far model for it any more
            return false;
        }
        up.epoch = epoch;
        up.first = first;
        up.indices = indices;
        sFarUploads++;
        return true;
    }
}  // namespace

void rvlNativeFarPrepare(J3DModel* pModel) {
    sFarPass++;
    J3DModelData* data = pModel != nullptr ? pModel->getModelData() : nullptr;
    NativeModel* n = data != nullptr ? findModel(data) : nullptr;
    if (n == nullptr || n->farVertexCount == 0 || n->farPrepared != 0 || sDelta != nullptr) {
        return;
    }
    // The main material's state, as its own draws take it: depth mode, fog, lighting channel.
    J3DMaterial* material = data->getMaterialNodePointer(n->farMaterial);
    J3DShape* shape = material != nullptr ? material->getShape() : nullptr;
    J3DShapePacket* packet = shape != nullptr ? pModel->getShapePacket(shape->getIndex()) : nullptr;
    if (packet == nullptr || !rvlNativeBegin(material, packet, false)) {
        n->farPrepared = 2;
        return;
    }
    HostGfxState& st = n->farState;
    st = sState;
    st.textures[0] = st.textures[1] = st.textures[2] = nullptr;
    st.stageCount = 1;
    st.stages[0] = HostGfxStage();  // the lit vertex colour (GPU_PRIMARY_COLOR, replace)
    st.alphaTestFunc = 1;           // GPU_ALWAYS
    st.alphaRef = 0;
    st.bufferUpdateRgb = st.bufferUpdateAlpha = 0;
    if (n->farCullNone) {
        st.cull = 0;
    }
    const u32 channel = sDraw.rasChannel & 1;
    // The material colour from the vertex (bit 0), the ambient from the register (bit 6 clear).
    n->farColorCtrl = (sDraw.colorCtrl[channel] | 1u) & ~0x40u;
    n->farAmbColor = sDraw.ambColor[channel];
    n->farPrepared = 1;
}

bool rvlNativeFarPacket(J3DShapePacket* pPacket, const ActorLightInfo* pActorLight) {
    J3DModel* model = pPacket != nullptr ? pPacket->mpModel : nullptr;
    J3DModelData* data = model != nullptr ? model->getModelData() : nullptr;
    NativeModel* n = data != nullptr ? findModel(data) : nullptr;
    if (n == nullptr || n->farVertexCount == 0 || n->farPrepared != 1) {
        return false;
    }
    FarInstance& e = farInstance(model);
    if (e.model == model && e.pass == sFarPass) {
        sFarPackets += e.far ? 1 : 0;
        return e.far;
    }
    const bool wasFar = e.model == model && e.far;
    e.model = model;
    e.pass = sFarPass;
    e.far = false;
    const f32 projection = rvlNativeProjectionScaleY();
    J3DMtxBuffer* buffer = pPacket->mpMtxBuffer;
    if (projection <= 0.0f || buffer == nullptr || pPacket->getBaseMtxPtr() == nullptr) {
        return false;
    }
    // A hidden part would show in the far model.
    for (u16 i = 0; i < data->getShapeNum(); i++) {
        J3DShapePacket* part = model->getShapePacket(i);
        if (part == nullptr || part->checkFlag(J3DShpFlag_Hidden) || (data->getShapeNodePointer(i)->mFlags & 1) != 0) {
            return false;
        }
    }
    // The view matrix of the far model's joint, as rvlNativePacket makes a packet's.
    static Mtx sFarMatrix;
    if ((data->getFlag() & 0x10) != 0) {
        MtxPtr table = buffer->getUserAnmMtx(0);
        if (table == nullptr) {
            return false;
        }
        PSMTXConcat(*pPacket->getBaseMtxPtr(), reinterpret_cast< Mtx* >(table)[data->getDrawMtxIndex(n->farDrw)], sFarMatrix);
    } else {
        if (buffer->getDrawMtx(n->farDrw) == nullptr) {
            return false;
        }
        PSMTXCopy(*buffer->getDrawMtx(n->farDrw), sFarMatrix);
    }
    const f32* c = n->farSphere[0];
    const f32 depth = -(sFarMatrix[2][0] * c[0] + sFarMatrix[2][1] * c[1] + sFarMatrix[2][2] * c[2] + sFarMatrix[2][3]);
    const f32 scale = std::sqrt(sFarMatrix[0][0] * sFarMatrix[0][0] + sFarMatrix[1][0] * sFarMatrix[1][0] +
                                sFarMatrix[2][0] * sFarMatrix[2][0]);
    if (!(depth > 0.0f)) {
        return false;
    }
    // Pixels of the top screen's 240 lines.
    const f32 pixels = c[3] * scale * projection * 120.0f / depth;
    if (!(pixels < (wasFar ? kNearPixels : kFarPixels)) || !uploadFar(*n)) {
        return false;
    }
    RvlNativeDraw d = RvlNativeDraw();
    d.state = &n->farState;
    d.unitCount = 0;
    d.rasChannel = 0;
    d.colorCtrl[0] = n->farColorCtrl;
    d.alphaCtrl[0] = 0;  // unlit, from the register: opaque
    d.matColor[0] = 0xFFFFFFFFu;
    d.ambColor[0] = n->farAmbColor;
    d.matrices[0] = sFarMatrix;
    d.matrixCount = 1;
    d.normals = true;
    d.actorLight = pActorLight;
    d.first = n->farUpload.first;
    d.indices = n->farUpload.indices;
    d.indexCount = n->farIndexCount;
    d.spheres = n->farSphere;
    d.slotMask = 1;
    e.far = rvlNativeDrawSubmit(d);
    if (e.far) {
        sFarDrawn++;
        sFarPackets++;
    }
    return e.far;
}

void rvl3DSLogTextureStats();  // jut_texture.cpp

void rvlNativeLogFrame() {
    rvl3DSLogTextureStats();
    u64 ms = hostGetTickFrequency() / 1000;
    hostLog("native timing: packet setup %lu ms, Citro3D submit %lu ms/%lu calls (overlaps packet setup)\n",
            static_cast< u32 >(sNativeSetupTicks / ms), static_cast< u32 >(sNativeSubmitTicks / ms), sNativeSubmitCalls);
    sNativeSetupTicks = sNativeSubmitTicks = 0;
    sNativeSubmitCalls = 0;
    hostLog("native uploads: %lu persistent, %lu transient groups, %lu vertices, %lu ms; %lu for a second key, %lu replacing a "
            "current key, %lu draws on another key's vertices\n",
            sPersistentUploads, sTransientUploads, sUploadedVertices, static_cast< u32 >(sUploadTicks / ms), sSecondKeyUploads,
            sReplacedKeyUploads, sCompatibleUploads);
    sPersistentUploads = sTransientUploads = sUploadedVertices = 0;
    sSecondKeyUploads = sReplacedKeyUploads = sCompatibleUploads = 0;
    sUploadTicks = 0;
    hostLog("native upload files: %lu read whole, %lu groups from memory, %lu KB kept\n", sUploadFileReads, sUploadFileHits,
            sUploadCacheBytes / 1024);
    sUploadFileReads = sUploadFileHits = 0;
    hostLog("native upload pacing: %lu ms reading whole files, %lu triangles asked of the GPU\n",
            static_cast< u32 >(sUploadFileTicks / ms), sSubmittedTriangles);
    sUploadFileTicks = 0;
    sSubmittedTriangles = 0;
    hostLog("native: %lu draws, skipped unsupported packets: no file %lu, user data %lu, model flags %lu, texture %lu, shape %lu, upload %lu, lights %lu, material %lu, submit %lu "
            "(models with a file: %lu, without: %lu)\n",
            gGXProfile.nativeDraws, gGXProfile.nativeFallbacks[FB_NO_FILE], gGXProfile.nativeFallbacks[FB_USER_DATA],
            gGXProfile.nativeFallbacks[FB_MODEL_FLAGS], gGXProfile.nativeFallbacks[FB_TEXTURE], gGXProfile.nativeFallbacks[FB_SHAPE],
            gGXProfile.nativeFallbacks[FB_UPLOAD], gGXProfile.nativeFallbacks[FB_LIGHTS], gGXProfile.nativeFallbacks[FB_MATERIAL],
            gGXProfile.nativeFallbacks[FB_SUBMIT],
            sLoaded, sMissing);
    hostLog("native shape fallbacks: bad index %lu, no matrix %lu, missing matrix table %lu\n",
            gGXProfile.nativeShapeFallbacks[SHAPE_BAD_INDEX], gGXProfile.nativeShapeFallbacks[SHAPE_NO_MATRIX],
            gGXProfile.nativeShapeFallbacks[SHAPE_MISSING_MATRIX_TABLE]);
    hostLog("native billboard packets: %lu\n", gGXProfile.nativeBillboards);
    u32 farModels = 0;
    for (u32 i = 0; i < sModelCount; i++) {
        farModels += isLive(sModels[i]) && sModels[i].farVertexCount != 0 ? 1 : 0;
    }
    hostLog("native far: %lu instances drawn far (%lu packets replaced), %lu uploads, %lu models with a far model\n", sFarDrawn,
            sFarPackets, sFarUploads, farModels);
    sFarDrawn = sFarPackets = sFarUploads = 0;
    hostLog("native snapshots: %lu found no room (of %lu places)\n", sSnapshotMisses, kSnapshots);
    sSnapshotMisses = 0;
    extern u32 gRvl3DSLodShown[4];  // LodCtrl.cpp
    hostLog("LOD models (instances x loops): high %lu, middle %lu, low %lu, hidden %lu\n", gRvl3DSLodShown[0], gRvl3DSLodShown[1],
            gRvl3DSLodShown[2], gRvl3DSLodShown[3]);
    memset(gRvl3DSLodShown, 0, sizeof(gRvl3DSLodShown));
    hostLog("native user data: proj %lu, env %lu, texgen source slots POS %lu, NRM %lu, other %lu\n",
            gGXProfile.nativeUserDataProj, gGXProfile.nativeUserDataEnv, gGXProfile.nativeUserDataTexGenSrc[0],
            gGXProfile.nativeUserDataTexGenSrc[1], gGXProfile.nativeUserDataTexGenSrc[2]);
}
