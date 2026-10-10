//
// RT64
//

// [wcw rt] Hybrid ray tracing pass (D3D12 only, inline ray queries, cs_6_5).
// Casts a primary ray from the game camera through every pixel of the traced projection's
// viewport against the frame's world-space geometry, then either visualizes the hit
// (debug modes) or traces a shadow ray toward the scene light (shadow mode).

#include "shared/rt64_wcw_rt.h"
#include "shared/rt64_f3d_defines.h"
#include "shared/rt64_gpu_tile.h"
#include "shared/rt64_rdp_tile.h"

ConstantBuffer<WcwRtParams> gParams : register(b0);
RaytracingAccelerationStructure gScene : register(t1);
StructuredBuffer<float4> gWorldPos : register(t2);
StructuredBuffer<uint> gFaceIndices : register(t3);
StructuredBuffer<uint> gMeshIndexStarts : register(t4);
RWTexture2D<float4> gOutput : register(u5);
// Shadow mode: (shadow factor, primary hit distance; 0 = no hit) for the resolve pass.
RWTexture2D<float2> gCurrent : register(u6);
StructuredBuffer<RDPTile> gRDPTiles : register(t7);
StructuredBuffer<GPUTile> gGPUTiles : register(t8);
StructuredBuffer<float2> gTexCoords : register(t9);
StructuredBuffer<uint> gMeshTiles : register(t10);
// The framebuffer renderer's bindless RGBA texture cache.
Texture2D<float4> gTextures[8192] : register(t0, space1);

int moduloInt(int a, int b) {
    return (b > 0) ? (a - b * int(floor(float(a) / float(b)))) : a;
}

// Texel alpha of an alpha-tested draw at a hit, point sampled with the same tile transform
// and clamp/wrap/mirror rules as TextureSampler.hlsli. Tiles needing raw TMEM decoding are
// treated as opaque.
float hitAlpha(uint geometryIndex, uint primitiveIndex, float2 barycentrics) {
    const uint tileWord = gMeshTiles[geometryIndex];
    const uint base = (gMeshIndexStarts[geometryIndex] & ~WCW_RT_MESH_FLAGS) + primitiveIndex * 3;
    const float2 uv0 = gTexCoords[gFaceIndices[base + 0]];
    const float2 uv1 = gTexCoords[gFaceIndices[base + 1]];
    const float2 uv2 = gTexCoords[gFaceIndices[base + 2]];
    const float2 inputUV = uv0 * (1.0f - barycentrics.x - barycentrics.y) + uv1 * barycentrics.x + uv2 * barycentrics.y;

    const uint tileIndex = tileWord & WCW_RT_TILE_INDEX_MASK;
    const RDPTile rdpTile = gRDPTiles[tileIndex];
    const GPUTile gpuTile = gGPUTiles[tileIndex];
    if (gpuTileFlagRawTMEM(gpuTile.flags)) {
        return 1.0f;
    }

    const float correction = ((tileWord & WCW_RT_TILE_UV_CORRECTION) != 0) ? 0.5f : 1.0f;
    float2 uvCoord = inputUV * correction * float2(rdpTile.shifts, rdpTile.shiftt);
    if (gpuTileFlagShiftedByHalf(gpuTile.flags)) {
        uvCoord += float2(0.5f, 0.5f);
    }

    uvCoord *= gpuTile.tcScale;
    uvCoord -= (float2(rdpTile.uls, rdpTile.ult) * gpuTile.ulScale) / 4.0f;
    int2 texel = int2(floor(uvCoord));
    const float2 tcScale = gpuTile.tcScale;
    if (rdpTile.cms & G_TX_CLAMP) {
        texel.x = clamp(texel.x, 0, int(round(tcScale.x * rdpTile.lrs) / 4 - round(tcScale.x * rdpTile.uls) / 4 + round(tcScale.x - 1.0f)));
    }

    if (rdpTile.cmt & G_TX_CLAMP) {
        texel.y = clamp(texel.y, 0, int(round(tcScale.y * rdpTile.lrt) / 4 - round(tcScale.y * rdpTile.ult) / 4 + round(tcScale.y - 1.0f)));
    }

    const int masks = int(round(rdpTile.masks * tcScale.x));
    if ((rdpTile.cms & G_TX_MIRROR) && (masks > 0) && (moduloInt(texel.x, masks * 2) >= masks)) {
        texel.x = (masks - 1) - moduloInt(texel.x, masks);
    }
    else {
        texel.x = moduloInt(texel.x, masks);
    }

    const int maskt = int(round(rdpTile.maskt * tcScale.y));
    if ((rdpTile.cmt & G_TX_MIRROR) && (maskt > 0) && (moduloInt(texel.y, maskt * 2) >= maskt)) {
        texel.y = (maskt - 1) - moduloInt(texel.y, maskt);
    }
    else {
        texel.y = moduloInt(texel.y, maskt);
    }

    const int2 size = int2(gpuTile.textureDimensions.xy);
    texel = clamp(texel, int2(0, 0), max(size - 1, int2(0, 0)));
    return gTextures[NonUniformResourceIndex(gpuTile.textureIndex)].Load(int3(texel, 0)).a;
}

