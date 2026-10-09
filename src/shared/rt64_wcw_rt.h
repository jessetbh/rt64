//
// RT64
//

#pragma once

#include "shared/rt64_hlsl.h"

// [wcw rt] Constants for the hybrid ray tracing pass (rt64_wcw_raytracer.cpp).

#define WCW_RT_MODE_HIT_NORMAL 0
#define WCW_RT_MODE_HIT_DEPTH 1
#define WCW_RT_MODE_SHADOW 2
#define WCW_RT_MODE_HIT_MESH 3
#define WCW_RT_MODE_SHADOW_OCCLUDER 4

#ifdef HLSL_CPU
namespace interop {
#endif
    struct WcwRtParams {
        float4x4 invViewProj;
        float4 eyePosition;
        float4 lightPosition;
        // Viewport of the traced projection in output pixels: x, y, width, height.
        float4 viewport;
        uint2 resolution;
        uint mode;
        float shadowStrength;
        // Lights sit at lightPosition +- lightSpread on X/Z (a ring lighting rig); 1 or 4 lights.
        float lightSpread;
        uint lightCount;
        float2 padding;
    };
#ifdef HLSL_CPU
};
#endif
