// Polygon clipping for the GPU.
//
// The PICA clips triangles only against the near and far planes: a triangle that crosses the
// camera plane with a large lateral extent leaves the clipper many thousand screens wide, and
// rasterizing it stalls the GPU for good (seen on hardware: the title screen backdrop, whose
// triangles reach ~9600 screen widths after the near clip). The Wii's GX clips against every
// side of the view volume, so such triangles are clipped here, before the GPU, against the near
// plane and a guard band of HOST_CLIP_GUARD screens on each side. What lies inside the screen
// is unchanged; only geometry far outside it is cut away.

#include "host.h"

#include <math.h>
#include <string.h>

// Signed distance of clip-space position c to clip plane `plane` (inside when >= 0).
static float planeDistance(const float* c, int plane) {
    switch (plane) {
    case 0: return c[2] + c[3];                    // near: z >= -w
    case 1: return HOST_CLIP_GUARD * c[3] - c[0];  // x <= k w
    case 2: return HOST_CLIP_GUARD * c[3] + c[0];  // x >= -k w
    case 3: return HOST_CLIP_GUARD * c[3] - c[1];  // y <= k w
    default: return HOST_CLIP_GUARD * c[3] + c[1]; // y >= -k w
    }
}

unsigned hostClipOutcode(const float* c) {
    // NaN or infinity compares false with every plane and would pass as inside; the PICA turns it
    // into an unbounded triangle. Such a vertex is invalid and its triangles are dropped.
    if (!isfinite(c[0]) || !isfinite(c[1]) || !isfinite(c[2]) || !isfinite(c[3])) {
        return HOST_CLIP_INVALID;
    }
    unsigned code = 0;
    for (int plane = 0; plane < HOST_CLIP_PLANES; plane++) {
        if (planeDistance(c, plane) < 0.0f) {
            code |= 1u << plane;
        }
    }
    return code;
}

int hostClipPolygon(unsigned char* pPolygon, int count, unsigned size, HostClipLerp lerp, unsigned char* pScratch) {
    unsigned char* in = pPolygon;
    unsigned char* out = pScratch;
    for (int plane = 0; plane < HOST_CLIP_PLANES && count > 0; plane++) {
        int n = 0;
        for (int i = 0; i < count; i++) {
            const unsigned char* a = in + i * size;
            const unsigned char* b = in + ((i + 1) % count) * size;
            float da = planeDistance((const float*)a, plane);
            float db = planeDistance((const float*)b, plane);
            if (da >= 0.0f) {
                memcpy(out + n++ * size, a, size);
            }
            if ((da >= 0.0f) != (db >= 0.0f)) {
                lerp(out + n++ * size, a, b, da / (da - db));
            }
        }
        unsigned char* swap = in;
        in = out;
        out = swap;
        count = n;
    }
    if (in != pPolygon && count > 0) {
        memcpy(pPolygon, in, (unsigned)count * size);
    }
    return count;
}
