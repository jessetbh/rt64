//
// RT64
//

// [wcw rt] Temporal resolve for the hybrid ray tracing shadows (D3D12 only).
// The trace pass samples each area light and the AO hemisphere a few times per pixel per frame; this
// pass filters the shadow and AO terms separately (a tight filter keeps shadow edges crisp, a wide one
// smooths AO), reprojects the pixel's world position into the previous frame of the same workload slot,
// clips each term's history to its neighborhood (limits ghosting on moving wrestlers) and blends.

#include "shared/rt64_wcw_rt.h"

ConstantBuffer<WcwRtParams> gParams : register(b0);
// (shadow factor, hit distance (0 = no hit), AO factor, unused) from the trace pass.
Texture2D<float4> gCurrent : register(t1);
// (shadow, AO) history. With WCW_RT_DENOISE_SPLIT's old path, x holds the combined factor and y is 1.
Texture2D<float2> gHistoryIn : register(t2);
RWTexture2D<float2> gHistoryOut : register(u3);
RWTexture2D<float4> gOutput : register(u4);

// A depth-aware tent filter's running sums for one term.
struct FilterSums {
    float sum;
    float sqSum;
    float weight;
};

void accumulate(inout FilterSums sums, float value, float weight) {
    sums.sum += value * weight;
    sums.sqSum += value * value * weight;
    sums.weight += weight;
}

// Mean, and the history clip range around it: mean +- 1.25 standard deviations + 0.04. A min/max clamp of one
// or two noisy samples is so tight it rejects the history every frame and the term shimmers; the spread bound
// still drops history when the shadow or occlusion really moves (a wrestler steps away).
float3 meanAndRange(FilterSums sums) {
    const float mean = sums.sum / max(sums.weight, 1e-4f);
    const float variance = max(sums.sqSum / max(sums.weight, 1e-4f) - mean * mean, 0.0f);
    const float spread = 1.25f * sqrt(variance) + 0.04f;
    return float3(mean, mean - spread, mean + spread);
}

[numthreads(8, 8, 1)]
void CSMain(uint2 dispatchPixel : SV_DispatchThreadID) {
    const uint2 pixel = dispatchPixel + gParams.traceOffset;
    if (any(pixel >= gParams.resolution)) {
        return;
    }

    const float4 current = gCurrent[pixel];
    if (current.y <= 0.0f) {
        gHistoryOut[pixel] = float2(1.0f, 1.0f);
        gOutput[pixel] = float4(0.5f, 0.5f, 0.5f, 0.0f);
        return;
    }

    // The old combined cleanup (shadow * AO through one filter and one history) for comparisons.
    const bool combined = (gParams.denoiseSplit != 0) && (pixel.x < (gParams.resolution.x / 2));
    const int shadowRadius = int(clamp(combined ? gParams.filterRadius : gParams.shadowFilterRadius, 1u, 4u));
    const int aoRadius = combined ? 0 : int(clamp(gParams.aoFilterRadius, 1u, 4u));
    const int radius = max(shadowRadius, aoRadius);

    // Depth-aware tent filters: neighbors on the same surface (similar hit distance) are averaged, so a few
    // jittered rays per pixel don't speckle, while object edges stay sharp. Neighbors outside the rect traced
    // this frame hold another workload's values and are skipped.
    const int2 rectMin = int2(gParams.traceOffset);
    const int2 rectMax = int2(min(gParams.traceEnd, gParams.resolution)) - 1;
    FilterSums shadowSums = { 0.0f, 0.0f, 0.0f };
    FilterSums aoSums = { 0.0f, 0.0f, 0.0f };
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            const int2 neighbor = int2(pixel) + int2(x, y);
            if (any(neighbor < rectMin) || any(neighbor > rectMax)) {
                continue;
            }

            const float4 sample = gCurrent[neighbor];
            if (sample.y <= 0.0f) {
                continue;
            }

            const float depth = exp(-abs(sample.y - current.y) / max(current.y * 0.01f, 0.5f));
            if ((abs(x) <= shadowRadius) && (abs(y) <= shadowRadius)) {
                const float spatial = float(shadowRadius + 1 - abs(x)) * float(shadowRadius + 1 - abs(y));
                accumulate(shadowSums, combined ? (sample.x * sample.z) : sample.x, spatial * depth);
            }

            if ((abs(x) <= aoRadius) && (abs(y) <= aoRadius)) {
                const float spatial = float(aoRadius + 1 - abs(x)) * float(aoRadius + 1 - abs(y));
                accumulate(aoSums, sample.z, spatial * depth);
            }
        }
    }

    const float3 shadowStats = meanAndRange(shadowSums);
    const float3 aoStats = combined ? float3(1.0f, 1.0f, 1.0f) : meanAndRange(aoSums);

    // Same primary ray as the trace pass, so the hit distance gives the world position.
    const float2 local = ((float2(pixel) + 0.5f) / gParams.traceScale - gParams.viewport.xy) / gParams.viewport.zw;
    const float2 ndc = float2(local.x * 2.0f - 1.0f, 1.0f - local.y * 2.0f);
    const float4 nearPoint = mul(gParams.invViewProj, float4(ndc, -1.0f, 1.0f));
    const float4 farPoint = mul(gParams.invViewProj, float4(ndc, 1.0f, 1.0f));
    const float3 rayStart = nearPoint.xyz / nearPoint.w;
    const float3 rayDir = normalize(farPoint.xyz / farPoint.w - rayStart);
    const float3 worldPos = rayStart + rayDir * current.y;

    float2 result = float2(shadowStats.x, aoStats.x);
    if (gParams.historyValid != 0) {
        const float4 prevClip = mul(gParams.prevViewProj, float4(worldPos, 1.0f));
        if (prevClip.w > 0.0f) {
            const float2 prevNdc = prevClip.xy / prevClip.w;
            const float2 prevLocal = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
            const float2 prevPixel = (gParams.viewport.xy + prevLocal * gParams.viewport.zw) * gParams.traceScale;
            if (all(prevPixel >= 0.0f) && all(prevPixel < float2(gParams.resolution))) {
                const float2 history = gHistoryIn[int2(prevPixel)];
                const float shadowAlpha = combined ? gParams.temporalAlpha : gParams.shadowTemporalAlpha;
                result.x = lerp(clamp(history.x, shadowStats.y, shadowStats.z), shadowStats.x, shadowAlpha);
                if (!combined) {
                    result.y = lerp(clamp(history.y, aoStats.y, aoStats.z), aoStats.x, gParams.aoTemporalAlpha);
                }
            }
        }
    }

    gHistoryOut[pixel] = result;
    // Stored at half scale: the composite's 2x multiply blend (dst * src * 2) applies factors up
    // to 2, so glows can brighten.
    const float encoded = min(result.x * result.y, 2.0f) * 0.5f;
    gOutput[pixel] = float4(encoded, encoded, encoded, 1.0f);
}
