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
#define WCW_RT_MODE_HIT_POSITION 5
#define WCW_RT_MODE_SHADOW_DISTANCE 6

// Bit 31 of a mesh index start: N64 face culling is on for that draw.
#define WCW_RT_MESH_CULLED 0x80000000u

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
        // A key light at lightPosition (weight keyWeight) plus lightCount fill lights at
        // lightPosition +- lightSpread on X/Z (a ring lighting rig) sharing the rest.
        float lightSpread;
        uint lightCount;
        // +1 or -1: which winding counts as front-facing for culled draws (WCW_RT_CULL_SIGN).
        float cullSign;
        float keyWeight;
    };
#ifdef HLSL_CPU
};
#endif
