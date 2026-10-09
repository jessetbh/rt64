//
// RT64
//

// [wcw rt] Hybrid ray tracing pass (D3D12 only, inline ray queries, cs_6_5).
// Casts a primary ray from the game camera through every pixel of the traced projection's
// viewport against the frame's world-space geometry, then either visualizes the hit
// (debug modes) or traces a shadow ray toward the scene light (shadow mode).

#include "shared/rt64_wcw_rt.h"

ConstantBuffer<WcwRtParams> gParams : register(b0);
RaytracingAccelerationStructure gScene : register(t1);
StructuredBuffer<float4> gWorldPos : register(t2);
StructuredBuffer<uint> gFaceIndices : register(t3);
StructuredBuffer<uint> gMeshIndexStarts : register(t4);
RWTexture2D<float4> gOutput : register(u5);

float3 triangleNormal(uint geometryIndex, uint primitiveIndex) {
    const uint base = gMeshIndexStarts[geometryIndex] + primitiveIndex * 3;
    const float3 v0 = gWorldPos[gFaceIndices[base + 0]].xyz;
    const float3 v1 = gWorldPos[gFaceIndices[base + 1]].xyz;
    const float3 v2 = gWorldPos[gFaceIndices[base + 2]].xyz;
    const float3 n = cross(v1 - v0, v2 - v0);
    const float len = length(n);
    return (len > 1e-8f) ? (n / len) : float3(0.0f, 1.0f, 0.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint2 pixel : SV_DispatchThreadID) {
    if (any(pixel >= gParams.resolution)) {
        return;
    }

    const float2 pixelCenter = float2(pixel) + 0.5f;
    const float2 local = (pixelCenter - gParams.viewport.xy) / gParams.viewport.zw;
    if (any(local < 0.0f) || any(local > 1.0f)) {
        gOutput[pixel] = float4(1.0f, 1.0f, 1.0f, 0.0f);
        return;
    }

    // Primary rays span the clip volume (N64 projections map near/far to NDC z -1/+1), so
    // geometry the raster clips at the near plane (e.g. a wrestler right at the camera) is skipped.
    const float2 ndc = float2(local.x * 2.0f - 1.0f, 1.0f - local.y * 2.0f);
    const float4 nearPoint = mul(gParams.invViewProj, float4(ndc, -1.0f, 1.0f));
    const float4 farPoint = mul(gParams.invViewProj, float4(ndc, 1.0f, 1.0f));
    const float3 rayStart = nearPoint.xyz / nearPoint.w;
    const float3 rayEnd = farPoint.xyz / farPoint.w;

    RayDesc ray;
    ray.Origin = rayStart;
    ray.Direction = normalize(rayEnd - rayStart);
    ray.TMin = 0.0f;
    ray.TMax = length(rayEnd - rayStart);

    RayQuery<RAY_FLAG_FORCE_OPAQUE> primary;
    primary.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, ray);
    primary.Proceed();
    if (primary.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        gOutput[pixel] = float4(1.0f, 1.0f, 1.0f, 0.0f);
        return;
    }

    const float hitT = primary.CommittedRayT();
    float3 normal = triangleNormal(primary.CommittedGeometryIndex(), primary.CommittedPrimitiveIndex());
    if (dot(normal, ray.Direction) > 0.0f) {
        normal = -normal;
    }

    if (gParams.mode == WCW_RT_MODE_HIT_NORMAL) {
        gOutput[pixel] = float4(normal * 0.5f + 0.5f, 0.75f);
        return;
    }
    else if (gParams.mode == WCW_RT_MODE_HIT_MESH) {
        const uint id = primary.CommittedGeometryIndex();
        const float3 hue = frac(float(id) * float3(0.618034f, 0.414214f, 0.732051f));
        gOutput[pixel] = float4(hue, 0.75f);
        return;
    }
    else if (gParams.mode == WCW_RT_MODE_HIT_DEPTH) {
        const float shade = frac(hitT / 200.0f);
        gOutput[pixel] = float4(shade, shade, shade, 0.75f);
        return;
    }

    const float3 hitPos = ray.Origin + ray.Direction * hitT;
    const float2 lightOffsets[4] = { float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(-1.0f, 1.0f), float2(1.0f, 1.0f) };
    const uint lightCount = clamp(gParams.lightCount, 1, 4);
    float lit = 0.0f;
    int occluderId = -1;
    for (uint l = 0; l < lightCount; l++) {
        const float2 offset = (lightCount > 1) ? lightOffsets[l] * gParams.lightSpread : float2(0.0f, 0.0f);
        const float3 lightPos = gParams.lightPosition.xyz + float3(offset.x, 0.0f, offset.y);
        const float3 toLight = lightPos - hitPos;
        const float lightDist = length(toLight);
        RayDesc shadowRay;
        shadowRay.Origin = hitPos + normal * 0.5f;
        shadowRay.Direction = toLight / lightDist;
        shadowRay.TMin = 0.5f;
        shadowRay.TMax = lightDist;

        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> shadow;
        shadow.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, shadowRay);
        shadow.Proceed();
        if (shadow.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            occluderId = int(shadow.CommittedGeometryIndex());
        }
        else {
            lit += 1.0f;
        }
    }

    if (gParams.mode == WCW_RT_MODE_SHADOW_OCCLUDER) {
        if (occluderId >= 0) {
            gOutput[pixel] = float4(frac(float(occluderId) * float3(0.618034f, 0.414214f, 0.732051f)), 0.75f);
        }
        else {
            gOutput[pixel] = float4(1.0f, 1.0f, 1.0f, 0.0f);
        }

        return;
    }

    const float factor = 1.0f - gParams.shadowStrength * (1.0f - lit / float(lightCount));
    gOutput[pixel] = float4(factor, factor, factor, 1.0f);
}
