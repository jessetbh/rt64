//
// RT64
//

#pragma once

#include <atomic>
#include <memory>
#include <array>
#include <cfloat>
#include <deque>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/rt64_common.h"
#include "common/rt64_plume.h"
#include "common/rt64_wcw_venues.h"
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
        // WCW_RT_MODE (startup); the presets file's "debugMode" overrides it while present.
        uint32_t envMode = WCW_RT_MODE_SHADOW;
        bool wasActive = false;

        // Scene gathered for the current workload.
        bool sceneActive = false;
        std::vector<RenderBottomLevelASMesh> meshes;
        std::vector<uint32_t> meshIndexStarts;
        const RenderBuffer *worldPosBuffer = nullptr;
        const RenderBuffer *worldNormBuffer = nullptr;
        uint64_t worldNormSize = 0;
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
        // Shadows and AO are only cast onto surfaces within receiverRadius (XZ) of the lights'
        // center: the ring and ringside. The arena and crowd beyond it only get primary rays,
        // and geometry entirely outside it doesn't widen the traced rect (WCW_RT_RADIUS, 0 = off).
        float receiverRadius = 750.0f;
        float receiverCenter[2] = {};
        float worldMin[3] = {};
        float worldMax[3] = {};

        // AKI games build one frame from several RSP workloads (WCW: floor bits, ring and
        // wrestlers apart), and each workload gets its own pass. Every slot keeps one scene's
        // bottom-level structure so the others' passes can include it as a shadow caster, plus
        // that scene's soft-shadow history. Slots follow scene identity (mesh count and world
        // bounds), not the workload's position within the present: the game's workload cycle
        // drifts against presents, so positions would swap scenes from frame to frame.
        static const uint32_t MaxSlots = 8;
        // How long other scenes stay shadow casters: WcwLogic::shadowCasterFresh (common/rt64_wcw_logic.h).
        struct Slot {
            std::unique_ptr<RenderBuffer> blasBuffer;
            std::unique_ptr<RenderAccelerationStructure> blas;
            uint64_t blasBufferSize = 0;
            uint64_t builtWorkload = 0;
            uint64_t builtFrame = 0;
            uint32_t meshCount = 0;
            float boundsMin[3] = {};
            float boundsMax[3] = {};
            bool valid = false;

            // Soft-shadow history (ping-pong) of this workload's pass, with the camera it used.
            std::unique_ptr<RenderTexture> history[2];
            uint32_t historyIndex = 0;
            uint32_t historyWidth = 0;
            uint32_t historyHeight = 0;
            bool historyValid = false;
            hlslpp::float4x4 prevViewProj;

            // This slot's trace output, and what it was traced for: AKI games render some frames
            // twice (two framebuffers, same camera), and the second pass reuses the first trace.
            std::unique_ptr<RenderTexture> output;
            uint64_t tracedPresent = UINT64_MAX;
            hlslpp::float4x4 tracedViewProj;
            uint32_t tracedRect[4] = {};
            uint32_t tracedMeshes = 0;
        };

        Slot slots[MaxSlots];
        uint32_t currentSlot = 0;
        uint32_t workloadInFrame = 0;
        uint64_t workloadCounter = 0;
        // Picks the slot whose last scene best matches the gathered one (or the oldest slot).
        uint32_t chooseSlot() const;
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
        // Blue noise tile for the trace's random samples (WcwLogic::blueNoiseTile), uploaded once.
        std::unique_ptr<RenderBuffer> blueNoiseBuffer;
        std::unique_ptr<RenderTexture> outputTexture;
        std::unique_ptr<RenderTexture> currentTexture;
        // The traced AO term beside currentTexture's colored shadow term and hit distance.
        std::unique_ptr<RenderTexture> currentAoTexture;
        uint32_t frameIndex = 0;
        uint32_t reusedTraces = 0;
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

        // Per-venue light presets: a JSON file (WCW_RT_LIGHTS_FILE, default assets/rt_lights.json,
        // relative to the working directory) with a "default" preset and a "venues" list. A venue
        // is recognized by the textures of the world geometry (its "match" list of texture hashes,
        // most of which must have been drawn in the last VenueWindow presents). The file is
        // reloaded when it changes, so presets can be tuned while the game runs. WCW_RT_VENUE_LOG=1
        // prints the current scene's stable world textures to build a match list from.
        // The preset data model, its parsing/writing and venue matching live in WcwLogic (common/rt64_wcw_venues,
        // unit tested); the Lighting editor (rt64_wcw_lighting_editor.cpp) edits presetFile.
        using LightPreset = WcwLogic::LightPreset;

        static const uint32_t VenueWindow = 60;
        LightPreset defaultPreset;
        // "room": fallback for unmatched small scenes (backstage rooms, whatever the scene's extent
        // on X/Z is at most maxExtent): "count" ceiling lights along the room's long axis at
        // "height" (fraction of floor to ceiling), with falloff from their spacing. Other fields as
        // in any preset (shadow, size, ...).
        bool roomEnabled = false;
        bool roomActive = false;
        float roomMaxExtent = 3000.0f;
        uint32_t roomCount = 3;
        float roomHeight = 0.85f;
        LightPreset roomPreset;
        void updateRoomPreset();
        std::vector<LightPreset> venuePresets;
        // The venue whose preset applies (-1: none), chosen by venueSelector once per present.
        int activeVenue = -1;
        WcwLogic::VenueSelector venueSelector;
        std::string presetsPath;
        int64_t presetsStamp = INT64_MIN;
        uint32_t presentCount = 0;
        std::unordered_map<uint64_t, uint32_t> frameTextureTris;
        std::deque<std::vector<uint64_t>> textureHistory;
        std::unordered_map<uint64_t, uint32_t> textureCounts;
        std::unordered_map<uint64_t, uint32_t> textureMaxTris;
        // World bounds of this present's non-character geometry, and of the last finished present.
        float presentMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float presentMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        float sceneMin[3] = {};
        float sceneMax[3] = {};
        bool sceneBoundsValid = false;
        // The preset's light origin: zero, or the scene anchor.
        void presetOrigin(const LightPreset &preset, float origin[3]) const;
        void loadPresets(bool force);
        void finishPresent();
        const LightPreset &activePreset() const;

        // [wcw rt] Lighting editor (rt64_wcw_lighting_editor.cpp, a "Lighting" window in the RT64 inspector).
        // presetFile is the parsed rt_lights.json and the source of truth for the presets in use (defaultPreset,
        // roomPreset, venuePresets), which applyPresetFile() refreshes. presetsMutex guards both against the
        // inspector thread; the render side reads presets through activePresetCopy().
        std::recursive_mutex presetsMutex;
        WcwLogic::LightPresetFile presetFile;
        // Forced preset: -2 none (detection), -1 "default", >= 0 a venue.
        int venueOverride = -2;
        bool presetsEdited = false;
        // Outside darkness: per color target, which traced pass of the frame darkens the misses.
        WcwLogic::MissDarkening missDarkening;
        std::string editorMessage;
        char editorNewVenueName[64] = {};
        void applyPresetFile();
        LightPreset activePresetCopy();
        // The current scene's stable world textures (hash, max triangles), seen in >= 90% of the venue window.
        std::vector<std::pair<uint64_t, uint32_t>> stableTextures() const;
        // Writes presetFile to the presets path (the previous file is kept as <path>.bak).
        bool savePresets();
        // A new venue matched by the current scene's textures no other venue lists, inheriting "default"; forced.
        int newVenueFromScene(const std::string &name);
        void drawLightingEditor();

        // Runtime settings from the frontend's graphics menu. Quality (WcwLogic::RtQuality / rtPreset): 0 Performance
        // (reduced resolution, 1 shadow ray per light, no AO), 1 Balanced (reduced resolution, 2, Subtle AO), 2 Quality
        // (full resolution, 2, Subtle AO), 3 Ultra (full resolution, 4, Strong AO with 8 rays). WCW_RT forces it on;
        // WCW_RT_SCALE / WCW_RT_SAMPLES / WCW_RT_AO_SAMPLES and the WCW_RT_AO* look overrides override the level.
        static void setUserSettings(bool enabled, uint32_t quality);
        // The menu's Venue Lighting option (WcwLogic::VenueLighting: 0 Full, 1 Subtle, 2 Off), applied to the active
        // preset in activePresetCopy. WCW_RT_VENUE_LIGHTING overrides it.
        static void setVenueLighting(uint32_t level);
        static uint32_t venueLighting();
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
        // WCW_RT_SPLIT: ray tracing only on the right half of the screen (before/after comparisons).
        static bool splitScreen();
        // The Lighting editor's split toggle (-1: WCW_RT_SPLIT decides).
        static void setSplitScreenOverride(int split);
        void beginScene(const hlslpp::float4x4 &view, const hlslpp::float4x4 &proj, const RenderViewport &viewport,
            const RenderBuffer *worldPosBuffer, uint64_t worldPosSize, const RenderBuffer *worldNormBuffer, uint64_t worldNormSize,
            const RenderBuffer *faceIndicesBuffer, uint64_t faceIndicesSize,
            const RenderBuffer *rdpTilesBuffer, const RenderBuffer *gpuTilesBuffer, const RenderBuffer *texCoordBuffer);
        // culled: the draw has N64 face culling on, so primary rays skip its back faces.
        // character: a wrestler draw, shadowed with the weaker character strength.
        // lit: the draw uses N64 lighting (G_LIGHTING), so its vertices carry normals the AO can interpolate.
        // alphaTest: rays skip the draw's transparent texels (tileIndex, uvCorrection: its texture).
        // textureHash: the draw's first texture (0 if none), a venue fingerprint for the light presets.
        void addMesh(uint32_t faceIndicesStart, uint32_t triangleCount, uint32_t vertexCount, bool culled, bool character, bool lit,
            bool alphaTest, uint32_t tileIndex, bool uvCorrection, uint64_t textureHash);
        // Grows the screen bounds with a world-space triangle (3 xyz positions) of a gathered draw.
        // character: a wrestler triangle (left out of the scene bounds the light anchor uses).
        void extendBounds(const float *positions, bool character);
        void recordBuild(RenderWorker *worker);
        // textureSet: the framebuffer renderer's bindless texture cache set (alpha tests).
        void recordTrace(RenderWorker *worker, RenderTarget *colorTarget, RenderDescriptorSet *textureSet);
        void recordComposite(RenderWorker *worker);
        // After the workload's GPU work finished: accumulates and logs the stage timings (WCW_GPU_LOG).
        void collectTimings();
    };
};
