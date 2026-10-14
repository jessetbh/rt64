//
// RT64
//

// [wcw] Unit tests for shared/rt64_wcw_sharp_texel.h: sharp bilinear texel coordinates ("2D Texture Edges: Smooth").
// The header is the same code TextureSampler.hlsli runs.

#ifndef HLSL_CPU
#define HLSL_CPU
#endif
#include "shared/rt64_wcw_sharp_texel.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "wcw_test.h"

// RT64's bilinear filter along one axis: lerp(texel[floor(u)], texel[floor(u) + 1], frac(u)).
static float bilinear(const std::vector<float> &texels, float u) {
    const float base = std::floor(u);
    const int i = int(base);
    const float f = u - base;
    auto at = [&](int k) { return texels[size_t(std::max(0, std::min(int(texels.size()) - 1, k)))]; };
    return at(i) + (at(i + 1) - at(i)) * f;
}

static float pointSample(const std::vector<float> &texels, float u) {
    return texels[size_t(int(std::floor(u)))];
}

static const std::vector<float> Texels = { 0.0f, 1.0f, 0.25f, 0.75f, 0.5f, 0.0f, 1.0f, 1.0f };

TEST_CASE("sharp texels: away from a seam the result is exactly the point-sampled texel") {
    // A 32-texel sprite blown up 9x at 4K: one screen pixel spans 1/9 texel.
    const float w = 1.0f / 9.0f;
    for (float u = 0.5f; u < 7.0f; u += 0.0137f) {
        const float d = u - std::floor(u + 0.5f);
        if (std::abs(d) <= w * 0.5f) {
            continue;
        }

        CHECK_EQ(bilinear(Texels, wcwSharpTexelCoord(u, w)), pointSample(Texels, u));
    }
}

TEST_CASE("sharp texels: a seam blends over one screen pixel, halfway at the seam") {
    const float w = 1.0f / 9.0f;
    const float seam = 3.0f;
    // Half a pixel either side: the texels themselves.
    CHECK_NEAR(bilinear(Texels, wcwSharpTexelCoord(seam - w * 0.5f, w)), Texels[2], 1e-5f);
    CHECK_NEAR(bilinear(Texels, wcwSharpTexelCoord(seam + w * 0.5f, w)), Texels[3], 1e-5f);
    // On the seam: the average.
    CHECK_NEAR(bilinear(Texels, wcwSharpTexelCoord(seam, w)), (Texels[2] + Texels[3]) * 0.5f, 1e-5f);
    // A quarter pixel past: three quarters of the way.
    CHECK_NEAR(bilinear(Texels, wcwSharpTexelCoord(seam + w * 0.25f, w)), Texels[2] + (Texels[3] - Texels[2]) * 0.75f, 1e-5f);
}

TEST_CASE("sharp texels: the ramp is monotonic and continuous across the seam (no crawl)") {
    const float w = 1.0f / 9.0f;
    float previous = bilinear(Texels, wcwSharpTexelCoord(0.9f, w));
    for (float u = 0.9f; u < 1.1f; u += 0.001f) {
        const float v = bilinear(Texels, wcwSharpTexelCoord(u, w));
        CHECK(v >= previous - 1e-6f);
        // Steps of a thousandth of a texel move it by at most that share of one pixel's ramp.
        CHECK(v - previous <= (0.001f / w) * (Texels[1] - Texels[0]) + 1e-4f);
        previous = v;
    }
}

TEST_CASE("sharp texels: a zero footprint acts as point sampling, without NaN") {
    for (float u : { 0.2f, 0.9999f, 1.0f, 2.5f, 6.75f }) {
        const float c = wcwSharpTexelCoord(u, 0.0f);
        CHECK(std::isfinite(c));
        CHECK_NEAR(bilinear(Texels, c), pointSample(Texels, u), 1e-5f);
    }
}

TEST_CASE("sharp texels: minified (more than a texel per pixel) stays between the neighbouring texels") {
    for (float u = 1.0f; u < 6.0f; u += 0.1f) {
        const float c = wcwSharpTexelCoord(u, 2.0f);
        CHECK(std::isfinite(c));
        const float seam = std::floor(u + 0.5f);
        CHECK(c >= seam - 1.0f - 1e-6f);
        CHECK(c <= seam + 1e-6f);
    }
}
