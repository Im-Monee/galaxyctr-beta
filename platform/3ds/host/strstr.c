#include <stddef.h>

// Replaces newlib's strstr for the whole program. Newlib's runs a two-way search whose setup
// (critical_factorization) costs more than the search itself on the game's short strings, and
// the game calls it constantly: each MR::getAreaObj tests the name against up to 67 area managers
// with it, a dozen times in every Mario update. A plain search like the Wii's (MSL) is the same
// result and usually stops at the first byte.
char* strstr(const char* haystack, const char* needle) {
    if (*needle == '\0') {
        return (char*)haystack;
    }
    for (; *haystack != '\0'; haystack++) {
        if (*haystack != *needle) {
            continue;
        }
        const char* h = haystack;
        const char* n = needle;
        while (*n != '\0' && *h == *n) {
            h++;
            n++;
        }
        if (*n == '\0') {
            return (char*)haystack;
        }
    }
    return NULL;
}
