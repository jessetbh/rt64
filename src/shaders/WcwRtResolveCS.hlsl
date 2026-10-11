//
// RT64
//

// [wcw rt] Temporal resolve for the hybrid ray tracing shadows (D3D12 only).
// The trace pass samples each area light once per pixel per frame; this pass reprojects the
// pixel's world position into the previous frame of the same workload slot, clamps that
// history to the current 3x3 neighborhood (limits ghosting on moving wrestlers) and blends.

#include "shared/rt64_wcw_rt.h"

ConstantBuffer<WcwRtParams> gParams : register(b0);
Texture2D<float2> gCurrent : register(t1);
Texture2D<float> gHistoryIn : register(t2);
RWTexture2D<float> gHistoryOut : register(u3);
RWTexture2D<float4> gOutput : register(u4);

[numthreads(8, 8, 1)]
void CSMain(uint2 dispatchPixel : SV_DispatchThreadID) {
    const uint2 pixel = dispatchPixel + gParams.traceOffset;
    if (any(pixel >= gParams.resolution)) {
        return;
    }

    const float2 current = gCurrent[pixel];
    if (current.y <= 0.0f) {
        // No hit: the trace's factor for the background (1, or outside darkness), stored at half scale.
        gHistoryOut[pixel] = 1.0f;
        const float missFactor = 0.5f * current.x;
        gOutput[pixel] = float4(missFactor, missFactor, missFactor, 0.0f);
        return;
    }

    // Neighborhood range of this frame's samples (hit pixels only) and a depth-aware tent filter of
    // them (filterRadius, 2 = 5x5): neighbors on the same surface (similar hit distance) are averaged,
    // so a few jittered shadow and AO rays per pixel don't speckle, while object edges stay sharp.
    const int radius = int(clamp(gParams.filterRadius, 1u, 4u));
    float filteredSum = 0.0f;
    float filteredSqSum = 0.0f;
    float weightSum = 0.0f;
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            const int2 neighbor = clamp(int2(pixel) + int2(x, y), int2(0, 0), int2(gParams.resolution) - 1);
            const float2 sample = gCurrent[neighbor];
            if (sample.y > 0.0f) {
                const float spatial = float(radius + 1 - abs(x)) * float(radius + 1 - abs(y));
                const float depth = exp(-abs(sample.y - current.y) / max(current.y * 0.01f, 0.5f));
                filteredSum += sample.x * spatial * depth;
                filteredSqSum += sample.x * sample.x * spatial * depth;
                weightSum += spatial * depth;
            }
        }
    }

    const float filtered = filteredSum / max(weightSum, 1e-4f);

    // History is clipped to the same-surface neighborhood's mean +- spread (variance clipping).
    // A min/max clamp of one or two noisy samples per light is so tight it rejects the history
    // every frame and the penumbrae shimmer; the spread bound still drops history when the
    // shadow really moves (a wrestler steps away).
    const float variance = max(filteredSqSum / max(weightSum, 1e-4f) - filtered * filtered, 0.0f);
    const float spread = 1.25f * sqrt(variance) + 0.04f;
    const float minFactor = filtered - spread;
    const float maxFactor = filtered + spread;

    // Same primary ray as the trace pass, so the hit distance gives the world position.
    const float2 local = ((float2(pixel) + 0.5f) / gParams.traceScale - gParams.viewport.xy) / gParams.viewport.zw;
    const float2 ndc = float2(local.x * 2.0f - 1.0f, 1.0f - local.y * 2.0f);
    const float4 nearPoint = mul(gParams.invViewProj, float4(ndc, -1.0f, 1.0f));
    const float4 farPoint = mul(gParams.invViewProj, float4(ndc, 1.0f, 1.0f));
    const float3 rayStart = nearPoint.xyz / nearPoint.w;
    const float3 rayDir = normalize(farPoint.xyz / farPoint.w - rayStart);
    const float3 worldPos = rayStart + rayDir * current.y;

    float result = filtered;
    if (gParams.historyValid != 0) {
        const float4 prevClip = mul(gParams.prevViewProj, float4(worldPos, 1.0f));
        if (prevClip.w > 0.0f) {
            const float2 prevNdc = prevClip.xy / prevClip.w;
            const float2 prevLocal = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
            const float2 prevPixel = (gParams.viewport.xy + prevLocal * gParams.viewport.zw) * gParams.traceScale;
            if (all(prevPixel >= 0.0f) && all(prevPixel < float2(gParams.resolution))) {
                const float history = clamp(gHistoryIn[int2(prevPixel)], minFactor, maxFactor);
                result = lerp(history, filtered, gParams.temporalAlpha);
            }
        }
    }

    gHistoryOut[pixel] = result;
    // Stored at half scale: the composite's 2x multiply blend (dst * src * 2) applies factors up
    // to 2, so glows can brighten.
    const float encoded = min(result, 2.0f) * 0.5f;
    gOutput[pixel] = float4(encoded, encoded, encoded, 1.0f);
}
