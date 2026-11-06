// ##############################################################################################
// # __abs for C                                                                                #
// #                                                                                            #
// # The Metrowerks intrinsic __abs with C linkage, for the decompiled SDK's C files            #
// # (mem_expHeap.c, wenc.c); computed through unsigned arithmetic so that the most negative    #
// # value does not overflow.                                                                   #
// ##############################################################################################
#include <revolution/types.h>

s32 __abs(s32 value) {
    return value < 0 ? (s32)(0u - (u32)value) : value;
}
