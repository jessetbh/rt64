//
// RT64
//

#pragma once

#include "shared/rt64_hlsl.h"

#ifdef HLSL_CPU
namespace interop {
#endif
    struct VideoInterfaceCB {
        float2 videoResolution;
        float2 textureResolution;
        float gamma;
        // [wcw hdr] 0 for SDR output; otherwise the output is converted from sRGB to linear and
        // scaled by this (paper white / 80 nits) for an scRGB swap chain.
        float hdrScale;
    };
#ifdef HLSL_CPU
};
#endif