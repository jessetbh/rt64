//
// RT64
//

// [wcw rt] Composites the hybrid ray tracing output (a slot's resolved history, or a debug view) onto the
// framebuffer's color target:
// alpha blended for the debug views, multiplied for shadows (pipeline blend state).

#include "shared/rt64_wcw_rt.h"

[[vk::push_constant]] ConstantBuffer<WcwRtCompositeCB> gComposite : register(b0);
Texture2D<float4> gRtOutput : register(t0);

// A resolved history texel as the factor the 2x multiply blend applies (dst * src * 2), stored at half scale so
// factors up to 2 brighten (glows). Misses carry their factor (1, or outside darkness) in RGB.
float4 encodeTexel(float4 texel) {
    if (gComposite.decodeHistory == 0) {
        return texel;
    }

    if (texel.a < WCW_RT_HISTORY_MISS_BELOW) {
        return float4(0.5f * texel.rgb, 0.0f);
    }

    return float4(min(texel.rgb * texel.a, 2.0f) * 0.5f, 1.0f);
}

// The trace may run below the color target's resolution: bilinear upsample (soft shadows are
// low frequency; no-hit texels carry factor 1 so edges just fade).
float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
    const float2 coord = pos.xy * gComposite.scale - 0.5f;
    const int2 maxTexel = int2(gComposite.traceSize) - 1;
    const int2 t0 = clamp(int2(floor(coord)), int2(0, 0), maxTexel);
    const int2 t1 = clamp(t0 + 1, int2(0, 0), maxTexel);
    const float2 f = saturate(coord - floor(coord));
    const float4 a = encodeTexel(gRtOutput.Load(int3(t0.x, t0.y, 0)));
    const float4 b = encodeTexel(gRtOutput.Load(int3(t1.x, t0.y, 0)));
    const float4 c = encodeTexel(gRtOutput.Load(int3(t0.x, t1.y, 0)));
    const float4 d = encodeTexel(gRtOutput.Load(int3(t1.x, t1.y, 0)));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
