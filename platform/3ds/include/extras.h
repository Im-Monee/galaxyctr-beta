#pragma once

// MSL compatibility shim for the non-standard string helpers in <extras.h>.
#include <strings.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int stricmp(const char* pA, const char* pB) {
    return strcasecmp(pA, pB);
}

static inline int strnicmp(const char* pA, const char* pB, size_t n) {
    return strncasecmp(pA, pB, n);
}

#ifdef __cplusplus
}
#endif
