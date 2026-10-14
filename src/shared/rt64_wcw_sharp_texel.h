//
// RT64
//

// [wcw] Sharp bilinear texel coordinates ("2D Texture Edges: Smooth"), shared by TextureSampler.hlsli and the unit
// tests (tests/wcw), so the tested function is the one the shader runs.
//
// Point-sampled 2D art (crowds, signs, HUD, fonts: 32-64 texel textures blown up ~9x at 4K) shows hard, crawling
// texel edges. Sharp bilinear keeps every texel a crisp square but blends across each seam between texels over one
// screen pixel, the per-texel version of VideoInterfacePS.hlsl's PixelAntialiasing.
//
// The coordinate is remapped per axis and then sampled with bilinear filtering, where value(u) =
// lerp(texel[floor(u)], texel[floor(u) + 1], frac(u)). Away from a seam the result is a whole number, so it samples
// exactly texel[floor(coord)], the same texel point sampling picks; within half a screen pixel of the seam it ramps
// linearly to the next texel.

#pragma once

#ifdef HLSL_CPU
#include <cmath>
#define WCW_SHARP_INLINE inline
#define WCW_SHARP_FLOOR std::floor
#else
#define WCW_SHARP_INLINE
#define WCW_SHARP_FLOOR floor
#endif

// coord: texel-space coordinate (texel k covers [k, k + 1) under point sampling). texelsPerPixel: how many texels
// one screen pixel spans along this axis (fwidth of coord).
WCW_SHARP_INLINE float wcwSharpTexelCoord(float coord, float texelsPerPixel) {
    // No footprint (a degenerate derivative): plain point sampling.
    if (!(texelsPerPixel > 1e-6f)) {
        return WCW_SHARP_FLOOR(coord);
    }

    const float seam = WCW_SHARP_FLOOR(coord + 0.5f);
    float t = (coord - seam) / texelsPerPixel;
    t = (t < -0.5f) ? -0.5f : ((t > 0.5f) ? 0.5f : t);
    return seam - 0.5f + t;
}
