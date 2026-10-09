//
// RT64
//

#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "common/rt64_common.h"
#include "common/rt64_plume.h"
#include "shared/rt64_wcw_rt.h"

namespace RT64 {
    // [wcw rt] Presented frame count (mirrors the WCW_PRESENT_LUM counter) for targeted dumps.
    extern std::atomic<int> wcwRtPresentCounter;

    struct RenderTarget;
    struct RenderWorker;

    // [wcw rt] Hybrid ray tracing for forceMatrix-only AKI games (WCW_RT=1, D3D12 + DXR 1.1).
    //
    // The raster image is left untouched. Per workload, the depth-writing triangles of the
    // first perspective projection are gathered (addMesh) into one bottom-level structure built
    // straight from RT64's world-space vertex and face index buffers (recordBuild), and after
    // that projection's raster scene a compute pass casts inline rays from the game camera
    // (recordTrace). Its output is composited over the color target: alpha blended for the
    // debug views, multiplied for shadows. WCW_RT_MODE selects the output (rt64_wcw_rt.h).
    struct WcwRaytracer {
        RenderDevice *device = nullptr;
        RenderShaderFormat shaderFormat = RenderShaderFormat::UNKNOWN;
        bool supported = false;
        uint32_t mode = WCW_RT_MODE_HIT_NORMAL;

        // Scene gathered for the current workload.
        bool sceneActive = false;
        std::vector<RenderBottomLevelASMesh> meshes;
        std::vector<uint32_t> meshIndexStarts;
        const RenderBuffer *worldPosBuffer = nullptr;
        const RenderBuffer *faceIndicesBuffer = nullptr;
        uint64_t worldPosSize = 0;
        uint64_t faceIndicesSize = 0;
        hlslpp::float4x4 viewMatrix;
        hlslpp::float4x4 projMatrix;
        RenderViewport viewport;
        bool structuresBuilt = false;

        // Acceleration structures (grow-only).
        RenderBottomLevelASBuildInfo blasInfo;
        RenderTopLevelASBuildInfo tlasInfo;
        std::unique_ptr<RenderBuffer> blasBuffer;
        std::unique_ptr<RenderBuffer> tlasBuffer;
        std::unique_ptr<RenderBuffer> scratchBuffer;
        std::unique_ptr<RenderBuffer> instancesBuffer;
        std::unique_ptr<RenderAccelerationStructure> blas;
        std::unique_ptr<RenderAccelerationStructure> tlas;
        uint64_t blasBufferSize = 0;
        uint64_t tlasBufferSize = 0;
        uint64_t scratchBufferSize = 0;
        uint64_t instancesBufferSize = 0;

        // Trace pass.
        std::unique_ptr<RenderBuffer> paramsBuffer;
        std::unique_ptr<RenderBuffer> meshStartsBuffer;
        uint64_t meshStartsBufferSize = 0;
        std::unique_ptr<RenderTexture> outputTexture;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        bool outputNeedsTransition = false;
        std::unique_ptr<RenderDescriptorSet> traceSet;
        std::unique_ptr<RenderPipelineLayout> traceLayout;
        std::unique_ptr<RenderPipeline> tracePipeline;
        std::unique_ptr<RenderDescriptorSet> compositeSet;
        std::unique_ptr<RenderPipelineLayout> compositeLayout;
        std::unique_ptr<RenderShader> fullScreenVS;
        std::unique_ptr<RenderShader> compositePS;
        std::unique_ptr<RenderPipeline> compositePipeline;
        RenderFormat compositeFormat = RenderFormat::UNKNOWN;
        RenderSampleCounts compositeSamples = RenderSampleCount::COUNT_0;
        uint32_t compositeMode = UINT32_MAX;

        // True when WCW_RT is set (the camera hand-off in rt64_rsp.cpp is active).
        static bool enabledByEnv();

        WcwRaytracer(RenderDevice *device, RenderShaderFormat shaderFormat);
        void reset();
        bool wantsScene() const;
        void beginScene(const hlslpp::float4x4 &view, const hlslpp::float4x4 &proj, const RenderViewport &viewport,
            const RenderBuffer *worldPosBuffer, uint64_t worldPosSize, const RenderBuffer *faceIndicesBuffer, uint64_t faceIndicesSize);
        void addMesh(uint32_t faceIndicesStart, uint32_t triangleCount, uint32_t vertexCount);
        void recordBuild(RenderWorker *worker);
        void recordTrace(RenderWorker *worker, RenderTarget *colorTarget);
        void recordComposite(RenderWorker *worker);
    };
};
