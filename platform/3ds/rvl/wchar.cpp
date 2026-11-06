// Wide-character functions for the game's 16-bit wchar_t (UTF-16, as on the Wii).
//
// The game world is built with -fshort-wchar, while the 3DS C library uses 32-bit wchar_t,
// so the library's versions cannot be used. Formatting is shared with printf.cpp (format.h).

#include "format.h"
#include <revolution/types.h>

extern "C" {

size_t wcslen(const wchar_t* pStr) {
    const wchar_t* p = pStr;
    while (*p != 0) {
        p++;
    }
    return p - pStr;
}

wchar_t* wcsncpy(wchar_t* pDst, const wchar_t* pSrc, size_t n) {
    size_t i = 0;
    for (; i < n && pSrc[i] != 0; i++) {
        pDst[i] = pSrc[i];
    }
    for (; i < n; i++) {
        pDst[i] = 0;
    }
    return pDst;
}

// Unlike snprintf, swprintf returns -1 when the output does not fit.
int vswprintf(wchar_t* pDst, size_t n, const wchar_t* pFormat, va_list args) {
    size_t length = rvl::formatString(pDst, n, pFormat, args);
    return length < n ? static_cast< int >(length) : -1;
}

int swprintf(wchar_t* pDst, size_t n, const wchar_t* pFormat, ...) {
    va_list args;
    va_start(args, pFormat);
    int result = vswprintf(pDst, n, pFormat, args);
    va_end(args);
    return result;
}

}  // extern "C"
