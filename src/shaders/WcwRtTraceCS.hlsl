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
// World-space vertex normals (zero for vertices without one); only meaningful on lit meshes (WCW_RT_MESH_LIT).
StructuredBuffer<float4> gWorldNorm : register(t11);
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

// The AO hemisphere's normal: on lit draws the vertex normals interpolated at the hit, so low-poly models
// (wrestlers) get smooth occlusion instead of one value per facet; otherwise the face normal. faceNormal faces
// the viewer; a smooth normal seen from behind is flipped to match.
float3 aoNormal(uint geometryIndex, uint primitiveIndex, float2 barycentrics, float3 faceNormal) {
    const uint meshStart = gMeshIndexStarts[geometryIndex];
    if ((gParams.aoSmoothNormals == 0) || ((meshStart & WCW_RT_MESH_LIT) == 0)) {
        return faceNormal;
    }

    const uint base = (meshStart & ~WCW_RT_MESH_FLAGS) + primitiveIndex * 3;
    const float3 n0 = gWorldNorm[gFaceIndices[base + 0]].xyz;
    const float3 n1 = gWorldNorm[gFaceIndices[base + 1]].xyz;
    const float3 n2 = gWorldNorm[gFaceIndices[base + 2]].xyz;
    const float3 n = n0 * (1.0f - barycentrics.x - barycentrics.y) + n1 * barycentrics.x + n2 * barycentrics.y;
    const float len = length(n);
    if (len < 1e-3f) {
        return faceNormal;
    }

    const float3 smoothNormal = n / len;
    return (dot(smoothNormal, faceNormal) < 0.0f) ? -smoothNormal : smoothNormal;
}

