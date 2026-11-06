#pragma once

// ##############################################################################################
// # Native renderer counters                                                                   #
// #                                                                                            #
// # Counters of the native renderer (native.cpp, native_submit.cpp), logged and reset every    #
// # 100 frames by rvl3DSEndFrame, and the profile timers, which run only with debug.txt        #
// # profile=1 or in a showcase build (reading the tick is a system call).                      #
// ##############################################################################################
#include <revolution/types.h>

struct RvlGXProfile {
    u64 host;        // native submissions (Citro3D state and draws)
    u32 nativeDraws; // native J3D draws (native.cpp)
    u32 nativeFallbacks[9];  // J3D packets rejected by native.cpp, by reason
    u32 nativeShapeFallbacks[4];  // bad index, no matrix, differed tex-mtx, missing matrix table
    u32 nativeBillboards;  // native J3D billboard packets
    u32 nativeUserDataProj, nativeUserDataEnv;  // ShapePacketUserData material classes
    u32 nativeUserDataTexGenSrc[3];  // POS, NRM, other source slots on those packets
    u32 cpuLights;   // draws refused: more lights than the shader holds
};
extern RvlGXProfile gGXProfile;

// Stage timers only run with debug.txt "profile=1": reading the tick is a system call.
extern bool gGXProfileOn;
extern "C" unsigned long long hostGetTick(void);
static inline u64 rvlGXProfileTick() {  // the host tick, or 0 when profiling is off
    return gGXProfileOn ? hostGetTick() : 0;
}
