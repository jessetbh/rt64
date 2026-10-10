//
// RT64
//

#include "rt64_wcw_raytracer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "rt64_descriptor_sets.h"
#include "rt64_render_target.h"
#include "shared/rt64_gpu_tile.h"
#include "shared/rt64_rdp_tile.h"
#include "rt64_render_worker.h"

#ifdef _WIN32
#   include "shaders/FullScreenVS.hlsl.dxil.h"
#   include "shaders/WcwRtTraceCS.hlsl.dxil.h"
#   include "shaders/WcwRtCompositePS.hlsl.dxil.h"
#   include "shaders/WcwRtResolveCS.hlsl.dxil.h"
#endif

namespace RT64 {
    std::atomic<int> wcwRtPresentCounter = 0;

    static uint64_t alignUp(uint64_t value, uint64_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    static float envFloat(const char *text, float fallback) {
        return (text != nullptr) ? float(atof(text)) : fallback;
    }

    static std::atomic<bool> wcwRtUserEnabled = false;
    static std::atomic<uint32_t> wcwRtUserQuality = 1;

    void WcwRaytracer::setUserSettings(bool enabled, uint32_t quality) {
        wcwRtUserEnabled = enabled;
        wcwRtUserQuality = std::min(quality, 2u);
    }

    bool WcwRaytracer::active() {
        // WCW_RT=1 forces it on, WCW_RT=0 forces it off (whatever the menu says).
        static const char *envText = getenv("WCW_RT");
        static const int forced = (envText == nullptr) ? -1 : ((atoi(envText) != 0) ? 1 : 0);
        return (forced >= 0) ? (forced == 1) : wcwRtUserEnabled.load();
    }

    uint32_t WcwRaytracer::userQuality() {
        return wcwRtUserQuality;
    }

    WcwRaytracer::WcwRaytracer(RenderDevice *device, RenderShaderFormat shaderFormat) {
        this->device = device;
        this->shaderFormat = shaderFormat;

        if (const char *modeText = getenv("WCW_RT_MODE")) {
            mode = uint32_t(atoi(modeText));
        }

        const RenderDeviceCapabilities capabilities = device->getCapabilities();
#   ifdef _WIN32
        supported = (shaderFormat == RenderShaderFormat::DXIL) && capabilities.raytracing && capabilities.raytracingStateUpdate;
#   endif
        fprintf(stderr, "[wcw rt] raytracer: dxr=%d dxr1.1=%d dxil=%d -> %s (mode %u)\n", capabilities.raytracing, capabilities.raytracingStateUpdate,
            shaderFormat == RenderShaderFormat::DXIL, supported ? "enabled" : "UNSUPPORTED", mode);

        if (!supported) {
            return;
        }

#   ifdef _WIN32
        // Trace pass: b0 params, t1 scene, t2 world positions, t3 face indices, t4 mesh index starts, u5 output.
        RenderDescriptorSetBuilder traceBuilder;
        traceBuilder.begin();
        traceBuilder.addConstantBuffer(0);
        traceBuilder.addAccelerationStructure(1);
        traceBuilder.addStructuredBuffer(2);
        traceBuilder.addStructuredBuffer(3);
        traceBuilder.addStructuredBuffer(4);
        traceBuilder.addReadWriteTexture(5);
        traceBuilder.addReadWriteTexture(6);
        traceBuilder.addStructuredBuffer(7);
        traceBuilder.addStructuredBuffer(8);
        traceBuilder.addStructuredBuffer(9);
        traceBuilder.addStructuredBuffer(10);
        traceBuilder.end();
        traceSet = traceBuilder.create(device);

        // Set 1: the framebuffer renderer's bindless texture cache (gTextures, space1).
        RenderPipelineLayoutBuilder layoutBuilder;
        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(traceBuilder);
        layoutBuilder.addDescriptorSet(FramebufferRendererDescriptorTextureSet());
        layoutBuilder.end();
        traceLayout = layoutBuilder.create(device);

        std::unique_ptr<RenderShader> traceShader = device->createShader(WcwRtTraceCSBlobDXIL, std::size(WcwRtTraceCSBlobDXIL), "CSMain", RenderShaderFormat::DXIL);
        tracePipeline = device->createComputePipeline(RenderComputePipelineDesc(traceLayout.get(), traceShader.get(), 8, 8, 1));

        // Resolve pass: b0 params, t1 current samples, t2 history in, u3 history out, u4 output.
        RenderDescriptorSetBuilder resolveBuilder;
        resolveBuilder.begin();
        resolveBuilder.addConstantBuffer(0);
        resolveBuilder.addTexture(1);
        resolveBuilder.addTexture(2);
        resolveBuilder.addReadWriteTexture(3);
        resolveBuilder.addReadWriteTexture(4);
        resolveBuilder.end();
        resolveSet = resolveBuilder.create(device);

        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(resolveBuilder);
        layoutBuilder.end();
        resolveLayout = layoutBuilder.create(device);

        std::unique_ptr<RenderShader> resolveShader = device->createShader(WcwRtResolveCSBlobDXIL, std::size(WcwRtResolveCSBlobDXIL), "CSMain", RenderShaderFormat::DXIL);
        resolvePipeline = device->createComputePipeline(RenderComputePipelineDesc(resolveLayout.get(), resolveShader.get(), 8, 8, 1));

        // Composite pass: t0 trace output.
        RenderDescriptorSetBuilder compositeBuilder;
        compositeBuilder.begin();
        compositeBuilder.addTexture(0);
        compositeBuilder.end();
        compositeSet = compositeBuilder.create(device);

        layoutBuilder.begin();
        layoutBuilder.addPushConstant(0, 0, sizeof(interop::WcwRtCompositeCB), RenderShaderStageFlag::PIXEL);
        layoutBuilder.addDescriptorSet(compositeBuilder);
        layoutBuilder.end();
        compositeLayout = layoutBuilder.create(device);

        fullScreenVS = device->createShader(FullScreenVSBlobDXIL, std::size(FullScreenVSBlobDXIL), "VSMain", RenderShaderFormat::DXIL);
        compositePS = device->createShader(WcwRtCompositePSBlobDXIL, std::size(WcwRtCompositePSBlobDXIL), "PSMain", RenderShaderFormat::DXIL);
#   endif

        if (getenv("WCW_GPU_LOG") != nullptr) {
            timingPool = device->createQueryPool(StampCount);
        }

        paramsBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(alignUp(sizeof(interop::WcwRtParams), 256), RenderBufferFlag::CONSTANT));
    }

