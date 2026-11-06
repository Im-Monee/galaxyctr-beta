// Portable versions of the Metrowerks PowerPC intrinsics declared in revolution/types.h.

#include <revolution/types.h>
#include <math.h>
#include <string.h>

f32 __frsqrte(f32 value) {
    return 1.0f / sqrtf(value);
}

// The same intrinsic for units that see revolution/types.h inside an extern "C" block
// (JPABaseShape.cpp's TUtil<f32>::inv_sqrt).
extern "C" f32 frsqrteC(f32 value) __asm__("__frsqrte");
extern "C" f32 frsqrteC(f32 value) {
    return 1.0f / sqrtf(value);
}

u32 __cntlzw(u32 value) {
    return value == 0 ? 32 : __builtin_clz(value);
}

s32 __abs(s32 value) {
    return value < 0 ? -value : value;
}

f32 __fabsf(f32 value) {
    return fabsf(value);
}

f64 __fabs(f64 value) {
    return fabs(value);
}

void* __memcpy(void* pDst, const void* pSrc, int size) {
    return memcpy(pDst, pSrc, size);
}
