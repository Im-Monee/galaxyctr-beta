// Movies: the H.264 pictures of the converted THP files (tools/3ds/builder/movie.py) through the
// New 3DS hardware video decoder (MVD), into a texture the game draws (THPSimplePlayerWrapper).
//
// The decoder takes one NAL unit at a time (with its 00 00 01 start code) from linear memory and
// says when a picture is ready; the picture is rendered as RGB565 rows into a linear buffer and
// copied here into the 8x8 tiled layout of a Citro3D texture. Two textures alternate, so the
// GPU can still draw the previous picture while the next one is written.

#include "host.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_SIZE (256 * 1024)  // largest NAL unit (an I picture of 512x256 is far smaller)

// jut_texture.cpp: frees cached textures until that much linear memory is free; frees the oldest
// one the GPU no longer reads (0 when there is none).
void rvl3DSFreeTextureMemory(unsigned long bytes);
int rvl3DSFreeOneTexture(void);

// ############################################################################################
// # Linear memory in one piece for the decoder                                               #
// #                                                                                          #
// # The level's texture cache fills the linear memory and leaves it in fragments: in the     #
// # CIA 0.21.1 log 4.1 MB were free, but no single 1.9 MB block for the decoder's work       #
// # buffer, so the movie showed only its subtitles over a blue screen. Each buffer is        #
// # allocated here, freeing one cached texture at a time until it fits (the textures are     #
// # decoded again when the game draws them after the movie).                                 #
// ############################################################################################
static void* linearAllocFreeingTextures(unsigned long size) {
    void* block;
    while ((block = linearAlloc(size)) == NULL && rvl3DSFreeOneTexture()) {
    }
    return block;
}

static bool sOpen;
static MVDSTD_Config sConfig;
static u8* sInput;
static u16* sOutput;
static C3D_Tex* sTextures[2];
static int sCurrent = -1;
static unsigned sWidth, sHeight, sPaddedWidth, sPaddedHeight;
static bool sErrorLogged;

// A New 3DS with the video decoder's service running. The decoder module (mvd) starts with the
// title only when its exheader lists it (cia_showcase.rsf); otherwise opening mvd:STD would
// wait forever for the service (srvGetServiceHandle blocks on unregistered services).
int hostMovieSupported(void) {
    static bool sLogged;
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    if (!isNew || hostLowMemory()) {
        return 0;
    }
    bool registered = false;
    if (R_FAILED(srvIsServiceRegistered(&registered, "mvd:STD")) || !registered) {
        if (!sLogged) {
            hostLog("movie: the video decoder service (mvd:STD) is not running; movies are skipped\n");
            sLogged = true;
        }
        return 0;
    }
    return 1;
}