    void WcwRaytracer::beginWorkload(uint64_t presentId) {
        if (presentId != lastPresentId) {
            lastPresentId = presentId;
            workloadInFrame = 0;
            frameCounter++;
        }
        else {
            workloadInFrame++;
        }

        currentSlot = std::min(workloadInFrame, MaxSlots - 1);
    }

    void WcwRaytracer::reset() {
        if (!active()) {
            wasActive = false;
        }

        sceneActive = false;
        structuresBuilt = false;
        meshes.clear();
        meshIndexStarts.clear();
        meshTiles.clear();
    }

    bool WcwRaytracer::wantsScene() const {
        return supported && active() && !sceneActive;
    }

    bool WcwRaytracer::replacesBlobShadows() const {
        static const bool keepBlobs = getenv("WCW_RT_KEEP_BLOBS") != nullptr;
        return supported && active() && (mode == WCW_RT_MODE_SHADOW) && !keepBlobs;
    }

    void WcwRaytracer::beginScene(const hlslpp::float4x4 &view, const hlslpp::float4x4 &proj, const RenderViewport &viewport,
        const RenderBuffer *worldPosBuffer, uint64_t worldPosSize, const RenderBuffer *faceIndicesBuffer, uint64_t faceIndicesSize,
        const RenderBuffer *rdpTilesBuffer, const RenderBuffer *gpuTilesBuffer, const RenderBuffer *texCoordBuffer)
    {
        boundsViewProj = hlslpp::mul(view, proj);
        boundsMin[0] = boundsMin[1] = FLT_MAX;
        boundsMax[0] = boundsMax[1] = -FLT_MAX;
        boundsFullScreen = false;
        this->rdpTilesBuffer = rdpTilesBuffer;
        this->gpuTilesBuffer = gpuTilesBuffer;
        this->texCoordBuffer = texCoordBuffer;
        sceneActive = true;
        viewMatrix = view;
        projMatrix = proj;
        this->viewport = viewport;
        this->worldPosBuffer = worldPosBuffer;
        this->worldPosSize = worldPosSize;
        this->faceIndicesBuffer = faceIndicesBuffer;
        this->faceIndicesSize = faceIndicesSize;
    }

