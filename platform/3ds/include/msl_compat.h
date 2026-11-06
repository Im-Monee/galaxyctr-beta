#pragma once

// ##############################################################################################
// # The Metrowerks C library environment                                                       #
// #                                                                                            #
// # Force-included into every file of the game world (tools/3ds/configure.py, -include). The   #
// # decompiled code was written against Metrowerks' MSL, whose headers pull in the C library   #
// # transitively and provide a few non-standard macros; this recreates that environment on top #
// # of newlib. The small headers beside it (mem.h, cmath, extras.h...) map MSL-only headers to #
// # newlib's.                                                                                  #
// ##############################################################################################

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// newlib's <ctype.h> defines single-letter classification macros that collide with
// decompiled member names such as _C and _B. Its include guard keeps them from coming back.
#undef _U
#undef _L
#undef _N
#undef _S
#undef _P
#undef _C
#undef _X
#undef _B

// MSL defines M_PI as a float; newlib's is a double, which would change the precision of game math.
#undef M_PI
#define M_PI 3.14159265358979323846f
#define M_SQRT3 1.73205f

// Metrowerks storage-class extensions.
#define __declspec(x) __declspec_##x
#define __declspec_weak __attribute__((weak))
#define __declspec_force_export __attribute__((used))
#define __declspec_section(x)

#define DEG_TO_RAD(degrees) (degrees * (M_PI / 180.0f))
#define RAD_TO_DEG(radians) (radians * (180.0f / M_PI))
