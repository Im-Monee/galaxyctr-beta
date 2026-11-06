// ##############################################################################################
// # Textures for the GPU                                                                       #
// #                                                                                            #
// # Every Wii texture the port draws becomes a GPU texture here: the JUTTextures of layouts,   #
// # particles and effects (rvl3DSGetJUTTexture), the textures of native materials              #
// # (rvl3DSGetNativeTIMGTexture), and images by address. A Wii texture is decoded from its GX  #
// # format into RGBA8 and uploaded (host/gfx.c picks the smallest GPU format for it); model    #
// # textures the converter already wrote in the GPU's layout are copied as they are.           #
// #                                                                                            #
// # The results are cached by image, palette and mode, and released least recently used first, #
// # but only once the GPU has finished every frame that reads them. Screen captures (EFB       #
// # copies) are textures made by the GPU, kept apart: their image is not texture data.         #
// ##############################################################################################

#include "JUTTexture3DS.hpp"
#include "host.h"
#include <JSystem/JUtility/JUTPalette.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <cstring>

namespace {
    enum {
        TF_I4 = 0x0,
        TF_I8 = 0x1,
        TF_IA4 = 0x2,
        TF_IA8 = 0x3,
        TF_RGB565 = 0x4,
        TF_RGB5A3 = 0x5,
        TF_RGBA8 = 0x6,
        TF_C4 = 0x8,
        TF_C8 = 0x9,
        TF_C14X2 = 0xA,
        TF_CMPR = 0xE,
    };

    struct CacheEntry {
        const ResTIMG* timg;
        const void* pixels;
        const void* palette;
        u32 format;
        u32 paletteFormat;
        u32 mode;
        u32 age;
        unsigned long frame;  // hostGfxFrameId of its last use
        void* handle;
    };

    // ##############################################################################################
    // # The texture cache                                                                          #
    // #                                                                                            #
    // # The cache entries, a hash of them for quick lookups, and the last textures used: most      #
    // # draws ask for the texture the draw before them used.                                       #
    // ##############################################################################################
    // Native model loading can expose well over 64 distinct materials before the first
    // complete frame. Keeping the cache large enough avoids repeated decode/upload churn
    // while the scene is still being assembled. 256 were too few for the Comet Observatory after
    // the Grand Star (0.14.4): with 7 MB of linear memory free, textures in use were replaced
    // and decoded again every frame, up to 3 s a frame in the map objects' draw.
    static CacheEntry sCache[1024];
    const u32 kCacheEntries = sizeof(sCache) / sizeof(sCache[0]);
    // Entry numbers (plus one) by key hash, checked against the entry: a lookup scanned all of
    // sCache (about 20 KB) for each texture unit of each material drawn, a few hundred times a
    // frame in the Comet Observatory (CIA 0.14.7). A stale or colliding slot falls back to the scan.
    static u16 sCacheIndex[2048];

    u32 cacheSlot(const void* timg, const void* pixels, u32 mode) {
        u32 h = (reinterpret_cast< u32 >(timg) >> 3) * 0x9E3779B1u ^ (reinterpret_cast< u32 >(pixels) >> 3) * 0x85EBCA6Bu ^ mode;
        return (h ^ (h >> 16)) & (sizeof(sCacheIndex) / sizeof(sCacheIndex[0]) - 1);
    }

    bool cacheMatches(const CacheEntry& e, const void* timg, const void* pixels, const void* palette, u32 format, u32 paletteFormat,
                      u32 mode) {
        return e.handle != nullptr && e.timg == timg && e.pixels == pixels && e.palette == palette && e.format == format &&
               e.paletteFormat == paletteFormat && e.mode == mode;
    }

    CacheEntry* findCached(const void* timg, const void* pixels, const void* palette, u32 format, u32 paletteFormat, u32 mode) {
        const u32 slot = cacheSlot(timg, pixels, mode);
        const u32 known = sCacheIndex[slot];
        if (known != 0 && cacheMatches(sCache[known - 1], timg, pixels, palette, format, paletteFormat, mode)) {
            return &sCache[known - 1];
        }
        for (u32 i = 0; i < kCacheEntries; i++) {
            if (cacheMatches(sCache[i], timg, pixels, palette, format, paletteFormat, mode)) {
                sCacheIndex[slot] = static_cast< u16 >(i + 1);
                return &sCache[i];
            }
        }
        return nullptr;
    }

    void rememberCached(const CacheEntry& e) {
        sCacheIndex[cacheSlot(e.timg, e.pixels, e.mode)] = static_cast< u16 >(&e - sCache + 1);
    }
    // Since the last rvl3DSLogTextureStats: images decoded, the ticks spent, and cached
    // textures replaced to make room (the cache full, not memory).
    static u32 sDecodes, sReplaced;
    static u64 sDecodeTicks;
    // Decoding scratch, RGBA8: 512x512 texels (1 MB), as many as a 1024x256 font sheet. A larger
    // image is decoded at 1/2^n of its size (every 2^n-th texel, decodeImage's shift).
    const u32 kScratchTexels = 512 * 512;
    static u32 sDecoded[kScratchTexels];
    static u32 sTextureUse;
    struct CaptureEntry {
        const JUTTexture* texture;
        const void* image;  // the texture's: JUTTextures made on the screen's ResTIMG (MR::getScreenResTIMG) share it
        void* handle;
        float coeffs[4];  // hostGfxCopyToTexture's mapping: u = c0 + c1 t, v = c2 + c3 s
    };
    static CaptureEntry sCaptures[32];
    // Bumped whenever sCaptures changes.
    static u32 sCaptureEpoch;

