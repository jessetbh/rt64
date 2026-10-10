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
        gHistoryOut[pixel] = 1.0f;
        gOutput[pixel] = float4(1.0f, 1.0f, 1.0f, 0.0f);
        return;
    }

    // Neighborhood range of this frame's samples (hit pixels only) and a depth-aware 3x3 tent
    // filter of them: neighbors on the same surface (similar hit distance) are averaged, so a
    // single jittered sample per light doesn't speckle penumbrae, while object edges stay sharp.
    float minFactor = current.x;
    float maxFactor = current.x;
    float filteredSum = 0.0f;
    float weightSum = 0.0f;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            const int2 neighbor = clamp(int2(pixel) + int2(x, y), int2(0, 0), int2(gParams.resolution) - 1);
            const float2 sample = gCurrent[neighbor];
            if (sample.y > 0.0f) {
                minFactor = min(minFactor, sample.x);
                maxFactor = max(maxFactor, sample.x);

                const float spatial = ((x == 0) ? 2.0f : 1.0f) * ((y == 0) ? 2.0f : 1.0f);
                const float depth = exp(-abs(sample.y - current.y) / max(current.y * 0.01f, 0.5f));
                filteredSum += sample.x * spatial * depth;
                weightSum += spatial * depth;
            }
        }
    }

    const float filtered = filteredSum / max(weightSum, 1e-4f);

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
    gOutput[pixel] = float4(result, result, result, 1.0f);
}