static u32 morton(u32 x, u32 y) {
    // Bits interleaved x0 y0 x1 y1 x2 y2 inside an 8x8 tile.
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

void hostMovieEnd(void) {
    if (!sOpen) {
        return;
    }
    mvdstdExit();
    linearFree(sInput);
    linearFree(sOutput);
    for (int i = 0; i < 2; i++) {
        if (sTextures[i] != NULL) {
            hostGfxTextureDestroy(sTextures[i]);  // after the GPU's last frame using it
            sTextures[i] = NULL;
        }
    }
    sInput = NULL;
    sOutput = NULL;
    sCurrent = -1;
    sOpen = false;
}

static bool sSawKeyPicture, sWaitKeyPicture, sFirstKeyResent;  // hostMovieDecode

int hostMovieBegin(unsigned width, unsigned height, unsigned paddedWidth, unsigned paddedHeight) {
    hostMovieEnd();
    if (!hostMovieSupported() || width > paddedWidth || height > paddedHeight) {
        return 0;
    }
    sWidth = width;
    sHeight = height;
    sPaddedWidth = paddedWidth;
    sPaddedHeight = paddedHeight;

    // The decoder's work buffer for this size: the larger of H.264 level 2.1's and one reference
    // frame's (the converter's x264 settings; 3dbrew MVDSTD:CalculateWorkBufSize). The default
    // (9 MB, for 854x480) does not fit the linear memory left in a level (about 8.6 MB).
    // The last size calculated for these dimensions: a later calculation has failed (0.13.15),
    // and the default would not fit.
    static u32 sWorkSize, sWorkWidth, sWorkHeight;
    u32 workSize = 0;
    MVDSTD_CalculateWorkBufSizeConfig calc;
    memset(&calc, 0, sizeof(calc));
    calc.level.enable = 1;
    calc.level.flag = MVD_CALC_WITH_LEVEL_FLAG_ENABLE_CALC;
    calc.level.level = MVD_H264_LEVEL_2_1;
    calc.ref_frames_a.enable = 1;
    calc.ref_frames_a.ref_frames = 1;
    calc.width = paddedWidth;
    calc.height = paddedHeight;
    if (R_SUCCEEDED(mvdstdCalculateBufferSize(&calc, &workSize)) && workSize != 0) {
        sWorkSize = workSize;
        sWorkWidth = paddedWidth;
        sWorkHeight = paddedHeight;
    } else if (sWorkSize != 0 && sWorkWidth == paddedWidth && sWorkHeight == paddedHeight) {
        workSize = sWorkSize;
    } else {
        workSize = MVD_DEFAULT_WORKBUF_SIZE;
    }

    // The level's texture cache fills the linear memory (2.9-3.5 MB left in 0.13.15, too little
    // and too fragmented for the decoder): give back what the GPU no longer reads, so that the
    // work buffer, the input, the output and the two textures fit, with the cache's own reserve.
    const u32 need = workSize + 4096 + INPUT_SIZE + paddedWidth * paddedHeight * 2 * 3 + 1536 * 1024;
    rvl3DSFreeTextureMemory(need);

    sInput = linearAllocFreeingTextures(INPUT_SIZE);
    sOutput = linearAllocFreeingTextures(paddedWidth * paddedHeight * 2);
    // mvdstdInit allocates the work buffer itself (linearAlloc): make room for it in one piece
    // first. Nothing else allocates linear memory between this test and mvdstdInit, so the
    // block it frees is the one mvdstdInit finds.
    void* workRoom = sInput != NULL && sOutput != NULL ? linearAllocFreeingTextures(workSize) : NULL;
    linearFree(workRoom);
    Result rc = workRoom != NULL ? mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, workSize, NULL) : -1;
    if (R_FAILED(rc)) {
        hostLog("movie: decoder not started (0x%08lX, work buffer %lu KB, linear free %lu KB)\n", (unsigned long)rc,
                (unsigned long)workSize / 1024, (unsigned long)linearSpaceFree() / 1024);
        linearFree(sInput);
        linearFree(sOutput);
        sInput = NULL;
        sOutput = NULL;
        return 0;
    }
    sOpen = true;
    for (int i = 0; i < 2; i++) {
        sTextures[i] = malloc(sizeof(C3D_Tex));
        bool made = false;
        // C3D_TexInit also needs its 256 KB in one piece.
        while (sTextures[i] != NULL && !(made = C3D_TexInit(sTextures[i], paddedWidth, paddedHeight, GPU_RGB565)) && rvl3DSFreeOneTexture()) {
        }
        if (!made) {
            free(sTextures[i]);
            sTextures[i] = NULL;
            hostMovieEnd();
            hostLog("movie: no texture memory\n");
            return 0;
        }
        memset(sTextures[i]->data, 0, sTextures[i]->size);
        GSPGPU_FlushDataCache(sTextures[i]->data, sTextures[i]->size);
        C3D_TexSetFilter(sTextures[i], GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(sTextures[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
    mvdstdGenerateDefaultConfig(&sConfig, paddedWidth, paddedHeight, paddedWidth, paddedHeight, NULL, (u32*)sOutput, NULL);
    sErrorLogged = false;
    sSawKeyPicture = sWaitKeyPicture = sFirstKeyResent = false;
    hostLog("movie: decoder started, %ux%u in %ux%u, work buffer %lu KB\n", width, height, paddedWidth, paddedHeight,
            (unsigned long)workSize / 1024);
    return 1;
}

// The rendered picture (rows top-down) into the next texture: image row y goes to memory row
// (paddedHeight - 1 - y), as hostGfxTextureCreate stores images (t = 0 samples the top row).
static void storePicture(void) {
    GSPGPU_InvalidateDataCache(sOutput, sPaddedWidth * sPaddedHeight * 2);
    const int next = sCurrent < 0 ? 0 : sCurrent ^ 1;
    C3D_Tex* tex = sTextures[next];
    u16* dst = (u16*)tex->data;
    const u32 tilesPerRow = sPaddedWidth / 8;
    for (u32 y = 0; y < sHeight; y++) {
        const u32 row = sPaddedHeight - 1 - y;
        const u16* src = sOutput + y * sPaddedWidth;
        u16* tileRow = dst + (row / 8) * tilesPerRow * 64;
        for (u32 x = 0; x < sWidth; x++) {
            tileRow[(x / 8) * 64 + morton(x & 7, row & 7)] = src[x];
        }
    }
    GSPGPU_FlushDataCache(tex->data, tex->size);
    sCurrent = next;
}

static int decodeUnits(const unsigned char* data, unsigned long size) {
    int shown = 0;
    unsigned long i = 0;
    while (i + 3 <= size) {
        // One NAL unit: from its start code to the next one (or the end, less padding).
        if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) {
            i++;
            continue;
        }
        unsigned long end = i + 3;
        while (end + 3 <= size && !(data[end] == 0 && data[end + 1] == 0 && data[end + 2] == 1)) {
            end++;
        }
        if (end + 3 > size) {
            end = size;
        }
        unsigned long length = end - i;
        while (length > 3 && data[i + length - 1] == 0) {
            length--;
        }
        if (length <= INPUT_SIZE) {
            memcpy(sInput, data + i, length);
            GSPGPU_FlushDataCache(sInput, length);
            // The decoder module takes addresses in the 0x30000000 LINEAR alias, as libctru gives
            // it the work buffer. A title declaring a kernel older than 2.44 has its linear heap
            // only at 0x14000000: mvd faulted on the input (0.14.1), then on the work buffer
            // (0.14.2). cia_showcase.rsf now declares 2.46, where linearAlloc already returns the
            // new alias and this conversion keeps it.
            const unsigned type = data[i + 3] & 0x1F;
            if (type == 5) {
                sSawKeyPicture = true;
                sWaitKeyPicture = false;
            }
            Result rc = mvdstdProcessVideoFrame(osConvertOldLINEARMemToNew(sInput), length, 0, NULL);
            if (rc == MVD_STATUS_FRAMEREADY) {
                // A picture is complete (the decoder sees it at the next picture's first NAL unit).
                if (R_SUCCEEDED(mvdstdRenderVideoFrame(&sConfig, true)) && !sWaitKeyPicture) {
                    storePicture();
                    shown = 1;
                }
            } else if (!MVD_CHECKNALUPROC_SUCCESS(rc)) {
                // Pictures built on a refused one are garbage until the next key picture: the last
                // good one stays.
                sWaitKeyPicture = true;
                if (!sErrorLogged) {
                    hostLog("movie: NAL unit type %u refused (0x%08lX)\n", type, (unsigned long)rc);
                    sErrorLogged = true;
                }
            }
        }
        i = end;
    }
    return shown;
}

int hostMovieDecode(const unsigned char* data, unsigned long size) {
    if (!sOpen || data == NULL) {
        return 0;
    }
    int shown = decodeUnits(data, size);
    // The pictures after the decoder starts came out as garbage up to the second key picture, four
    // seconds on (0.15.2, a P picture refused): it takes the first key picture a second time.
    if (sSawKeyPicture && !sFirstKeyResent) {
        sFirstKeyResent = true;
        shown |= decodeUnits(data, size);
    }
    return shown;
}

void* hostMovieTexture(void) {
    return sOpen && sCurrent >= 0 ? sTextures[sCurrent] : NULL;
}
