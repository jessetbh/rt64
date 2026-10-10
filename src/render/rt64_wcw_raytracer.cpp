//
// RT64
//

#include "rt64_wcw_raytracer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "rt64_render_target.h"
#include "rt64_render_worker.h"

#ifdef _WIN32
#   include "shaders/FullScreenVS.hlsl.dxil.h"
#   include "shaders/WcwRtTraceCS.hlsl.dxil.h"
#   include "shaders/WcwRtCompositePS.hlsl.dxil.h"
#endif

namespace RT64 {
    std::atomic<int> wcwRtPresentCounter = 0;

    static uint64_t alignUp(uint64_t value, uint64_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    static float envFloat(const char *text, float fallback) {
        return (text != nullptr) ? float(atof(text)) : fallback;
    }

    bool WcwRaytracer::enabledByEnv() {
        static const bool enabled = getenv("WCW_RT") != nullptr;
        return enabled;
    }

    WcwRaytracer::WcwRaytracer(RenderDevice *device, RenderShaderFormat shaderFormat) {
        this->device = device;
        this->shaderFormat = shaderFormat;

        if (!enabledByEnv()) {
            return;
        }

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
        traceBuilder.end();
        traceSet = traceBuilder.create(device);

        RenderPipelineLayoutBuilder layoutBuilder;
        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(traceBuilder);
        layoutBuilder.end();
        traceLayout = layoutBuilder.create(device);

        std::unique_ptr<RenderShader> traceShader = device->createShader(WcwRtTraceCSBlobDXIL, std::size(WcwRtTraceCSBlobDXIL), "CSMain", RenderShaderFormat::DXIL);
        tracePipeline = device->createComputePipeline(RenderComputePipelineDesc(traceLayout.get(), traceShader.get(), 8, 8, 1));

        // Composite pass: t0 trace output.
        RenderDescriptorSetBuilder compositeBuilder;
        compositeBuilder.begin();
        compositeBuilder.addTexture(0);
        compositeBuilder.end();
        compositeSet = compositeBuilder.create(device);

        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(compositeBuilder);
        layoutBuilder.end();
        compositeLayout = layoutBuilder.create(device);

        fullScreenVS = device->createShader(FullScreenVSBlobDXIL, std::size(FullScreenVSBlobDXIL), "VSMain", RenderShaderFormat::DXIL);
        compositePS = device->createShader(WcwRtCompositePSBlobDXIL, std::size(WcwRtCompositePSBlobDXIL), "PSMain", RenderShaderFormat::DXIL);
#   endif

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
        sceneActive = false;
        structuresBuilt = false;
        meshes.clear();
        meshIndexStarts.clear();
    }

    bool WcwRaytracer::wantsScene() const {
        return supported && !sceneActive;
    }

    bool WcwRaytracer::replacesBlobShadows() const {
        static const bool keepBlobs = getenv("WCW_RT_KEEP_BLOBS") != nullptr;
        return supported && (mode == WCW_RT_MODE_SHADOW) && !keepBlobs;
    }

    void WcwRaytracer::beginScene(const hlslpp::float4x4 &view, const hlslpp::float4x4 &proj, const RenderViewport &viewport,
        const RenderBuffer *worldPosBuffer, uint64_t worldPosSize, const RenderBuffer *faceIndicesBuffer, uint64_t faceIndicesSize)
    {
        sceneActive = true;
        viewMatrix = view;
        projMatrix = proj;
        this->viewport = viewport;
        this->worldPosBuffer = worldPosBuffer;
        this->worldPosSize = worldPosSize;
        this->faceIndicesBuffer = faceIndicesBuffer;
        this->faceIndicesSize = faceIndicesSize;
    }

    void WcwRaytracer::addMesh(uint32_t faceIndicesStart, uint32_t triangleCount, uint32_t vertexCount, bool culled) {
        const uint32_t IndexStride = sizeof(uint32_t);
        const uint32_t PosStride = sizeof(float) * 4;
        meshes.emplace_back(RenderBufferReference(faceIndicesBuffer, faceIndicesStart * IndexStride), RenderBufferReference(worldPosBuffer, 0),
            RenderFormat::R32_UINT, RenderFormat::R32G32B32_FLOAT, triangleCount * 3, vertexCount, PosStride, !culled);

        // Culled draws are non-opaque so the trace shader sees their candidates; bit 31 flags them.
        meshIndexStarts.emplace_back(faceIndicesStart | (culled ? WCW_RT_MESH_CULLED : 0u));
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

        worker->commandList->buildBottomLevelAS(slot.blas.get(), RenderBufferReference(scratchBuffer.get(), 0), blasInfo);
        worker->commandList->barriers(RenderBarrierStage::COMPUTE, RenderBufferBarrier(slot.blasBuffer.get(), RenderBufferAccess::READ));
        worker->commandList->buildTopLevelAS(tlas.get(), RenderBufferReference(scratchBuffer.get(), blasScratch), RenderBufferReference(instancesBuffer.get()), tlasInfo);
        worker->commandList->barriers(RenderBarrierStage::COMPUTE, RenderBufferBarrier(tlasBuffer.get(), RenderBufferAccess::READ));
        structuresBuilt = true;

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

    void WcwRaytracer::recordTrace(RenderWorker *worker, RenderTarget *colorTarget) {
        if (!structuresBuilt || (colorTarget == nullptr)) {
            return;
        }

        // Output texture at the color target's resolution.
        if ((outputTexture == nullptr) || (outputWidth != colorTarget->width) || (outputHeight != colorTarget->height)) {
            outputWidth = colorTarget->width;
            outputHeight = colorTarget->height;
            outputTexture = device->createTexture(RenderTextureDesc::Texture2D(outputWidth, outputHeight, 1, RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
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
            if (mode == WCW_RT_MODE_SHADOW) {
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
        params.resolution = { outputWidth, outputHeight };
        params.mode = mode;
        params.shadowStrength = envFloat(getenv("WCW_RT_SHADOW"), 0.6f);
        params.lightSpread = envFloat(getenv("WCW_RT_LIGHT_SPREAD"), 200.0f);
        params.lightCount = uint32_t(envFloat(getenv("WCW_RT_LIGHTS"), 4.0f));
        params.cullSign = envFloat(getenv("WCW_RT_CULL_SIGN"), 1.0f);
        params.keyWeight = envFloat(getenv("WCW_RT_KEY_WEIGHT"), 0.5f);
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
        compositeSet->setTexture(0, outputTexture.get(), RenderTextureLayout::SHADER_READ);

        // Trace.
        worker->commandList->barriers(RenderBarrierStage::COMPUTE, RenderTextureBarrier(outputTexture.get(), RenderTextureLayout::GENERAL));
        worker->commandList->setPipeline(tracePipeline.get());
        worker->commandList->setComputePipelineLayout(traceLayout.get());
        worker->commandList->setComputeDescriptorSet(traceSet.get(), 0);
        worker->commandList->dispatch((outputWidth + 7) / 8, (outputHeight + 7) / 8, 1);
        worker->commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(outputTexture.get(), RenderTextureLayout::SHADER_READ));
    }

    void WcwRaytracer::recordComposite(RenderWorker *worker) {
        if (!structuresBuilt || (compositePipeline == nullptr)) {
            return;
        }

        const RenderViewport fullViewport(0.0f, 0.0f, float(outputWidth), float(outputHeight));
        const RenderRect fullRect(0, 0, int32_t(outputWidth), int32_t(outputHeight));
        worker->commandList->setPipeline(compositePipeline.get());
        worker->commandList->setGraphicsPipelineLayout(compositeLayout.get());
        worker->commandList->setGraphicsDescriptorSet(compositeSet.get(), 0);
        worker->commandList->setViewports(fullViewport);
        worker->commandList->setScissors(fullRect);
        worker->commandList->drawInstanced(3, 1, 0, 0);
    }
};