// Non-opaque candidates of this workload's alpha-tested draws only count where the texel is
// solid; other workloads' geometry (instance ID > 0) has no bound texture data here.
bool candidateIsSolid(uint instanceId, uint geometryIndex, uint primitiveIndex, float2 barycentrics) {
    if ((instanceId != 0) || ((gMeshTiles[geometryIndex] & WCW_RT_TILE_ALPHA_TEST) == 0)) {
        return true;
    }

    return hitAlpha(geometryIndex, primitiveIndex, barycentrics) >= 0.5f;
}

// Interleaved gradient noise, decorrelated per frame and per use.
float noise(uint2 pixel, uint seed) {
    const float2 p = float2(pixel) + float(seed) * 5.588238f;
    return frac(52.9829189f * frac(0.06711056f * p.x + 0.00583715f * p.y));
}

bool resolvedMode() {
    return (gParams.mode == WCW_RT_MODE_SHADOW) || (gParams.mode == WCW_RT_MODE_AO);
}

void writeNoHit(uint2 pixel) {
    if (resolvedMode()) {
        gCurrent[pixel] = float2(1.0f, 0.0f);
    }
    else {
        gOutput[pixel] = float4(1.0f, 1.0f, 1.0f, 0.0f);
    }
}

// Unnormalized: the winding (v0, v1, v2) is kept, which back-face culling relies on.
float3 triangleCross(uint geometryIndex, uint primitiveIndex) {
    const uint base = (gMeshIndexStarts[geometryIndex] & ~WCW_RT_MESH_FLAGS) + primitiveIndex * 3;
    const float3 v0 = gWorldPos[gFaceIndices[base + 0]].xyz;
    const float3 v1 = gWorldPos[gFaceIndices[base + 1]].xyz;
    const float3 v2 = gWorldPos[gFaceIndices[base + 2]].xyz;
    return cross(v1 - v0, v2 - v0);
}

