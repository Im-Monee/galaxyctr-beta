#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long u32;
typedef unsigned long long u64;

u32 __cvt_fp2unsigned(double);
#ifdef __MWERKS__
asm void __div2u(void);
asm void __div2i(void);
asm void __mod2u(void);
asm void __mod2i(void);
asm void __shl2i(void);
asm void __shr2u(void);
u64 __cvt_dbl_usll(double);
asm void __cvt_dbl_ull(void);
#else
static inline u64 __cvt_dbl_usll(double value) {
    return (u64)value;
}
#endif

#ifdef __cplusplus
}
#endif
