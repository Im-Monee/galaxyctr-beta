// RVLFaceLib on the 3DS: the Miis of Mii Maker in place of the Wii's Mii Channel.
//
// The official database is Mii Maker's CFL_DB.dat (shared extdata 0xF000000B), read once: the
// file select's Mii page lists its Miis as the Wii's lists the Mii Channel's. Heads are drawn
// from the console's CFL_Res.dat (below), icons by a small CPU rasteriser from the same parts.
//
#include "Game/Util/DirectDraw.hpp"
#include "GXToCitro3D.hpp"
#include "JUTTexture3DS.hpp"
#include "host.h"
#include "rvl_internal.h"
#include <math.h>
#include <RVLFaceLib.h>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <revolution.h>
#include <string.h>

extern "C" {

namespace {

const u32 kMiiSize = 0x5c;  // a MiiData record (libctru mii.h)

// Mii Maker's database: the records of its 100 slots, loaded once.
u8 sDatabase[RFL_DB_CHAR_MAX][kMiiSize];
bool sDatabaseValid[RFL_DB_CHAR_MAX];
bool sDatabaseLoaded;
// RFLDataSource_Default: a plain Mii (the file select's placeholder face).
u8 sDefaultMii[kMiiSize] = {3};
// The record the helpers below read (miiBits and the colour lookups).
const u8* sSelectedMii = sDefaultMii;

struct MiiModelState {
    RFLCharModel* model;
    Mtx matrix;
    RFLExpression expression;
    bool initialized;
    u8 mii[kMiiSize];
};

MiiModelState sModels[32];

MiiModelState* findModelState(const RFLCharModel* model) {
    if (model == nullptr) {
        return nullptr;
    }
    for (u32 i = 0; i < sizeof(sModels) / sizeof(sModels[0]); i++) {
        if (sModels[i].model == model) {
            return &sModels[i];
        }
    }
    return nullptr;
}

MiiModelState* allocateModelState(RFLCharModel* model) {
    MiiModelState* state = findModelState(model);
    if (state != nullptr) {
        return state;
    }
    for (u32 i = 0; i < sizeof(sModels) / sizeof(sModels[0]); i++) {
        if (sModels[i].model == nullptr) {
            sModels[i].model = model;
            return &sModels[i];
        }
    }
    return nullptr;
}

u16 readLE16(const u8* p) {
    return static_cast< u16 >(p[0] | (p[1] << 8));
}

// CFL_DB.dat: "CFOG", a version word, then the slots' MiiData records (0x5C each; empty slots
// are zero). The stride is checked against the records' version byte (3), 0x5C or 0x60.
void loadDatabase() {
    if (sDatabaseLoaded) {
        return;
    }
    sDatabaseLoaded = true;
    static u8 sFile[0x10000];
    unsigned long size = 0;
    if (!hostMiiDatabase(sFile, sizeof(sFile), &size) || size < 8 || memcmp(sFile, "CFOG", 4) != 0) {
        hostLog("mii: Mii Maker database unavailable (%lu bytes)\n", size);
        return;
    }
    const u32 strides[2] = {0x5c, 0x60};
    u32 bestStride = strides[0];
    u32 bestScore = 0;
    for (u32 s = 0; s < 2; s++) {
        u32 score = 0;
        for (u32 i = 0; i < RFL_DB_CHAR_MAX && 8 + (i + 1) * strides[s] <= size; i++) {
            score += sFile[8 + i * strides[s]] == 3 ? 1 : 0;
        }
        if (score > bestScore) {
            bestScore = score;
            bestStride = strides[s];
        }
    }
    u32 count = 0;
    for (u32 i = 0; i < RFL_DB_CHAR_MAX && 8 + (i + 1) * bestStride <= size; i++) {
        const u8* record = sFile + 8 + i * bestStride;
        // A Mii: the version byte, an id and a name.
        if (record[0] != 3 || (record[0x0c] | record[0x0d] | record[0x0e] | record[0x0f]) == 0 || readLE16(record + 0x1a) == 0) {
            continue;
        }
        memcpy(sDatabase[i], record, kMiiSize);
        sDatabaseValid[i] = true;
        count++;
    }
    hostLog("mii: Mii Maker database %lu bytes, stride 0x%lx, %lu Miis\n", size, (unsigned long)bestStride,
            (unsigned long)count);
}

const u8* miiRecord(RFLDataSource source, u16 index) {
    if (source == RFLDataSource_Default) {
        return sDefaultMii;
    }
    if (source != RFLDataSource_Official) {
        return nullptr;
    }
    loadDatabase();
    return index < RFL_DB_CHAR_MAX && sDatabaseValid[index] ? sDatabase[index] : nullptr;
}

// The Wii's 8-byte create ID: the Mii's id (its creation time) and the first half of the system id.
void makeCreateID(const u8* record, RFLCreateID* id) {
    memcpy(id->data, record + 0x0c, 4);
    memcpy(id->data + 4, record + 0x04, 4);
}

u8 miiSkinIndex() {
    return static_cast< u8 >((readLE16(sSelectedMii + 0x30) >> 5) & 7);
}

u8 miiSkinColor(u8 index, u8 channel) {
    static const u8 colors[][3] = {{255, 211, 173}, {255, 182, 107}, {222, 121, 66},
                                   {255, 170, 140}, {173, 81, 41}, {99, 44, 24}};
    index = index < sizeof(colors) / sizeof(colors[0]) ? index : 0;
    return colors[index][channel < 3 ? channel : 0];
}

// The head from the console's own Mii parts, CFL_Res.dat (a system title's RomFS). Read offline
// from a dump of that file, with _workspace/tools/mii/render.py as the reference picture:
// - The file: u16 section count, then u32 section offsets. A section: u16 item count, u16 largest
//   item size, a u32 per item (low 22 bits the offset after the table and a u32 data size, high
//   bits not an index: items with the same offset share their data), then the data.
// - A shape: u16 vertex, normal and texcoord counts (each the vertex count, or 1 for one shared
//   value), u16 indexed flag, the vertices interleaved as s16 (position /256, normal /256,
//   texcoord /8192), the shared values, then u16 primitive (4 triangles), u16 index count and u8
//   indices. Faceline items start with 9 floats (hair, nose and beard places); hair items with 18
//   (places for hats). Hair, forehead and cap hold two items per style, the second for under a hat.
// - A texture: u16 width, u16 height, u8 levels, u8 format, u8 wrap s, u8 wrap t, then the GPU's
//   layout (8x8 Morton tiles) at the power-of-two size, but with the image's top row first in
//   memory (unlike the converter's textures: seen against Mii Maker on the moustache, eyebrows
//   and mouths). Formats: 0x02 A4, 0x04 LA4, 0x0A RGBA4.
// Placement, colours and the layered channels of eye and mouth textures follow the Wii U face
// library (FFL), the 3DS one's descendant: the face parts are drawn into a "mask" texture laid on
// the mask shape, makeup, wrinkles and beard shading into a "faceline" texture on the faceline.

enum {
    kCflBeard = 0,
    kCflFaceline = 2,
    kCflForehead = 3,
    kCflGlassShape = 4,
    kCflHair = 5,
    kCflMask = 6,
    kCflNoselineShape = 7,
    kCflNose = 8,
    kCflEye = 10,
    kCflEyebrow = 11,
    kCflBeardTexture = 12,
    kCflWrinkle = 13,
    kCflMakeup = 14,
    kCflGlass = 15,
    kCflMole = 16,
    kCflMouth = 17,
    kCflMustache = 18,
    kCflNoseline = 19,
};

enum { kPartFaceline, kPartForehead, kPartHair, kPartBeard, kPartNose, kPartMask, kPartNoseline, kPartGlass, kPartCount };

// A texture's picture kept on the CPU for the icon rasteriser: top row first, as uploaded.
struct CflImage {
    const u32* pixels;
    u32 width, height;
    u8 wrapS, wrapT;  // 0 clamp, 1 repeat, 2 mirror
};

struct CflPart {
    u32 first;
    u32 count;
    u32 color;  // 0xRRGGBBAA, multiplied by the texture when there is one
    void* texture;
    CflImage image;  // valid right after the head is built only (shared buffers)
    bool lit;
};

const u32 kCflVertices = 2400;
const u32 kMaskSize = 128;
const u32 kFacelineWidth = 64;
const u32 kFacelineHeight = 128;

const u8* sCfl;
u32 sCflSize;

// A head built for one Mii: its parts as triangle lists in the model's space.
struct CflHead {
    u8 key[kMiiSize];
    bool built;
    u32 lastUse;
    CflPart parts[kPartCount];
    u32 vertexCount;
    TVec3f positions[kCflVertices];
    TVec3f normals[kCflVertices];
    TVec2f texcoords[kCflVertices];
};

// The heads drawn in the file select (one per file shown) and one for making icons.
CflHead sHeads[4];
CflHead sIconHead;
CflHead* sHead = &sHeads[0];
u32 sHeadUse;
u32 sNoselineImage[64 * 64];
u32 sGlassImage[64 * 64];
TVec3f sTransformed[192];
u32 sLitColors[192];
TVec2f sDrawTexcoords[192][3];
u32 sCflSource[64 * 64];
u32 sMaskCanvas[kMaskSize * kMaskSize];
u32 sFacelineCanvas[kFacelineWidth * kFacelineHeight];

u16 cfl16(const u8* p) { return static_cast< u16 >(p[0] | (p[1] << 8)); }
u32 cfl32(const u8* p) { return static_cast< u32 >(cfl16(p) | (cfl16(p + 2) << 16)); }
f32 cflFloat(const u8* p) {
    f32 value;
    memcpy(&value, p, sizeof(value));
    return value;
}

// The MiiData record (libctru mii.h, with the eyebrow bit libctru's struct leaves out: the same
// layout as FFL's store data).
u32 miiBits(u32 offset, u32 shift, u32 bits) {
    const u32 value = sSelectedMii[offset] | (sSelectedMii[offset + 1] << 8) | (sSelectedMii[offset + 2] << 16) |
                      (sSelectedMii[offset + 3] << 24);
    return (value >> shift) & ((1u << bits) - 1);
}

const u8* cflItem(u32 section, u32 item) {
    if (sCfl == nullptr || sCflSize < 8 || section >= cfl16(sCfl)) {
        return nullptr;
    }
    const u32 sectionOffset = cfl32(sCfl + 4 + section * 4);
    if (sectionOffset + 8 > sCflSize) {
        return nullptr;
    }
    const u8* header = sCfl + sectionOffset;
    const u32 count = cfl16(header);
    if (item >= count || sectionOffset + 8 + count * 4 > sCflSize) {
        return nullptr;
    }
    const u32 offset = sectionOffset + 8 + count * 4 + (cfl32(header + 4 + item * 4) & 0x003fffff);
    if (offset + 8 > sCflSize) {
        return nullptr;
    }
    return sCfl + offset;
}

f32 colorChannel(u32 color, u32 channel) {
    return static_cast< f32 >((color >> (24 - channel * 8)) & 255) / 255.0f;
}

u32 packColor(f32 r, f32 g, f32 b, f32 a) {
    const f32 values[4] = {r, g, b, a};
    u32 out = 0;
    for (u32 i = 0; i < 4; i++) {
        const f32 v = values[i] < 0.0f ? 0.0f : values[i] > 1.0f ? 1.0f : values[i];
        out = (out << 8) | static_cast< u32 >(v * 255.0f + 0.5f);
    }
    return out;
}

u32 mortonOffset(u32 x, u32 y) {
    u32 offset = 0;
    for (u32 i = 0; i < 3; i++) {
        offset |= ((x >> i) & 1) << (2 * i);
        offset |= ((y >> i) & 1) << (2 * i + 1);
    }
    return offset;
}

// Decodes a texture item into sCflSource (0xRRGGBBAA, top row first, at the power-of-two size).
// A4 texels come out white with their alpha, LA4 as grey with alpha.
bool decodeCflTexture(const u8* item, u32* pWidth, u32* pHeight, u32* pPotWidth, u32* pPotHeight, u32* pWrapS, u32* pWrapT) {
    if (item == nullptr) {
        return false;
    }
    const u32 width = cfl16(item);
    const u32 height = cfl16(item + 2);
    const u8 format = item[5];
    u32 potWidth = 8;
    u32 potHeight = 8;
    while (potWidth < width) potWidth <<= 1;
    while (potHeight < height) potHeight <<= 1;
    const u32 bits = format == 0x02 ? 4 : format == 0x04 ? 8 : format == 0x0a ? 16 : 0;
    if (bits == 0 || potWidth * potHeight > sizeof(sCflSource) / sizeof(sCflSource[0]) ||
        static_cast< u32 >(item - sCfl) + 8 + potWidth * potHeight * bits / 8 > sCflSize) {
        return false;
    }
    const u8* data = item + 8;
    for (u32 tileY = 0; tileY < potHeight; tileY += 8) {
        for (u32 tileX = 0; tileX < potWidth; tileX += 8) {
            const u32 tile = ((tileY / 8) * (potWidth / 8) + tileX / 8) * 64;
            for (u32 y = 0; y < 8; y++) {
                for (u32 x = 0; x < 8; x++) {
                    const u32 i = tile + mortonOffset(x, y);
                    u32 color;
                    if (bits == 4) {
                        const u32 alpha = (data[i >> 1] >> ((i & 1) * 4)) & 15;
                        color = 0xffffff00u | (alpha * 17);
                    } else if (bits == 8) {
                        const u32 luminance = (data[i] >> 4) * 17;
                        color = (luminance << 24) | (luminance << 16) | (luminance << 8) | ((data[i] & 15) * 17);
                    } else {
                        const u32 v = cfl16(data + i * 2);
                        color = (((v >> 12) & 15) * 17) << 24 | (((v >> 8) & 15) * 17) << 16 | (((v >> 4) & 15) * 17) << 8 |
                                (v & 15) * 17;
                    }
                    sCflSource[(tileY + y) * potWidth + tileX + x] = color;
                }
            }
        }
    }
    *pWidth = width;
    *pHeight = height;
    *pPotWidth = potWidth;
    *pPotHeight = potHeight;
    *pWrapS = item[6];
    *pWrapT = item[7];
    return true;
}

// Bilinear texel of sCflSource at u, v in [0, 1) of the nominal size (v = 0 the top row), as
// r, g, b, a in [0, 1].
void sampleSource(u32 width, u32 height, u32 potWidth, f32 u, f32 v, f32* rgba) {
    const f32 x = u * width - 0.5f;
    const f32 y = v * height - 0.5f;
    const s32 x0 = static_cast< s32 >(floorf(x));
    const s32 y0 = static_cast< s32 >(floorf(y));
    const f32 fx = x - x0;
    const f32 fy = y - y0;
    for (u32 c = 0; c < 4; c++) {
        rgba[c] = 0.0f;
    }
    for (u32 j = 0; j < 4; j++) {
        s32 sx = x0 + static_cast< s32 >(j & 1);
        s32 sy = y0 + static_cast< s32 >(j >> 1);
        sx = sx < 0 ? 0 : sx >= static_cast< s32 >(width) ? width - 1 : sx;
        sy = sy < 0 ? 0 : sy >= static_cast< s32 >(height) ? height - 1 : sy;
        const f32 weight = ((j & 1) ? fx : 1.0f - fx) * ((j >> 1) ? fy : 1.0f - fy);
        const u32 texel = sCflSource[sy * potWidth + sx];
        for (u32 c = 0; c < 4; c++) {
            rgba[c] += colorChannel(texel, c) * weight;
        }
    }
}

// FFL modulate modes: 1 the texture as it is, 2 the r, g, b channels choose colours 0, 1, 2,
// 3 colour 0 with the texture's alpha.
void modulateTexel(f32* rgba, u32 mode, const u32* colors) {
    if (mode == 2) {
        f32 out[3];
        for (u32 c = 0; c < 3; c++) {
            out[c] = rgba[0] * colorChannel(colors[0], c) + rgba[1] * colorChannel(colors[1], c) +
                     rgba[2] * colorChannel(colors[2], c);
        }
        rgba[0] = out[0];
        rgba[1] = out[1];
        rgba[2] = out[2];
    } else if (mode == 3) {
        for (u32 c = 0; c < 3; c++) {
            rgba[c] = colorChannel(colors[0], c);
        }
    }
}

void blendOver(u32* destination, const f32* source) {
    const f32 alpha = source[3];
    if (alpha <= 0.0f) {
        return;
    }
    const f32 destinationAlpha = colorChannel(*destination, 3);
    const f32 outAlpha = alpha + destinationAlpha * (1.0f - alpha);
    f32 out[3];
    for (u32 c = 0; c < 3; c++) {
        out[c] = (source[c] * alpha + colorChannel(*destination, c) * destinationAlpha * (1.0f - alpha)) / outAlpha;
    }
    *destination = packColor(out[0], out[1], out[2], outAlpha);
}

enum { kOriginCenter, kOriginLeft, kOriginRight };

// FFLiRawMaskParts: a unit quad from its origin, scaled, turned, scaled by (0.8896, 0.9277) and
// placed, in a 64-unit square with y downwards.
void drawMaskPart(u32 section, u32 item, u32 mode, const u32* colors, f32 x, f32 y, f32 scaleX, f32 scaleY,
                  f32 degrees, u32 origin) {
    u32 width, height, potWidth, potHeight, wrapS, wrapT;
    if (!decodeCflTexture(cflItem(section, item), &width, &height, &potWidth, &potHeight, &wrapS, &wrapT)) {
        return;
    }
    const f32 radians = degrees * 0.01745329252f;
    const f32 cosine = cosf(radians);
    const f32 sine = sinf(radians);
    const f32 unit = kMaskSize / 64.0f;
    for (u32 py = 0; py < kMaskSize; py++) {
        for (u32 px = 0; px < kMaskSize; px++) {
            const f32 dx = ((px + 0.5f) / unit - x) / 0.88961464f;
            const f32 dy = ((py + 0.5f) / unit - y) / 0.9276675f;
            const f32 localX = (dx * cosine + dy * sine) / scaleX;
            const f32 localY = (-dx * sine + dy * cosine) / scaleY;
            const f32 u = origin == kOriginCenter ? localX + 0.5f : origin == kOriginLeft ? localX + 1.0f : 1.0f - localX;
            const f32 v = localY + 0.5f;
            if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) {
                continue;
            }
            f32 rgba[4];
            sampleSource(width, height, potWidth, u, v, rgba);
            modulateTexel(rgba, mode, colors);
            blendOver(&sMaskCanvas[py * kMaskSize + px], rgba);
        }
    }
}

const u32 kHairColors[8] = {0x1e1a18ff, 0x402010ff, 0x5c180aff, 0x7c3a14ff, 0x787880ff, 0x4e3e10ff, 0x885818ff, 0xd0a04aff};
const u32 kFacelineColors[6] = {0xffd3adff, 0xffb66bff, 0xde7942ff, 0xffaa8cff, 0xad5129ff, 0x632c18ff};
const u32 kEyeColors[6] = {0x000000ff, 0x6c7070ff, 0x663c2cff, 0x605e30ff, 0x4654a8ff, 0x387058ff};
const u32 kGlassColors[6] = {0x181818ff, 0x603810ff, 0xa81008ff, 0x203068ff, 0xa86000ff, 0x787068ff};
const u32 kMouthColorsR[5] = {0xd85208ff, 0xf00c08ff, 0xf54848ff, 0xf09a74ff, 0x8c5040ff};
const u32 kMouthColorsG[5] = {0x823018ff, 0x780c0cff, 0x882028ff, 0xdc7850ff, 0x461e0aff};
const u8 kEyeRotate[62] = {3, 4, 4, 4, 3, 4, 4, 4, 3, 4, 4, 4, 4, 3, 3, 4, 4, 4, 3, 3, 4, 3, 4, 3, 3, 4, 3, 4, 4, 3, 4,
                           4, 4, 3, 3, 3, 4, 4, 3, 3, 3, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 3, 4, 4, 3, 4, 4};
const u8 kEyebrowRotate[24] = {6, 6, 5, 7, 6, 7, 6, 7, 4, 7, 6, 8, 5, 5, 6, 6, 7, 7, 6, 6, 5, 6, 7, 5};

// FFLiRawMask's CalcRawMask. FFL draws each part under the ones before it (mustache, mouth,
// eyebrows, eyes, mole): the same as drawing them the other way round, each over the last.
void buildMaskCanvas() {
    memset(sMaskCanvas, 0, sizeof(sMaskCanvas));
    const f32 posYAdd = 4.629278f;
    const f32 spacingMul = 0.88961464f;
    const f32 posXMul = 1.7792293f;
    const f32 posYMul = 1.0760943f;
    const u32 eyeType = miiBits(0x34, 0, 6);
    const u32 eyeColor = miiBits(0x34, 6, 3);
    const u32 eyeScale = miiBits(0x34, 9, 4);
    const u32 eyeScaleY = miiBits(0x34, 13, 3);
    const u32 eyeRotate = miiBits(0x34, 16, 5);
    const u32 eyeSpacing = miiBits(0x34, 21, 4);
    const u32 eyeY = miiBits(0x34, 25, 5);
    const u32 browType = miiBits(0x38, 0, 5);
    const u32 browColor = miiBits(0x38, 5, 3);
    const u32 browScale = miiBits(0x38, 8, 4);
    const u32 browScaleY = miiBits(0x38, 12, 3);
    const u32 browRotate = miiBits(0x38, 16, 5);
    const u32 browSpacing = miiBits(0x38, 21, 4);
    const u32 browY = miiBits(0x38, 25, 5);
    const u32 mouthType = miiBits(0x3e, 0, 6);
    const u32 mouthColor = miiBits(0x3e, 6, 3);
    const u32 mouthScale = miiBits(0x3e, 9, 4);
    const u32 mouthScaleY = miiBits(0x3e, 13, 3);
    const u32 mouthY = miiBits(0x40, 0, 5);
    const u32 mustacheType = miiBits(0x40, 5, 3);
    const u32 beardColor = miiBits(0x42, 3, 3);
    const u32 mustacheScale = miiBits(0x42, 6, 4);
    const u32 mustacheY = miiBits(0x42, 10, 5);
    const u32 moleOn = miiBits(0x46, 0, 1);
    const u32 moleScale = miiBits(0x46, 1, 4);
    const u32 moleX = miiBits(0x46, 5, 5);
    const u32 moleY = miiBits(0x46, 10, 5);

    if (moleOn) {
        const u32 color[1] = {0x120f0fff};
        const f32 scale = 0.4f * moleScale + 1.0f;
        drawMaskPart(kCflMole, 1, 3, color, moleX * posXMul + 3.5323312f + 14.233834f,
                     moleY * posYMul + posYAdd + 11.178394f + 2.0f * posYMul, scale, scale, 0.0f, kOriginCenter);
    }
    if (eyeType < 62) {
        const u32 colors[3] = {0x00ffffff, 0xffffffff, kEyeColors[eyeColor < 6 ? eyeColor : 0]};
        const f32 scale = 0.4f * eyeScale + 1.0f;
        f32 height = 4.5f * scale * (0.12f * eyeScaleY + 0.64f);
        if ((eyeType == 14 || eyeType == 26) && height < 12.0f) {
            height = 12.0f;
        }
        const f32 rotate = static_cast< f32 >((eyeRotate + 32 - kEyeRotate[eyeType]) % 32) * 11.25f;
        const f32 y = eyeY * posYMul + posYAdd + 13.822246f;
        drawMaskPart(kCflEye, eyeType, 2, colors, 32.0f - eyeSpacing * spacingMul, y, 5.34375f * scale, height, rotate,
                     kOriginLeft);
        drawMaskPart(kCflEye, eyeType, 2, colors, 32.0f + eyeSpacing * spacingMul, y, 5.34375f * scale, height,
                     360.0f - rotate, kOriginRight);
    }
    if (browType < 24) {
        const u32 color[1] = {kHairColors[browColor]};
        const f32 scale = 0.4f * browScale + 1.0f;
        const f32 height = 4.5f * scale * (0.12f * browScaleY + 0.64f);
        const f32 rotate = static_cast< f32 >((browRotate + 32 - kEyebrowRotate[browType]) % 32) * 11.25f;
        const f32 y = browY * posYMul + posYAdd + 11.920528f;
        drawMaskPart(kCflEyebrow, browType, 3, color, 32.0f - browSpacing * spacingMul, y, 5.0625f * scale, height, rotate,
                     kOriginLeft);
        drawMaskPart(kCflEyebrow, browType, 3, color, 32.0f + browSpacing * spacingMul, y, 5.0625f * scale, height,
                     360.0f - rotate, kOriginRight);
    }
    {
        const u32 index = mouthColor < 5 ? mouthColor : 0;
        const u32 colors[3] = {kMouthColorsR[index], kMouthColorsG[index], 0xffffffff};
        const f32 scale = 0.4f * mouthScale + 1.0f;
        f32 height = 4.5f * scale * (0.12f * mouthScaleY + 0.64f);
        if ((mouthType == 3 || mouthType == 15 || mouthType == 19 || mouthType == 20 || mouthType == 21 ||
             mouthType == 23 || mouthType == 25) && height < 12.0f) {
            height = 12.0f;
        }
        drawMaskPart(kCflMouth, mouthType, 2, colors, 32.0f, mouthY * posYMul + posYAdd + 24.629572f, 6.1875f * scale,
                     height, 0.0f, kOriginCenter);
    }
    if (mustacheType != 0) {
        const u32 color[1] = {kHairColors[beardColor]};
        const f32 scale = 0.4f * mustacheScale + 1.0f;
        const f32 y = mustacheY * posYMul + posYAdd + 27.134275f;
        drawMaskPart(kCflMustache, mustacheType, 3, color, 32.0f, y, 4.5f * scale, 9.0f * scale, 0.0f, kOriginLeft);
        drawMaskPart(kCflMustache, mustacheType, 3, color, 32.0f, y, 4.5f * scale, 9.0f * scale, 0.0f, kOriginRight);
    }
}

// FFLiFacelineTexture: the skin colour, makeup, wrinkles and beard shading, over the whole
// texture (the faceline's texcoords mirror it across the face).
void buildFacelineCanvas(u32 skin) {
    for (u32 i = 0; i < kFacelineWidth * kFacelineHeight; i++) {
        sFacelineCanvas[i] = skin;
    }
    const u32 wrinkles = miiBits(0x30, 8, 4);
    const u32 makeup = miiBits(0x30, 12, 4);
    const u32 beardType = miiBits(0x42, 0, 3);
    const u32 beardColor = miiBits(0x42, 3, 3);
    const u32 black[1] = {0x000000ff};
    const u32 hair[1] = {kHairColors[beardColor]};
    const struct {
        u32 section, item, mode;
        const u32* colors;
    } layers[3] = {
        {kCflMakeup, makeup, 1, nullptr},
        {kCflWrinkle, wrinkles, 3, black},
        {kCflBeardTexture, beardType >= 4 ? beardType - 3 : 0, 3, hair},
    };
    for (u32 layer = 0; layer < 3; layer++) {
        u32 width, height, potWidth, potHeight, wrapS, wrapT;
        if (layers[layer].item == 0 ||
            !decodeCflTexture(cflItem(layers[layer].section, layers[layer].item), &width, &height, &potWidth, &potHeight,
                              &wrapS, &wrapT)) {
            continue;
        }
        for (u32 y = 0; y < kFacelineHeight; y++) {
            for (u32 x = 0; x < kFacelineWidth; x++) {
                f32 rgba[4];
                sampleSource(width, height, potWidth, (x + 0.5f) / kFacelineWidth, (y + 0.5f) / kFacelineHeight, rgba);
                modulateTexel(rgba, layers[layer].mode, layers[layer].colors);
                blendOver(&sFacelineCanvas[y * kFacelineWidth + x], rgba);
            }
        }
    }
}

// A texture straight from the file, coloured: noseline (colour with the texture's alpha) and
// glasses (LA4, the colour times the luminance). Laid straight on a shape, whose t counts from
// the first row in memory: the rows go back to memory order, bottom row first, to meet the
// shape's t turned for hostGfxTextureCreate.
void* cflColoredTexture(u32 section, u32 item, u32 color, bool luminance, u32* pCopy, CflImage* pImage, bool gpu) {
    u32 width, height, potWidth, potHeight, wrapS, wrapT;
    pImage->pixels = nullptr;
    if (!decodeCflTexture(cflItem(section, item), &width, &height, &potWidth, &potHeight, &wrapS, &wrapT)) {
        return nullptr;
    }
    for (u32 i = 0; i < potWidth * potHeight; i++) {
        const f32 level = luminance ? colorChannel(sCflSource[i], 0) : 1.0f;
        sCflSource[i] = packColor(colorChannel(color, 0) * level, colorChannel(color, 1) * level,
                                  colorChannel(color, 2) * level, colorChannel(sCflSource[i], 3));
    }
    for (u32 y = 0; y < potHeight / 2; y++) {
        for (u32 x = 0; x < potWidth; x++) {
            const u32 top = sCflSource[y * potWidth + x];
            sCflSource[y * potWidth + x] = sCflSource[(potHeight - 1 - y) * potWidth + x];
            sCflSource[(potHeight - 1 - y) * potWidth + x] = top;
        }
    }
    const u8 modeS = wrapS == 2 ? 2 : wrapS == 1 ? 1 : 0;
    const u8 modeT = wrapT == 2 ? 2 : wrapT == 1 ? 1 : 0;
    memcpy(pCopy, sCflSource, potWidth * potHeight * sizeof(u32));
    pImage->pixels = pCopy;
    pImage->width = potWidth;
    pImage->height = potHeight;
    pImage->wrapS = modeS;
    pImage->wrapT = modeT;
    if (!gpu) {
        return nullptr;
    }
    return hostGfxTextureCreate(reinterpret_cast< const unsigned long* >(sCflSource), potWidth, potHeight, potWidth,
                                potHeight, modeS, modeT, 1, 1);
}

// Appends a shape, scaled and placed (FFLiAdjustShape: z takes the mean of the x and y scales),
// mirrored for flipped hair. Texcoord t is turned to the top-row-first convention of
// hostGfxTextureCreate.
void appendCflShape(u32 partIndex, const u8* item, u32 prefix, f32 scale, f32 x, f32 y, f32 z, bool flip, u32 color,
                    void* texture, bool lit) {
    CflPart& part = sHead->parts[partIndex];
    part.first = sHead->vertexCount;
    part.count = 0;
    part.color = color;
    part.texture = texture;
    part.lit = lit;
    if (item == nullptr) {
        return;
    }
    const u8* data = item + prefix;
    if (static_cast< u32 >(data - sCfl) + 8 > sCflSize) {
        return;
    }
    const u32 vertices = cfl16(data);
    const u32 normals = cfl16(data + 2);
    const u32 texcoords = cfl16(data + 4);
    const u32 indexed = cfl16(data + 6);
    if (vertices == 0 || vertices > 255 || indexed != 1) {
        return;
    }
    const u32 stride = 6 + (normals == vertices ? 6 : 0) + (texcoords == vertices ? 4 : 0);
    const u8* vertexData = data + 8;
    const u8* shared = vertexData + stride * vertices;
    const u8* indexHeader = shared + (normals == 1 ? 6 : 0) + (texcoords == 1 ? 4 : 0);
    if (static_cast< u32 >(indexHeader - sCfl) + 4 > sCflSize || cfl16(indexHeader) != 4) {
        return;
    }
    const u32 indexCount = cfl16(indexHeader + 2) / 3 * 3;
    const u8* indices = indexHeader + 4;
    if (static_cast< u32 >(indices - sCfl) + indexCount > sCflSize || sHead->vertexCount + indexCount > kCflVertices) {
        return;
    }
    const f32 flipX = flip ? -1.0f : 1.0f;
    const f32 modelScale = 32.0f / 30.0f;
    for (u32 i = 0; i < indexCount; i++) {
        // Mirrored geometry keeps its front faces by taking each triangle's corners the other way.
        const u32 corner = flip ? (i % 3 == 0 ? i : i % 3 == 1 ? i + 1 : i - 1) : i;
        const u32 index = indices[corner];
        if (index >= vertices) {
            sHead->vertexCount = part.first;
            part.count = 0;
            return;
        }
        const u8* vertex = vertexData + index * stride;
        const u32 out = sHead->vertexCount++;
        sHead->positions[out].set(((static_cast< s16 >(cfl16(vertex)) / 256.0f) * scale * flipX + x) * modelScale,
                               ((static_cast< s16 >(cfl16(vertex + 2)) / 256.0f) * scale + y) * modelScale + 8.0f,
                               ((static_cast< s16 >(cfl16(vertex + 4)) / 256.0f) * scale + z) * modelScale);
        const u8* normal = normals == vertices ? vertex + 6 : normals == 1 ? shared : nullptr;
        if (normal != nullptr) {
            sHead->normals[out].set(static_cast< s16 >(cfl16(normal)) / 256.0f * flipX, static_cast< s16 >(cfl16(normal + 2)) / 256.0f,
                                 static_cast< s16 >(cfl16(normal + 4)) / 256.0f);
        } else {
            sHead->normals[out].set(0.0f, 0.0f, 1.0f);
        }
        const u8* texcoord = texcoords == vertices ? vertex + 6 + (normals == vertices ? 6 : 0) :
                             texcoords == 1 ? shared + (normals == 1 ? 6 : 0) : nullptr;
        if (texcoord != nullptr) {
            sHead->texcoords[out].set(static_cast< s16 >(cfl16(texcoord)) / 8192.0f,
                                   1.0f - static_cast< s16 >(cfl16(texcoord + 2)) / 8192.0f);
        } else {
            sHead->texcoords[out].set(0.0f, 0.0f);
        }
    }
    part.count = indexCount;
}

void releaseHead(CflHead& head) {
    for (u32 i = 0; i < kPartCount; i++) {
        if (head.parts[i].texture != nullptr) {
            hostGfxTextureDestroy(head.parts[i].texture);
        }
    }
    memset(head.parts, 0, sizeof(head.parts));
    head.vertexCount = 0;
    head.built = false;
}

bool loadCflResource() {
    if (sCfl != nullptr) {
        return true;
    }
    const void* resource;
    unsigned long resourceSize;
    if (!hostCflResource(&resource, &resourceSize)) {
        return false;
    }
    sCfl = static_cast< const u8* >(resource);
    sCflSize = static_cast< u32 >(resourceSize);
    return true;
}

// Builds a Mii's head into `head`. With gpu false (icons) no texture is made: the parts keep
// only their CPU pictures.
void buildHead(CflHead& head, const u8* mii, bool gpu) {
    releaseHead(head);
    sHead = &head;
    sSelectedMii = mii;
    memcpy(head.key, mii, kMiiSize);
    head.built = true;
    if (!loadCflResource()) {
        return;
    }

    const u32 faceType = miiBits(0x30, 1, 4);
    const u32 skin = kFacelineColors[miiSkinIndex() < 6 ? miiSkinIndex() : 0];
    const u32 hairType = miiBits(0x32, 0, 8);
    const u32 hairColor = kHairColors[miiBits(0x32, 8, 3)];
    const bool hairFlip = miiBits(0x32, 11, 1) != 0;
    const u32 beardType = miiBits(0x42, 0, 3);
    const u32 beardColor = kHairColors[miiBits(0x42, 3, 3)];
    const u32 noseType = miiBits(0x3c, 0, 5);
    const u32 noseScaleValue = miiBits(0x3c, 5, 4);
    const u32 noseY = miiBits(0x3c, 9, 5);
    const u32 glassType = miiBits(0x44, 0, 4);
    const u32 glassColor = miiBits(0x44, 4, 3);
    const u32 glassScaleValue = miiBits(0x44, 7, 4);
    const u32 glassY = miiBits(0x44, 11, 5);

    // The faceline's places for the hair, the nose (the face's centre) and the beard.
    const u8* faceline = cflItem(kCflFaceline, faceType);
    f32 places[9] = {};
    if (faceline != nullptr && static_cast< u32 >(faceline - sCfl) + 0x24 <= sCflSize) {
        for (u32 i = 0; i < 9; i++) {
            places[i] = cflFloat(faceline + i * 4);
        }
    }

    buildFacelineCanvas(skin);
    void* facelineTexture = gpu ? hostGfxTextureCreate(reinterpret_cast< const unsigned long* >(sFacelineCanvas), kFacelineWidth,
                                                       kFacelineHeight, kFacelineWidth, kFacelineHeight, 2, 0, 1, 1)
                                : nullptr;
    appendCflShape(kPartFaceline, faceline, 0x24, 1.0f, 0.0f, 0.0f, 0.0f, false, 0xffffffff, facelineTexture, true);
    head.parts[kPartFaceline].image = {sFacelineCanvas, kFacelineWidth, kFacelineHeight, 2, 0};
    appendCflShape(kPartForehead, cflItem(kCflForehead, hairType * 2), 0, 1.0f, places[0], places[1], places[2], hairFlip,
                   skin, nullptr, true);
    appendCflShape(kPartHair, cflItem(kCflHair, hairType * 2), 0x48, 1.0f, places[0], places[1], places[2], hairFlip,
                   hairColor, nullptr, true);
    if (beardType > 0 && beardType < 4) {
        appendCflShape(kPartBeard, cflItem(kCflBeard, beardType), 0, 1.0f, places[6], places[7], places[8], false, beardColor,
                       nullptr, true);
    }
    const f32 noseScale = noseScaleValue * 0.175f + 0.4f;
    const f32 nosePlaceY = places[4] + (static_cast< f32 >(noseY) - 8.0f) * -1.5f;
    appendCflShape(kPartNose, cflItem(kCflNose, noseType), 0, noseScale, places[3], nosePlaceY, places[5], false, skin,
                   nullptr, true);

    buildMaskCanvas();
    void* maskTexture = gpu ? hostGfxTextureCreate(reinterpret_cast< const unsigned long* >(sMaskCanvas), kMaskSize, kMaskSize,
                                                   kMaskSize, kMaskSize, 0, 0, 1, 1)
                            : nullptr;
    appendCflShape(kPartMask, cflItem(kCflMask, faceType), 0, 1.0f, 0.0f, 0.0f, 0.0f, false, 0xffffffff, maskTexture, true);
    head.parts[kPartMask].image = {sMaskCanvas, kMaskSize, kMaskSize, 0, 0};
    CflImage noselineImage;
    void* noselineTexture = cflColoredTexture(kCflNoseline, noseType, 0x000000ff, false, sNoselineImage, &noselineImage, gpu);
    appendCflShape(kPartNoseline, cflItem(kCflNoselineShape, noseType), 0, noseScale, places[3], nosePlaceY, places[5],
                   false, 0xffffffff, noselineTexture, false);
    head.parts[kPartNoseline].image = noselineImage;
    if (glassType != 0) {
        const f32 glassScale = glassScaleValue * 0.15f + 0.4f;
        CflImage glassImage;
        void* glassTexture = cflColoredTexture(kCflGlass, glassType, kGlassColors[glassColor < 6 ? glassColor : 0], true,
                                               sGlassImage, &glassImage, gpu);
        appendCflShape(kPartGlass, cflItem(kCflGlassShape, 0), 0, glassScale, places[3],
                       places[4] + (static_cast< f32 >(glassY) - 11.0f) * -1.5f + 5.0f, places[5] + 2.0f, false,
                       0xffffffff, glassTexture, false);
        head.parts[kPartGlass].image = glassImage;
    }
    if (gpu) {
        hostLog("mii: CFL head face %lu hair %lu%s, %lu vertices\n", (unsigned long)faceType, (unsigned long)hairType,
                hairFlip ? " flipped" : "", (unsigned long)sHead->vertexCount);
    }
}

// The cached head of a Mii, built on first use in the least recently used slot.
CflHead* headFor(const u8* mii) {
    CflHead* slot = &sHeads[0];
    for (u32 i = 0; i < sizeof(sHeads) / sizeof(sHeads[0]); i++) {
        if (sHeads[i].built && memcmp(sHeads[i].key, mii, kMiiSize) == 0) {
            sHeads[i].lastUse = ++sHeadUse;
            return &sHeads[i];
        }
        if (!sHeads[i].built || (slot->built && sHeads[i].lastUse < slot->lastUse)) {
            slot = &sHeads[i];
        }
    }
    buildHead(*slot, mii, true);
    slot->lastUse = ++sHeadUse;
    return slot;
}

void drawHead(const MiiModelState& rState) {
    sHead = headFor(rState.mii);
    if (sHead->vertexCount == 0) {
        return;
    }
    Mtx identity;
    PSMTXIdentity(identity);
    TDDraw::setupDirect(1, 1, 0);
    TDDraw::setDirectViewMtx(identity);
    TDDraw::setDirectAlphaTest(GXToCitro3D::alphaTest(GX_GREATER), 0);
    // The whole screen, as the Wii's 3D pass: the file select's 2D windows leave their scissor
    // behind (J2DGrafContext::setScissor), which cut the turning head at a fixed column.
    TDDraw::setDirectScissor(0, 0, 640, 456);
    // The mask, noseline and glasses lie half a unit over the face, too little for the depth
    // buffer at the head's size and distance: they come one head unit nearer along the line of
    // sight (the same place on screen), still well behind the nose's tip.
    const f32 matrixScale = sqrtf(rState.matrix[0][0] * rState.matrix[0][0] + rState.matrix[1][0] * rState.matrix[1][0] +
                                  rState.matrix[2][0] * rState.matrix[2][0]);
    const f32 overlayPull = matrixScale * (32.0f / 30.0f);
    // A light from the front, a little above and to the right, as the Mii's own shading.
    const f32 lightX = 0.2611f;
    const f32 lightY = 0.4352f;
    const f32 lightZ = 0.8616f;
    for (u32 partIndex = 0; partIndex < kPartCount; partIndex++) {
        const CflPart& part = sHead->parts[partIndex];
        if (part.count == 0) {
            continue;
        }
        // Opaque parts write depth; the mask, noseline and glasses lie over them.
        const bool overlay = partIndex >= kPartMask;
        TDDraw::setDirectDepthMode(true, GXToCitro3D::depthTest(GX_LEQUAL), !overlay);
        HostGfxState state = {};
        if (part.texture != nullptr) {
            state.textures[0] = part.texture;
            state.stageCount = 1;
            state.stages[0].colorSrc[0] = 0x03;  // texture 0 times the vertex colour
            state.stages[0].colorSrc[1] = 0x00;
            state.stages[0].colorFunc = 1;
            state.stages[0].alphaSrc[0] = 0x03;
            state.stages[0].alphaSrc[1] = 0x00;
            state.stages[0].alphaFunc = 1;
        }
        for (u32 offset = 0; offset < part.count; offset += 192) {
            const u32 count = part.count - offset > 192 ? 192 : part.count - offset;
            for (u32 i = 0; i < count; i++) {
                const u32 v = part.first + offset + i;
                PSMTXMultVec(rState.matrix, &sHead->positions[v], &sTransformed[i]);
                if (overlay) {
                    TVec3f& p = sTransformed[i];
                    const f32 distance = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
                    if (distance > overlayPull * 2.0f) {
                        const f32 keep = 1.0f - overlayPull / distance;
                        p.set(p.x * keep, p.y * keep, p.z * keep);
                    }
                }
                u32 color = part.color;
                if (part.lit) {
                    TVec3f normal;
                    PSMTXMultVecSR(rState.matrix, &sHead->normals[v], &normal);
                    const f32 length = sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                    f32 diffuse = length > 0.0f ? (normal.x * lightX + normal.y * lightY + normal.z * lightZ) / length : 1.0f;
                    diffuse = diffuse < 0.0f ? 0.0f : diffuse;
                    const f32 level = 0.55f + 0.45f * diffuse;
                    color = packColor(colorChannel(color, 0) * level, colorChannel(color, 1) * level,
                                      colorChannel(color, 2) * level, colorChannel(color, 3));
                }
                sLitColors[i] = color;
                sDrawTexcoords[i][0] = sHead->texcoords[v];
                sDrawTexcoords[i][1] = sHead->texcoords[v];
                sDrawTexcoords[i][2] = sHead->texcoords[v];
            }
            if (part.texture == nullptr) {
                TDDraw::drawDirectTriangles(sTransformed, sLitColors, count);
            } else {
                TDDraw::drawStateTriangles(sTransformed, sDrawTexcoords, sLitColors, state, count);
            }
        }
    }
    TDDraw::closeDirect();
}

// Icons (RFLMakeIcon): the head from the front, drawn by the CPU into the caller's RGB5A3 picture.
// The Wii renders them with the GPU into the frame buffer and copies them out; here a small
// rasteriser draws the same parts as drawHead, with the same light, the head fitted to the picture.

const u32 kFavoriteColors[12] = {0xd21e14ff, 0xff6e19ff, 0xffd820ff, 0x78d220ff, 0x007830ff, 0x0a48b4ff,
                                 0x3caadeff, 0xf55a7dff, 0x7328adff, 0x483818ff, 0xe0e0e0ff, 0x181814ff};
const u32 kIconLimit = 128;
u32 sIconColor[kIconLimit * kIconLimit];
f32 sIconDepth[kIconLimit * kIconLimit];

u32 sampleImage(const CflImage& image, f32 s, f32 t) {
    f32 coords[2] = {s, t};
    const u8 modes[2] = {image.wrapS, image.wrapT};
    for (u32 c = 0; c < 2; c++) {
        f32 v = coords[c];
        if (modes[c] == 2) {
            v = fabsf(v);
            v = v - 2.0f * floorf(v * 0.5f);
            v = v > 1.0f ? 2.0f - v : v;
        } else if (modes[c] == 1) {
            v = v - floorf(v);
        }
        coords[c] = v < 0.0f ? 0.0f : v > 0.9999f ? 0.9999f : v;
    }
    return image.pixels[static_cast< u32 >(coords[1] * image.height) * image.width + static_cast< u32 >(coords[0] * image.width)];
}

void rasterPart(const CflPart& part, bool overlay, u32 size, f32 scale, f32 centerX, f32 centerY) {
    const f32 lightX = 0.2611f;
    const f32 lightY = 0.4352f;
    const f32 lightZ = 0.8616f;
    const bool textured = part.image.pixels != nullptr;
    for (u32 t = 0; t + 2 < part.count; t += 3) {
        f32 sx[3], sy[3], sz[3], level[3];
        for (u32 k = 0; k < 3; k++) {
            const u32 v = part.first + t + k;
            const TVec3f& p = sHead->positions[v];
            sx[k] = size * 0.5f + (p.x - centerX) * scale;
            sy[k] = size * 0.5f - (p.y - centerY) * scale;
            sz[k] = p.z + (overlay ? 1.0f : 0.0f);
            const TVec3f& n = sHead->normals[v];
            const f32 length = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
            f32 diffuse = length > 0.0f ? (n.x * lightX + n.y * lightY + n.z * lightZ) / length : 1.0f;
            level[k] = part.lit ? 0.55f + 0.45f * (diffuse < 0.0f ? 0.0f : diffuse) : 1.0f;
        }
        const f32 area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (fabsf(area) < 1e-6f) {
            continue;
        }
        f32 minX = sx[0], maxX = sx[0], minY = sy[0], maxY = sy[0];
        for (u32 k = 1; k < 3; k++) {
            minX = sx[k] < minX ? sx[k] : minX;
            maxX = sx[k] > maxX ? sx[k] : maxX;
            minY = sy[k] < minY ? sy[k] : minY;
            maxY = sy[k] > maxY ? sy[k] : maxY;
        }
        const s32 x0 = minX < 0.0f ? 0 : static_cast< s32 >(minX);
        const s32 y0 = minY < 0.0f ? 0 : static_cast< s32 >(minY);
        const s32 x1 = maxX > size - 1 ? size - 1 : static_cast< s32 >(maxX);
        const s32 y1 = maxY > size - 1 ? size - 1 : static_cast< s32 >(maxY);
        for (s32 y = y0; y <= y1; y++) {
            for (s32 x = x0; x <= x1; x++) {
                const f32 px = x + 0.5f;
                const f32 py = y + 0.5f;
                const f32 w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
                const f32 w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
                const f32 w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                    continue;
                }
                const u32 pixel = y * size + x;
                const f32 z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                if (z < sIconDepth[pixel]) {
                    continue;
                }
                f32 rgba[4];
                for (u32 c = 0; c < 4; c++) {
                    rgba[c] = colorChannel(part.color, c);
                }
                if (textured) {
                    const u32 a = part.first + t;
                    const f32 s = w0 * sHead->texcoords[a].x + w1 * sHead->texcoords[a + 1].x + w2 * sHead->texcoords[a + 2].x;
                    const f32 tt = w0 * sHead->texcoords[a].y + w1 * sHead->texcoords[a + 1].y + w2 * sHead->texcoords[a + 2].y;
                    const u32 texel = sampleImage(part.image, s, tt);
                    for (u32 c = 0; c < 4; c++) {
                        rgba[c] *= colorChannel(texel, c);
                    }
                }
                if (rgba[3] <= 0.0f) {
                    continue;
                }
                const f32 light = w0 * level[0] + w1 * level[1] + w2 * level[2];
                rgba[0] *= light;
                rgba[1] *= light;
                rgba[2] *= light;
                if (overlay) {
                    blendOver(&sIconColor[pixel], rgba);
                } else {
                    sIconColor[pixel] = packColor(rgba[0], rgba[1], rgba[2], 1.0f);
                    sIconDepth[pixel] = z;
                }
            }
        }
    }
}

