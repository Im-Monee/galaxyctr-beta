// ##############################################################################################
// # sprintf with the Wii's behaviour                                                           #
// #                                                                                            #
// # The game's formatted strings must come out as on the Wii (Metrowerks MSL: format.h). The   #
// # linker redirects every call to these functions here with --wrap (tools/3ds/configure.py).  #
// ##############################################################################################
#include "format.h"

extern "C" {

int __wrap_vsnprintf(char* pDst, size_t size, const char* pFormat, va_list args) {
    return static_cast< int >(rvl::formatString(pDst, size, pFormat, args));
}

int __wrap_vsprintf(char* pDst, const char* pFormat, va_list args) {
    return static_cast< int >(rvl::formatString(pDst, static_cast< size_t >(-1) / 2, pFormat, args));
}

int __wrap_snprintf(char* pDst, size_t size, const char* pFormat, ...) {
    va_list args;
    va_start(args, pFormat);
    int result = __wrap_vsnprintf(pDst, size, pFormat, args);
    va_end(args);
    return result;
}

int __wrap_sprintf(char* pDst, const char* pFormat, ...) {
    va_list args;
    va_start(args, pFormat);
    int result = __wrap_vsprintf(pDst, pFormat, args);
    va_end(args);
    return result;
}

}  // extern "C"
