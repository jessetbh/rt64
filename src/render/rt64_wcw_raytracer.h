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
        uint32_t mode = WCW_RT_MODE_SHADOW;
        bool wasActive = false;

        // Scene gathered for the current workload.
        bool sceneActive = false;
        std::vector<RenderBottomLevelASMesh> meshes;
        std::vector<uint32_t> meshIndexStarts;
        const RenderBuffer *worldPosBuffer = nullptr;
        const RenderBuffer *rdpTilesBuffer = nullptr;
        const RenderBuffer *gpuTilesBuffer = nullptr;
        const RenderBuffer *texCoordBuffer = nullptr;
        std::vector<uint32_t> meshTiles;
        std::unique_ptr<RenderBuffer> meshTilesBuffer;
        uint64_t meshTilesBufferSize = 0;
        const RenderBuffer *faceIndicesBuffer = nullptr;
        uint64_t worldPosSize = 0;
        uint64_t faceIndicesSize = 0;
        hlslpp::float4x4 viewMatrix;
        hlslpp::float4x4 projMatrix;
        RenderViewport viewport;
        bool structuresBuilt = false;

        // Screen bounds (color target pixels) of this workload's gathered geometry.
        hlslpp::float4x4 boundsViewProj;
        float boundsMin[2] = {};
        float boundsMax[2] = {};
        bool boundsFullScreen = false;
        float worldMin[3] = {};
        float worldMax[3] = {};

        // AKI games build one frame from several RSP workloads (WCW: ring and wrestlers apart),
        // and each workload gets its own pass. Every workload slot keeps its bottom-level
        // structure so the others' passes can include it as a shadow caster: slots before the
        // current one are from this frame, later ones from the previous frame (one frame stale).
        static const uint32_t MaxSlots = 8;
        struct Slot {
            std::unique_ptr<RenderBuffer> blasBuffer;
            std::unique_ptr<RenderAccelerationStructure> blas;
            uint64_t blasBufferSize = 0;
            uint64_t builtFrame = 0;
            bool valid = false;

            // Soft-shadow history (ping-pong) of this workload's pass, with the camera it used.
            std::unique_ptr<RenderTexture> history[2];
            uint32_t historyIndex = 0;
            uint32_t historyWidth = 0;
            uint32_t historyHeight = 0;
            bool historyValid = false;
            hlslpp::float4x4 prevViewProj;
        };

        Slot slots[MaxSlots];
        uint32_t currentSlot = 0;
        uint32_t workloadInFrame = 0;
        uint64_t lastPresentId = UINT64_MAX;
        uint64_t frameCounter = 0;
        std::vector<RenderTopLevelASInstance> instances;

        // Acceleration structures (grow-only).
        RenderBottomLevelASBuildInfo blasInfo;
        RenderTopLevelASBuildInfo tlasInfo;
        std::unique_ptr<RenderBuffer> tlasBuffer;
        std::unique_ptr<RenderBuffer> scratchBuffer;
        std::unique_ptr<RenderBuffer> instancesBuffer;
        std::unique_ptr<RenderAccelerationStructure> tlas;
        uint64_t tlasBufferSize = 0;
        uint64_t scratchBufferSize = 0;
        uint64_t instancesBufferSize = 0;

        // Trace pass.
        std::unique_ptr<RenderBuffer> paramsBuffer;
        std::unique_ptr<RenderBuffer> meshStartsBuffer;
        uint64_t meshStartsBufferSize = 0;
        std::unique_ptr<RenderTexture> outputTexture;
        std::unique_ptr<RenderTexture> currentTexture;
        uint32_t frameIndex = 0;
        std::unique_ptr<RenderDescriptorSet> resolveSet;
        std::unique_ptr<RenderPipelineLayout> resolveLayout;
        std::unique_ptr<RenderPipeline> resolvePipeline;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        uint32_t traceWidth = 0;
        uint32_t traceHeight = 0;
        float traceScale = 0.5f;
        bool traceRectEmpty = true;
        uint32_t traceRect[4] = {};
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

        // WCW_GPU_LOG=1: per-stage GPU timestamps (build, trace, resolve, composite) of each workload.
        enum TimingStamp : uint32_t { StampBuildBegin, StampBuildEnd, StampTraceBegin, StampTraceEnd, StampResolveEnd, StampCompositeBegin, StampCompositeEnd, StampCount };
        std::unique_ptr<RenderQueryPool> timingPool;
        uint32_t timingWritten = 0;
        uint32_t timingTriangles = 0;
        uint32_t timingMeshes = 0;
        uint32_t timingAlphaMeshes = 0;
        uint32_t timingPixels = 0;
        uint32_t timingTraceHeight = 0;
        bool timingFullScreen = false;

        // Runtime settings from the frontend's graphics menu. Quality: 0 Performance (half-res,
        // 1 sample per light, 1 AO ray), 1 Balanced (half-res, 2/2), 2 Quality (full-res, 2/2).
        // WCW_RT forces it on; WCW_RT_SCALE / WCW_RT_SAMPLES / WCW_RT_AO_SAMPLES override quality.
        static void setUserSettings(bool enabled, uint32_t quality);
        // True when ray tracing should run this frame (WCW_RT or the menu option). The camera
        // hand-off in rt64_rsp.cpp follows it, so Off keeps the original rendering.
        static bool active();
        static uint32_t userQuality();

        WcwRaytracer(RenderDevice *device, RenderShaderFormat shaderFormat);
        void reset();
        // Called before each workload is recorded. Workloads sharing a presentId belong to the
        // same displayed frame; the slot is the workload's index within it.
        void beginWorkload(uint64_t presentId);
        bool wantsScene() const;
        // Shadow mode drops the game's blob shadows (WCW_RT_KEEP_BLOBS=1 keeps them).
        bool replacesBlobShadows() const;
        void beginScene(const hlslpp::float4x4 &view, const hlslpp::float4x4 &proj, const RenderViewport &viewport,
            const RenderBuffer *worldPosBuffer, uint64_t worldPosSize, const RenderBuffer *faceIndicesBuffer, uint64_t faceIndicesSize,
            const RenderBuffer *rdpTilesBuffer, const RenderBuffer *gpuTilesBuffer, const RenderBuffer *texCoordBuffer);
        // culled: the draw has N64 face culling on, so primary rays skip its back faces.
        // character: a wrestler draw, shadowed with the weaker character strength.
        // alphaTest: rays skip the draw's transparent texels (tileIndex, uvCorrection: its texture).
        void addMesh(uint32_t faceIndicesStart, uint32_t triangleCount, uint32_t vertexCount, bool culled, bool character,
            bool alphaTest, uint32_t tileIndex, bool uvCorrection);
        // Grows the screen bounds with a world-space triangle (3 xyz positions) of a gathered draw.
        void extendBounds(const float *positions);
        void recordBuild(RenderWorker *worker);
        // textureSet: the framebuffer renderer's bindless texture cache set (alpha tests).
        void recordTrace(RenderWorker *worker, RenderTarget *colorTarget, RenderDescriptorSet *textureSet);
        void recordComposite(RenderWorker *worker);
        // After the workload's GPU work finished: accumulates and logs the stage timings (WCW_GPU_LOG).
        void collectTimings();
    };
};