float3 triangleNormal(uint geometryIndex, uint primitiveIndex) {
    const uint base = (gMeshIndexStarts[geometryIndex] & ~WCW_RT_MESH_FLAGS) + primitiveIndex * 3;
    const float3 v0 = gWorldPos[gFaceIndices[base + 0]].xyz;
    const float3 v1 = gWorldPos[gFaceIndices[base + 1]].xyz;
    const float3 v2 = gWorldPos[gFaceIndices[base + 2]].xyz;
    const float3 n = cross(v1 - v0, v2 - v0);
    const float len = length(n);
    return (len > 1e-8f) ? (n / len) : float3(0.0f, 1.0f, 0.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint2 dispatchPixel : SV_DispatchThreadID) {
    const uint2 pixel = dispatchPixel + gParams.traceOffset;
    if (any(pixel >= gParams.resolution)) {
        return;
    }

    const float2 pixelCenter = (float2(pixel) + 0.5f) / gParams.traceScale;
    const float2 local = (pixelCenter - gParams.viewport.xy) / gParams.viewport.zw;
    if (any(local < 0.0f) || any(local > 1.0f)) {
        writeNoHit(pixel);
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

    // Culled draws are non-opaque: commit their candidates only when front-facing, matching
    // the raster (N64 front faces wind counter-clockwise as seen from the camera).
    // Primary rays only see this workload's geometry (instance mask 0x1); the other workloads'
    // geometry (0x2) is drawn and shaded by their own passes but still casts shadows here.
    RayQuery<RAY_FLAG_NONE> primary;
    primary.TraceRayInline(gScene, RAY_FLAG_NONE, 0x1, ray);
    while (primary.Proceed()) {
        if (primary.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            const uint geometryIndex = primary.CandidateGeometryIndex();
            const uint primitiveIndex = primary.CandidatePrimitiveIndex();
            const bool culled = (gMeshIndexStarts[geometryIndex] & WCW_RT_MESH_CULLED) != 0;
            const float3 n = triangleCross(geometryIndex, primitiveIndex);
            const bool facing = !culled || ((dot(n, ray.Direction) * gParams.cullSign) < 0.0f);
            if (facing && candidateIsSolid(primary.CandidateInstanceID(), geometryIndex, primitiveIndex, primary.CandidateTriangleBarycentrics())) {
                primary.CommitNonOpaqueTriangleHit();
            }
        }
    }

    if (primary.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        writeNoHit(pixel);
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
    if (gParams.mode == WCW_RT_MODE_HIT_POSITION) {
        // R/B: 50-unit X/Z grid; G: height in 0..300.
        const float2 grid = step(0.5f, frac(hitPos.xz / 50.0f));
        gOutput[pixel] = float4(grid.x, saturate(hitPos.y / 300.0f), grid.y, 0.85f);
        return;
    }

    if (gParams.mode == WCW_RT_MODE_SHADOW_DISTANCE) {
        // Key light only, closest hit: red = hit distance / light distance, blue = unoccluded.
        const float3 toLight = gParams.lightPosition.xyz - hitPos;
        const float lightDist = length(toLight);
        RayDesc probe;
        probe.Origin = hitPos + normal * 0.5f;
        probe.Direction = toLight / lightDist;
        probe.TMin = 0.5f;
        probe.TMax = lightDist;
        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, probe);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            gOutput[pixel] = float4(saturate(q.CommittedRayT() / lightDist), 0.0f, 0.0f, 0.85f);
        }
        else {
            gOutput[pixel] = float4(0.0f, 0.0f, 1.0f, 0.85f);
        }

        return;
    }

    const float2 lightOffsets[5] = { float2(0.0f, 0.0f), float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(-1.0f, 1.0f), float2(1.0f, 1.0f) };
    const uint fillCount = min(gParams.lightCount, 4);
    const float keyWeight = (fillCount > 0) ? gParams.keyWeight : 1.0f;
    const float fillWeight = (fillCount > 0) ? (1.0f - keyWeight) / float(fillCount) : 0.0f;
    float lit = 0.0f;
    int occluderId = -1;
    const uint samples = max(gParams.samplesPerLight, 1);
    for (uint i = 0; i < (fillCount + 1) * samples; i++) {
        // Area light: jittered samples on the light's panel (soft penumbrae, refined over
        // frames by the resolve pass).
        const uint l = i / samples;
        const uint seed = gParams.frameIndex * 64 + i * 2;
        const float2 jitter = float2(noise(pixel, seed), noise(pixel + uint2(37, 113), seed + 1)) - 0.5f;
        const float2 offset = lightOffsets[l] * gParams.lightSpread + jitter * gParams.lightSize;
        const float weight = ((l == 0) ? keyWeight : fillWeight) / float(samples);
        const float3 lightPos = gParams.lightPosition.xyz + float3(offset.x, 0.0f, offset.y);
        const float3 toLight = lightPos - hitPos;
        const float lightDist = length(toLight);

        // Surfaces facing away from a light are already dark from N64 vertex lighting:
        // only light-facing surfaces receive its shadow (no double darkening).
        if (dot(normal, toLight) <= 0.0f) {
            lit += weight;
            continue;
        }

        RayDesc shadowRay;
        shadowRay.Origin = hitPos + normal * 0.5f;
        shadowRay.Direction = toLight / lightDist;
        shadowRay.TMin = 0.5f;
        shadowRay.TMax = lightDist;

        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> shadow;
        shadow.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, shadowRay);
        while (shadow.Proceed()) {
            if (candidateIsSolid(shadow.CandidateInstanceID(), shadow.CandidateGeometryIndex(), shadow.CandidatePrimitiveIndex(), shadow.CandidateTriangleBarycentrics())) {
                shadow.CommitNonOpaqueTriangleHit();
            }
        }

        if (shadow.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            occluderId = int(shadow.CommittedGeometryIndex());
        }
        else {
            lit += weight;
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

    const bool characterReceiver = (gMeshIndexStarts[primary.CommittedGeometryIndex()] & WCW_RT_MESH_CHARACTER) != 0;
    const float strength = characterReceiver ? gParams.characterShadowStrength : gParams.shadowStrength;
    float factor = 1.0f - strength * (1.0f - lit);

    // Contact shadows: short cosine-weighted hemisphere rays against all workloads' geometry.
    if (gParams.aoSamples > 0) {
        const float3 helper = (abs(normal.y) < 0.99f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        const float3 tangent = normalize(cross(helper, normal));
        const float3 bitangent = cross(normal, tangent);
        float occluded = 0.0f;
        for (uint a = 0; a < gParams.aoSamples; a++) {
            const uint seed = gParams.frameIndex * 64 + 40 + a * 2;
            const float u = noise(pixel + uint2(71, 29), seed);
            const float v = noise(pixel + uint2(13, 97), seed + 1);
            const float r = sqrt(u);
            const float phi = 6.2831853f * v;
            const float3 dir = normalize(tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + normal * sqrt(max(1.0f - u, 0.0f)));

            RayDesc aoRay;
            aoRay.Origin = hitPos + normal * 0.5f;
            aoRay.Direction = dir;
            aoRay.TMin = 0.5f;
            aoRay.TMax = gParams.aoRadius;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> ao;
            ao.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, aoRay);
            while (ao.Proceed()) {
                if (candidateIsSolid(ao.CandidateInstanceID(), ao.CandidateGeometryIndex(), ao.CandidatePrimitiveIndex(), ao.CandidateTriangleBarycentrics())) {
                    ao.CommitNonOpaqueTriangleHit();
                }
            }

            if (ao.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                occluded += 1.0f;
            }
        }

        const float aoStrength = gParams.aoStrength * (characterReceiver ? 0.5f : 1.0f);
        const float aoFactor = 1.0f - aoStrength * (occluded / float(gParams.aoSamples));
        factor = (gParams.mode == WCW_RT_MODE_AO) ? aoFactor : factor * aoFactor;
    }

    gCurrent[pixel] = float2(factor, hitT);
}
