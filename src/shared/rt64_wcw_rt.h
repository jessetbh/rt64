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
// Raw ambient occlusion of this frame in grey, not filtered or composited (tuning the AO's look and noise).
#define WCW_RT_MODE_AO_RAW 9
// Raw shadow term before AO and filtering, grey (white = lit).
#define WCW_RT_MODE_SHADOW_RAW 10
// Wrestler sheen (highlights + backlight) alone, grey on black.
#define WCW_RT_MODE_SHEEN_RAW 12

// Side of the blue noise tile the trace shader samples (WcwLogic::BlueNoiseSize; a power of two).
#define WCW_RT_BLUE_NOISE_SIZE 64

#define WCW_RT_MAX_LIGHTS 8
#define WCW_RT_MAX_GLOWS 4
#define WCW_RT_MAX_LIT_AREAS 2

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
        // Colored light: each light's and glow's RGB. Lights are normalized to unit luminance
        // (WcwLogic::normalizedLightColor), so a color tints without changing the brightness; white = no tint.
        float4 lightColors[WCW_RT_MAX_LIGHTS];
        float4 glowColors[WCW_RT_MAX_GLOWS];
        float aoCharacterStrength;
        float aoCharacterRadius;
        // 1: lit draws use their interpolated vertex normals for the AO hemisphere (WCW_RT_AO_SMOOTH=0 turns it off).
        uint aoSmoothNormals;
        // Contrast: the occluded fraction is scaled by this (then clamped to 1) before darkening, so creases
        // that block part of the hemisphere read clearly. Wrestlers are mostly convex: without it, AO only shows
        // in the tightest corners.
        float aoGain;
        // WCW_RT_AO_SPLIT=1: no AO on the left half of the screen (same-moment before/after comparisons).
        uint aoSplit;
        // Resolve: radius of the depth-aware tent filter (2 = 5x5). Wider smooths AO and soft shadow grain;
        // the depth weights keep object edges sharp. WCW_RT_FILTER_RADIUS overrides.
        uint filterRadius;
        // Contrast gain on characters (aoGain is the world's): wrestlers carry the AO the owner wants to see.
        float aoCharacterGain;
        // 1 while WCW_RT_AO_CYCLE has AO switched off (full-screen on/off comparisons over time).
        uint aoCycleOff;
        // Random samples (shadow jitter, AO directions): 1 = blue noise from the WCW_RT_BLUE_NOISE_SIZE tile, 0 = the
        // old interleaved gradient noise (WCW_RT_NOISE=0).
        uint noiseMode;
        // WCW_RT_NOISE_SPLIT=1: the old noise on the left half of the screen (same-moment comparisons).
        uint noiseSplit;
        // Shadows on lit (smooth-shaded) draws, i.e. wrestlers: 1 = which side faces a light comes from the interpolated
        // vertex normals and shadow rays start from the hit lifted onto the vertices' tangent planes, so the edge where
        // light turns to shadow follows the smooth surface instead of the facets (WCW_RT_SHADOW_SMOOTH=0 turns it off).
        uint shadowSmooth;
        // WCW_RT_SHADOW_SMOOTH_SPLIT=1: flat shadows on the left half of the screen (same-moment comparisons).
        uint shadowSmoothSplit;
        // With shadowSmooth: a light's ray-traced shadow fades in over this much of the facing cosine past the
        // terminator, where the game's vertex lighting already darkens the surface (WCW_RT_TERMINATOR_FADE).
        float terminatorFade;
        // Resolve: shadow and AO are filtered and accumulated separately (WcwLogic::denoiseSettings): tent filter
        // radii in trace pixels and the weight of this frame against the history, per term.
        uint shadowFilterRadius;
        uint aoFilterRadius;
        float shadowTemporalAlpha;
        float aoTemporalAlpha;
        // WCW_RT_DENOISE_SPLIT=1: the old combined cleanup (shadow * AO, filterRadius, temporalAlpha) on the left half.
        uint denoiseSplit;
        // End of the traced rect (exclusive, trace pixels): the resolve filter doesn't read past it.
        uint2 traceEnd;
        // WCW_RT_COLOR_SPLIT=1: white lights and glows on the left half of the screen (same-moment comparisons).
        uint colorSplit;
        // Wrestler sheen (WcwLogic::sheenSettings): highlight strength and exponent, backlight strength and falloff.
        float sheenSpecular;
        float sheenShininess;
        float sheenRim;
        float sheenRimPower;
        // WCW_RT_SHEEN_SPLIT=1: no sheen on the left half of the screen (same-moment comparisons).
        uint sheenSplit;
        // [wcw rt] Outside darkness (the venue preset's outsideDarkness / outsideEdge; WcwLogic::outsideDarknessFactor):
        // surfaces beyond receiverRadius (wrestlers too) darken by up to outsideDarkness over outsideEdge units.
        // darkenMisses: ray misses (background, crowd sprites) darken too, on the arena pass of a target only
        // (WcwLogic::MissDarkening), so other passes over the same image don't darken it again.
        float outsideDarkness;
        float outsideEdge;
        uint darkenMisses;
        // Misses are judged where their ray lands on the arena floor plane at this height (WcwLogic::missFloorDistance).
        float missFloorY;
        // Lit areas (the preset's litAreas, WcwLogic::outsideDarknessAt): world X/Z center, radius and edge of up to
        // WCW_RT_MAX_LIT_AREAS places where outside darkness eases off like around the ring. Scalars, so the C++ and
        // HLSL layouts can't disagree on padding.
        uint litAreaCount;
        float litArea0X;
        float litArea0Z;
        float litArea0Radius;
        float litArea0Edge;
        float litArea1X;
        float litArea1Z;
        float litArea1Radius;
        float litArea1Edge;
    };

    struct WcwRtCompositeCB {
        // Trace texels per color target pixel, and the trace texture size.
        float2 scale;
        float2 traceSize;
    };
#ifdef HLSL_CPU
};
#endif