// Brightening from the preset's glows at a hit point (0 = none).
float glowLight(float3 hitPos, float3 normal) {
    float glow = 0.0f;
    const uint count = min(gParams.glowCount, WCW_RT_MAX_GLOWS);
    for (uint g = 0; g < count; g++) {
        const float3 toGlow = gParams.glows[g].xyz - hitPos;
        const float dist = length(toGlow);
        const float range = gParams.glows[g].w;
        const float facing = dot(normal, toGlow) / max(dist, 1e-3f);
        if ((dist >= range) || (facing <= 0.0f)) {
            continue;
        }

        const float t = dist / range;
        const float atten = (1.0f - t * t) * (1.0f - t * t);
        RayDesc glowRay;
        glowRay.Origin = hitPos + normal * 0.5f;
        glowRay.Direction = toGlow / dist;
        glowRay.TMin = 0.5f;
        glowRay.TMax = max(dist - gParams.lightClearance, 0.6f);
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, glowRay);
        while (q.Proceed()) {
            if (candidateIsSolid(q.CandidateInstanceID(), q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics())) {
                q.CommitNonOpaqueTriangleHit();
            }
        }

        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            glow += gParams.glowIntensity[g] * atten * facing;
        }
    }

    return glow;
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
    if (gParams.mode == WCW_RT_MODE_LIGHT_MAP) {
        float4 color = float4(0.0f, 0.0f, 0.0f, 0.0f);
        const float2 grid = abs(frac(hitPos.xz / 100.0f + 0.5f) - 0.5f) * 100.0f;
        if (min(grid.x, grid.y) < 2.0f) {
            color = float4(1.0f, 1.0f, 1.0f, 0.5f);
        }

        const float2 rel = hitPos.xz - gParams.lightPosition.xz;
        if (abs(rel.y) < 4.0f) {
            color = float4(1.0f, 0.2f, 0.2f, 0.9f);
        }
        else if (abs(rel.x) < 4.0f) {
            color = float4(0.2f, 0.4f, 1.0f, 0.9f);
        }

        const uint count = min(gParams.lightListCount, WCW_RT_MAX_LIGHTS);
        for (uint l = 0; l < count; l++) {
            if (length(hitPos.xz - gParams.lights[l].xz) < 25.0f) {
                color = float4(1.0f, 0.0f, 0.0f, 1.0f);
            }
        }

        gOutput[pixel] = color;
        return;
    }

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

    // Outside the receiver radius (arena, crowd): no shadow or AO rays, faded out over its
    // last 15% so the cutoff has no visible edge.
    float receiverFade = 0.0f;
    if (resolvedMode() && (gParams.receiverRadius > 0.0f)) {
        const float receiverDist = length(hitPos.xz - gParams.lightPosition.xz);
        if (receiverDist >= gParams.receiverRadius) {
            gCurrent[pixel] = float2(1.0f + glowLight(hitPos, normal), hitT);
            return;
        }

        receiverFade = smoothstep(gParams.receiverRadius * 0.85f, gParams.receiverRadius, receiverDist);
    }

    const uint lightCount = min(gParams.lightListCount, WCW_RT_MAX_LIGHTS);
    float lightWeights[WCW_RT_MAX_LIGHTS];
    float weightSum = 0.0f;
    for (uint w = 0; w < lightCount; w++) {
        float lw = gParams.lights[w].w;
        if (gParams.lightFalloff > 0.0f) {
            const float d = length(gParams.lights[w].xyz - hitPos) / gParams.lightFalloff;
            lw /= 1.0f + d * d;
        }

        lightWeights[w] = lw;
        weightSum += lw;
    }

    float lit = 0.0f;
    int occluderId = -1;
    // Heavier lights cast proportionally darker shadows, so their sampling noise shows more:
    // each light gets samples in proportion to its preset weight (key light 2 -> twice the rays).
    uint i = 0;
    for (uint l = 0; l < lightCount; l++)
    for (uint s = 0, samples = max(gParams.samplesPerLight, 1) * uint(clamp(round(gParams.lights[l].w), 1.0f, 4.0f)); s < samples; s++, i++) {
        // Area light: jittered samples on the light's panel (soft penumbrae, refined over
        // frames by the resolve pass).
        const uint seed = gParams.frameIndex * 64 + i * 2;
        const float2 jitter = float2(noise(pixel, seed), noise(pixel + uint2(37, 113), seed + 1)) - 0.5f;
        const float2 offset = jitter * gParams.lightSize;
        const float weight = lightWeights[l] / max(weightSum, 1e-6f) / float(samples);
        const float3 lightPos = gParams.lights[l].xyz + float3(offset.x, 0.0f, offset.y);
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
        shadowRay.TMax = lightDist - gParams.lightClearance;
        if (shadowRay.TMax <= shadowRay.TMin) {
            lit += weight;
            continue;
        }

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

    // Ambient occlusion: cosine-weighted hemisphere rays against all workloads' geometry, around the smooth
    // normal on lit draws. Directions that dip below the flat triangle are mirrored back above it (no self-hits
    // from the smoothing); a hit at distance t occludes by 1 - (t / radius)^2, so far hits fade in softly.
    if (gParams.aoSamples > 0) {
        const float3 hemisphere = aoNormal(primary.CommittedGeometryIndex(), primary.CommittedPrimitiveIndex(), primary.CommittedTriangleBarycentrics(), normal);
        const float3 helper = (abs(hemisphere.y) < 0.99f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        const float3 tangent = normalize(cross(helper, hemisphere));
        const float3 bitangent = cross(hemisphere, tangent);
        const float aoRadius = characterReceiver ? gParams.aoCharacterRadius : gParams.aoRadius;
        float occluded = 0.0f;
        for (uint a = 0; a < gParams.aoSamples; a++) {
            const uint seed = gParams.frameIndex * 64 + 40 + a * 2;
            const float u = noise(pixel + uint2(71, 29), seed);
            const float v = noise(pixel + uint2(13, 97), seed + 1);
            const float r = sqrt(u);
            const float phi = 6.2831853f * v;
            float3 dir = normalize(tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + hemisphere * sqrt(max(1.0f - u, 0.0f)));
            const float below = dot(dir, normal);
            if (below < 0.0f) {
                dir = normalize(dir - 2.0f * below * normal);
            }

            RayDesc aoRay;
            aoRay.Origin = hitPos + normal * 0.5f;
            aoRay.Direction = dir;
            aoRay.TMin = 0.5f;
            aoRay.TMax = aoRadius;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> ao;
            ao.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, aoRay);
            while (ao.Proceed()) {
                if (candidateIsSolid(ao.CandidateInstanceID(), ao.CandidateGeometryIndex(), ao.CandidatePrimitiveIndex(), ao.CandidateTriangleBarycentrics())) {
                    ao.CommitNonOpaqueTriangleHit();
                }
            }

            if (ao.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                const float reach = saturate(ao.CommittedRayT() / aoRadius);
                occluded += 1.0f - reach * reach;
            }
        }

        const float aoStrength = characterReceiver ? gParams.aoCharacterStrength : gParams.aoStrength;
        const float aoFactor = 1.0f - aoStrength * (occluded / float(gParams.aoSamples));
        factor = (gParams.mode == WCW_RT_MODE_AO) ? aoFactor : factor * aoFactor;
    }

    factor = lerp(factor, 1.0f, receiverFade);

    if (resolvedMode() && (gParams.mode == WCW_RT_MODE_SHADOW)) {
        factor *= 1.0f + glowLight(hitPos, normal);
    }

    gCurrent[pixel] = float2(factor, hitT);
}
