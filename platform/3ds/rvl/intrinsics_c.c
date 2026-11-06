// ##############################################################################################
// # PowerPC intrinsics for C                                                                   #
// #                                                                                            #
// # C-linkage versions for the decompiled SDK's C files (intrinsics.cpp has the C++ ones the   #
// # game uses).                                                                                #
// ##############################################################################################
#include <revolution/types.h>

u32 __cntlzw(u32 value) {
    return value == 0 ? 32 : __builtin_clz(value);
}
