//
// RT64
//

// [wcw] Unit tests for shared/rt64_wcw_cvg_clamp.h: the coverage a CVG_DST_CLAMP draw stores (aki-roadmap#209).

#define HLSL_CPU
#include "shared/rt64_wcw_cvg_clamp.h"

#include <cmath>

#include "wcw_test.h"

namespace {
    const float CvgRange = 255.0f;

    // What RasterPS computes before storing: 8 samples times the combiner's alpha under CVG_X_ALPHA.
    float pixelCvg(float alpha) {
        return (8.0f / CvgRange) * alpha;
    }

    // What TextureSampler.hlsli reads back from a framebuffer texture: the coverage's top bit.
    bool alphaBit(float stored) {
        const int cvgModulo = static_cast<int>(std::round(stored * CvgRange)) % 8;
        return (cvgModulo & 0x4) != 0;
    }
}

TEST_CASE("CLAMP with image read off: a texture's partial alpha leaves the alpha bit set (VPW2 dissolve lines, #209)") {
    for (float alpha = 0.0f; alpha <= 1.0f; alpha += 1.0f / 64.0f) {
        CHECK(alphaBit(wcwClampCvg(pixelCvg(alpha), false, CvgRange)));
    }
}

TEST_CASE("CLAMP with image read off stores full coverage in HDR's range too") {
    const float hdrRange = 65535.0f;
    CHECK_NEAR(wcwClampCvg((8.0f / hdrRange) * 0.25f, false, hdrRange), 7.0f / hdrRange, 1e-7f);
}

TEST_CASE("CLAMP with image read on keeps the pixel's own coverage, clamped to full") {
    CHECK_NEAR(wcwClampCvg(pixelCvg(0.25f), true, CvgRange), pixelCvg(0.25f), 1e-6f);
    CHECK_NEAR(wcwClampCvg(pixelCvg(1.0f), true, CvgRange), 7.0f / CvgRange, 1e-6f);
}