    // The texture rvl3DSGetJUTTexture found last, outside the captures: particles and other
    // DirectDraw draws ask for the same texture many times in a row, and the scans of sCaptures
    // (once per vertex in rvl3DSMapJUTTextureCoord) and sCache cost more than the draw. The
    // entry is checked against its key at every use, as the scan does.
    static const JUTTexture* sLastTexture;
    static CacheEntry* sLastEntry;
    static u32 sLastCaptureEpoch;
    // The cache entries of the textures used last, by texture address: particles alternate their
    // textures from one draw to the next, and each change scanned the 1024 entries (about 80 us a
    // particle draw in the Comet Observatory, CIA 0.20.14). Checked like sLastEntry.
    struct RecentTexture {
        const JUTTexture* texture;
        CacheEntry* entry;
        u32 epoch;
    };
    static RecentTexture sRecent[64];
    RecentTexture& recentSlot(const JUTTexture* texture) {
        return sRecent[(reinterpret_cast< u32 >(texture) >> 5) & 63];
    }
    void rememberTexture(const JUTTexture* texture, CacheEntry* entry) {
        RecentTexture& slot = recentSlot(texture);
        slot.texture = texture;
        slot.entry = entry;
        slot.epoch = sCaptureEpoch;
    }

    // ##############################################################################################
    // # Memory limits                                                                              #
    // #                                                                                            #
    // # How much linear memory the cache leaves free, and how large a texture may be: smaller in   #
    // # the 96 MB of an Old 3DS.                                                                   #
    // ##############################################################################################
    // Linear memory left free for the frame's EFB copies and new textures. Without a bound the
    // cache filled the linear heap within a few seconds of Good Egg (CIA 0.10.14) and textures
    // made later failed: HUD parts and others drawn white, black or not at all.
    const unsigned long kLinearReserve = 1536 * 1024;

    // Largest side kept for a texture. Model and effect textures are halved down to 128 (the top
    // screen is 400x240); layout and font images keep theirs, up to the GPU's 1024: font sheets
    // (1024x256) at 128 left the text unreadable and blurred (CIA 0.10.15).
    const u32 kModelTextureLimit = 128;
    // A quarter of the memory where the linear heap is 22 MB instead of 32 (Old 3DS).
    const u32 kModelTextureLowLimit = 64;
    const u32 kLayoutTextureLimit = 1024;

    // Frees at once the least recently used cached texture the GPU no longer reads (last used in a
    // frame it has finished). Returns false when there is none.
    bool evictFinishedTexture() {
        CacheEntry* oldest = nullptr;
        const unsigned long completed = hostGfxCompletedFrameId();
        for (CacheEntry& entry : sCache) {
            if (entry.handle != nullptr && entry.frame <= completed && (oldest == nullptr || entry.age < oldest->age)) {
                oldest = &entry;
            }
        }
        if (oldest == nullptr) {
            return false;
        }
        hostGfxTextureDestroyNow(oldest->handle);
        oldest->handle = nullptr;
        return true;
    }

    // ##############################################################################################
    // # Decoding the GX texture formats                                                            #
    // #                                                                                            #
    // # The Wii's formats (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CMPR and the palette formats   #
    // # C4, C8, C14X2), stored big-endian in tiles, decoded into RGBA8 rows; an image too large    #
    // # for the scratch is decoded at half size, then fitted to the GPU's power-of-two sizes.      #
    // ##############################################################################################
    inline u16 readBE16(const u8* p) {
        return static_cast<u16>((static_cast<u16>(p[0]) << 8) | p[1]);
    }

    inline u32 rgba(u32 r, u32 g, u32 b, u32 a) {
        return (r << 24) | (g << 16) | (b << 8) | a;
    }

