#pragma once

// Capture of the GX command stream on the 3DS.
//
// On the Wii, GX commands are written to the write-gather pipe at 0xCC008000. The 3DS build
// redirects those writes (GX_WRITE_* in GXRegs.h and the vertex functions in GXVert.h) to
// the current FIFO buffer, stored big-endian like the real stream so that it has the same
// format as the display lists in the game files. platform/3ds/gx interprets the stream.

#include <revolution/types.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

extern u8* rvlGXFifoPtr;
extern u8* rvlGXFifoEnd;

// Called when the current buffer is full: grows the capture buffer or marks a display-list
// overflow.
void rvlGXFifoOverflow(void);

// Appends one complete GX value while serializing writers from the game and async threads.
void rvlGXWriteBytes(const void* data, u32 size);

static inline void rvlGXWriteU8(u8 value) {
    rvlGXWriteBytes(&value, 1);
}

// 16/32-bit values are stored in one go when they fit (byte by byte they cost a bounds check
// each: particle quads and other immediate-mode vertices write thousands per frame).
static inline void rvlGXWriteU16(u16 value) {
    u16 big = __builtin_bswap16(value);
    rvlGXWriteBytes(&big, 2);
}

static inline void rvlGXWriteU32(u32 value) {
    u32 big = __builtin_bswap32(value);
    rvlGXWriteBytes(&big, 4);
}

static inline void rvlGXWriteF32(f32 value) {
    union {
        f32 f;
        u32 u;
    } bits;
    bits.f = value;
    rvlGXWriteU32(bits.u);
}

// Names matching the PPCWGPipe members used by GXVert.h.
#define rvlGXWrite_u8(v) rvlGXWriteU8((u8)(v))
#define rvlGXWrite_s8(v) rvlGXWriteU8((u8)(v))
#define rvlGXWrite_u16(v) rvlGXWriteU16((u16)(v))
#define rvlGXWrite_s16(v) rvlGXWriteU16((u16)(v))
#define rvlGXWrite_u32(v) rvlGXWriteU32((u32)(v))
#define rvlGXWrite_s32(v) rvlGXWriteU32((u32)(v))
#define rvlGXWrite_f32(v) rvlGXWriteF32((f32)(v))

#ifdef __cplusplus
}

// Stand-in for the write-gather pipe: `GXWGFifo.f32 = x` appends to the captured stream.
// Only C++ code writes to GXWGFifo directly; the SDK's C files use the GX_WRITE_* macros.
struct RvlGXPipe {
    struct U8 {
        void operator=(unsigned char v) { rvlGXWriteU8(v); }
    } u8;
    struct S8 {
        void operator=(signed char v) { rvlGXWriteU8((unsigned char)v); }
    } s8;
    struct U16 {
        void operator=(unsigned short v) { rvlGXWriteU16(v); }
    } u16;
    struct S16 {
        void operator=(short v) { rvlGXWriteU16((unsigned short)v); }
    } s16;
    struct U32 {
        void operator=(unsigned long v) { rvlGXWriteU32(v); }
    } u32;
    struct S32 {
        void operator=(long v) { rvlGXWriteU32((unsigned long)v); }
    } s32;
    struct F32 {
        void operator=(float v) { rvlGXWriteF32(v); }
    } f32;
};
#endif
