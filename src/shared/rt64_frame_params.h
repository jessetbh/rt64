//
// RT64
//

#pragma once

#include "shared/rt64_hlsl.h"

#ifdef HLSL_CPU
namespace interop {
#endif
    struct FrameParams {
        uint frameCount;
        uint viewUbershaders;
        float ditherNoiseStrength;
        // [wcw] "2D Texture Edges: Smooth": sharp bilinear for point-sampled textures (TextureSampler.hlsli).
        uint wcwSmooth2DEdges;
    };
#ifdef HLSL_CPU
};
#endif