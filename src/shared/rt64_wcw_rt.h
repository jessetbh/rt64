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
#define WCW_RT_MODE_AO 7
// Light placement view: red dots under the lights, a 100-unit world grid, and the lights'
// center lines (X red, Z blue) on every surface. Also set by "debugMode": 8 in the presets file.
#define WCW_RT_MODE_LIGHT_MAP 8

#define WCW_RT_MAX_LIGHTS 8
#define WCW_RT_MAX_GLOWS 4

// Bit 31 of a mesh index start: N64 face culling is on for that draw.
#define WCW_RT_MESH_CULLED 0x80000000u
// Bit 30: a character draw (AKI scales wrestler models, world geometry has unit scale).
#define WCW_RT_MESH_CHARACTER 0x40000000u
// Bit 29: a lit draw (G_LIGHTING): its vertices carry real normals, which the AO interpolates.
#define WCW_RT_MESH_LIT 0x20000000u
#define WCW_RT_MESH_FLAGS (WCW_RT_MESH_CULLED | WCW_RT_MESH_CHARACTER | WCW_RT_MESH_LIT)

// Per-mesh tile word: RDP/GPU tile index in the low bits, plus whether the draw alpha tests
// (rays skip its transparent texels) and whether its UVs get the non-perspective correction.
#define WCW_RT_TILE_ALPHA_TEST 0x80000000u
#define WCW_RT_TILE_UV_CORRECTION 0x40000000u
#define WCW_RT_TILE_INDEX_MASK 0x3FFFFFFFu

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
        // Shadow strength on character surfaces (they already carry N64 vertex shading).
        float characterShadowStrength;
        // Soft shadows: each light is a square panel lightSize wide on X/Z, sampled once per
        // pixel per frame; the resolve pass blends with reprojected history by temporalAlpha.
        float lightSize;
        float temporalAlpha;
        uint frameIndex;
        float4x4 prevViewProj;
        uint historyValid;
        // Jittered shadow rays per light per pixel per frame.
        uint samplesPerLight;
        // Ambient occlusion: aoSamples cosine-weighted rays of length aoRadius per pixel per frame; a hit at
        // distance t occludes by 1 - (t / radius)^2 and the occluded fraction darkens by aoStrength. Characters
        // use aoCharacterStrength / aoCharacterRadius (below).
        uint aoSamples;
        float aoRadius;
        float aoStrength;
        // Trace/resolve resolution relative to the color target (resolution is the trace size;
        // viewport stays in color target pixels).
        float traceScale;
        // Only the screen rect covered by this workload's geometry is traced (trace pixels).
        uint2 traceOffset;
        float receiverRadius;
        // The lights (xyz, weight; weights sum to 1), from the venue's light preset.
        uint lightListCount;
        // Shadow rays stop this far short of the light, so geometry right around a light (a
        // titantron the light sits behind) doesn't block it.
        float lightClearance;
        // Distance falloff of the lights' weights (0 = off): each light counts with
        // 1 / (1 + (distance / lightFalloff)^2), so the nearest lights dominate a pixel's shadow.
        float lightFalloff;
        float4 lights[WCW_RT_MAX_LIGHTS];
        // Glows: lights that brighten the surfaces facing them (xyz, range; intensity separately),
        // ray traced so blockers still shade them. The shadow output is then a factor up to 2.
        uint glowCount;
        float glowPadding0;
        float glowPadding1;
        float glowPadding2;
        float4 glows[WCW_RT_MAX_GLOWS];
        float4 glowIntensity;
        float aoCharacterStrength;
        float aoCharacterRadius;
        // 1: lit draws use their interpolated vertex normals for the AO hemisphere (WCW_RT_AO_SMOOTH=0 turns it off).
        uint aoSmoothNormals;
        float aoPadding0;
    };

    struct WcwRtCompositeCB {
        // Trace texels per color target pixel, and the trace texture size.
        float2 scale;
        float2 traceSize;
    };
#ifdef HLSL_CPU
};
#endif
