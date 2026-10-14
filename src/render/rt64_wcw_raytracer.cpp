//
// RT64
//

#include "rt64_wcw_raytracer.h"

#include "common/rt64_wcw_logic.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <json/json.hpp>

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
        wcwRtUserQuality = std::min(quality, uint32_t(WcwLogic::RtQuality::Ultra));
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
            envMode = mode;
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
        // Trace pass: b0 params, t1 scene, t2 world positions, t3 face indices, t4 mesh index starts, u5 output,
        // u6 current factors, t7-t10 tiles/texcoords/mesh tiles, t11 world normals.
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
        traceBuilder.addStructuredBuffer(11);
        traceBuilder.addStructuredBuffer(12);
        traceBuilder.addReadWriteTexture(13);
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
        resolveBuilder.addTexture(5);
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

        static_assert(WcwLogic::BlueNoiseSize == WCW_RT_BLUE_NOISE_SIZE, "the trace shader's blue noise tile size");
        const std::vector<float> blueNoise = WcwLogic::blueNoiseTile(WcwLogic::BlueNoiseSize);
        blueNoiseBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(blueNoise.size() * sizeof(float), RenderBufferFlag::STORAGE));
        void *blueNoiseBytes = blueNoiseBuffer->map();
        memcpy(blueNoiseBytes, blueNoise.data(), blueNoise.size() * sizeof(float));
        blueNoiseBuffer->unmap();
    }

    void WcwRaytracer::beginWorkload(uint64_t presentId) {
        if (presentId != lastPresentId) {
            finishPresent();
            lastPresentId = presentId;
            workloadInFrame = 0;
            frameCounter++;
        }
        else {
            workloadInFrame++;
        }

        workloadCounter++;
    }

    void WcwRaytracer::loadPresets(bool force) {
        if (presetsPath.empty()) {
            const char *pathText = getenv("WCW_RT_LIGHTS_FILE");
            presetsPath = (pathText != nullptr) ? pathText : "assets/rt_lights.json";
        }

        std::error_code ec;
        const auto stampTime = std::filesystem::last_write_time(presetsPath, ec);
        const int64_t stamp = ec ? INT64_MIN + 1 : int64_t(stampTime.time_since_epoch().count());
        if (!force && (stamp == presetsStamp)) {
            return;
        }

        const std::lock_guard<std::recursive_mutex> lock(presetsMutex);
        presetsStamp = stamp;
        presetFile = WcwLogic::LightPresetFile();
        mode = envMode;
        activeVenue = -1;
        venueSelector.reset();
        presetsEdited = false;
        if (ec) {
            applyPresetFile();
            return;
        }

        try {
            std::ifstream file(presetsPath, std::ios::binary);
            std::stringstream text;
            text << file.rdbuf();
            presetFile = WcwLogic::parseLightPresets(text.str());
            if (presetFile.hasDebugMode) {
                mode = presetFile.debugMode;
            }

            applyPresetFile();
            fprintf(stderr, "[wcw rt] light presets loaded from %s: %zu venue(s)\n", presetsPath.c_str(), venuePresets.size());
        }
        catch (const std::exception &e) {
            presetFile = WcwLogic::LightPresetFile();
            applyPresetFile();
            fprintf(stderr, "[wcw rt] light presets %s failed to parse: %s\n", presetsPath.c_str(), e.what());
        }
    }

    void WcwRaytracer::applyPresetFile() {
        const std::lock_guard<std::recursive_mutex> lock(presetsMutex);
        WcwLogic::computeUniqueMatches(presetFile.venues);
        defaultPreset = presetFile.defaultPreset;
        defaultPreset.name = "default";
        roomEnabled = presetFile.roomEnabled;
        roomPreset = presetFile.roomPreset;
        roomPreset.name = "room (fallback)";
        roomMaxExtent = presetFile.roomMaxExtent;
        roomCount = std::clamp(presetFile.roomCount, 1u, uint32_t(WCW_RT_MAX_LIGHTS));
        roomHeight = presetFile.roomHeight;
        venuePresets = presetFile.venues;
        if (activeVenue >= int(venuePresets.size())) {
            activeVenue = -1;
        }

        if (venueOverride >= int(venuePresets.size())) {
            venueOverride = -2;
        }

        // The room rig is generated from the scene bounds every present.
        updateRoomPreset();
    }

    WcwRaytracer::LightPreset WcwRaytracer::activePresetCopy() {
        const std::lock_guard<std::recursive_mutex> lock(presetsMutex);
        return activePreset();
    }

    void WcwRaytracer::finishPresent() {
        const std::lock_guard<std::recursive_mutex> lock(presetsMutex);
        // Textures drawn by world geometry this present, kept for the last VenueWindow presents.
        std::vector<uint64_t> hashes;
        hashes.reserve(frameTextureTris.size());
        for (const auto &it : frameTextureTris) {
            hashes.push_back(it.first);
            textureCounts[it.first]++;
            uint32_t &maxTris = textureMaxTris[it.first];
            maxTris = std::max(maxTris, it.second);
        }

        frameTextureTris.clear();
        const double presentSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        static const bool venueCuts = (getenv("WCW_RT_VENUE_CUT") == nullptr) || (strcmp(getenv("WCW_RT_VENUE_CUT"), "0") != 0);
        venueSelector.cutsEnabled = venueCuts;
        const int recentBefore = venueSelector.recent();
        const WcwLogic::VenueSelector::Change change = venueSelector.update(venuePresets, hashes, presentSeconds);
        textureHistory.push_back(std::move(hashes));
        if (presentMin[0] <= presentMax[0]) {
            for (uint32_t i = 0; i < 3; i++) {
                sceneMin[i] = presentMin[i];
                sceneMax[i] = presentMax[i];
                presentMin[i] = FLT_MAX;
                presentMax[i] = -FLT_MAX;
            }

            sceneBoundsValid = true;
        }

        if (textureHistory.size() > VenueWindow) {
            for (uint64_t h : textureHistory.front()) {
                auto it = textureCounts.find(h);
                if ((it != textureCounts.end()) && (--it->second == 0)) {
                    textureCounts.erase(it);
                    textureMaxTris.erase(h);
                }
            }

            textureHistory.pop_front();
        }

        if ((presentCount++ % 30) == 0) {
            loadPresets(false);
        }

        // WCW_RT_VENUE_TRACE: when the last frames start pointing to another venue (a camera cut), to time the switch.
        static const bool venueTrace = getenv("WCW_RT_VENUE_TRACE") != nullptr;
        auto venueName = [this](int v) { return (v >= 0) ? venuePresets[v].name.c_str() : "default"; };
        if (venueTrace && (venueSelector.recent() != recentBefore)) {
            const std::pair<int, float> leader = venueSelector.recentLeader(venuePresets);
            const std::pair<uint32_t, uint32_t> fresh = venueSelector.recentNew();
            fprintf(stderr, "[wcw rt] venue seen: %s (present %d, %.3f s; closest %s %.2f; new textures %u of %u)\n", venueName(venueSelector.recent()), int(wcwRtPresentCounter), presentSeconds,
                venueName(leader.first), leader.second, fresh.first, fresh.second);
        }

        activeVenue = venueSelector.active();
        if (change != WcwLogic::VenueSelector::Change::None) {
            // AkiTools' aki-run reads the venue from the last "venue:" line: keep it the bare name.
            fprintf(stderr, "[wcw rt] venue: %s\n", venueName(activeVenue));
            if (venueTrace) {
                fprintf(stderr, "[wcw rt] venue switch: %s (%s, present %d, %.3f s)\n", venueName(activeVenue),
                    (change == WcwLogic::VenueSelector::Change::Cut) ? "cut" : "window", int(wcwRtPresentCounter), presentSeconds);
            }
        }

        updateRoomPreset();

        static const bool venueLog = getenv("WCW_RT_VENUE_LOG") != nullptr;
        if (venueLog && ((presentCount % 300) == 0) && (textureHistory.size() == VenueWindow)) {
            std::vector<std::pair<uint32_t, uint64_t>> stable;
            for (const auto &it : textureCounts) {
                if (it.second >= (VenueWindow * 9) / 10) {
                    stable.emplace_back(textureMaxTris[it.first], it.first);
                }
            }

            std::sort(stable.begin(), stable.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            fprintf(stderr, "[wcw rt] venue log present %d (active %s%s): scene (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f), %zu stable world textures (hash(max tris)), largest first:", int(wcwRtPresentCounter), (activeVenue >= 0) ? venuePresets[activeVenue].name.c_str() : "default", roomActive ? ", room" : "",
                sceneMin[0], sceneMin[1], sceneMin[2], sceneMax[0], sceneMax[1], sceneMax[2], stable.size());
            for (size_t i = 0; i < stable.size(); i++) {
                fprintf(stderr, " %016llX(%u)", (unsigned long long)stable[i].second, stable[i].first);
            }

            fprintf(stderr, "\n");
        }
    }

    void WcwRaytracer::presetOrigin(const LightPreset &preset, float origin[3]) const {
        if (preset.anchorScene && sceneBoundsValid) {
            origin[0] = (sceneMin[0] + sceneMax[0]) * 0.5f;
            origin[1] = sceneMin[1];
            origin[2] = (sceneMin[2] + sceneMax[2]) * 0.5f;
        }
        else {
            origin[0] = origin[1] = origin[2] = 0.0f;
        }
    }

    void WcwRaytracer::updateRoomPreset() {
        const float extentX = sceneMax[0] - sceneMin[0];
        const float extentZ = sceneMax[2] - sceneMin[2];
        const bool wasActive = roomActive;
        roomActive = roomEnabled && (activeVenue < 0) && sceneBoundsValid && (std::max(extentX, extentZ) <= roomMaxExtent);
        if (roomActive != wasActive) {
            fprintf(stderr, "[wcw rt] venue: %s\n", roomActive ? "room (fallback)" : "default");
        }

        if (!roomActive) {
            return;
        }

        // Lights along the long axis, centered on the short one, just under the ceiling.
        const bool alongX = extentX >= extentZ;
        const float longMin = alongX ? sceneMin[0] : sceneMin[2];
        const float longLen = alongX ? extentX : extentZ;
        const float shortCenter = alongX ? (sceneMin[2] + sceneMax[2]) * 0.5f : (sceneMin[0] + sceneMax[0]) * 0.5f;
        const float height = sceneMin[1] + (sceneMax[1] - sceneMin[1]) * roomHeight;
        roomPreset.lights.clear();
        roomPreset.lightColors.clear();
        roomPreset.lightOwnColor.clear();
        for (uint32_t i = 0; i < roomCount; i++) {
            const float along = longMin + longLen * (float(i) + 0.5f) / float(roomCount);
            roomPreset.lights.push_back(alongX ? std::array<float, 4>{ along, height, shortCenter, 1.0f } : std::array<float, 4>{ shortCenter, height, along, 1.0f });
        }

        roomPreset.center[0] = (sceneMin[0] + sceneMax[0]) * 0.5f;
        roomPreset.center[1] = height;
        roomPreset.center[2] = (sceneMin[2] + sceneMax[2]) * 0.5f;
        roomPreset.radius = 0.0f;
        if (roomPreset.falloff <= 0.0f) {
            roomPreset.falloff = 0.0f;
        }
    }

    const WcwRaytracer::LightPreset &WcwRaytracer::activePreset() const {
        // The Lighting editor can force a preset.
        if ((venueOverride >= 0) && (venueOverride < int(venuePresets.size()))) {
            return venuePresets[venueOverride];
        }
        else if (venueOverride == -1) {
            return defaultPreset;
        }

        if (activeVenue >= 0) {
            return venuePresets[activeVenue];
        }

        return roomActive ? roomPreset : defaultPreset;
    }

    uint32_t WcwRaytracer::chooseSlot() const {
        // Score = (1 - IoU of the padded world bounds) + relative mesh count difference. The ring
        // and the floor bits are static, the wrestlers move a little between their workloads.
        const float Pad = 10.0f;
        auto volume = [](const float *mn, const float *mx) {
            return std::max(mx[0] - mn[0], 0.0f) * std::max(mx[1] - mn[1], 0.0f) * std::max(mx[2] - mn[2], 0.0f);
        };

        float curMin[3], curMax[3];
        for (uint32_t i = 0; i < 3; i++) {
            curMin[i] = worldMin[i] - Pad;
            curMax[i] = worldMax[i] + Pad;
        }

        const uint32_t meshCount = uint32_t(meshes.size());
        uint32_t best = UINT32_MAX;
        float bestScore = 1.0f;
        uint32_t oldest = 0;
        for (uint32_t s = 0; s < MaxSlots; s++) {
            const Slot &slot = slots[s];
            if (!slot.valid) {
                if (slots[oldest].valid) {
                    oldest = s;
                }

                continue;
            }

            if (slots[oldest].valid && (slot.builtWorkload < slots[oldest].builtWorkload)) {
                oldest = s;
            }

            float slotMin[3], slotMax[3], interMin[3], interMax[3];
            for (uint32_t i = 0; i < 3; i++) {
                slotMin[i] = slot.boundsMin[i] - Pad;
                slotMax[i] = slot.boundsMax[i] + Pad;
                interMin[i] = std::max(curMin[i], slotMin[i]);
                interMax[i] = std::min(curMax[i], slotMax[i]);
            }

            const float inter = volume(interMin, interMax);
            const float uni = volume(curMin, curMax) + volume(slotMin, slotMax) - inter;
            const float iou = (uni > 0.0f) ? (inter / uni) : 0.0f;
            const float meshDiff = float(std::abs(int(meshCount) - int(slot.meshCount))) / float(std::max(std::max(meshCount, slot.meshCount), 1u));
            const float score = (1.0f - iou) + meshDiff;
            // Different scenes can have similar mesh counts (a backstage room and the wrestlers in
            // it): reuse a slot only when the bounds really overlap.
            if ((iou >= 0.25f) && (score < bestScore)) {
                bestScore = score;
                best = s;
            }
        }

        return (best != UINT32_MAX) ? best : oldest;
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

    static std::atomic<int> wcwRtSplitOverride = -1;

    void WcwRaytracer::setSplitScreenOverride(int split) {
        wcwRtSplitOverride = split;
    }

    bool WcwRaytracer::splitScreen() {
        static const bool split = getenv("WCW_RT_SPLIT") != nullptr;
        const int forced = wcwRtSplitOverride;
        return (forced >= 0) ? (forced != 0) : split;
    }

    bool WcwRaytracer::replacesBlobShadows() const {
        static const bool keepBlobs = getenv("WCW_RT_KEEP_BLOBS") != nullptr;
        return supported && active() && (mode == WCW_RT_MODE_SHADOW) && !keepBlobs;
    }

    bool WcwRaytracer::keepsBlobShadow(float x, float z) const {
        return WcwLogic::keepBlobShadow(x, z, receiverCenter[0], receiverCenter[1], receiverRadius);
    }

    void WcwRaytracer::beginScene(const hlslpp::float4x4 &view, const hlslpp::float4x4 &proj, const RenderViewport &viewport,
        const RenderBuffer *worldPosBuffer, uint64_t worldPosSize, const RenderBuffer *worldNormBuffer, uint64_t worldNormSize,
        const RenderBuffer *faceIndicesBuffer, uint64_t faceIndicesSize,
        const RenderBuffer *rdpTilesBuffer, const RenderBuffer *gpuTilesBuffer, const RenderBuffer *texCoordBuffer)
    {
        boundsViewProj = hlslpp::mul(view, proj);
        boundsMin[0] = boundsMin[1] = FLT_MAX;
        boundsMax[0] = boundsMax[1] = -FLT_MAX;
        boundsFullScreen = false;
        if (presetsStamp == INT64_MIN) {
            loadPresets(true);
        }

        const LightPreset preset = activePresetCopy();
        receiverRadius = std::max(envFloat(getenv("WCW_RT_RADIUS"), preset.radius), 0.0f);
        float origin[3];
        presetOrigin(preset, origin);
        receiverCenter[0] = envFloat(getenv("WCW_RT_LIGHT_X"), origin[0] + preset.center[0]);
        receiverCenter[1] = envFloat(getenv("WCW_RT_LIGHT_Z"), origin[2] + preset.center[2]);
        for (uint32_t i = 0; i < 3; i++) {
            worldMin[i] = FLT_MAX;
            worldMax[i] = -FLT_MAX;
        }

        this->rdpTilesBuffer = rdpTilesBuffer;
        this->gpuTilesBuffer = gpuTilesBuffer;
        this->texCoordBuffer = texCoordBuffer;
        sceneActive = true;
        viewMatrix = view;
        projMatrix = proj;
        this->viewport = viewport;
        this->worldPosBuffer = worldPosBuffer;
        this->worldPosSize = worldPosSize;
        this->worldNormBuffer = worldNormBuffer;
        this->worldNormSize = worldNormSize;
        this->faceIndicesBuffer = faceIndicesBuffer;
        this->faceIndicesSize = faceIndicesSize;
    }

    void WcwRaytracer::addMesh(uint32_t faceIndicesStart, uint32_t triangleCount, uint32_t vertexCount, bool culled, bool character, bool lit,
        bool alphaTest, uint32_t tileIndex, bool uvCorrection, uint64_t textureHash)
    {
        if ((textureHash != 0) && !character) {
            frameTextureTris[textureHash] += triangleCount;
        }

        // Without tile data the alpha test can't be evaluated: keep the draw opaque.
        alphaTest = alphaTest && (rdpTilesBuffer != nullptr) && (gpuTilesBuffer != nullptr) && (texCoordBuffer != nullptr);
        const uint32_t IndexStride = sizeof(uint32_t);
        const uint32_t PosStride = sizeof(float) * 4;
        meshes.emplace_back(RenderBufferReference(faceIndicesBuffer, faceIndicesStart * IndexStride), RenderBufferReference(worldPosBuffer, 0),
            RenderFormat::R32_UINT, RenderFormat::R32G32B32_FLOAT, triangleCount * 3, vertexCount, PosStride, !culled && !alphaTest);

        // Culled draws are non-opaque so the trace shader sees their candidates; the top bits
        // carry the culled/character flags.
        meshIndexStarts.emplace_back(faceIndicesStart | (culled ? WCW_RT_MESH_CULLED : 0u) | (character ? WCW_RT_MESH_CHARACTER : 0u) |
            ((lit && (worldNormBuffer != nullptr)) ? WCW_RT_MESH_LIT : 0u));
        meshTiles.emplace_back((tileIndex & WCW_RT_TILE_INDEX_MASK) | (alphaTest ? WCW_RT_TILE_ALPHA_TEST : 0u) | (uvCorrection ? WCW_RT_TILE_UV_CORRECTION : 0u));
    }

    void WcwRaytracer::extendBounds(const float *positions, bool character) {
        for (uint32_t i = 0; i < 9; i++) {
            worldMin[i % 3] = std::min(worldMin[i % 3], positions[i]);
            worldMax[i % 3] = std::max(worldMax[i % 3], positions[i]);
            if (!character) {
                presentMin[i % 3] = std::min(presentMin[i % 3], positions[i]);
                presentMax[i % 3] = std::max(presentMax[i % 3], positions[i]);
            }
        }

        if (boundsFullScreen) {
            return;
        }

        // Triangles entirely outside the receiver radius only need primary rays where other
        // geometry is traced anyway: they don't widen the rect.
        if (receiverRadius > 0.0f) {
            float minX = FLT_MAX, maxX = -FLT_MAX, minZ = FLT_MAX, maxZ = -FLT_MAX;
            for (uint32_t i = 0; i < 3; i++) {
                minX = std::min(minX, positions[i * 3 + 0]);
                maxX = std::max(maxX, positions[i * 3 + 0]);
                minZ = std::min(minZ, positions[i * 3 + 2]);
                maxZ = std::max(maxZ, positions[i * 3 + 2]);
            }

            const float dx = std::max({ minX - receiverCenter[0], 0.0f, receiverCenter[0] - maxX });
            const float dz = std::max({ minZ - receiverCenter[1], 0.0f, receiverCenter[1] - maxZ });
            if ((dx * dx + dz * dz) > (receiverRadius * receiverRadius)) {
                return;
            }
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
        if (!sceneActive || meshes.empty()) {
            return;
        }

        currentSlot = chooseSlot();
        Slot &slot = slots[currentSlot];
        if (!slot.valid) {
            slot.historyValid = false;
        }

        // Bottom level: every gathered draw call is one geometry over the shared world position buffer.
        device->setBottomLevelASBuildInfo(blasInfo, meshes.data(), uint32_t(meshes.size()), true, false);
        if (blasInfo.accelerationStructureSize > slot.blasBufferSize) {
            slot.blasBufferSize = alignUp((blasInfo.accelerationStructureSize * 3) / 2, 256);
            slot.blasBuffer = device->createBuffer(RenderBufferDesc::AccelerationStructureBuffer(slot.blasBufferSize));
            slot.blas = device->createAccelerationStructure(RenderAccelerationStructureDesc(RenderAccelerationStructureType::BOTTOM_LEVEL, RenderBufferReference(slot.blasBuffer.get()), slot.blasBufferSize));
        }

        slot.valid = true;
        slot.builtWorkload = workloadCounter;
        slot.builtFrame = frameCounter;
        slot.meshCount = uint32_t(meshes.size());
        for (uint32_t i = 0; i < 3; i++) {
            slot.boundsMin[i] = worldMin[i];
            slot.boundsMax[i] = worldMax[i];
        }
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
        // scenes built recently (mask 0x2: shadow rays only). Scenes the game stopped drawing
        // age out after a few game frames.
        instances.clear();
        instances.emplace_back(RenderBufferReference(slot.blasBuffer.get()), 0, 0x1, 0, true, RenderAffineTransform());
        for (uint32_t s = 0; s < MaxSlots; s++) {
            const Slot &other = slots[s];
            if ((s != currentSlot) && other.valid && WcwLogic::shadowCasterFresh(frameCounter, other.builtFrame)) {
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
        // The level's trace resolution and ray counts (WcwLogic::rtPreset). WCW_RT_MAX_LINES overrides the line cap,
        // WCW_RT_SCALE the whole scale.
        WcwLogic::RtPreset qualityPreset = WcwLogic::rtPreset(quality);
        qualityPreset.maxLines = envFloat(getenv("WCW_RT_MAX_LINES"), qualityPreset.maxLines);
        const float frameScale = std::clamp(envFloat(getenv("WCW_RT_SCALE"), WcwLogic::rtTraceScale(qualityPreset, colorTarget->height)), 0.1f, 1.0f);
        const uint32_t presetSamples = qualityPreset.samplesPerLight;

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
            outputTexture = device->createTexture(RenderTextureDesc::Texture2D(traceWidth, traceHeight, 1, RenderFormat::R16G16B16A16_FLOAT, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
            currentTexture = device->createTexture(RenderTextureDesc::Texture2D(traceWidth, traceHeight, 1, RenderFormat::R32G32B32A32_FLOAT, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
            currentAoTexture = device->createTexture(RenderTextureDesc::Texture2D(traceWidth, traceHeight, 1, RenderFormat::R16_FLOAT, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
        }

        Slot &slot = slots[currentSlot];
        if ((slot.history[0] == nullptr) || (slot.historyWidth != traceWidth) || (slot.historyHeight != traceHeight)) {
            slot.historyWidth = traceWidth;
            slot.historyHeight = traceHeight;
            for (auto &history : slot.history) {
                history = device->createTexture(RenderTextureDesc::Texture2D(traceWidth, traceHeight, 1, RenderFormat::R16G16B16A16_FLOAT, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
            }

            slot.output = device->createTexture(RenderTextureDesc::Texture2D(traceWidth, traceHeight, 1, RenderFormat::R16G16B16A16_FLOAT, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
            slot.tracedPresent = UINT64_MAX;

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
                // 2x multiply: dst * src + src * dst. The resolve stores factors at half scale.
                multiply.srcBlend = RenderBlend::DEST_COLOR;
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
        const LightPreset preset = activePresetCopy();
        float origin[3];
        presetOrigin(preset, origin);
        const float centerX = envFloat(getenv("WCW_RT_LIGHT_X"), origin[0] + preset.center[0]);
        const float centerY = envFloat(getenv("WCW_RT_LIGHT_Y"), origin[1] + preset.center[1]);
        const float centerZ = envFloat(getenv("WCW_RT_LIGHT_Z"), origin[2] + preset.center[2]);
        params.lightPosition = hlslpp::float4(centerX, centerY, centerZ, 1.0f);
        params.viewport = hlslpp::float4(viewport.x, viewport.y, viewport.width, viewport.height);
        params.resolution = { traceWidth, traceHeight };
        params.mode = mode;
        params.shadowStrength = envFloat(getenv("WCW_RT_SHADOW"), preset.shadow);
        params.lightSpread = envFloat(getenv("WCW_RT_LIGHT_SPREAD"), preset.spread);
        params.lightCount = std::min(uint32_t(envFloat(getenv("WCW_RT_LIGHTS"), float(preset.fills))), 4u);
        params.cullSign = envFloat(getenv("WCW_RT_CULL_SIGN"), 1.0f);
        params.keyWeight = envFloat(getenv("WCW_RT_KEY_WEIGHT"), preset.keyWeight);
        params.characterShadowStrength = envFloat(getenv("WCW_RT_SHADOW_CHAR"), preset.characterShadow);
        params.lightSize = envFloat(getenv("WCW_RT_LIGHT_SIZE"), preset.size);

        // Light list: the preset's explicit lights, or the generated rig (key + fills at +-spread).
        params.lightListCount = 0;
        params.lightClearance = std::max(preset.clearance, 0.0f);
        params.lightFalloff = std::max(envFloat(getenv("WCW_RT_FALLOFF"), preset.falloff), 0.0f);
        params.glowCount = 0;
        // Colored light (WcwLogic::normalizedLightColor): white unless the preset says otherwise.
        for (uint32_t c = 0; c < WCW_RT_MAX_LIGHTS; c++) {
            params.lightColors[c] = { 1.0f, 1.0f, 1.0f, 0.0f };
        }

        for (uint32_t c = 0; c < WCW_RT_MAX_GLOWS; c++) {
            params.glowColors[c] = { 1.0f, 1.0f, 1.0f, 0.0f };
        }

        auto setLightColor = [&](uint32_t index, const std::array<float, 3> &rgb) {
            const std::array<float, 3> n = WcwLogic::normalizedLightColor(rgb[0], rgb[1], rgb[2]);
            params.lightColors[index] = { n[0], n[1], n[2], 0.0f };
        };

        params.colorSplit = (envFloat(getenv("WCW_RT_COLOR_SPLIT"), 0.0f) != 0.0f) ? 1u : 0u;
        {
            const WcwLogic::SheenSettings sheen = WcwLogic::sheenSettings();
            params.sheenSpecular = std::max(envFloat(getenv("WCW_RT_SHEEN"), sheen.specular), 0.0f);
            params.sheenShininess = std::max(envFloat(getenv("WCW_RT_SHEEN_SHININESS"), sheen.shininess), 1.0f);
            params.sheenRim = std::max(envFloat(getenv("WCW_RT_RIM"), sheen.rim), 0.0f);
            params.sheenRimPower = std::max(envFloat(getenv("WCW_RT_RIM_POWER"), sheen.rimPower), 1.0f);
            params.sheenSplit = (envFloat(getenv("WCW_RT_SHEEN_SPLIT"), 0.0f) != 0.0f) ? 1u : 0u;
        }
        params.glowPadding0 = params.glowPadding1 = params.glowPadding2 = 0.0f;
        params.glowIntensity = { 0.0f, 0.0f, 0.0f, 0.0f };
        // Flickering glows (WcwLogic::glowFlickerScale) run on the wall clock since the ray tracer started.
        static const auto flickerStart = std::chrono::steady_clock::now();
        const double flickerTime = std::chrono::duration<double>(std::chrono::steady_clock::now() - flickerStart).count();
        for (const auto &g : preset.glows) {
            if (params.glowCount < WCW_RT_MAX_GLOWS) {
                const size_t glowIndex = size_t(&g - preset.glows.data());
                params.glows[params.glowCount] = { origin[0] + g[0], origin[1] + g[1], origin[2] + g[2], std::max(g[4], 1.0f) };
                params.glowIntensity[params.glowCount] = g[3] * WcwLogic::glowFlickerScale(flickerTime, uint32_t(glowIndex) * 7919u + 17u, WcwLogic::presetGlowFlicker(preset, glowIndex));
                const std::array<float, 3> glowColor = WcwLogic::presetGlowColor(preset, glowIndex);
                params.glowColors[params.glowCount] = { std::max(glowColor[0], 0.0f), std::max(glowColor[1], 0.0f), std::max(glowColor[2], 0.0f), 0.0f };
                params.glowCount++;
            }
        }
        if (!preset.lights.empty() && (getenv("WCW_RT_LIGHTS") == nullptr)) {
            float weightSum = 0.0f;
            for (const auto &l : preset.lights) {
                weightSum += l[3];
            }

            for (const auto &l : preset.lights) {
                if (params.lightListCount < WCW_RT_MAX_LIGHTS) {
                    const size_t lightIndex = size_t(&l - preset.lights.data());
                    setLightColor(params.lightListCount, WcwLogic::presetLightColor(preset, lightIndex));
                    params.lights[params.lightListCount++] = { origin[0] + l[0], origin[1] + l[1], origin[2] + l[2], l[3] / std::max(weightSum, 1e-6f) };
                }
            }
        }
        else {
            const float offsets[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f } };
            const uint32_t fills = params.lightCount;
            const float keyWeight = (fills > 0) ? params.keyWeight : 1.0f;
            const float fillWeight = (fills > 0) ? (1.0f - keyWeight) / float(fills) : 0.0f;
            setLightColor(params.lightListCount, preset.keyColor);
            params.lights[params.lightListCount++] = { centerX, centerY, centerZ, keyWeight };
            for (uint32_t f = 0; f < fills; f++) {
                setLightColor(params.lightListCount, preset.fillColor);
                params.lights[params.lightListCount++] = { centerX + offsets[f][0] * params.lightSpread, centerY, centerZ + offsets[f][1] * params.lightSpread, fillWeight };
            }
        }
        params.temporalAlpha = envFloat(getenv("WCW_RT_TEMPORAL"), 0.1f);
        params.frameIndex = frameIndex++;
        params.prevViewProj = slot.prevViewProj;
        params.historyValid = slot.historyValid ? 1 : 0;
        params.samplesPerLight = std::max(uint32_t(envFloat(getenv("WCW_RT_SAMPLES"), float(presetSamples))), 1u);
        // Ambient occlusion: the quality level's look and rays (WcwLogic::aoSettings), each overridable from the environment.
        const WcwLogic::AoSettings ao = WcwLogic::aoSettings(quality);
        params.aoSamples = uint32_t(envFloat(getenv("WCW_RT_AO_SAMPLES"), float(ao.samples)));
        params.aoRadius = envFloat(getenv("WCW_RT_AO_RADIUS"), ao.radius);
        params.aoStrength = envFloat(getenv("WCW_RT_AO"), ao.strength);
        params.aoCharacterRadius = envFloat(getenv("WCW_RT_AO_RADIUS_CHAR"), ao.characterRadius);
        params.aoCharacterStrength = envFloat(getenv("WCW_RT_AO_CHAR"), ao.characterStrength);
        params.aoSmoothNormals = (envFloat(getenv("WCW_RT_AO_SMOOTH"), 1.0f) != 0.0f) ? 1u : 0u;
        params.aoGain = std::max(envFloat(getenv("WCW_RT_AO_GAIN"), ao.gain), 0.0f);
        params.aoSplit = (envFloat(getenv("WCW_RT_AO_SPLIT"), 0.0f) != 0.0f) ? 1u : 0u;
        params.filterRadius = uint32_t(std::clamp(envFloat(getenv("WCW_RT_FILTER_RADIUS"), 2.0f), 1.0f, 4.0f));
        {
            const WcwLogic::DenoiseSettings denoise = WcwLogic::denoiseSettings();
            const float maxRadius = float(WcwLogic::DenoiseMaxRadius);
            params.shadowFilterRadius = uint32_t(std::clamp(envFloat(getenv("WCW_RT_SHADOW_FILTER_RADIUS"), float(denoise.shadowRadius)), 1.0f, maxRadius));
            params.aoFilterRadius = uint32_t(std::clamp(envFloat(getenv("WCW_RT_AO_FILTER_RADIUS"), float(denoise.aoRadius)), 1.0f, maxRadius));
            params.shadowTemporalAlpha = std::clamp(envFloat(getenv("WCW_RT_SHADOW_ALPHA"), denoise.shadowAlpha), 0.01f, 1.0f);
            params.aoTemporalAlpha = std::clamp(envFloat(getenv("WCW_RT_AO_ALPHA"), denoise.aoAlpha), 0.01f, 1.0f);
            params.denoiseSplit = (envFloat(getenv("WCW_RT_DENOISE_SPLIT"), 0.0f) != 0.0f) ? 1u : 0u;
        }
        params.aoCharacterGain = std::max(envFloat(getenv("WCW_RT_AO_GAIN_CHAR"), ao.characterGain), 0.0f);
        params.noiseMode = (envFloat(getenv("WCW_RT_NOISE"), 1.0f) != 0.0f) ? 1u : 0u;
        params.noiseSplit = (envFloat(getenv("WCW_RT_NOISE_SPLIT"), 0.0f) != 0.0f) ? 1u : 0u;
        params.shadowSmooth = (envFloat(getenv("WCW_RT_SHADOW_SMOOTH"), 1.0f) != 0.0f) ? 1u : 0u;
        // WCW_RT_SHADOW_SMOOTH_CYCLE=1: smooth shadows on odd presents only (consecutive-present comparisons).
        if ((envFloat(getenv("WCW_RT_SHADOW_SMOOTH_CYCLE"), 0.0f) != 0.0f) && (lastPresentId != UINT64_MAX)) {
            params.shadowSmooth = uint32_t(lastPresentId & 1);
        }
        params.shadowSmoothSplit = (envFloat(getenv("WCW_RT_SHADOW_SMOOTH_SPLIT"), 0.0f) != 0.0f) ? 1u : 0u;
        params.terminatorFade = std::clamp(envFloat(getenv("WCW_RT_TERMINATOR_FADE"), 0.2f), 0.0f, 1.0f);
        {
            static const uint64_t aoCyclePeriodMs = uint64_t(std::max(envFloat(getenv("WCW_RT_AO_CYCLE"), 0.0f), 0.0f) * 1000.0f);
            const uint64_t unixMs = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            params.aoCycleOff = WcwLogic::aoCycleOn(unixMs, aoCyclePeriodMs) ? 0u : 1u;
        }
        params.traceScale = float(traceWidth) / float(outputWidth);

        // [wcw rt] Outside darkness (the preset's outsideDarkness; WCW_RT_DARKNESS overrides): the arena and crowd
        // beyond the receiver radius fall into shadow. Misses (the crowd and background, never in the traced scene)
        // darken once per frame, on the arena pass (WcwLogic::MissDarkening).
        const float outsideDarkness = std::clamp(envFloat(getenv("WCW_RT_DARKNESS"), preset.outsideDarkness), 0.0f, 1.0f);
        params.outsideDarkness = outsideDarkness;
        params.outsideEdge = std::max(envFloat(getenv("WCW_RT_DARKNESS_EDGE"), preset.outsideEdge), 0.0f);
        // Misses land on the arena floor: the lowest point of the scene's world geometry.
        params.missFloorY = sceneBoundsValid ? sceneMin[1] : 0.0f;
        // Lit areas (offsets from the anchor like lights and glows): where outside darkness eases off.
        {
            float litOrigin[3];
            presetOrigin(preset, litOrigin);
            float *litFields[WCW_RT_MAX_LIT_AREAS][4] = {
                { &params.litArea0X, &params.litArea0Z, &params.litArea0Radius, &params.litArea0Edge },
                { &params.litArea1X, &params.litArea1Z, &params.litArea1Radius, &params.litArea1Edge }
            };

            params.litAreaCount = 0;
            for (uint32_t a = 0; a < WCW_RT_MAX_LIT_AREAS; a++) {
                const bool used = a < preset.litAreas.size();
                *litFields[a][0] = used ? litOrigin[0] + preset.litAreas[a][0] : 0.0f;
                *litFields[a][1] = used ? litOrigin[2] + preset.litAreas[a][1] : 0.0f;
                *litFields[a][2] = used ? preset.litAreas[a][2] : 0.0f;
                *litFields[a][3] = used ? preset.litAreas[a][3] : 0.0f;
                params.litAreaCount += used ? 1u : 0u;
            }
        }
        const bool darkenMisses = missDarkening.darkenThisPass(colorTarget, lastPresentId, meshes.size());
        params.darkenMisses = ((outsideDarkness > 0.0f) && darkenMisses) ? 1u : 0u;

        {
            static const bool darknessLog = getenv("WCW_RT_DARKNESS_LOG") != nullptr;
            static uint32_t darknessLogCount = 0;
            if (darknessLog && ((darknessLogCount++ % 97) < 6)) {
                fprintf(stderr, "[wcw rt darkness] present %llu workload %u target %p %ux%u darkness %.2f misses %u meshes %zu preset %s\n",
                    (unsigned long long)lastPresentId, workloadInFrame, (void *)colorTarget, colorTarget->width, colorTarget->height,
                    outsideDarkness, params.darkenMisses, meshes.size(), preset.name.c_str());
            }
        }

        // Trace only this workload's screen footprint (plus a margin for the bilinear upsample
        // and the resolve filters' neighborhoods); the whole screen with outside darkness.
        const float margin = float(std::max({ 3u, params.shadowFilterRadius + 1, params.aoFilterRadius + 1, params.filterRadius + 1 }));
        if (boundsFullScreen || (outsideDarkness > 0.0f)) {
            traceRect[0] = traceRect[1] = 0;
            traceRect[2] = traceWidth;
            traceRect[3] = traceHeight;
        }
        else {
            const float scale = params.traceScale;
            traceRect[0] = uint32_t(std::clamp(std::floor(boundsMin[0] * scale) - margin, 0.0f, float(traceWidth)));
            traceRect[1] = uint32_t(std::clamp(std::floor(boundsMin[1] * scale) - margin, 0.0f, float(traceHeight)));
            traceRect[2] = uint32_t(std::clamp(std::ceil(boundsMax[0] * scale) + margin, 0.0f, float(traceWidth)));
            traceRect[3] = uint32_t(std::clamp(std::ceil(boundsMax[1] * scale) + margin, 0.0f, float(traceHeight)));
        }

        traceRectEmpty = (traceRect[2] <= traceRect[0]) || (traceRect[3] <= traceRect[1]);
        if (traceRectEmpty) {
            return;
        }

        // Same scene already traced this present with the same camera: composite that result again.
        static const bool noReuse = getenv("WCW_RT_NO_REUSE") != nullptr;
        if (!noReuse && (slot.tracedPresent == lastPresentId) && (memcmp(&slot.tracedViewProj, &viewProj, sizeof(viewProj)) == 0) &&
            (memcmp(slot.tracedRect, traceRect, sizeof(traceRect)) == 0) && (slot.tracedMeshes == uint32_t(meshes.size())))
        {
            compositeSet->setTexture(0, slot.output.get(), RenderTextureLayout::SHADER_READ);
            reusedTraces++;
            return;
        }

        slot.tracedPresent = lastPresentId;
        slot.tracedViewProj = viewProj;
        memcpy(slot.tracedRect, traceRect, sizeof(traceRect));
        slot.tracedMeshes = uint32_t(meshes.size());

        params.traceOffset = { traceRect[0], traceRect[1] };
        params.traceEnd = { traceRect[2], traceRect[3] };
        params.receiverRadius = receiverRadius;
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
        traceSet->setTexture(5, slot.output.get(), RenderTextureLayout::GENERAL);
        traceSet->setTexture(6, currentTexture.get(), RenderTextureLayout::GENERAL);
        traceSet->setTexture(13, currentAoTexture.get(), RenderTextureLayout::GENERAL);
        const RenderBufferStructuredView rdpTileView(sizeof(interop::RDPTile));
        const RenderBufferStructuredView gpuTileView(sizeof(interop::GPUTile));
        const RenderBufferStructuredView float2View(sizeof(float) * 2);
        traceSet->setBuffer(7, (rdpTilesBuffer != nullptr) ? rdpTilesBuffer : meshTilesBuffer.get(), 0, &rdpTileView);
        traceSet->setBuffer(8, (gpuTilesBuffer != nullptr) ? gpuTilesBuffer : meshTilesBuffer.get(), 0, &gpuTileView);
        traceSet->setBuffer(9, (texCoordBuffer != nullptr) ? texCoordBuffer : meshTilesBuffer.get(), 0, &float2View);
        traceSet->setBuffer(10, meshTilesBuffer.get(), meshTilesBufferSize, &uintView);
        traceSet->setBuffer(11, (worldNormBuffer != nullptr) ? worldNormBuffer : worldPosBuffer, (worldNormBuffer != nullptr) ? worldNormSize : worldPosSize, &float4View);
        const RenderBufferStructuredView floatView(sizeof(float));
        traceSet->setBuffer(12, blueNoiseBuffer.get(), WcwLogic::BlueNoiseSize * WcwLogic::BlueNoiseSize * sizeof(float), &floatView);
        compositeSet->setTexture(0, slot.output.get(), RenderTextureLayout::SHADER_READ);

        // Trace.
        worker->commandList->barriers(RenderBarrierStage::COMPUTE, {
            RenderTextureBarrier(slot.output.get(), RenderTextureLayout::GENERAL),
            RenderTextureBarrier(currentTexture.get(), RenderTextureLayout::GENERAL),
            RenderTextureBarrier(currentAoTexture.get(), RenderTextureLayout::GENERAL)
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
            // WCW_RT_TRACELOG=1: every 100th frame; =<present>: every trace for 30 presents from there.
            static const int traceLogAt = getenv("WCW_RT_TRACELOG") ? atoi(getenv("WCW_RT_TRACELOG")) : -1;
            const bool traceLogNow = (traceLogAt == 1) ? ((frameCounter % 100) == 0) : ((traceLogAt > 1) && (wcwRtPresentCounter >= traceLogAt) && (wcwRtPresentCounter < traceLogAt + 30));
            if (traceLogNow) {
                fprintf(stderr, "[wcw rt trace] present %d frame %llu slot %u others %u hist %d target %p %ux%u rect (%u,%u)-(%u,%u) of %ux%u meshes %u tris %u world (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)\n",
                    int(wcwRtPresentCounter), (unsigned long long)frameCounter, currentSlot, uint32_t(instances.size() - 1), slot.historyValid ? 1 : 0, (void *)colorTarget, outputWidth, outputHeight, traceRect[0], traceRect[1], traceRect[2], traceRect[3],
                    traceWidth, traceHeight, timingMeshes, timingTriangles, worldMin[0], worldMin[1], worldMin[2], worldMax[0], worldMax[1], worldMax[2]);
            }
        }

        // Shadow mode: temporal resolve of the jittered samples into this slot's history.
        if ((mode == WCW_RT_MODE_SHADOW) || (mode == WCW_RT_MODE_AO)) {
            RenderTexture *historyIn = slot.history[slot.historyIndex].get();
            RenderTexture *historyOut = slot.history[slot.historyIndex ^ 1].get();
            resolveSet->setBuffer(0, paramsBuffer.get(), alignUp(sizeof(interop::WcwRtParams), 256));
            resolveSet->setTexture(1, currentTexture.get(), RenderTextureLayout::SHADER_READ);
            resolveSet->setTexture(5, currentAoTexture.get(), RenderTextureLayout::SHADER_READ);
            resolveSet->setTexture(2, historyIn, RenderTextureLayout::SHADER_READ);
            resolveSet->setTexture(3, historyOut, RenderTextureLayout::GENERAL);
            resolveSet->setTexture(4, slot.output.get(), RenderTextureLayout::GENERAL);
            worker->commandList->barriers(RenderBarrierStage::COMPUTE, {
                RenderTextureBarrier(currentTexture.get(), RenderTextureLayout::SHADER_READ),
                RenderTextureBarrier(currentAoTexture.get(), RenderTextureLayout::SHADER_READ),
                RenderTextureBarrier(historyIn, RenderTextureLayout::SHADER_READ),
                RenderTextureBarrier(historyOut, RenderTextureLayout::GENERAL),
                RenderTextureBarrier(slot.output.get(), RenderTextureLayout::GENERAL)
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

        worker->commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(slot.output.get(), RenderTextureLayout::SHADER_READ));
    }

    void WcwRaytracer::recordComposite(RenderWorker *worker) {
        if (!structuresBuilt || (compositePipeline == nullptr) || traceRectEmpty) {
            return;
        }

        // Composite only the traced rect (inside its margin), in color target pixels.
        const float invScale = float(outputWidth) / float(traceWidth);
        const RenderViewport fullViewport(0.0f, 0.0f, float(outputWidth), float(outputHeight));
        RenderRect fullRect(int32_t((traceRect[0] + 1) * invScale), int32_t((traceRect[1] + 1) * invScale),
            std::min(int32_t((traceRect[2] - 1) * invScale), int32_t(outputWidth)), std::min(int32_t((traceRect[3] - 1) * invScale), int32_t(outputHeight)));

        // WCW_RT_SPLIT: composite only the right half of the screen (before/after comparisons).
        if (splitScreen()) {
            fullRect.left = std::max(fullRect.left, int32_t(outputWidth / 2));
            if (fullRect.left >= fullRect.right) {
                return;
            }
        }
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
            fprintf(stderr, "[wcw rt gpu] per traced workload avg: build=%.3f trace=%.3f resolve=%.3f composite=%.3f ms, build->composite span=%.3f (max %.3f) ms; %llu meshes (%llu alpha-tested), %llu tris, %llu trace px of %u lines, %llu full-screen, %u traces reused (300 workloads)\n",
                sums[0] / count, sums[1] / count, sums[2] / count, sums[3] / count, sums[4] / count, maxTotal,
                (unsigned long long)(meshSum / count), (unsigned long long)(alphaSum / count), (unsigned long long)(triangleSum / count), (unsigned long long)(pixelSum / count), timingTraceHeight, (unsigned long long)fullScreenSum, reusedTraces);
            reusedTraces = 0;
            memset(sums, 0, sizeof(sums));
            maxTotal = 0.0;
            triangleSum = meshSum = alphaSum = pixelSum = fullScreenSum = 0;
            count = 0;
        }
    }
};