void writeBE16(u8* destination, u16 value) {
    destination[0] = static_cast< u8 >(value >> 8);
    destination[1] = static_cast< u8 >(value);
}

// GX RGB5A3: opaque texels 1RRRRRGGGGGBBBBB, others 0AAARRRRGGGGBBBB.
u16 rgb5a3(u32 color) {
    const u32 r = color >> 24, g = (color >> 16) & 255, b = (color >> 8) & 255, a = color & 255;
    if (a >= 0xe0) {
        return static_cast< u16 >(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
    }
    return static_cast< u16 >(((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
}

void makeIcon(const u8* mii, u8* image, u32 width, u32 height, u32 background) {
    buildHead(sIconHead, mii, false);
    sHead = &sIconHead;
    const u32 size = width > height ? width : height;
    const u32 drawn = size > kIconLimit ? kIconLimit : size;
    for (u32 i = 0; i < drawn * drawn; i++) {
        sIconColor[i] = background;
        sIconDepth[i] = -1e30f;
    }
    if (sIconHead.vertexCount != 0) {
        // The head's outline from the front (the opaque parts), fitted with a small margin.
        f32 minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
        for (u32 partIndex = 0; partIndex < kPartMask; partIndex++) {
            const CflPart& part = sIconHead.parts[partIndex];
            for (u32 i = 0; i < part.count; i++) {
                const TVec3f& p = sIconHead.positions[part.first + i];
                minX = p.x < minX ? p.x : minX;
                maxX = p.x > maxX ? p.x : maxX;
                minY = p.y < minY ? p.y : minY;
                maxY = p.y > maxY ? p.y : maxY;
            }
        }
        const f32 extent = (maxX - minX) > (maxY - minY) ? (maxX - minX) : (maxY - minY);
        const f32 scale = extent > 0.0f ? drawn * 0.94f / extent : 1.0f;
        for (u32 partIndex = 0; partIndex < kPartCount; partIndex++) {
            rasterPart(sIconHead.parts[partIndex], partIndex >= kPartMask, drawn, scale, (minX + maxX) * 0.5f,
                       (minY + maxY) * 0.5f);
        }
    }
    // Into 4x4 tiles of big-endian texels, scaled when the icon is larger than the drawing.
    const u32 tilesX = (width + 3) / 4;
    for (u32 y = 0; y < ((height + 3) & ~3u); y++) {
        for (u32 x = 0; x < tilesX * 4; x++) {
            const u32 sx = (x < width ? x : width - 1) * drawn / size + (size - width) * drawn / size / 2;
            const u32 sy = (y < height ? y : height - 1) * drawn / size + (size - height) * drawn / size / 2;
            u8* tile = image + ((y / 4) * tilesX + x / 4) * 32;
            writeBE16(tile + ((y & 3) * 4 + (x & 3)) * 2, rgb5a3(sIconColor[sy * drawn + sx]));
        }
    }
    sHead = &sHeads[0];
}

}

u32 RFLGetWorkSize(BOOL) {
    return 32;
}

RFLErrcode RFLInitResAsync(void*, void*, u32, BOOL) {
    return RFLErrcode_Success;
}

RFLErrcode RFLGetAsyncStatus(void) {
    return RFLErrcode_Success;
}

s32 RFLGetLastReason(void) {
    return 0;
}

void RFLExit(void) {
}

BOOL RFLIsAvailableOfficialData(u16 index) {
    return miiRecord(RFLDataSource_Official, index) != nullptr;
}

BOOL RFLSearchOfficialData(const RFLCreateID* id, u16* index) {
    if (id == nullptr || index == nullptr) {
        return FALSE;
    }
    loadDatabase();
    for (u16 i = 0; i < RFL_DB_CHAR_MAX; i++) {
        RFLCreateID own;
        if (sDatabaseValid[i] && (makeCreateID(sDatabase[i], &own), memcmp(own.data, id->data, RFL_CREATEID_LEN) == 0)) {
            *index = i;
            return TRUE;
        }
    }
    return FALSE;
}

RFLErrcode RFLGetAdditionalInfo(RFLAdditionalInfo* info, RFLDataSource source, struct RFLMiddleDB*, u16 index) {
    const u8* record = miiRecord(source, index);
    if (info == nullptr || record == nullptr) {
        return RFLErrcode_NotAvailable;
    }
    sSelectedMii = record;
    memset(info, 0, sizeof(*info));
    memcpy(info->name, record + 0x1A, 20);
    memcpy(info->creator, record + 0x48, 20);
    makeCreateID(record, &info->createID);
    const u16 details = readLE16(record + 0x18);
    info->sex = details & 1;
    info->bmonth = (details >> 1) & 15;
    info->bday = (details >> 5) & 31;
    info->color = (details >> 10) & 15;
    info->favorite = (details >> 14) & 1;
    info->height = record[0x2e];
    info->build = record[0x2f];
    info->skinColor.r = miiSkinColor(miiSkinIndex(), 0);
    info->skinColor.g = miiSkinColor(miiSkinIndex(), 1);
    info->skinColor.b = miiSkinColor(miiSkinIndex(), 2);
    info->skinColor.a = 255;
    return RFLErrcode_Success;
}

u32 RFLGetModelBufferSize(RFLResolution, u32) {
    return 32;
}

RFLErrcode RFLInitCharModel(RFLCharModel* model, RFLDataSource source, struct RFLMiddleDB*, u16 index, void*, RFLResolution,
                            u32) {
    if (model == nullptr) {
        return RFLErrcode_WrongParam;
    }
    const u8* record = miiRecord(source, index);
    if (record == nullptr) {
        memset(model, 0, sizeof(*model));
        return RFLErrcode_NotAvailable;
    }
    MiiModelState* state = allocateModelState(model);
    if (state == nullptr) {
        memset(model, 0, sizeof(*model));
        return RFLErrcode_Fatal;
    }
    memset(model, 0, sizeof(*model));
    PSMTXIdentity(state->matrix);
    state->expression = RFLExp_Normal;
    memcpy(state->mii, record, kMiiSize);
    state->initialized = true;
    return RFLErrcode_Success;
}

RFLErrcode RFLMakeIcon(void* buffer, RFLDataSource source, RFLMiddleDB*, u16 index, RFLExpression, const RFLIconSetting* setting) {
    if (buffer == nullptr || setting == nullptr || setting->width == 0 || setting->height == 0) {
        return RFLErrcode_WrongParam;
    }
    const u8* record = miiRecord(source, index);
    if (record == nullptr) {
        return RFLErrcode_NotAvailable;
    }
    u32 background;
    if (setting->bgType == RFLIconBG_Favorite) {
        background = kFavoriteColors[(readLE16(record + 0x18) >> 10) % 12];
    } else {
        // Transparent, with the colour for the edges' filtering: the layout draws the icon's
        // own shape as its shadow.
        background = (static_cast< u32 >(setting->bgColor.r) << 24) | (static_cast< u32 >(setting->bgColor.g) << 16) |
                     (static_cast< u32 >(setting->bgColor.b) << 8);
    }
    u8* image = static_cast< u8* >(buffer) + sizeof(ResTIMG);
    makeIcon(record, image, setting->width, setting->height, background);
    rvl3DSInvalidateTextureImage(image);
    return RFLErrcode_Success;
}

void RFLSetMtx(RFLCharModel* model, const Mtx matrix) {
    MiiModelState* state = findModelState(model);
    if (state == nullptr || !state->initialized || matrix == nullptr) {
        return;
    }
    PSMTXCopy(matrix, state->matrix);
}

void RFLSetExpression(RFLCharModel* model, RFLExpression expression) {
    MiiModelState* state = findModelState(model);
    if (state == nullptr || !state->initialized || expression >= RFLExp_Max) {
        return;
    }
    state->expression = expression;
}

RFLExpression RFLGetExpression(const RFLCharModel* model) {
    MiiModelState* state = findModelState(model);
    return state != nullptr && state->initialized ? state->expression : RFLExp_Normal;
}

void RFLLoadVertexSetting(const RFLDrawCoreSetting* setting) {
    if (setting == nullptr || setting->txcGenNum > 1 || setting->tevStageNum > 3) {
        return;
    }
}

void RFLLoadMaterialSetting(const RFLDrawCoreSetting* setting) {
    if (setting == nullptr || setting->txcGenNum > 1 || setting->tevStageNum > 3) {
        return;
    }
}

void RFLDrawOpa(const RFLCharModel* model) {
    const MiiModelState* state = findModelState(model);
    if (state == nullptr || !state->initialized) {
        return;
    }
    drawHead(*state);
}

void RFLDrawXlu(const RFLCharModel*) {
}

void RFLDrawOpaCore(const RFLCharModel*, const RFLDrawCoreSetting*) {
}

void RFLDrawXluCore(const RFLCharModel*, const RFLDrawCoreSetting*) {
}

}  // extern "C"
