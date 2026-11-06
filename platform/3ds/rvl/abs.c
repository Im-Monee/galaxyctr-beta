#include <revolution/types.h>

s32 __abs(s32 value) {
    return value < 0 ? (s32)(0u - (u32)value) : value;
}