    void WcwRaytracer::addMesh(uint32_t faceIndicesStart, uint32_t triangleCount, uint32_t vertexCount, bool culled, bool character,
        bool alphaTest, uint32_t tileIndex, bool uvCorrection)
    {
        // Without tile data the alpha test can't be evaluated: keep the draw opaque.
        alphaTest = alphaTest && (rdpTilesBuffer != nullptr) && (gpuTilesBuffer != nullptr) && (texCoordBuffer != nullptr);
        const uint32_t IndexStride = sizeof(uint32_t);
        const uint32_t PosStride = sizeof(float) * 4;
        meshes.emplace_back(RenderBufferReference(faceIndicesBuffer, faceIndicesStart * IndexStride), RenderBufferReference(worldPosBuffer, 0),
            RenderFormat::R32_UINT, RenderFormat::R32G32B32_FLOAT, triangleCount * 3, vertexCount, PosStride, !culled && !alphaTest);

        // Culled draws are non-opaque so the trace shader sees their candidates; the top bits
        // carry the culled/character flags.
        meshIndexStarts.emplace_back(faceIndicesStart | (culled ? WCW_RT_MESH_CULLED : 0u) | (character ? WCW_RT_MESH_CHARACTER : 0u));
        meshTiles.emplace_back((tileIndex & WCW_RT_TILE_INDEX_MASK) | (alphaTest ? WCW_RT_TILE_ALPHA_TEST : 0u) | (uvCorrection ? WCW_RT_TILE_UV_CORRECTION : 0u));
    }

    void WcwRaytracer::extendBounds(const float *positions) {
        if (boundsFullScreen) {
            return;
        }

        // Clip the triangle against the near plane (w > NearW) so vertices behind the camera
        // (crowd and set pieces around it) don't blow the footprint up to the whole screen.
        const float NearW = 1e-2f;
        hlslpp::float4 clip[3];
        bool inside[3];
        uint32_t insideCount = 0;
        for (uint32_t i = 0; i < 3; i++) {
            clip[i] = hlslpp::mul(hlslpp::float4(positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2], 1.0f), boundsViewProj);
            inside[i] = float(clip[i].w) > NearW;
            insideCount += inside[i] ? 1 : 0;
        }

        if (insideCount == 0) {
            return;
        }

        auto extend = [&](const hlslpp::float4 &c) {
            const float w = c.w;
            const float px = viewport.x + (float(c.x) / w * 0.5f + 0.5f) * viewport.width;
            const float py = viewport.y + (0.5f - float(c.y) / w * 0.5f) * viewport.height;
            boundsMin[0] = std::min(boundsMin[0], px);
            boundsMin[1] = std::min(boundsMin[1], py);
            boundsMax[0] = std::max(boundsMax[0], px);
            boundsMax[1] = std::max(boundsMax[1], py);
        };

