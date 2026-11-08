//
// RT64
//

// [wcw] Coverage stored by CVG_DST_CLAMP draws, shared by RasterPS.hlsl and the unit tests (tests/wcw), so the tested
// function is the one the shader runs (aki-roadmap#209).
//
// The RDP adds the pixel's coverage to memory coverage and clamps. With IM_RD off it doesn't read memory, and memory
// coverage counts as 7, so the sum always clamps to full. A framebuffer's alpha is its coverage: VPW2's dissolves redraw
// the last frame with texture alpha, so a draw that stored partial coverage (CVG_X_ALPHA on a texture's partial alpha)
// made the old frame transparent along those edges, as thin lines.
//
// With IM_RD on, the sum needs memory coverage, which the shader can't read: it stays the pixel's own, clamped.

#pragma once

#ifdef HLSL_CPU
#define WCW_CVG_INLINE inline
#else
#define WCW_CVG_INLINE
#endif

// resultCvg and the return value are stored as coverage / cvgRange; full coverage is 7 / cvgRange.
WCW_CVG_INLINE float wcwClampCvg(float resultCvg, bool imageRead, float cvgRange) {
    const float fullCvg = 7.0f / cvgRange;
    if (!imageRead) {
        return fullCvg;
    }

    return (resultCvg < fullCvg) ? resultCvg : fullCvg;
}