    u32 decodeRGB565(u16 v) {
        u32 r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
        return rgba((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
    }

    u32 decodeRGB5A3(u16 v) {
        if (v & 0x8000) {
            u32 r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
            return rgba((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255);
        }
        u32 a = (v >> 12) & 7, r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
        return rgba(r * 17, g * 17, b * 17, (a << 5) | (a << 2) | (a >> 1));
    }

    u32 decodeIA8(u16 v) {
        u32 i = v & 0xFF;
        return rgba(i, i, i, v >> 8);
    }

    u32 decodePalette(u16 v, u32 format) {
        if (format == 0) return decodeIA8(v);
        if (format == 1) return decodeRGB565(v);
        return decodeRGB5A3(v);
    }

    void blockSize(u32 format, u32& width, u32& height) {
        switch (format) {
        case TF_I4:
        case TF_C4:
        case TF_CMPR: width = height = 8; break;
        case TF_I8:
        case TF_IA4:
        case TF_C8: width = 8; height = 4; break;
        default: width = height = 4; break;
        }
    }

    // As the Wii's texture unit decodes CMPR (Dolphin's DecodeDXTBlock, from hardware tests): the
    // two middle colours are 3/8 blends (5 c0 + 3 c1 and 3 c0 + 5 c1, >> 3), not thirds, and the
    // transparent texel keeps the average colour with alpha 0, not black (filtering would darken
    // the edges of cut-out textures).
    void decodeCMPR(const u8* src, u32* dst, u32 stride) {
        u16 c0 = readBE16(src), c1 = readBE16(src + 2);
        u32 colors[4] = {decodeRGB565(c0), decodeRGB565(c1), 0, 0};
        const auto channel = [](u32 c, int shift) { return (c >> shift) & 0xFF; };
        if (c0 > c1) {
            for (int k = 0; k < 2; k++) {
                int w0 = k == 0 ? 5 : 3, w1 = k == 0 ? 3 : 5;
                colors[2 + k] = rgba((channel(colors[0], 24) * w0 + channel(colors[1], 24) * w1) >> 3,
                                     (channel(colors[0], 16) * w0 + channel(colors[1], 16) * w1) >> 3,
                                     (channel(colors[0], 8) * w0 + channel(colors[1], 8) * w1) >> 3, 255);
            }
        } else {
            const u32 r = (channel(colors[0], 24) + channel(colors[1], 24)) / 2, g = (channel(colors[0], 16) + channel(colors[1], 16)) / 2,
                      b = (channel(colors[0], 8) + channel(colors[1], 8)) / 2;
            colors[2] = rgba(r, g, b, 255);
            colors[3] = rgba(r, g, b, 0);
        }
        for (int y = 0; y < 4; y++) {
            u8 bits = src[4 + y];
            for (int x = 0; x < 4; x++) dst[y * stride + x] = colors[(bits >> (6 - x * 2)) & 3];
        }
    }

    // Model textures the converter stored in the GPU's own layout (texreduce.py), past the GX formats.
    const u32 kNativeFormatFirst = 0x20;
    const u32 kNativeFormatCount = 3;

    bool isNativeFormat(u32 format) {
        return format >= kNativeFormatFirst && format < kNativeFormatFirst + kNativeFormatCount;
    }

    bool validNativeImage(u32 address, u32 format, u32 width, u32 height) {
        const u32 bits = format == kNativeFormatFirst ? 4 : 8;  // ETC1, then ETC1A4 and LA4
        return width >= 8 && height >= 8 && width <= 1024 && height <= 1024 && address != 0 &&
               hostIsValidRange(address, width * height * bits / 8);
    }

    bool validImage(u32 address, u32 format, u32 width, u32 height) {
        static const u8 bitsPerPixel[16] = {4, 8, 8, 16, 16, 16, 32, 0, 4, 8, 16, 0, 0, 0, 4, 0};
        if (format >= 16 || width == 0 || height == 0 || width > 1024 || height > 1024 || bitsPerPixel[format] == 0) return false;
        u32 bytes = ((width + 7) & ~7) * ((height + 7) & ~7) * bitsPerPixel[format] / 8;
        return address != 0 && hostIsValidRange(address, bytes);
    }

    void decodeImage(const u8* src, u32 width, u32 height, u32 format, const u8* palette, u32 paletteFormat, u32* dst, u32 shift = 0) {
        // Texel (x, y) of the image, kept when both are multiples of 2^shift.
        const u32 mask = (1u << shift) - 1, outWidth = (width + mask) >> shift;
        auto store = [&](u32 x, u32 y, u32 color) {
            if (((x | y) & mask) == 0) dst[(y >> shift) * outWidth + (x >> shift)] = color;
        };
        u32 blockW, blockH;
        blockSize(format, blockW, blockH);
        for (u32 by = 0; by < height; by += blockH) {
            for (u32 bx = 0; bx < width; bx += blockW) {
                if (format == TF_CMPR) {
                    for (u32 sub = 0; sub < 4; sub++) {
                        u32 sx = bx + (sub & 1) * 4, sy = by + (sub >> 1) * 4, block[16];
                        decodeCMPR(src, block, 4);
                        src += 8;
                        for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < 4; x++)
                            if (sx + x < width && sy + y < height) store(sx + x, sy + y, block[y * 4 + x]);
                    }
                    continue;
                }
                if (format == TF_RGBA8) {
                    for (u32 i = 0; i < 16; i++) {
                        u32 x = bx + (i & 3), y = by + (i >> 2);
                        if (x < width && y < height) store(x, y, rgba(src[i * 2 + 1], src[32 + i * 2], src[32 + i * 2 + 1], src[i * 2]));
                    }
                    src += 64;
                    continue;
                }
                for (u32 y = 0; y < blockH; y++) for (u32 x = 0; x < blockW; x++) {
                    u32 color;
                    switch (format) {
                    case TF_I4: { u8 v = src[(y * blockW + x) / 2]; u32 i = ((x & 1) ? (v & 0xF) : (v >> 4)) * 17; color = rgba(i, i, i, i); break; }
                    case TF_I8: { u32 i = src[y * blockW + x]; color = rgba(i, i, i, i); break; }
                    case TF_IA4: { u8 v = src[y * blockW + x]; u32 i = (v & 0xF) * 17; color = rgba(i, i, i, (v >> 4) * 17); break; }
                    case TF_IA8: color = decodeIA8(readBE16(src + (y * blockW + x) * 2)); break;
                    case TF_RGB565: color = decodeRGB565(readBE16(src + (y * blockW + x) * 2)); break;
                    case TF_RGB5A3: color = decodeRGB5A3(readBE16(src + (y * blockW + x) * 2)); break;
                    case TF_C4: { u8 v = src[(y * blockW + x) / 2]; u32 i = (x & 1) ? (v & 0xF) : (v >> 4); color = decodePalette(readBE16(palette + i * 2), paletteFormat); break; }
                    case TF_C8: color = decodePalette(readBE16(palette + src[y * blockW + x] * 2), paletteFormat); break;
                    case TF_C14X2: { u32 i = readBE16(src + (y * blockW + x) * 2) & 0x3FFF; color = decodePalette(readBE16(palette + i * 2), paletteFormat); break; }
                    default: color = rgba(255, 0, 255, 255); break;
                    }
                    if (bx + x < width && by + y < height) store(bx + x, by + y, color);
                }
                src += 32;
            }
        }
    }

    // Decodes the image into sDecoded, at half size as many times as the scratch needs; width and
    // height become the decoded size.
    void decodeToScratch(const u8* src, u32& width, u32& height, u32 format, const u8* palette, u32 paletteFormat) {
        const u64 start = hostGetTick();
        sDecodes++;
        u32 shift = 0;
        while ((((width + (1u << shift) - 1) >> shift) * ((height + (1u << shift) - 1) >> shift)) > kScratchTexels) shift++;
        decodeImage(src, width, height, format, palette, paletteFormat, sDecoded, shift);
        width = (width + (1u << shift) - 1) >> shift;
        height = (height + (1u << shift) - 1) >> shift;
        sDecodeTicks += hostGetTick() - start;
    }

    // Halves the image until neither side is above limit, then stretches it to its power-of-two size.
    void fitImage(u32* pixels, u32& width, u32& height, u32& potWidth, u32& potHeight, u32 limit) {
        while ((width > limit || height > limit) && potWidth > 8 && potHeight > 8) {
            u32 newWidth = (width + 1) / 2, newHeight = (height + 1) / 2;
            for (u32 y = 0; y < newHeight; y++) {
                const u32* row0 = pixels + y * 2 * width;
                const u32* row1 = pixels + (y * 2 + 1 < height ? y * 2 + 1 : y * 2) * width;
                for (u32 x = 0; x < newWidth; x++) {
                    u32 x0 = x * 2, x1 = x * 2 + 1 < width ? x * 2 + 1 : x * 2;
                    u32 a = row0[x0], b = row0[x1], c = row1[x0], d = row1[x1], out = 0;
                    for (u32 shift = 0; shift < 32; shift += 8) {
                        u32 sum = ((a >> shift) & 255) + ((b >> shift) & 255) + ((c >> shift) & 255) + ((d >> shift) & 255);
                        out |= ((sum + 2) / 4) << shift;
                    }
                    pixels[y * newWidth + x] = out;
                }
            }
            width = newWidth; height = newHeight; potWidth >>= 1; potHeight >>= 1;
        }
        if (width == potWidth && height == potHeight) return;
        for (u32 y = potHeight; y-- > 0;) {
            const u32* src = pixels + (y * height / potHeight) * width;
            u32* dst = pixels + y * potWidth;
            for (u32 x = potWidth; x-- > 0;) dst[x] = src[x * width / potWidth];
        }
        width = potWidth; height = potHeight;
    }
}

// ##############################################################################################
// # For other users                                                                            #
// #                                                                                            #
// # The decoding scratch for other code, and freeing cached textures for the movie decoder     #
// # (host/movie.c).                                                                            #
// ##############################################################################################
u32* rvl3DSTextureScratch() {
    return sDecoded;
}

// Frees cached textures the GPU no longer reads, oldest first, until bytes of linear memory are
// free or none is left (they are decoded again when drawn). For the movie decoder (movie.c).
extern "C" void rvl3DSFreeTextureMemory(unsigned long bytes) {
    while (hostGfxLinearFree() < bytes && evictFinishedTexture()) {
    }
}

// Frees the oldest cached texture the GPU no longer reads; 0 when there is none. Free linear
// memory in total is not enough for the movie decoder, which needs each buffer in one piece:
// movie.c frees one texture at a time until the buffer can be allocated (CIA 0.21.1, 4.1 MB free
// but no 1.9 MB block: "decoder not started").
extern "C" int rvl3DSFreeOneTexture(void) {
    return evictFinishedTexture() ? 1 : 0;
}

namespace {
    struct TexObj3DS {
        void* image;
        u16 width, height;
        u8 format, wrapS, wrapT, mipmap;
        u8 minFilter, magFilter, biasClamp, edgeLod, anisotropy, tlut, pad[2];
        f32 minLod, maxLod, lodBias;
    };
    static_assert(sizeof(TexObj3DS) <= sizeof(GXTexObj), "TexObj3DS must fit in a GXTexObj");
    // Uploads sDecoded, keeping kLinearReserve free and evicting finished textures when the
    // upload does not fit.
    void* createTexture(u32 width, u32 height, u32 potWidth, u32 potHeight, u32 mode) {
        while (hostGfxLinearFree() < kLinearReserve && evictFinishedTexture()) {
        }
        for (;;) {
            void* handle = hostGfxTextureCreate(reinterpret_cast<const unsigned long*>(sDecoded), width, height, potWidth, potHeight,
                                                mode & 3, (mode >> 2) & 3, (mode >> 4) & 1, (mode >> 5) & 1);
            if (handle != nullptr || !evictFinishedTexture()) {
                return handle;
            }
        }
    }

    // As createTexture, for an image already in the GPU's layout: nothing to decode or encode, only
    // copied. Encoding ETC1 here took tens of milliseconds a texture, in the draw: with 123 new ones
    // at once, the stop on the bridge to Good Egg's DiskGardenPlanet (CIA 0.18.8 log).
    void* createNativeTexture(const void* image, u32 format, u32 width, u32 height, u32 mode) {
        const u64 start = hostGetTick();
        sDecodes++;
        while (hostGfxLinearFree() < kLinearReserve && evictFinishedTexture()) {
        }
        void* handle;
        do {
            handle = hostGfxTextureCreateNative(image, format - kNativeFormatFirst, width, height, mode & 3, (mode >> 2) & 3, (mode >> 4) & 1,
                                                (mode >> 5) & 1);
        } while (handle == nullptr && evictFinishedTexture());
        sDecodeTicks += hostGetTick() - start;
        return handle;
    }
}  // namespace

// ##############################################################################################
// # GX texture objects                                                                         #
// #                                                                                            #
// # GXTexObj kept as data (the image, size, format and sampling), since GX itself is not part  #
// # of the 3DS build, and the texture buffer sizes the game computes.                          #
// ##############################################################################################
void rvl3DSInitTexObj(GXTexObj* obj, void* image, u16 width, u16 height, u32 format, u32 wrapS, u32 wrapT, bool mipmap, u32 tlut) {
    TexObj3DS* t = reinterpret_cast< TexObj3DS* >(obj);
    memset(obj, 0, sizeof(GXTexObj));
    t->image = image;
    t->width = width;
    t->height = height;
    t->format = static_cast< u8 >(format);
    t->wrapS = static_cast< u8 >(wrapS);
    t->wrapT = static_cast< u8 >(wrapT);
    t->mipmap = mipmap;
    t->tlut = static_cast< u8 >(tlut);
    t->minFilter = t->magFilter = GX_LINEAR;  // GXInitTexObj's defaults
    t->edgeLod = 1;
    t->maxLod = mipmap ? 10.0f : 0.0f;
}

void rvl3DSInitTexObjLOD(GXTexObj* obj, u32 minFilter, u32 magFilter, f32 minLod, f32 maxLod, f32 lodBias, bool biasClamp, bool edgeLod,
                         u32 anisotropy) {
    TexObj3DS* t = reinterpret_cast< TexObj3DS* >(obj);
    t->minFilter = static_cast< u8 >(minFilter);
    t->magFilter = static_cast< u8 >(magFilter);
    t->minLod = minLod;
    t->maxLod = maxLod;
    t->lodBias = lodBias;
    t->biasClamp = biasClamp;
    t->edgeLod = edgeLod;
    t->anisotropy = static_cast< u8 >(anisotropy);
}

void rvl3DSGetTexObjAll(const GXTexObj* obj, void** image, u16* width, u16* height, GXTexFmt* format, GXTexWrapMode* wrapS,
                        GXTexWrapMode* wrapT, GXBool* mipmap) {
    const TexObj3DS* t = reinterpret_cast< const TexObj3DS* >(obj);
    *image = t->image;
    *width = t->width;
    *height = t->height;
    *format = static_cast< GXTexFmt >(t->format);
    *wrapS = static_cast< GXTexWrapMode >(t->wrapS);
    *wrapT = static_cast< GXTexWrapMode >(t->wrapT);
    *mipmap = t->mipmap;
}

void rvl3DSGetTexObjLODAll(const GXTexObj* obj, GXTexFilter* minFilter, GXTexFilter* magFilter, f32* minLod, f32* maxLod, f32* lodBias,
                           GXBool* biasClamp, GXBool* edgeLod, GXAnisotropy* anisotropy) {
    const TexObj3DS* t = reinterpret_cast< const TexObj3DS* >(obj);
    *minFilter = static_cast< GXTexFilter >(t->minFilter);
    *magFilter = static_cast< GXTexFilter >(t->magFilter);
    *minLod = t->minLod;
    *maxLod = t->maxLod;
    *lodBias = t->lodBias;
    *biasClamp = t->biasClamp;
    *edgeLod = t->edgeLod;
    *anisotropy = static_cast< GXAnisotropy >(t->anisotropy);
}

u32 rvl3DSGetTexObjTlut(const GXTexObj* obj) {
    return reinterpret_cast< const TexObj3DS* >(obj)->tlut;
}

// GXGetTexBufferSize without mipmaps (RVL_SDK GXTexture.c): whole 32-byte tiles (64 for RGBA8 and
// Z24X8) of 8x8, 8x4 or 4x4 texels by bits per texel.
u32 rvl3DSTexBufferSize(u16 width, u16 height, u32 format) {
    u32 shiftX = 0, shiftY = 0;
    switch (format) {
    case GX_TF_C4: case GX_TF_I4: case GX_TF_CMPR: case GX_CTF_R4: case GX_CTF_Z4:
        shiftX = 3; shiftY = 3;
        break;
    case GX_TF_C8: case GX_TF_I8: case GX_TF_IA4: case GX_TF_A8: case GX_TF_Z8: case GX_CTF_RA4: case GX_CTF_R8:
    case GX_CTF_G8: case GX_CTF_B8: case GX_CTF_Z8M: case GX_CTF_Z8L:
        shiftX = 3; shiftY = 2;
        break;
    case GX_TF_C14X2: case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: case GX_TF_RGBA8: case GX_TF_Z16:
    case GX_TF_Z24X8: case GX_CTF_RA8: case GX_CTF_RG8: case GX_CTF_GB8: case GX_CTF_Z16L:
        shiftX = 2; shiftY = 2;
        break;
    default:
        break;
    }
    const u32 tileBytes = format == GX_TF_RGBA8 || format == GX_TF_Z24X8 ? 64 : 32;
    return ((width + (1u << shiftX) - 1) >> shiftX) * ((height + (1u << shiftY) - 1) >> shiftY) * tileBytes;
}

// A texture captured from the screen (rvl3DSCaptureJUTTexture): its image is not texture data.

// ##############################################################################################
// # Looking up textures                                                                        #
// #                                                                                            #
// # A JUTTexture's GPU texture: a capture's when it is one, else the cached one, decoded and   #
// # uploaded on first use. A texture whose image changes is invalidated.                       #
// ##############################################################################################
bool rvl3DSIsEfbCopy(const JUTTexture* texture) {
    if (texture == nullptr) return false;
    for (const CaptureEntry& capture : sCaptures) {
        if (capture.texture == texture) return true;
    }
    return false;
}

void rvl3DSInvalidateJUTTexture(const JUTTexture* texture) {
    if (texture == nullptr) return;
    for (CaptureEntry& capture : sCaptures) {
        if (capture.texture == texture) {
            if (capture.handle != nullptr) {
                hostGfxTextureDestroy(capture.handle);
            }
            std::memset(&capture, 0, sizeof(capture));
            sCaptureEpoch++;
        }
    }
    for (CacheEntry& entry : sCache) {
        if (entry.handle != nullptr && entry.timg == texture->mTIMG && entry.pixels == texture->mImage) {
            hostGfxTextureDestroy(entry.handle);
            std::memset(&entry, 0, sizeof(entry));
        }
    }
}

void rvl3DSInvalidateTextureImage(const void* image) {
    if (image == nullptr) return;
    for (CacheEntry& entry : sCache) {
        if (entry.handle != nullptr && entry.pixels == image) {
            hostGfxTextureDestroy(entry.handle);
            std::memset(&entry, 0, sizeof(entry));
        }
    }
    sLastTexture = nullptr;
    std::memset(sRecent, 0, sizeof(sRecent));
}

void* rvl3DSGetJUTTexture(const JUTTexture* texture) {
    if (texture == nullptr || !hostIsValidRange(reinterpret_cast<u32>(texture), sizeof(*texture))) return nullptr;
    CacheEntry* known = texture == sLastTexture && sLastCaptureEpoch == sCaptureEpoch ? sLastEntry : nullptr;
    if (known == nullptr) {
        const RecentTexture& recent = recentSlot(texture);
        known = recent.texture == texture && recent.epoch == sCaptureEpoch ? recent.entry : nullptr;
    }
    if (known != nullptr) {
        CacheEntry& entry = *known;
        const JUTPalette* palette = texture->mPalette;
        const u32 format = entry.format;
        const bool indexed = format == TF_C4 || format == TF_C8 || format == TF_C14X2;
        const void* paletteData = indexed && palette != nullptr ? palette->mColorTable : nullptr;
        const u32 mode = (texture->mWrapS & 3) | ((texture->mWrapT & 3) << 2) | ((texture->mMagType & 1) << 4) | ((texture->mMinType & 1) << 5);
        if (entry.handle != nullptr && entry.timg == texture->mTIMG && entry.pixels == texture->mImage && entry.timg != nullptr &&
            entry.format == texture->mTIMG->mFormat && entry.palette == paletteData &&
            entry.paletteFormat == (indexed && palette != nullptr ? palette->mFormat : 0) && entry.mode == mode) {
            entry.age = ++sTextureUse;
            entry.frame = hostGfxFrameId();
            sLastTexture = texture;
            sLastEntry = known;
            sLastCaptureEpoch = sCaptureEpoch;
            return entry.handle;
        }
        sLastTexture = nullptr;
    }
    for (CaptureEntry& capture : sCaptures) {
        if (capture.texture == texture || (capture.texture != nullptr && texture->mImage != nullptr && capture.image == texture->mImage)) {
            return capture.handle;
        }
    }
    if (texture->mTIMG == nullptr || !hostIsValidRange(reinterpret_cast<u32>(texture->mTIMG), sizeof(*texture->mTIMG)) ||
        texture->mImage == nullptr || rvl3DSIsEfbCopy(texture)) return nullptr;
    const ResTIMG* timg = texture->mTIMG;
    const u32 width = timg->mWidth, height = timg->mHeight, format = timg->mFormat;
    const u32 address = reinterpret_cast<u32>(texture->mImage);
    const JUTPalette* palette = texture->mPalette;
    const bool indexed = format == TF_C4 || format == TF_C8 || format == TF_C14X2;
    const bool native = isNativeFormat(format);
    if (native ? !validNativeImage(address, format, width, height) : !validImage(address, format, width, height)) return nullptr;
    if (indexed) {
        if (palette == nullptr || !hostIsValidRange(reinterpret_cast<u32>(palette), sizeof(*palette)) ||
            palette->mColorTable == nullptr || palette->mLutNum == 0 ||
            !hostIsValidRange(reinterpret_cast<u32>(palette->mColorTable), palette->mLutNum * sizeof(u16))) {
            return nullptr;
        }
    }

    const void* paletteData = indexed ? palette->mColorTable : nullptr;
    const u32 paletteFormat = indexed ? palette->mFormat : 0;
    const u32 mode = (texture->mWrapS & 3) | ((texture->mWrapT & 3) << 2) | ((texture->mMagType & 1) << 4) | ((texture->mMinType & 1) << 5);
    for (CacheEntry& entry : sCache) {
        if (entry.handle != nullptr && entry.timg == timg && entry.pixels == texture->mImage && entry.palette == paletteData &&
            entry.format == format && entry.paletteFormat == paletteFormat && entry.mode == mode) {
            entry.age = ++sTextureUse;
            entry.frame = hostGfxFrameId();
            sLastTexture = texture;
            sLastEntry = &entry;
            sLastCaptureEpoch = sCaptureEpoch;
            rememberTexture(texture, &entry);
            return entry.handle;
        }
    }
    CacheEntry* slot = nullptr;
    for (CacheEntry& entry : sCache) {
        if (entry.handle == nullptr) { slot = &entry; break; }
        if (slot == nullptr || entry.age < slot->age) slot = &entry;
    }
    if (slot == nullptr) {
        return nullptr;
    }
    if (slot->handle != nullptr) {
        sReplaced++;
        hostGfxTextureDestroy(slot->handle);
        slot->handle = nullptr;
    }

    void* handle;
    if (native) {
        handle = createNativeTexture(texture->mImage, format, width, height, mode);
    } else {
        const u8* paletteBytes = indexed ? reinterpret_cast<const u8*>(paletteData) : nullptr;
        u32 decodedWidth = width, decodedHeight = height;
        decodeToScratch(reinterpret_cast<const u8*>(address), decodedWidth, decodedHeight, format, paletteBytes, paletteFormat);
        u32 potWidth = 8, potHeight = 8;
        while (potWidth < decodedWidth) potWidth <<= 1;
        while (potHeight < decodedHeight) potHeight <<= 1;
        u32 uploadWidth = decodedWidth, uploadHeight = decodedHeight;
        fitImage(sDecoded, uploadWidth, uploadHeight, potWidth, potHeight, hostLowMemory() ? kModelTextureLowLimit : kModelTextureLimit);
        handle = createTexture(uploadWidth, uploadHeight, potWidth, potHeight, mode);
    }
    if (handle == nullptr) return nullptr;
    slot->timg = timg;
    slot->pixels = texture->mImage;
    slot->palette = paletteData;
    slot->format = format;
    slot->paletteFormat = paletteFormat;
    slot->mode = mode;
    slot->age = ++sTextureUse;
    slot->frame = hostGfxFrameId();
    slot->handle = handle;
    sLastTexture = texture;
    sLastEntry = slot;
    sLastCaptureEpoch = sCaptureEpoch;
    rememberTexture(texture, slot);
    return handle;
}

// ##############################################################################################
// # Screen captures                                                                            #
// #                                                                                            #
// # JUTTexture::capture on the Wii copies the frame buffer into the texture; here the GPU      #
// # copies its render target (host/gfx.c, hostGfxCopyToTexture), and texture coordinates over  #
// # the capture are mapped onto the copy, which is stored on its side.                         #
// ##############################################################################################
void rvl3DSCaptureJUTTexture(JUTTexture* texture) {
    rvl3DSCaptureJUTTextureRegion(texture, 0.0f, 0.0f, 640.0f, 456.0f);
}

void rvl3DSMapJUTTextureCoord(const JUTTexture* texture, f32 s, f32 t, f32& u, f32& v) {
    if (texture == sLastTexture && texture != nullptr && sLastCaptureEpoch == sCaptureEpoch) {
        u = s;  // not a capture (sLastTexture)
        v = t;
        return;
    }
    for (const CaptureEntry& capture : sCaptures) {
        if (texture != nullptr && capture.handle != nullptr &&
            (capture.texture == texture || (texture->mImage != nullptr && capture.image == texture->mImage))) {
            u = capture.coeffs[0] + capture.coeffs[1] * t;
            v = capture.coeffs[2] + capture.coeffs[3] * s;
            return;
        }
    }
    u = s;
    v = t;
}

void rvl3DSCaptureJUTTextureRegion(JUTTexture* texture, f32 x0, f32 y0, f32 x1, f32 y1) {
    if (texture == nullptr) {
        return;
    }
    float coeffs[4];
    void* handle = hostGfxCopyToTexture(nullptr, x0, y0, x1, y1, coeffs);
    if (handle == nullptr) {
        return;
    }
    CaptureEntry* slot = nullptr;
    for (CaptureEntry& capture : sCaptures) {
        if (capture.texture == texture) {
            slot = &capture;
            break;
        }
        if (slot == nullptr && capture.texture == nullptr) {
            slot = &capture;
        }
    }
    if (slot == nullptr) {
        slot = &sCaptures[0];
        if (slot->handle != nullptr) {
            hostGfxTextureDestroy(slot->handle);
        }
    } else if (slot->handle != nullptr && slot->handle != handle) {
        hostGfxTextureDestroy(slot->handle);
    }
    slot->texture = texture;
    slot->image = texture->mImage;
    slot->handle = handle;
    for (int c = 0; c < 4; c++) {
        slot->coeffs[c] = coeffs[c];
    }
    sCaptureEpoch++;
}

void* rvl3DSGetNativeTIMGCapture(const ResTIMG* timg, f32* coeffs) {
    if (timg == nullptr) return nullptr;
    const u8* image = reinterpret_cast<const u8*>(timg) + (timg->mImageDataOffset ? timg->mImageDataOffset : 0x20);
    for (const CaptureEntry& capture : sCaptures) {
        if (capture.texture != nullptr && capture.handle != nullptr && capture.texture->mImage == image) {
            for (int c = 0; c < 4; c++) {
                coeffs[c] = capture.coeffs[c];
            }
            return capture.handle;
        }
    }
    return nullptr;
}

void* rvl3DSGetNativeTIMGTexture(const ResTIMG* timg) {
    if (timg == nullptr) return nullptr;
    const u32 width = timg->mWidth, height = timg->mHeight, format = timg->mFormat;
    const u8* image = reinterpret_cast<const u8*>(timg) + (timg->mImageDataOffset ? timg->mImageDataOffset : 0x20);
    const u32 address = reinterpret_cast<u32>(image);
    const bool indexed = format == TF_C4 || format == TF_C8 || format == TF_C14X2;
    const bool native = isNativeFormat(format);
    if (native ? !validNativeImage(address, format, width, height) : !validImage(address, format, width, height)) return nullptr;
    const u8* palette = nullptr;
    const u32 paletteBytes = static_cast<u32>(timg->mPaletteNum) * sizeof(u16);
    if (indexed) {
        if (timg->mPaletteDataOffset == 0 || timg->mPaletteNum == 0) return nullptr;
        palette = reinterpret_cast<const u8*>(timg) + timg->mPaletteDataOffset;
        if (!hostIsValidRange(reinterpret_cast<u32>(palette), paletteBytes)) return nullptr;
    }
    const u32 paletteFormat = indexed ? timg->mPaletteFormat : 0;

    const u32 mode = (timg->mWrapS & 3) | ((timg->mWrapT & 3) << 2) | ((timg->mMagType & 1) << 4) | ((timg->mMinType & 1) << 5);
    if (CacheEntry* entry = findCached(timg, image, palette, format, paletteFormat, mode)) {
        entry->age = ++sTextureUse;
        entry->frame = hostGfxFrameId();
        return entry->handle;
    }
    CacheEntry* slot = nullptr;
    for (CacheEntry& entry : sCache) {
        if (entry.handle == nullptr) { slot = &entry; break; }
        if (slot == nullptr || entry.age < slot->age) slot = &entry;
    }
    if (slot == nullptr) return nullptr;
    if (slot->handle != nullptr) {
        sReplaced++;
        hostGfxTextureDestroy(slot->handle);
        slot->handle = nullptr;
    }

    void* handle;
    if (native) {
        handle = createNativeTexture(image, format, width, height, mode);
    } else {
        u32 decodedWidth = width, decodedHeight = height;
        decodeToScratch(image, decodedWidth, decodedHeight, format, palette, paletteFormat);
        u32 potWidth = 8, potHeight = 8;
        while (potWidth < decodedWidth) potWidth <<= 1;
        while (potHeight < decodedHeight) potHeight <<= 1;
        u32 uploadWidth = decodedWidth, uploadHeight = decodedHeight;
        fitImage(sDecoded, uploadWidth, uploadHeight, potWidth, potHeight, hostLowMemory() ? kModelTextureLowLimit : kModelTextureLimit);
        handle = createTexture(uploadWidth, uploadHeight, potWidth, potHeight, mode);
    }
    if (handle == nullptr) return nullptr;
    slot->timg = timg;
    slot->pixels = image;
    slot->palette = palette;
    slot->format = format;
    slot->paletteFormat = paletteFormat;
    slot->mode = mode;
    slot->age = ++sTextureUse;
    slot->frame = hostGfxFrameId();
    slot->handle = handle;
    rememberCached(*slot);
    return handle;
}

void* rvl3DSGetNativeTexture(const void* image, u16 width, u16 height, u32 format, const void* palette, u32 paletteFormat,
                             u16 paletteEntries, u8 wrapS, u8 wrapT, u8 magFilter) {
    static u32 sInvalidTextureReports;
    if (image == nullptr || width == 0 || height == 0 || !validImage(reinterpret_cast<u32>(image), format, width, height)) {
        if (sInvalidTextureReports < 8) {
            hostLog("native texture invalid image=%p size=%ux%u format=%lu\n", image, width, height, static_cast< unsigned long >(format));
            sInvalidTextureReports++;
        }
        return nullptr;
    }
    const bool indexed = format == TF_C4 || format == TF_C8 || format == TF_C14X2;
    if (indexed && (palette == nullptr || paletteEntries == 0 ||
                    !hostIsValidRange(reinterpret_cast<u32>(palette), static_cast<u32>(paletteEntries) * sizeof(u16)))) return nullptr;
    const u32 mode = (wrapS & 3) | ((wrapT & 3) << 2) | ((magFilter & 1) << 4) | ((magFilter & 1) << 5);
    if (CacheEntry* entry = findCached(image, image, palette, format, paletteFormat, mode)) {
        entry->age = ++sTextureUse;
        entry->frame = hostGfxFrameId();
        return entry->handle;
    }
    CacheEntry* slot = nullptr;
    for (CacheEntry& entry : sCache) {
        if (entry.handle == nullptr) { slot = &entry; break; }
        if (slot == nullptr || entry.age < slot->age) slot = &entry;
    }
    if (slot == nullptr) return nullptr;
    if (slot->handle != nullptr) {
        sReplaced++;
        hostGfxTextureDestroy(slot->handle);
    }
    u32 decodedWidth = width, decodedHeight = height;
    decodeToScratch(reinterpret_cast<const u8*>(image), decodedWidth, decodedHeight, format,
                    indexed ? reinterpret_cast<const u8*>(palette) : nullptr, indexed ? paletteFormat : 0);
    u32 potWidth = 8, potHeight = 8;
    while (potWidth < decodedWidth) potWidth <<= 1;
    while (potHeight < decodedHeight) potHeight <<= 1;
    u32 uploadWidth = decodedWidth, uploadHeight = decodedHeight;
    fitImage(sDecoded, uploadWidth, uploadHeight, potWidth, potHeight, kLayoutTextureLimit);
    void* handle = createTexture(uploadWidth, uploadHeight, potWidth, potHeight, mode);
    if (handle == nullptr) return nullptr;
    slot->timg = reinterpret_cast<const ResTIMG*>(image);
    slot->pixels = image;
    slot->palette = palette;
    slot->format = format;
    slot->paletteFormat = paletteFormat;
    slot->mode = mode;
    slot->age = ++sTextureUse;
    slot->frame = hostGfxFrameId();
    slot->handle = handle;
    rememberCached(*slot);
    return handle;
}

// ##############################################################################################
// # Statistics                                                                                 #
// #                                                                                            #
// # Images decoded, time spent, and textures replaced because the cache was full, for the      #
// # periodic log (native.cpp).                                                                 #
// ##############################################################################################
void rvl3DSLogTextureStats() {
    const unsigned long long ms = hostGetTickFrequency() / 1000;
    hostLog("textures: %lu decoded (%lu ms), %lu replaced in the full cache\n", static_cast< unsigned long >(sDecodes),
            static_cast< unsigned long >(sDecodeTicks / ms), static_cast< unsigned long >(sReplaced));
    sDecodes = sReplaced = 0;
    sDecodeTicks = 0;
}