        for (uint32_t i = 0; i < 3; i++) {
            const uint32_t j = (i + 1) % 3;
            if (inside[i]) {
                extend(clip[i]);
            }

            if (inside[i] != inside[j]) {
                const float t = (NearW - float(clip[i].w)) / (float(clip[j].w) - float(clip[i].w));
                extend(clip[i] + (clip[j] - clip[i]) * t);
            }
        }
    }

    void WcwRaytracer::recordBuild(RenderWorker *worker) {
        structuresBuilt = false;
        Slot &slot = slots[currentSlot];
        if (!sceneActive || meshes.empty()) {
            slot.valid = false;
            return;
        }

        // Bottom level: every gathered draw call is one geometry over the shared world position buffer.
        device->setBottomLevelASBuildInfo(blasInfo, meshes.data(), uint32_t(meshes.size()), true, false);
        if (blasInfo.accelerationStructureSize > slot.blasBufferSize) {
            slot.blasBufferSize = alignUp((blasInfo.accelerationStructureSize * 3) / 2, 256);
            slot.blasBuffer = device->createBuffer(RenderBufferDesc::AccelerationStructureBuffer(slot.blasBufferSize));
            slot.blas = device->createAccelerationStructure(RenderAccelerationStructureDesc(RenderAccelerationStructureType::BOTTOM_LEVEL, RenderBufferReference(slot.blasBuffer.get()), slot.blasBufferSize));
        }

        slot.valid = true;
        slot.builtFrame = frameCounter;
        if (timingPool != nullptr) {
            worker->commandList->resetQueryPool(timingPool.get(), 0, StampCount);
            worker->commandList->writeTimestamp(timingPool.get(), StampBuildBegin);
            timingWritten = (1u << StampBuildBegin);
            timingMeshes = uint32_t(meshes.size());
            timingTriangles = 0;
            timingAlphaMeshes = 0;
            timingPixels = 0;
            for (size_t m = 0; m < meshes.size(); m++) {
                timingTriangles += meshes[m].indexCount / 3;
                timingAlphaMeshes += (meshTiles[m] & WCW_RT_TILE_ALPHA_TEST) ? 1 : 0;
            }
        }

        // Top level: this workload's geometry (mask 0x1: primary and shadow rays) plus the other
        // workload slots built this frame or the previous one (mask 0x2: shadow rays only).
        instances.clear();
        instances.emplace_back(RenderBufferReference(slot.blasBuffer.get()), 0, 0x1, 0, true, RenderAffineTransform());
        for (uint32_t s = 0; s < MaxSlots; s++) {
            const Slot &other = slots[s];
            if ((s != currentSlot) && other.valid && ((frameCounter - other.builtFrame) <= 1)) {
                instances.emplace_back(RenderBufferReference(other.blasBuffer.get()), s + 1, 0x2, 0, true, RenderAffineTransform());
            }
        }

        device->setTopLevelASBuildInfo(tlasInfo, instances.data(), uint32_t(instances.size()), true, false);
        if (tlasInfo.accelerationStructureSize > tlasBufferSize) {
            tlasBufferSize = alignUp((tlasInfo.accelerationStructureSize * 3) / 2, 256);
            tlasBuffer = device->createBuffer(RenderBufferDesc::AccelerationStructureBuffer(tlasBufferSize));
            tlas = device->createAccelerationStructure(RenderAccelerationStructureDesc(RenderAccelerationStructureType::TOP_LEVEL, RenderBufferReference(tlasBuffer.get()), tlasBufferSize));
        }

        const uint64_t blasScratch = alignUp(blasInfo.scratchSize, 256);
        const uint64_t requiredScratch = blasScratch + alignUp(tlasInfo.scratchSize, 256);
        if (requiredScratch > scratchBufferSize) {
            scratchBufferSize = alignUp((requiredScratch * 3) / 2, 256);
            scratchBuffer = device->createBuffer(RenderBufferDesc::DefaultBuffer(scratchBufferSize, RenderBufferFlag::ACCELERATION_STRUCTURE_SCRATCH | RenderBufferFlag::UNORDERED_ACCESS));
        }

        const uint64_t instancesSize = tlasInfo.instancesBufferData.size();
        if (instancesSize > instancesBufferSize) {
            instancesBufferSize = alignUp(instancesSize, 256);
            instancesBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(instancesBufferSize, RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT));
        }

        void *instancesBytes = instancesBuffer->map();
        memcpy(instancesBytes, tlasInfo.instancesBufferData.data(), instancesSize);
        instancesBuffer->unmap();

        const uint64_t meshStartsSize = meshIndexStarts.size() * sizeof(uint32_t);
        if (meshStartsSize > meshStartsBufferSize) {
            meshStartsBufferSize = alignUp((meshStartsSize * 3) / 2, 256);
            meshStartsBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(meshStartsBufferSize, RenderBufferFlag::STORAGE));
        }

        void *meshStartsBytes = meshStartsBuffer->map();
        memcpy(meshStartsBytes, meshIndexStarts.data(), meshStartsSize);
        meshStartsBuffer->unmap();

        const uint64_t meshTilesSize = meshTiles.size() * sizeof(uint32_t);
        if (meshTilesSize > meshTilesBufferSize) {
            meshTilesBufferSize = alignUp((meshTilesSize * 3) / 2, 256);
            meshTilesBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(meshTilesBufferSize, RenderBufferFlag::STORAGE));
        }

        void *meshTilesBytes = meshTilesBuffer->map();
        memcpy(meshTilesBytes, meshTiles.data(), meshTilesSize);
        meshTilesBuffer->unmap();

        worker->commandList->buildBottomLevelAS(slot.blas.get(), RenderBufferReference(scratchBuffer.get(), 0), blasInfo);
        worker->commandList->barriers(RenderBarrierStage::COMPUTE, RenderBufferBarrier(slot.blasBuffer.get(), RenderBufferAccess::READ));
        worker->commandList->buildTopLevelAS(tlas.get(), RenderBufferReference(scratchBuffer.get(), blasScratch), RenderBufferReference(instancesBuffer.get()), tlasInfo);
        worker->commandList->barriers(RenderBarrierStage::COMPUTE, RenderBufferBarrier(tlasBuffer.get(), RenderBufferAccess::READ));
        structuresBuilt = true;
        if (timingPool != nullptr) {
            worker->commandList->writeTimestamp(timingPool.get(), StampBuildEnd);
            timingWritten |= (1u << StampBuildEnd);
        }

        static const bool wcwRtLog = getenv("WCW_RT_LOG") != nullptr;
        if (wcwRtLog) {
            static uint32_t buildCount = 0;
            if ((buildCount++ % 300) == 0) {
                uint32_t triangleCount = 0;
                for (const RenderBottomLevelASMesh &mesh : meshes) {
                    triangleCount += mesh.indexCount / 3;
                }

                fprintf(stderr, "[wcw rt] build #%u slot %u (+%zu other): %zu meshes, %u triangles, blas=%llu tlas=%llu scratch=%llu vp=(%.0f,%.0f %.0fx%.0f)\n",
                    buildCount, currentSlot, instances.size() - 1, meshes.size(), triangleCount, (unsigned long long)blasInfo.accelerationStructureSize,
                    (unsigned long long)tlasInfo.accelerationStructureSize, (unsigned long long)requiredScratch,
                    viewport.x, viewport.y, viewport.width, viewport.height);
            }
        }
    }

    void WcwRaytracer::recordTrace(RenderWorker *worker, RenderTarget *colorTarget, RenderDescriptorSet *textureSet) {
        if (!structuresBuilt || (colorTarget == nullptr)) {
            return;
        }

        // Output texture at the color target's resolution.
        // Quality preset (or env overrides) for this frame.
        const uint32_t quality = userQuality();
        // Soft, temporally resolved shadows don't need the full internal resolution: the presets
        // also cap the trace height (Performance 720 lines, Balanced 1080), so 4K+ targets don't
        // multiply the ray count. WCW_RT_MAX_LINES overrides the cap, WCW_RT_SCALE the whole scale.
        const float presetLines = envFloat(getenv("WCW_RT_MAX_LINES"), (quality == 0) ? 720.0f : ((quality == 1) ? 1080.0f : 0.0f));
        float presetScale = (quality >= 2) ? 1.0f : 0.5f;
        if ((presetLines > 0.0f) && (colorTarget->height > 0)) {
            presetScale = std::min(presetScale, presetLines / float(colorTarget->height));
        }

        const float frameScale = std::clamp(envFloat(getenv("WCW_RT_SCALE"), presetScale), 0.1f, 1.0f);
        const uint32_t presetSamples = (quality == 0) ? 1u : 2u;

        // Re-enabled after being off: the histories are stale.
        if (!wasActive) {
            for (Slot &s : slots) {
                s.historyValid = false;
            }
        }

        wasActive = true;

        // Trace-resolution textures (traceScale of the color target).
        if ((outputTexture == nullptr) || (outputWidth != colorTarget->width) || (outputHeight != colorTarget->height) || (frameScale != traceScale)) {
            traceScale = frameScale;
            outputWidth = colorTarget->width;
            outputHeight = colorTarget->height;
            traceWidth = std::max(uint32_t(std::ceil(outputWidth * traceScale)), 1u);
            traceHeight = std::max(uint32_t(std::ceil(outputHeight * traceScale)), 1u);
            outputTexture = device->createTexture(RenderTextureDesc::Texture2D(traceWidth, traceHeight, 1, RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
            currentTexture = device->createTexture(RenderTextureDesc::Texture2D(traceWidth, traceHeight, 1, RenderFormat::R32G32_FLOAT, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
        }

        Slot &slot = slots[currentSlot];
        if ((slot.history[0] == nullptr) || (slot.historyWidth != traceWidth) || (slot.historyHeight != traceHeight)) {
            slot.historyWidth = traceWidth;
            slot.historyHeight = traceHeight;
            for (auto &history : slot.history) {
                history = device->createTexture(RenderTextureDesc::Texture2D(traceWidth, traceHeight, 1, RenderFormat::R16_FLOAT, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
            }

            slot.historyValid = false;
        }

        // Composite pipeline matching the target's format and sample count; blend depends on the mode.
        if ((compositePipeline == nullptr) || (compositeFormat != colorTarget->format) || (compositeSamples != colorTarget->multisampling.sampleCount) || (compositeMode != mode)) {
            compositeFormat = colorTarget->format;
            compositeSamples = colorTarget->multisampling.sampleCount;
            compositeMode = mode;

            RenderGraphicsPipelineDesc pipelineDesc;
            pipelineDesc.pipelineLayout = compositeLayout.get();
            pipelineDesc.vertexShader = fullScreenVS.get();
            pipelineDesc.pixelShader = compositePS.get();
            pipelineDesc.renderTargetFormat[0] = compositeFormat;
            pipelineDesc.renderTargetCount = 1;
            pipelineDesc.depthTargetFormat = RenderFormat::D32_FLOAT;
            pipelineDesc.multisampling = colorTarget->multisampling;
            if ((mode == WCW_RT_MODE_SHADOW) || (mode == WCW_RT_MODE_AO)) {
                RenderBlendDesc multiply;
                multiply.blendEnabled = true;
                multiply.srcBlend = RenderBlend::ZERO;
                multiply.dstBlend = RenderBlend::SRC_COLOR;
                multiply.blendOp = RenderBlendOperation::ADD;
                multiply.srcBlendAlpha = RenderBlend::ZERO;
                multiply.dstBlendAlpha = RenderBlend::ONE;
                multiply.blendOpAlpha = RenderBlendOperation::ADD;
                pipelineDesc.renderTargetBlend[0] = multiply;
            }
            else {
                pipelineDesc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
            }

            pipelineDesc.renderTargetBlend[0].renderTargetWriteMask = uint8_t(RenderColorWriteEnable::RED) | uint8_t(RenderColorWriteEnable::GREEN) | uint8_t(RenderColorWriteEnable::BLUE);
            compositePipeline = device->createGraphicsPipeline(pipelineDesc);
        }

        // Parameters.
        interop::WcwRtParams params;
        const hlslpp::float4x4 viewProj = hlslpp::mul(viewMatrix, projMatrix);
        const hlslpp::float4x4 invView = hlslpp::inverse(viewMatrix);
        params.invViewProj = hlslpp::inverse(viewProj);
        params.eyePosition = hlslpp::float4(invView[3].xyz, 1.0f);
        params.lightPosition = hlslpp::float4(envFloat(getenv("WCW_RT_LIGHT_X"), 0.0f), envFloat(getenv("WCW_RT_LIGHT_Y"), 460.0f), envFloat(getenv("WCW_RT_LIGHT_Z"), 0.0f), 1.0f);
        params.viewport = hlslpp::float4(viewport.x, viewport.y, viewport.width, viewport.height);
        params.resolution = { traceWidth, traceHeight };
        params.mode = mode;
        params.shadowStrength = envFloat(getenv("WCW_RT_SHADOW"), 0.6f);
        params.lightSpread = envFloat(getenv("WCW_RT_LIGHT_SPREAD"), 200.0f);
        params.lightCount = uint32_t(envFloat(getenv("WCW_RT_LIGHTS"), 4.0f));
        params.cullSign = envFloat(getenv("WCW_RT_CULL_SIGN"), 1.0f);
        params.keyWeight = envFloat(getenv("WCW_RT_KEY_WEIGHT"), 0.5f);
        params.characterShadowStrength = envFloat(getenv("WCW_RT_SHADOW_CHAR"), 0.35f);
        params.lightSize = envFloat(getenv("WCW_RT_LIGHT_SIZE"), 20.0f);
        params.temporalAlpha = envFloat(getenv("WCW_RT_TEMPORAL"), 0.15f);
        params.frameIndex = frameIndex++;
        params.prevViewProj = slot.prevViewProj;
        params.historyValid = slot.historyValid ? 1 : 0;
        params.samplesPerLight = std::max(uint32_t(envFloat(getenv("WCW_RT_SAMPLES"), float(presetSamples))), 1u);
        params.aoSamples = uint32_t(envFloat(getenv("WCW_RT_AO_SAMPLES"), float(presetSamples)));
        params.aoRadius = envFloat(getenv("WCW_RT_AO_RADIUS"), 25.0f);
        params.aoStrength = envFloat(getenv("WCW_RT_AO"), 0.5f);
        params.traceScale = float(traceWidth) / float(outputWidth);

        // Trace only this workload's screen footprint (plus a margin for the bilinear upsample
        // and the resolve's 3x3 neighborhood).
        if (boundsFullScreen) {
            traceRect[0] = traceRect[1] = 0;
            traceRect[2] = traceWidth;
            traceRect[3] = traceHeight;
        }
        else {
            const float scale = params.traceScale;
            traceRect[0] = uint32_t(std::clamp(std::floor(boundsMin[0] * scale) - 3.0f, 0.0f, float(traceWidth)));
            traceRect[1] = uint32_t(std::clamp(std::floor(boundsMin[1] * scale) - 3.0f, 0.0f, float(traceHeight)));
            traceRect[2] = uint32_t(std::clamp(std::ceil(boundsMax[0] * scale) + 3.0f, 0.0f, float(traceWidth)));
            traceRect[3] = uint32_t(std::clamp(std::ceil(boundsMax[1] * scale) + 3.0f, 0.0f, float(traceHeight)));
        }

        traceRectEmpty = (traceRect[2] <= traceRect[0]) || (traceRect[3] <= traceRect[1]);
        if (traceRectEmpty) {
            return;
        }

        params.traceOffset = { traceRect[0], traceRect[1] };
        params.padding2 = 0.0f;
        void *paramsBytes = paramsBuffer->map();
        memcpy(paramsBytes, &params, sizeof(params));
        paramsBuffer->unmap();

        traceSet->setBuffer(0, paramsBuffer.get(), alignUp(sizeof(interop::WcwRtParams), 256));
        traceSet->setAccelerationStructure(1, tlas.get());
        const RenderBufferStructuredView float4View(sizeof(float) * 4);
        const RenderBufferStructuredView uintView(sizeof(uint32_t));
        traceSet->setBuffer(2, worldPosBuffer, worldPosSize, &float4View);
        traceSet->setBuffer(3, faceIndicesBuffer, faceIndicesSize, &uintView);
        traceSet->setBuffer(4, meshStartsBuffer.get(), meshStartsBufferSize, &uintView);
        traceSet->setTexture(5, outputTexture.get(), RenderTextureLayout::GENERAL);
        traceSet->setTexture(6, currentTexture.get(), RenderTextureLayout::GENERAL);
        const RenderBufferStructuredView rdpTileView(sizeof(interop::RDPTile));
        const RenderBufferStructuredView gpuTileView(sizeof(interop::GPUTile));
        const RenderBufferStructuredView float2View(sizeof(float) * 2);
        traceSet->setBuffer(7, (rdpTilesBuffer != nullptr) ? rdpTilesBuffer : meshTilesBuffer.get(), 0, &rdpTileView);
        traceSet->setBuffer(8, (gpuTilesBuffer != nullptr) ? gpuTilesBuffer : meshTilesBuffer.get(), 0, &gpuTileView);
        traceSet->setBuffer(9, (texCoordBuffer != nullptr) ? texCoordBuffer : meshTilesBuffer.get(), 0, &float2View);
        traceSet->setBuffer(10, meshTilesBuffer.get(), meshTilesBufferSize, &uintView);
        compositeSet->setTexture(0, outputTexture.get(), RenderTextureLayout::SHADER_READ);

        // Trace.
        worker->commandList->barriers(RenderBarrierStage::COMPUTE, {
            RenderTextureBarrier(outputTexture.get(), RenderTextureLayout::GENERAL),
            RenderTextureBarrier(currentTexture.get(), RenderTextureLayout::GENERAL)
        });
        if (timingPool != nullptr) {
            worker->commandList->writeTimestamp(timingPool.get(), StampTraceBegin);
            timingWritten |= (1u << StampTraceBegin);
        }

        worker->commandList->setPipeline(tracePipeline.get());
        worker->commandList->setComputePipelineLayout(traceLayout.get());
        worker->commandList->setComputeDescriptorSet(traceSet.get(), 0);
        worker->commandList->setComputeDescriptorSet(textureSet, 1);
        const uint32_t rectWidth = traceRect[2] - traceRect[0];
        const uint32_t rectHeight = traceRect[3] - traceRect[1];
        worker->commandList->dispatch((rectWidth + 7) / 8, (rectHeight + 7) / 8, 1);
        if (timingPool != nullptr) {
            worker->commandList->writeTimestamp(timingPool.get(), StampTraceEnd);
            timingWritten |= (1u << StampTraceEnd);
            timingPixels = rectWidth * rectHeight;
            timingFullScreen = boundsFullScreen;
            timingTraceHeight = traceHeight;
        }

        // Shadow mode: temporal resolve of the jittered samples into this slot's history.
        if ((mode == WCW_RT_MODE_SHADOW) || (mode == WCW_RT_MODE_AO)) {
            RenderTexture *historyIn = slot.history[slot.historyIndex].get();
            RenderTexture *historyOut = slot.history[slot.historyIndex ^ 1].get();
            resolveSet->setBuffer(0, paramsBuffer.get(), alignUp(sizeof(interop::WcwRtParams), 256));
            resolveSet->setTexture(1, currentTexture.get(), RenderTextureLayout::SHADER_READ);
            resolveSet->setTexture(2, historyIn, RenderTextureLayout::SHADER_READ);
            resolveSet->setTexture(3, historyOut, RenderTextureLayout::GENERAL);
            resolveSet->setTexture(4, outputTexture.get(), RenderTextureLayout::GENERAL);
            worker->commandList->barriers(RenderBarrierStage::COMPUTE, {
                RenderTextureBarrier(currentTexture.get(), RenderTextureLayout::SHADER_READ),
                RenderTextureBarrier(historyIn, RenderTextureLayout::SHADER_READ),
                RenderTextureBarrier(historyOut, RenderTextureLayout::GENERAL),
                RenderTextureBarrier(outputTexture.get(), RenderTextureLayout::GENERAL)
            });

            worker->commandList->setPipeline(resolvePipeline.get());
            worker->commandList->setComputePipelineLayout(resolveLayout.get());
            worker->commandList->setComputeDescriptorSet(resolveSet.get(), 0);
            worker->commandList->dispatch((rectWidth + 7) / 8, (rectHeight + 7) / 8, 1);

            slot.historyIndex ^= 1;
            slot.historyValid = true;
            slot.prevViewProj = viewProj;
        }

        if (timingPool != nullptr) {
            worker->commandList->writeTimestamp(timingPool.get(), StampResolveEnd);
            timingWritten |= (1u << StampResolveEnd);
        }

        worker->commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(outputTexture.get(), RenderTextureLayout::SHADER_READ));
    }

    void WcwRaytracer::recordComposite(RenderWorker *worker) {
        if (!structuresBuilt || (compositePipeline == nullptr) || traceRectEmpty) {
            return;
        }

        // Composite only the traced rect (inside its margin), in color target pixels.
        const float invScale = float(outputWidth) / float(traceWidth);
        const RenderViewport fullViewport(0.0f, 0.0f, float(outputWidth), float(outputHeight));
        const RenderRect fullRect(int32_t((traceRect[0] + 1) * invScale), int32_t((traceRect[1] + 1) * invScale),
            std::min(int32_t((traceRect[2] - 1) * invScale), int32_t(outputWidth)), std::min(int32_t((traceRect[3] - 1) * invScale), int32_t(outputHeight)));
        worker->commandList->setPipeline(compositePipeline.get());
        worker->commandList->setGraphicsPipelineLayout(compositeLayout.get());
        interop::WcwRtCompositeCB compositeCB;
        compositeCB.scale = { float(traceWidth) / float(outputWidth), float(traceHeight) / float(outputHeight) };
        compositeCB.traceSize = { float(traceWidth), float(traceHeight) };
        worker->commandList->setGraphicsPushConstants(0, &compositeCB);
        worker->commandList->setGraphicsDescriptorSet(compositeSet.get(), 0);
        worker->commandList->setViewports(fullViewport);
        worker->commandList->setScissors(fullRect);
        if (timingPool != nullptr) {
            worker->commandList->writeTimestamp(timingPool.get(), StampCompositeBegin);
        }

        worker->commandList->drawInstanced(3, 1, 0, 0);
        if (timingPool != nullptr) {
            worker->commandList->writeTimestamp(timingPool.get(), StampCompositeEnd);
            timingWritten |= (1u << StampCompositeBegin) | (1u << StampCompositeEnd);
        }
    }

    void WcwRaytracer::collectTimings() {
        if ((timingPool == nullptr) || (timingWritten == 0)) {
            return;
        }

        static double sums[5] = {};
        static double maxTotal = 0.0;
        static uint64_t triangleSum = 0, meshSum = 0, alphaSum = 0, pixelSum = 0, fullScreenSum = 0;
        static uint32_t count = 0;
        timingPool->queryResults();
        const uint64_t *t = timingPool->getResults();
        auto span = [&](uint32_t a, uint32_t b) {
            const uint32_t need = (1u << a) | (1u << b);
            return ((timingWritten & need) == need) ? double(t[b] - t[a]) / 1000000.0 : 0.0;
        };

        const double build = span(StampBuildBegin, StampBuildEnd);
        const double trace = span(StampTraceBegin, StampTraceEnd);
        const double resolve = span(StampTraceEnd, StampResolveEnd);
        const double composite = span(StampCompositeBegin, StampCompositeEnd);
        const double total = span(StampBuildBegin, StampCompositeEnd);
        sums[0] += build;
        sums[1] += trace;
        sums[2] += resolve;
        sums[3] += composite;
        sums[4] += total;
        maxTotal = std::max(maxTotal, total);
        triangleSum += timingTriangles;
        meshSum += timingMeshes;
        alphaSum += timingAlphaMeshes;
        pixelSum += timingPixels;
        fullScreenSum += timingFullScreen ? 1 : 0;
        timingWritten = 0;
        if (++count == 300) {
            fprintf(stderr, "[wcw rt gpu] per traced workload avg: build=%.3f trace=%.3f resolve=%.3f composite=%.3f ms, build->composite span=%.3f (max %.3f) ms; %llu meshes (%llu alpha-tested), %llu tris, %llu trace px of %u lines, %llu full-screen (300 workloads)\n",
                sums[0] / count, sums[1] / count, sums[2] / count, sums[3] / count, sums[4] / count, maxTotal,
                (unsigned long long)(meshSum / count), (unsigned long long)(alphaSum / count), (unsigned long long)(triangleSum / count), (unsigned long long)(pixelSum / count), timingTraceHeight, (unsigned long long)fullScreenSum);
            memset(sums, 0, sizeof(sums));
            maxTotal = 0.0;
            triangleSum = meshSum = alphaSum = pixelSum = fullScreenSum = 0;
            count = 0;
        }
    }
};
