// Texture palette libraries (TPL), based on the decompiled src/RVL_SDK/tpl/TPL.c.
//
// nw4r decides whether a TPL still needs binding with `descriptorArray < 0x80000000`, which
// is how a Wii offset differs from a Wii pointer. On the 3DS every pointer is below that, so
// the same TPL is bound again on each use. Binding is therefore made idempotent: once bound,
// the descriptor array points at or after the TPL itself, while an offset is always smaller.

#include "host.h"
#include <revolution/tpl.h>

static const u32 kTplVersion = 2142000;

extern "C" {

void TPLBind(TPLPalettePtr ptr) {
    if (ptr->versionNumber != kTplVersion) {
        hostFatal("Invalid TPL version %lu at %p.", ptr->versionNumber, ptr);
    }

    u32 base = reinterpret_cast< u32 >(ptr);
    if (reinterpret_cast< u32 >(ptr->descriptorArray) >= base) {
        return;
    }

    ptr->descriptorArray = reinterpret_cast< TPLDescriptorPtr >(reinterpret_cast< u32 >(ptr->descriptorArray) + base);

    for (u16 i = 0; i < ptr->numDescriptors; i++) {
        TPLDescriptor& descriptor = ptr->descriptorArray[i];

        if (descriptor.textureHeader != nullptr) {
            descriptor.textureHeader = reinterpret_cast< TPLHeaderPtr >(reinterpret_cast< u32 >(descriptor.textureHeader) + base);
            if (!descriptor.textureHeader->unpacked) {
                descriptor.textureHeader->data = reinterpret_cast< char* >(reinterpret_cast< u32 >(descriptor.textureHeader->data) + base);
                descriptor.textureHeader->unpacked = 1;
            }
        }

        if (descriptor.CLUTHeader != nullptr) {
            descriptor.CLUTHeader = reinterpret_cast< TPLClutHeaderPtr >(reinterpret_cast< u32 >(descriptor.CLUTHeader) + base);
            if (!descriptor.CLUTHeader->unpacked) {
                descriptor.CLUTHeader->data = reinterpret_cast< char* >(reinterpret_cast< u32 >(descriptor.CLUTHeader->data) + base);
                descriptor.CLUTHeader->unpacked = 1;
            }
        }
    }
}

TPLDescriptorPtr TPLGet(TPLPalettePtr ptr, u32 id) {
    id %= ptr->numDescriptors;
    return &ptr->descriptorArray[id];
}

}  // extern "C"
