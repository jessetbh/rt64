//
// RT64
//

#include "rt64_wcw_logic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace RT64::WcwLogic {
    bool shadowCasterFresh(uint64_t frameCounter, uint64_t builtFrame) {
        return (frameCounter >= builtFrame) && ((frameCounter - builtFrame) <= ShadowCasterStaleFrames);
    }

    uint64_t rtTextureBytes(uint32_t traceWidth, uint32_t traceHeight, uint32_t allocatedSlots, bool before72) {
        const uint64_t texels = uint64_t(traceWidth) * uint64_t(traceHeight);
        const uint64_t slotBytes = before72 ? RtSlotTexelBytesBefore72 : RtSlotTexelBytes;
        const uint64_t sharedBytes = before72 ? RtSharedTexelBytesBefore72 : RtSharedTexelBytes;
        return texels * (sharedBytes + slotBytes * allocatedSlots);
    }

    bool releaseSlotTextures(uint64_t frameCounter, uint64_t builtFrame, bool sizeMatches) {
        const uint64_t limit = sizeMatches ? RtSlotIdleFrames : ShadowCasterStaleFrames;
        return (frameCounter > builtFrame) && ((frameCounter - builtFrame) > limit);
    }

    void resolveRegion(const uint32_t rect[4], uint32_t traceWidth, uint32_t traceHeight, uint32_t region[4]) {
        const uint32_t width = (rect[2] > rect[0]) ? (rect[2] - rect[0]) : 0;
        const uint32_t height = (rect[3] > rect[1]) ? (rect[3] - rect[1]) : 0;
        region[0] = std::min(rect[0], traceWidth);
        region[1] = std::min(rect[1], traceHeight);
        region[2] = uint32_t(std::min(uint64_t(rect[0]) + ((uint64_t(width) + 7) / 8) * 8, uint64_t(traceWidth)));
        region[3] = uint32_t(std::min(uint64_t(rect[1]) + ((uint64_t(height) + 7) / 8) * 8, uint64_t(traceHeight)));
    }

    TraceArea traceArea(bool darkenMisses, float outsideDarkness) {
        if (outsideDarkness <= 0.0f) {
            return TraceArea::Receivers;
        }

        return darkenMisses ? TraceArea::FullScreen : TraceArea::Geometry;
    }

    bool crossesNearPlane(const float *clip, size_t vertexCount) {
        bool front = false;
        bool behind = false;
        for (size_t i = 0; i < vertexCount; i++) {
            const float z = clip[i * 4 + 2];
            const float w = clip[i * 4 + 3];
            if (!std::isfinite(z) || !std::isfinite(w)) {
                continue;
            }

            if (w <= 0.0f) {
                continue;
            }

            if (z >= -w) {
                front = true;
            }
            else {
                behind = true;
            }

            if (front && behind) {
                return true;
            }
        }

        return false;
    }

    void drawnViewProj(const float viewProj[16], float projRatioScale, float out[16]) {
        for (int i = 0; i < 16; i++) {
            out[i] = ((i % 4) == 0) ? (viewProj[i] * projRatioScale) : viewProj[i];
        }
    }

    bool pinUnmappedToRealView(bool inMatch, bool generated, bool realCrosses, bool generatedCrosses) {
        return inMatch && generated && (realCrosses || generatedCrosses);
    }

    bool generatedFrame(float curFrameWeight) {
        return curFrameWeight < (1.0f - 1e-4f);
    }

    bool traceRender(bool generated, bool haveTrace, uint32_t reusesSinceTrace) {
        return !generated || !haveTrace || (reusesSinceTrace >= MaxGeneratedReuse);
    }

    bool reusableTrace(bool traced, uint64_t tracedSignature, uint64_t signature, bool tracedDarkened, bool darkens) {
        return traced && (tracedSignature == signature) && (tracedDarkened == darkens);
    }

    bool buildForPass(bool traceEveryFrame, bool generated, bool haveTrace, uint32_t reusesSinceTrace, bool traceTexturesChange) {
        return traceEveryFrame || traceTexturesChange || traceRender(generated, haveTrace, reusesSinceTrace);
    }

    bool traceTexturesChange(const TraceTextures &textures, uint32_t targetWidth, uint32_t targetHeight, float targetScale, uint32_t mode) {
        if (!textures.active || !textures.haveTextures || (textures.outputWidth != targetWidth) || (textures.outputHeight != targetHeight) ||
            (textures.traceScale != targetScale))
        {
            return true;
        }

        // The trace size the textures were made at (recordTrace: the output size times the trace scale, at least 1).
        const uint32_t traceWidth = std::max(uint32_t(std::ceil(textures.outputWidth * textures.traceScale)), 1u);
        const uint32_t traceHeight = std::max(uint32_t(std::ceil(textures.outputHeight * textures.traceScale)), 1u);
        return !textures.haveHistory || (textures.historyWidth != traceWidth) || (textures.historyHeight != traceHeight) || (textures.historyMode != mode);
    }

    uint64_t passSignature(const uint32_t *triangleCounts, size_t count) {
        // FNV-1a over the count and the sorted triangle counts.
        std::vector<uint32_t> sorted(triangleCounts, triangleCounts + count);
        std::sort(sorted.begin(), sorted.end());
        uint64_t hash = 14695981039346656037ULL;
        auto mix = [&hash](uint64_t value) {
            for (uint32_t i = 0; i < 8; i++) {
                hash ^= (value >> (i * 8)) & 0xFF;
                hash *= 1099511628211ULL;
            }
        };

        mix(count);
        for (uint32_t triangles : sorted) {
            mix(triangles);
        }

        return hash;
    }

    AoSettings aoLookSettings(AoLevel level, uint32_t samples) {
        AoSettings settings;
        if (samples == 0) {
            return settings;
        }

        switch (level) {
        // Radii are in AKI world units (a ring is several hundred across, a wrestler about 150 tall). Wrestlers
        // are mostly convex, so their occlusion lives in creases and contacts: the gain brings those out, and
        // a shorter character radius keeps a limb from darkening the whole torso next to it.
        case AoLevel::Subtle:
            settings.samples = samples;
            settings.strength = 0.45f;
            settings.characterStrength = 0.4f;
            settings.radius = 45.0f;
            settings.characterRadius = 25.0f;
            settings.gain = 1.5f;
            settings.characterGain = 1.5f;
            break;
        case AoLevel::Strong:
            settings.samples = samples;
            settings.strength = 0.6f;
            settings.characterStrength = 0.6f;
            settings.radius = 60.0f;
            settings.characterRadius = 30.0f;
            settings.gain = 1.75f;
            settings.characterGain = 1.75f;
            break;
        case AoLevel::Off:
        default:
            break;
        }

        return settings;
    }

    RtPreset rtPreset(uint32_t quality) {
        RtPreset preset;
        switch (RtQuality(std::min(quality, uint32_t(RtQuality::Ultra)))) {
        case RtQuality::Performance:
            preset.scale = 0.5f;
            preset.maxLines = 720.0f;
            preset.samplesPerLight = 1;
            break;
        case RtQuality::Balanced:
            preset.scale = 0.5f;
            preset.maxLines = 1080.0f;
            preset.samplesPerLight = 2;
            preset.ao = AoLevel::Subtle;
            preset.aoSamples = 2;
            break;
        case RtQuality::Quality:
            // The full look: Strong AO. With 4 rays and the separate AO cleanup it reads clean (before that cleanup,
            // Strong mottled the mat next to the wrestlers). Every pixel up to 1440 lines: a full 4K trace cost ~22 ms
            // of GPU per game frame in No Mercy's arenas (38 fps), and 1440 lines brings it back to ~60 (aki-roadmap#84).
            preset.maxLines = 1440.0f;
            preset.samplesPerLight = 2;
            preset.ao = AoLevel::Strong;
            preset.aoSamples = 4;
            break;
        case RtQuality::Ultra:
        default:
            // Quality's look, cleaner: twice its shadow rays and AO rays.
            preset.samplesPerLight = 4;
            preset.ao = AoLevel::Strong;
            preset.aoSamples = 8;
            break;
        }

        return preset;
    }

    float rtTraceScale(const RtPreset &preset, uint32_t targetHeight, uint32_t downsampleMultiplier) {
        float scale = preset.scale;
        if ((preset.maxLines > 0.0f) && (targetHeight > 0)) {
            scale = std::min(scale, preset.maxLines / float(targetHeight));
        }

        if (downsampleMultiplier > 1) {
            scale = std::min(scale, 1.0f / float(downsampleMultiplier));
        }

        return scale;
    }

    float windowResolutionMultiplier(uint32_t windowHeight, uint32_t referenceHeight, uint32_t downsampleMultiplier) {
        float multiplier = 1.0f;
        if ((windowHeight > 0) && (referenceHeight > 0)) {
            multiplier = std::max(float((windowHeight + referenceHeight - 1) / referenceHeight), 1.0f);
        }

        return multiplier * float(std::max(downsampleMultiplier, 1u));
    }

    AoSettings aoSettings(uint32_t quality) {
        const RtPreset preset = rtPreset(quality);
        return aoLookSettings(preset.ao, preset.aoSamples);
    }

    bool aoCycleOn(uint64_t unixMs, uint64_t periodMs) {
        return (periodMs == 0) || (((unixMs / periodMs) % 2) == 0);
    }

    DenoiseSettings denoiseSettings() {
        return DenoiseSettings();
    }

    SheenSettings sheenSettings() {
        SheenSettings settings;
        // Owner pick from live matches: 0.3 / 0.35 barely showed; 0.65 at exponent 8 was too much.
        settings.specular = 0.5f;
        settings.shininess = 12.0f;
        settings.rim = 0.55f;
        settings.rimPower = 3.0f;
        return settings;
    }

    std::array<float, 3> normalizedLightColor(float r, float g, float b) {
        r = std::max(r, 0.0f);
        g = std::max(g, 0.0f);
        b = std::max(b, 0.0f);
        const float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        if (!(luminance > 1e-4f)) {
            return { 1.0f, 1.0f, 1.0f };
        }

        return { std::min(r / luminance, MaxLightChannel), std::min(g / luminance, MaxLightChannel), std::min(b / luminance, MaxLightChannel) };
    }

    std::vector<float> blueNoiseTile(uint32_t size) {
        const uint32_t count = size * size;
        std::vector<float> values(count, 0.0f);
        if ((size == 0) || ((size & (size - 1)) != 0)) {
            return values;
        }

        // Toroidal Gaussian energy kernel (sigma 1.5), indexed by the offset between two cells.
        const uint32_t mask = size - 1;
        std::vector<float> kernel(count);
        for (uint32_t y = 0; y < size; y++) {
            for (uint32_t x = 0; x < size; x++) {
                const float dx = float(std::min(x, size - x));
                const float dy = float(std::min(y, size - y));
                kernel[y * size + x] = std::exp(-(dx * dx + dy * dy) / (2.0f * 1.5f * 1.5f));
            }
        }

        // energy[c] = sum of the kernel over every set cell, kept up to date as cells are set or cleared.
        std::vector<uint8_t> set(count, 0);
        std::vector<float> energy(count, 0.0f);
        auto toggle = [&](uint32_t cell, float sign) {
            set[cell] = (sign > 0.0f) ? 1 : 0;
            const uint32_t cx = cell & mask, cy = cell / size;
            for (uint32_t y = 0; y < size; y++) {
                for (uint32_t x = 0; x < size; x++) {
                    energy[y * size + x] += sign * kernel[((y - cy) & mask) * size + ((x - cx) & mask)];
                }
            }
        };

        // The densest set cell (tightest cluster) or the emptiest unset cell (largest void); ties go to the lowest index.
        auto pick = [&](bool wantSet) {
            uint32_t best = 0;
            bool found = false;
            for (uint32_t c = 0; c < count; c++) {
                if ((set[c] != 0) != wantSet) {
                    continue;
                }

                if (!found || (wantSet ? (energy[c] > energy[best]) : (energy[c] < energy[best]))) {
                    best = c;
                    found = true;
                }
            }

            return best;
        };

        // Initial pattern: about 10% of the cells from a fixed LCG, then relaxed by moving the tightest cluster's
        // cell into the largest void until that stops changing anything.
        uint32_t lcg = 0x2545F491u;
        const uint32_t initialCount = std::max(count / 10, 1u);
        for (uint32_t placed = 0; placed < initialCount;) {
            lcg = lcg * 1664525u + 1013904223u;
            const uint32_t cell = (lcg >> 8) % count;
            if (set[cell] == 0) {
                toggle(cell, 1.0f);
                placed++;
            }
        }

        for (uint32_t i = 0; i < count; i++) {
            const uint32_t cluster = pick(true);
            toggle(cluster, -1.0f);
            const uint32_t voidCell = pick(false);
            if (voidCell == cluster) {
                toggle(cluster, 1.0f);
                break;
            }

            toggle(voidCell, 1.0f);
        }

        const std::vector<uint8_t> initialSet = set;
        const std::vector<float> initialEnergy = energy;
        std::vector<uint32_t> rank(count, 0);

        // Phase 1: remove the tightest clusters one by one; the last cell removed gets rank 0.
        for (uint32_t r = initialCount; r > 0; r--) {
            const uint32_t cluster = pick(true);
            toggle(cluster, -1.0f);
            rank[cluster] = r - 1;
        }

        // Phases 2 and 3: from the initial pattern, fill the largest void until every cell is set. With a kernel
        // that sums to the same total everywhere, the "tightest cluster of unset cells" of phase 3 is the same
        // cell as the largest void, so one loop covers both.
        set = initialSet;
        energy = initialEnergy;
        for (uint32_t r = initialCount; r < count; r++) {
            const uint32_t voidCell = pick(false);
            toggle(voidCell, 1.0f);
            rank[voidCell] = r;
        }

        for (uint32_t c = 0; c < count; c++) {
            values[c] = (float(rank[c]) + 0.5f) / float(count);
        }

        return values;
    }

    BeamDecision decideBeam(uint32_t displayRefreshRate, bool vsync, bool interpolating, uint32_t contentRate) {
        BeamDecision decision;
        if (interpolating && (double(contentRate) > BeamCrtRate) && ((double(displayRefreshRate) / double(contentRate)) >= 1.95)) {
            decision.crtRate = double(contentRate);
        }

        decision.ratio = double(displayRefreshRate) / decision.crtRate;
        if (!vsync) {
            decision.unavailable = BeamUnavailable::NeedsVSync;
        }
        else if (decision.ratio < 1.95) {
            decision.unavailable = BeamUnavailable::RefreshRateTooLow;
        }

        if (decision.unavailable != BeamUnavailable::None) {
            decision.ratio = 0.0;
        }

        return decision;
    }

    std::chrono::steady_clock::time_point evenPresentTarget(std::chrono::steady_clock::time_point frameStart, int i, int framesToPresent, uint32_t viOriginalRate) {
        const auto gameFrame = std::chrono::microseconds(1000000 / viOriginalRate);
        return frameStart + (gameFrame * i) / framesToPresent;
    }

    bool sleepFramePeriodBeforePresent(bool havePreviousPresent, uint32_t targetRate, uint32_t viOriginalRate, bool beamActive) {
        return havePreviousPresent && (targetRate > 0) && (targetRate > viOriginalRate) && !beamActive;
    }

    bool holdPresentsToGameRate(uint32_t displayRate, uint32_t targetRate, uint32_t viOriginalRate, bool beamActive) {
        return (viOriginalRate > 0) && (displayRate > 0) && ((displayRate * 2) >= (viOriginalRate * 3)) && (targetRate <= viOriginalRate) && !beamActive;
    }

    uint32_t GameRatePacer::pacingRate(uint32_t displayRate, uint32_t targetRate, uint32_t viOriginalRate, bool beamActive) {
        const bool paceable = holdPresentsToGameRate(displayRate, targetRate, viOriginalRate, beamActive);
        if (paceable && (viOriginalRate >= pacedRate)) {
            pacedRate = viOriginalRate;
            stalePresents = 0;
            return pacedRate;
        }

        // A missing, too fast or slower reading: keep the paced rate for a few presents.
        if ((pacedRate > 0) && (stalePresents < GameRateDropoutPresents) && holdPresentsToGameRate(displayRate, targetRate, pacedRate, beamActive)) {
            stalePresents++;
            return pacedRate;
        }

        // The change lasted: a new rate, or no pacing.
        if (paceable) {
            pacedRate = viOriginalRate;
            stalePresents = 0;
            return pacedRate;
        }

        reset();
        return 0;
    }

    std::chrono::steady_clock::time_point GameRatePacer::target(std::chrono::steady_clock::time_point now, uint32_t rate) {
        const auto period = std::chrono::microseconds(1000000 / std::max(rate, 1u));
        const auto step = period - std::chrono::microseconds(GameRateSlackUs);
        std::chrono::steady_clock::time_point t = now;
        if (haveGrid && ((grid + step) > now) && (((grid + step) - now) < period)) {
            t = grid + step;
        }

        haveGrid = true;
        grid = t;
        return t;
    }

    void GameRatePacer::reset() {
        haveGrid = false;
        pacedRate = 0;
        stalePresents = 0;
    }

    uint32_t framesPerGameFrame(uint32_t targetRate, uint32_t viOriginalRate, uint32_t capOverride) {
        if (capOverride > 0) {
            return capOverride;
        }

        const uint32_t gameRate = std::max(viOriginalRate, MinInterpolatedGameRate);
        return std::max(2u, (targetRate + gameRate / 2) / gameRate);
    }

    void InterpolationTicks::reset() {
        logical = 0;
        display = 0;
    }

    uint32_t InterpolationTicks::beginGameFrame(uint32_t targetRate, uint32_t viOriginalRate, uint32_t capOverride) {
        logical += targetRate;
        uint32_t frames = uint32_t((logical - display) / viOriginalRate);
        const uint32_t maxFrames = framesPerGameFrame(targetRate, viOriginalRate, capOverride);
        if (frames > maxFrames) {
            display += int64_t(viOriginalRate) * (frames - maxFrames);
            frames = maxFrames;
        }

        return frames;
    }

    FrameWeights InterpolationTicks::advance(uint32_t targetRate, uint32_t viOriginalRate) {
        FrameWeights weights;
        weights.prev = std::clamp((targetRate + display - logical) / float(targetRate), 0.0f, 1.0f);
        display += viOriginalRate;
        weights.cur = std::clamp((targetRate + display - logical) / float(targetRate), 0.0f, 1.0f);
        return weights;
    }

    void InterpolationTicks::skip(uint32_t viOriginalRate, uint32_t frames) {
        display += int64_t(viOriginalRate) * frames;
    }

    // HDR output

    // A full-strength mask passes a third of the light: transmission = 1 - strength * HdrMaskLoss.
    static const float HdrMaskLoss = 2.0f / 3.0f;
    // Below this paper white the picture reads as dim.
    static const float HdrDimNits = 120.0f;

    static float hdrPeakOrDefault(float peakNits) {
        return (peakNits > 0.0f) ? peakNits : HdrUnknownPeakNits;
    }

    static float hdrSdrWhiteOrDefault(const HdrDisplay &display) {
        return (display.sdrWhiteNits > 0.0f) ? display.sdrWhiteNits : HdrUnknownSdrWhiteNits;
    }

    // Headroom (peak / paper white) at which the mask reaches full strength: the preset's transmission over a full
    // mask's. With the beam simulator the mask gets the square root of the headroom, so it needs the square.
    static float hdrFullMaskHeadroom(const HdrCrt &crt) {
        const float base = std::clamp(crt.baseMaskStrength, 0.0f, 1.0f);
        const float needed = (1.0f - base * HdrMaskLoss) / (1.0f - HdrMaskLoss);
        return crt.beam ? (needed * needed) : needed;
    }

    float hdrPaperWhite(HdrPicture picture, float customNits, const HdrDisplay &display, const HdrCrt &crt) {
        const float brighter = hdrSdrWhiteOrDefault(display);
        const float fullMask = hdrPeakOrDefault(display.peakNits) / hdrFullMaskHeadroom(crt);
        const float crtDetail = std::min(std::max(fullMask, HdrCrtDetailFloorNits), brighter);
        float paperWhite = brighter;
        switch (picture) {
        case HdrPicture::Brighter:
            paperWhite = brighter;
            break;
        case HdrPicture::Balanced:
            paperWhite = std::sqrt(brighter * crtDetail);
            break;
        case HdrPicture::CrtDetail:
            paperWhite = crtDetail;
            break;
        case HdrPicture::Custom:
            paperWhite = customNits;
            break;
        case HdrPicture::Auto:
        default:
            paperWhite = crt.on ? std::max(crtDetail, std::min(HdrComfortNits, brighter)) : brighter;
            break;
        }

        return std::clamp(paperWhite, HdrMinPaperWhiteNits, HdrMaxPaperWhiteNits);
    }

    HdrMask hdrMask(float paperWhiteNits, float peakNits, float baseMaskStrength, bool beam) {
        HdrMask mask;
        const float base = std::clamp(baseMaskStrength, 0.0f, 1.0f);
        const float headroom = std::max(hdrPeakOrDefault(peakNits) / std::max(paperWhiteNits, 1.0f), 1.0f);
        const float maskHeadroom = beam ? std::sqrt(headroom) : headroom;
        const float baseTransmission = 1.0f - base * HdrMaskLoss;
        mask.strength = std::clamp((1.0f - baseTransmission / maskHeadroom) / HdrMaskLoss, base, 1.0f);
        mask.gain = baseTransmission / (1.0f - mask.strength * HdrMaskLoss);
        mask.beamScale = beam ? std::clamp(headroom / mask.gain, 1.0f, 4.0f) : 1.0f;
        return mask;
    }

    HdrAdvice hdrAdvice(float paperWhiteNits, const HdrDisplay &display, const HdrCrt &crt) {
        // 5% slack: displays round their full-frame figure.
        if ((display.fullFrameNits > 0.0f) && (paperWhiteNits > display.fullFrameNits * 1.05f)) {
            return HdrAdvice::TooBright;
        }

        if (crt.on) {
            // Less than halfway from the preset's own mask to full strength.
            const float base = std::clamp(crt.baseMaskStrength, 0.0f, 1.0f);
            if (hdrMask(paperWhiteNits, display.peakNits, base, crt.beam).strength < (base + (1.0f - base) * 0.5f)) {
                return HdrAdvice::LowHeadroom;
            }
        }

        if (paperWhiteNits < HdrDimNits) {
            return HdrAdvice::Dim;
        }

        return HdrAdvice::Good;
    }

    bool publishShortRun(uint32_t runLength, uint32_t previousRunLength, uint32_t runBeforePrevious) {
        return (runLength >= previousRunLength) || ((runBeforePrevious > 0) && (runLength >= runBeforePrevious));
    }

    NextLayer nextLayer(bool queued, uint32_t nextColorAddress, bool nextDrawsNothing, uint32_t runColorAddress) {
        if (!queued) {
            return NextLayer::Unknown;
        }

        return (nextDrawsNothing || (nextColorAddress == runColorAddress)) ? NextLayer::SameRun : NextLayer::NewRun;
    }

    bool publishRunAtLayer(uint32_t runLength, uint32_t expectedRunLength, NextLayer next) {
        return (next != NextLayer::SameRun) && (runLength >= expectedRunLength);
    }

    bool dropRunGeneratedFrame(bool layerSkipped, bool cutInRun) {
        return layerSkipped || cutInRun;
    }

    float matrixChange(const float prev[16], const float cur[16]) {
        double num = 0.0, den = 0.0;
        for (int k = 0; k < 16; k++) {
            if (!std::isfinite(prev[k]) || !std::isfinite(cur[k])) {
                return -1.0f;
            }

            num += double(cur[k] - prev[k]) * double(cur[k] - prev[k]);
            den += double(cur[k]) * double(cur[k]);
        }

        return (den > 0.0) ? float(std::sqrt(num / den)) : -1.0f;
    }

    void CameraCutCounter::reset() {
        *this = CameraCutCounter();
    }

    void CameraCutCounter::add(float change) {
        if (change < 0.0f) {
            return;
        }

        pairs++;
        jumped += (change > JumpChange) ? 1 : 0;
        maxChange = std::max(maxChange, change);
    }

    bool CameraCutCounter::cut() const {
        return (pairs >= MinPairs) && (jumped * 2 > pairs);
    }

    void MatrixSplitCounter::reset() {
        *this = MatrixSplitCounter();
    }

    void MatrixSplitCounter::add(bool pairSplit, float drawnChange) {
        pairs++;
        split += pairSplit ? 1 : 0;
        jumped += (drawnChange >= SplitJumpChange) ? 1 : 0;
        extreme += (drawnChange > SplitExtremeChange) ? 1 : 0;
    }

    bool MatrixSplitCounter::splitLayer() const {
        return (extreme > 0) || ((pairs >= MinPairs) && ((split * 2 > pairs) || (jumped * 3 > pairs)));
    }

    bool splitPair(float drawnChange, float stillChange) {
        return (drawnChange > SplitDrawnJump) && (stillChange >= 0.0f) && (stillChange <= SplitStillChange);
    }

    bool primeGeneratedTargets(bool primeEnabled, bool generateInterpolatedFrames, bool usingMSAA, uint32_t runLength, uint32_t generatedLayersBefore) {
        if (!generateInterpolatedFrames || usingMSAA || (generatedLayersBefore > 0)) {
            return false;
        }

        return (runLength == 1) ? primeEnabled : true;
    }

    bool framebufferPassCoversScreen(int32_t scissorWidth, int32_t scissorHeight, int32_t fbWidth, int32_t fbHeight) {
        if ((scissorWidth <= 0) || (scissorHeight <= 0) || (fbWidth <= 0) || (fbHeight <= 0)) {
            return false;
        }

        const float SimilarityPercentage = 0.1f;
        const float scissorRatio = float(scissorWidth) / float(scissorHeight);
        const float fbPixelRatio = float(fbWidth) / float(fbHeight);
        if (std::abs((scissorRatio / fbPixelRatio) - 1.0f) < SimilarityPercentage) {
            return true;
        }

        return (scissorWidth == fbWidth) && (scissorHeight >= fbHeight);
    }

    uint32_t modifiedScreenXYForAspect(uint32_t value, float viewportCenterX, float projRatioScale) {
        if (projRatioScale == 1.0f) {
            return value;
        }

        const float x = float(int16_t((value >> 16) & 0xFFFF)) / 4.0f;
        const float adjusted = viewportCenterX + (x - viewportCenterX) * projRatioScale;
        const long quarters = std::clamp(std::lround(adjusted * 4.0f), long(INT16_MIN), long(INT16_MAX));
        return (uint32_t(uint16_t(int16_t(quarters))) << 16) | (value & 0xFFFF);
    }

    MatrixSlotIdentity::MatrixSlotIdentity(uint32_t stride) : stride(stride) { }

    uint32_t MatrixSlotIdentity::canonical(uint32_t address) {
        if (stride == 0) {
            return address;
        }

        std::lock_guard<std::mutex> lock(identitiesMutex);
        auto it = identities.find(address);
        if (it != identities.end()) {
            return it->second;
        }

        uint32_t id = address;
        auto lower = identities.find(address - stride);
        auto upper = identities.find(address + stride);
        if (lower != identities.end()) {
            id = lower->second;
        }
        else if (upper != identities.end()) {
            id = upper->second;
        }

        identities.emplace(address, id);
        return id;
    }

    std::vector<HudAnchor> hudAnchors(const std::vector<HudBox> &boxes, float screenLeft, float screenRight) {
        const size_t n = boxes.size();
        std::vector<HudAnchor> anchors(n, HudAnchor::Centre);
        const float width = screenRight - screenLeft;
        if ((n == 0) || (n > HudMaxBoxes) || !(width > 0.0f)) {
            return anchors;
        }

        // Group boxes closer than the gap (union-find).
        std::vector<size_t> parent(n);
        for (size_t i = 0; i < n; i++) {
            parent[i] = i;
        }

        auto root = [&](size_t i) {
            while (parent[i] != i) {
                parent[i] = parent[parent[i]];
                i = parent[i];
            }

            return i;
        };

        const float gap = HudElementGap * width;
        for (size_t i = 0; i < n; i++) {
            for (size_t j = i + 1; j < n; j++) {
                const HudBox &a = boxes[i];
                const HudBox &b = boxes[j];
                const bool near = (a.left - gap <= b.right) && (b.left - gap <= a.right) && (a.top - gap <= b.bottom) && (b.top - gap <= a.bottom);
                if (near) {
                    parent[root(i)] = root(j);
                }
            }
        }

        // Each element's horizontal extent.
        std::vector<float> elementLeft(n, 0.0f), elementRight(n, 0.0f);
        std::vector<bool> elementSeen(n, false);
        for (size_t i = 0; i < n; i++) {
            const size_t r = root(i);
            if (!elementSeen[r]) {
                elementSeen[r] = true;
                elementLeft[r] = boxes[i].left;
                elementRight[r] = boxes[i].right;
            }
            else {
                elementLeft[r] = std::min(elementLeft[r], boxes[i].left);
                elementRight[r] = std::max(elementRight[r], boxes[i].right);
            }
        }

        const float centre = (screenLeft + screenRight) / 2.0f;
        for (size_t i = 0; i < n; i++) {
            const size_t r = root(i);
            const float left = elementLeft[r];
            const float right = elementRight[r];
            const bool crossesCentre = (left < centre) && (right > centre);
            const bool nearCentre = std::abs((left + right) / 2.0f - centre) <= (HudCentreBand * width);
            if (crossesCentre || nearCentre) {
                anchors[i] = HudAnchor::Centre;
            }
            else {
                anchors[i] = (right <= centre) ? HudAnchor::Left : HudAnchor::Right;
            }
        }

        return anchors;
    }

    bool hudPlacementActive(float centredWidthFraction, float placement) {
        return (placement > 0.0f) && (centredWidthFraction < 0.999f) && (centredWidthFraction > 0.0f);
    }

    float hudShiftNdc(HudAnchor anchor, float centredWidthFraction, float placement) {
        if (!hudPlacementActive(centredWidthFraction, placement)) {
            return 0.0f;
        }

        const float shift = (1.0f - centredWidthFraction) * std::min(placement, 1.0f);
        switch (anchor) {
        case HudAnchor::Left:
            return -shift;
        case HudAnchor::Right:
            return shift;
        case HudAnchor::Centre:
        default:
            return 0.0f;
        }
    }

    std::string lowMipCacheKey(uint32_t fileSystemIndex, const std::string &relativePath) {
        // '|' can't appear in a pack's file paths.
        return std::to_string(fileSystemIndex) + "|" + relativePath;
    }

    TexDumpWrite texDumpWrite(bool dumpedThisSession, bool packFileExists) {
        if (dumpedThisSession) {
            return TexDumpWrite::Nothing;
        }

        return packFileExists ? TexDumpWrite::SidecarsOnly : TexDumpWrite::All;
    }

    std::wstring extendedLengthPath(const std::wstring &absolutePath) {
        if ((absolutePath.rfind(L"\\\\?\\", 0) == 0) || (absolutePath.rfind(L"\\\\.\\", 0) == 0)) {
            return absolutePath;
        }

        std::wstring path = absolutePath;
        std::replace(path.begin(), path.end(), L'/', L'\\');
        std::wstring prefix;
        size_t rest = 0;
        const bool drive = (path.size() >= 3) && (path[1] == L':') && (path[2] == L'\\') &&
            (((path[0] >= L'A') && (path[0] <= L'Z')) || ((path[0] >= L'a') && (path[0] <= L'z')));
        if (drive) {
            prefix = L"\\\\?\\" + path.substr(0, 2);
            rest = 3;
        }
        else if ((path.size() > 2) && (path[0] == L'\\') && (path[1] == L'\\')) {
            // UNC: \\server\share\... -> \\?\UNC\server\share\...; the server and share stay as the root.
            const size_t server = path.find(L'\\', 2);
            const size_t share = (server == std::wstring::npos) ? std::wstring::npos : path.find(L'\\', server + 1);
            if ((server == std::wstring::npos) || (server == 2)) {
                return absolutePath;
            }

            const size_t rootEnd = (share == std::wstring::npos) ? path.size() : share;
            prefix = L"\\\\?\\UNC\\" + path.substr(2, rootEnd - 2);
            rest = (share == std::wstring::npos) ? path.size() : share + 1;
        }
        else {
            return absolutePath;
        }

        std::vector<std::wstring> parts;
        size_t start = rest;
        while (start <= path.size()) {
            size_t end = path.find(L'\\', start);
            if (end == std::wstring::npos) {
                end = path.size();
            }

            const std::wstring part = path.substr(start, end - start);
            if (part == L"..") {
                if (!parts.empty()) {
                    parts.pop_back();
                }
            }
            else if (!part.empty() && (part != L".")) {
                parts.push_back(part);
            }

            start = end + 1;
        }

        std::wstring result = prefix + L"\\";
        for (size_t i = 0; i < parts.size(); i++) {
            result += (i > 0) ? L"\\" : L"";
            result += parts[i];
        }

        return result;
    }

    std::wstring extendedLengthJoin(const std::wstring &root, const std::wstring &relative) {
        const auto joined = [&relative](const std::wstring &base) {
            const bool separated = base.empty() || (base.back() == L'\\') || (base.back() == L'/');
            return base + (separated ? L"" : L"\\") + relative;
        };

        // Back to the plain form, so extendedLengthPath normalizes the whole joined path and adds the prefix again.
        if (root.rfind(L"\\\\?\\UNC\\", 0) == 0) {
            return extendedLengthPath(joined(L"\\\\" + root.substr(8)));
        }
        else if (root.rfind(L"\\\\?\\", 0) == 0) {
            return extendedLengthPath(joined(root.substr(4)));
        }
        else {
            return joined(root);
        }
    }

    DeviceLostWork deviceLostWork(bool deviceLost) {
        return deviceLost ? DeviceLostWork::SkipGpuWork : DeviceLostWork::Render;
    }

    DeviceLossQuit::Action DeviceLossQuit::update(bool deviceLost, double nowSeconds, bool messageClosed) {
        if (quit || !deviceLost) {
            return Action::None;
        }

        if (!shown) {
            shown = true;
            shownSeconds = nowSeconds;
            return Action::ShowMessage;
        }

        if (messageClosed || ((nowSeconds - shownSeconds) >= DeviceLostMessageSeconds)) {
            quit = true;
            return Action::Quit;
        }

        return Action::None;
    }

    uint32_t regrowSettleMs(uint64_t regrownPixels, int32_t overrideMs) {
        if (regrownPixels < RegrowSettleMinPixels) {
            return 0;
        }

        return (overrideMs >= 0) ? uint32_t(overrideMs) : RegrowSettleDefaultMs;
    }

    // HDR readback for the luminance log and BMP dumps (aki-roadmap#128)

    float halfToFloat(uint16_t half) {
        const uint32_t sign = uint32_t(half & 0x8000) << 16;
        const uint32_t exponent = (half >> 10) & 0x1F;
        const uint32_t mantissa = half & 0x3FF;
        uint32_t bits = sign;
        if (exponent == 0x1F) {
            bits |= 0x7F800000 | (mantissa << 13);
        }
        else if (exponent != 0) {
            bits |= ((exponent + 112) << 23) | (mantissa << 13);
        }
        else if (mantissa != 0) {
            const float value = float(mantissa) / 16777216.0f;
            return (sign != 0) ? -value : value;
        }

        float value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }

    uint8_t linearToSrgb8(float linearValue) {
        // NaN clamps to 0 (std::clamp would keep it).
        const float c = (linearValue > 0.0f) ? std::min(linearValue, 1.0f) : 0.0f;
        const float s = (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * powf(c, 1.0f / 2.4f) - 0.055f);
        return uint8_t(std::lround(s * 255.0f));
    }

    void HdrReadbackConverter::convertRow(const uint16_t *src, uint8_t *dst, uint32_t width, float invScale, double &nitsSum, float &nitsMax) {
        if (halfTable.empty()) {
            halfTable.resize(65536);
            for (uint32_t i = 0; i < 65536; i++) {
                halfTable[i] = halfToFloat(uint16_t(i));
            }
        }

        // Compared by bits: a NaN scale must not rebuild the table every row.
        if (srgbTable.empty() || (memcmp(&srgbTableScale, &invScale, sizeof(float)) != 0)) {
            srgbTable.resize(65536);
            for (uint32_t i = 0; i < 65536; i++) {
                srgbTable[i] = linearToSrgb8(halfTable[i] * invScale);
            }

            srgbTableScale = invScale;
            srgbBuilds++;
        }

        for (uint32_t x = 0; x < width; x++) {
            const uint16_t *pixel = src + uint64_t(x) * 4;
            const float nits = 80.0f * (0.2126f * halfTable[pixel[0]] + 0.7152f * halfTable[pixel[1]] + 0.0722f * halfTable[pixel[2]]);
            nitsSum += nits;
            nitsMax = std::max(nitsMax, nits);
            dst[x * 4 + 0] = srgbTable[pixel[2]];
            dst[x * 4 + 1] = srgbTable[pixel[1]];
            dst[x * 4 + 2] = srgbTable[pixel[0]];
            dst[x * 4 + 3] = 255;
        }
    }

    // In-game Datadog metrics (aki-roadmap#125)

    bool envSwitchOn(const char *value) {
        return (value != nullptr) && (value[0] != '\0') && (strcmp(value, "0") != 0);
    }

    void DdFrameWindow::start(double nowMs) {
        windowStartMs = nowMs;
        presents = generated = repeated = 0;
        intervals.clear();
        gpuRender.clear();
        rtBuild.clear();
        rtTrace.clear();
        gpuPresent.clear();
        frameGpu.clear();
        frameRt.clear();
    }

    void DdFrameWindow::addPresent(double tMs, bool generatedFrame, bool repeatedFrame) {
        if ((lastPresentMs >= 0.0) && (tMs >= lastPresentMs)) {
            intervals.push_back(tMs - lastPresentMs);
        }

        lastPresentMs = tMs;
        presents++;
        generated += generatedFrame ? 1 : 0;
        repeated += repeatedFrame ? 1 : 0;
    }

    void DdFrameWindow::addGpuRender(uint64_t presentId, double gpuMs, double rtBuildMs, double rtTraceMs) {
        if (frameOpen && (presentId != framePresentId)) {
            frameGpu.push_back(frameGpuSum);
            frameRt.push_back(frameRtSum);
            frameGpuSum = frameRtSum = 0.0;
        }

        frameOpen = true;
        framePresentId = presentId;
        frameGpuSum += gpuMs;
        frameRtSum += rtBuildMs + rtTraceMs;
        gpuRender.push_back(gpuMs);
        rtBuild.push_back(rtBuildMs);
        rtTrace.push_back(rtTraceMs);
    }

    void DdFrameWindow::addGpuPresent(double gpuMs) {
        gpuPresent.push_back(gpuMs);
    }

    DdFrameStats DdFrameWindow::take(double nowMs, uint32_t fpsTarget) {
        DdFrameStats s;
        s.seconds = (windowStartMs >= 0.0) ? std::max(0.0, (nowMs - windowStartMs) / 1000.0) : 0.0;
        s.presents = presents;
        s.generated = generated;
        s.repeated = repeated;
        if (s.seconds > 0.0) {
            s.presentsPerS = presents / s.seconds;
            s.interpolatedPerS = generated / s.seconds;
            s.realFps = (presents - generated) / s.seconds;
            s.repeatedPerMin = repeated * 60.0 / s.seconds;
        }

        if (!intervals.empty()) {
            std::vector<double> sorted = intervals;
            std::sort(sorted.begin(), sorted.end());
            s.hasIntervals = true;
            s.p50Ms = ddPercentile(sorted, 0.50);
            s.p95Ms = ddPercentile(sorted, 0.95);
            s.p99Ms = ddPercentile(sorted, 0.99);
            s.maxMs = sorted.back();
            const size_t slowest = std::max<size_t>(1, sorted.size() / 100);
            double sum = 0.0;
            for (size_t i = sorted.size() - slowest; i < sorted.size(); i++) {
                sum += sorted[i];
            }

            const double mean = sum / double(slowest);
            s.low1PctFps = (mean > 0.0) ? (1000.0 / mean) : 0.0;
            const double targetMs = (fpsTarget > 0) ? (1000.0 / fpsTarget) : 0.0;
            const double threshold = DdHitchFactor * std::max(targetMs, s.p50Ms);
            for (double v : sorted) {
                s.hitches += (v > threshold) ? 1 : 0;
            }

            s.hitchesPerMin = (s.seconds > 0.0) ? (s.hitches * 60.0 / s.seconds) : 0.0;
        }

        s.gpuRenderRows = uint32_t(gpuRender.size());
        if (!gpuRender.empty()) {
            s.gpuRenderMs = ddPercentile(gpuRender, 0.50);
            s.gpuRenderP95Ms = ddPercentile(gpuRender, 0.95);
            s.rtBuildMs = ddPercentile(rtBuild, 0.50);
            s.rtTraceMs = ddPercentile(rtTrace, 0.50);
        }

        s.gameFrames = uint32_t(frameGpu.size());
        if (!frameGpu.empty()) {
            s.gpuGameFrameMs = ddPercentile(frameGpu, 0.50);
            s.gpuGameFrameP95Ms = ddPercentile(frameGpu, 0.95);
            s.rtGameFrameMs = ddPercentile(frameRt, 0.50);
        }

        s.gpuPresentRows = uint32_t(gpuPresent.size());
        if (!gpuPresent.empty()) {
            s.gpuPresentMs = ddPercentile(gpuPresent, 0.50);
        }

        start(nowMs);
        return s;
    }

    double ddPercentile(std::vector<double> values, double p) {
        if (values.empty()) {
            return 0.0;
        }

        std::sort(values.begin(), values.end());
        const double rank = std::ceil(std::clamp(p, 0.0, 1.0) * double(values.size()));
        const size_t index = (rank < 1.0) ? 0 : std::min(values.size() - 1, size_t(rank) - 1);
        return values[index];
    }

    uint32_t ddFpsTarget(uint32_t targetRate, uint32_t viOriginalRate) {
        return (targetRate > 0) ? targetRate : viOriginalRate;
    }

    std::vector<std::pair<std::string, double>> ddWindowMetrics(const DdFrameStats &s, const DdProcessSample &process) {
        std::vector<std::pair<std::string, double>> m;
        auto put = [&m](const char *name, double value) { m.emplace_back(std::string("aki.game.") + name, value); };
        put("active", 1.0);
        if (s.seconds > 0.0) {
            put("presents_per_s", s.presentsPerS);
            put("real_fps", s.realFps);
            put("interpolated_per_s", s.interpolatedPerS);
            put("pacing.repeated_per_min", s.repeatedPerMin);
        }

        if (s.hasIntervals) {
            put("pacing.p50_ms", s.p50Ms);
            put("pacing.p95_ms", s.p95Ms);
            put("pacing.p99_ms", s.p99Ms);
            put("pacing.max_ms", s.maxMs);
            put("pacing.low_1pct_fps", s.low1PctFps);
            put("pacing.hitches_per_min", s.hitchesPerMin);
        }

        if (s.gpuRenderRows > 0) {
            put("perf.gpu_render_ms", s.gpuRenderMs);
            put("perf.gpu_render_p95_ms", s.gpuRenderP95Ms);
            // Only with ray tracing running: 0 ms with it off would read as free RT on a dashboard.
            if ((s.rtBuildMs > 0.0) || (s.rtTraceMs > 0.0) || (s.rtGameFrameMs > 0.0)) {
                put("perf.rt_build_ms", s.rtBuildMs);
                put("perf.rt_trace_ms", s.rtTraceMs);
            }
        }

        if (s.gameFrames > 0) {
            put("perf.gpu_game_frame_ms", s.gpuGameFrameMs);
            put("perf.gpu_game_frame_p95_ms", s.gpuGameFrameP95Ms);
            if (s.rtGameFrameMs > 0.0) {
                put("perf.rt_game_frame_ms", s.rtGameFrameMs);
            }
        }

        if (s.gpuPresentRows > 0) {
            put("perf.gpu_present_ms", s.gpuPresentMs);
        }

        if (process.has) {
            put("memory.private_mb", process.privateMb);
            put("memory.working_set_mb", process.workingSetMb);
            put("process.handles", double(process.handles));
        }

        if (process.hasVram) {
            put("memory.vram_mb", process.vramMb);
            put("memory.vram_budget_mb", process.vramBudgetMb);
        }

        return m;
    }

    bool ddWindowDue(double nowMs, double windowStartMs) {
        return (nowMs - windowStartMs) >= (DdWindowSeconds * 1000.0);
    }

    DdVramAdapter ddPickVram(const std::vector<DdVramAdapter> &adapters) {
        DdVramAdapter out;
        bool picked = false;
        uint64_t mostUsage = 0;
        for (const DdVramAdapter &a : adapters) {
            out.usage += a.usage;
            if (!picked || (a.usage > mostUsage)) {
                picked = true;
                mostUsage = a.usage;
                out.budget = a.budget;
            }
        }

        return out;
    }

    bool DdContext::operator==(const DdContext &o) const {
        return (game == o.game) && (fpsTarget == o.fpsTarget) && (framerate == o.framerate) && (rt == o.rt) && (rtQuality == o.rtQuality) &&
            (resolution == o.resolution) && (aspect == o.aspect) && (msaa == o.msaa) && (downsample == o.downsample) && (hdr == o.hdr) &&
            (crt == o.crt) && (beam == o.beam) && (venue == o.venue) && (extraTags == o.extraTags);
    }

    const char *rtQualityName(uint32_t quality) {
        switch (RtQuality(quality)) {
        case RtQuality::Performance:
            return "performance";
        case RtQuality::Balanced:
            return "balanced";
        case RtQuality::Quality:
            return "quality";
        case RtQuality::Ultra:
            return "ultra";
        default:
            return "unknown";
        }
    }

    std::string ddCrtPresetName(const std::string &preset) {
        const size_t slash = preset.find_last_of("/\\");
        std::string name = (slash == std::string::npos) ? preset : preset.substr(slash + 1);
        const size_t dot = name.rfind('.');
        if ((dot != std::string::npos) && (dot > 0)) {
            std::string ext = name.substr(dot);
            for (char &ch : ext) {
                ch = ((ch >= 'A') && (ch <= 'Z')) ? char(ch - 'A' + 'a') : ch;
            }

            if (ext == ".slangp") {
                name.resize(dot);
            }
        }

        return name;
    }

    std::vector<std::string> ddContextTags(const DdContext &c) {
        static const char *builtInKeys[] = { "game", "fps_target", "framerate", "rt", "rt_quality", "resolution", "aspect", "msaa", "downsample",
            "hdr", "crt", "beam", "rt_venue" };
        std::vector<std::string> tags;
        auto add = [&tags](const char *key, const std::string &value) {
            std::string t = ddTag(key, value);
            if (!t.empty() && (std::find(tags.begin(), tags.end(), t) == tags.end())) {
                tags.push_back(std::move(t));
            }
        };

        add("game", c.game);
        add("fps_target", (c.fpsTarget > 0) ? std::to_string(c.fpsTarget) : std::string());
        add("framerate", c.framerate);
        add("rt", c.rt ? "on" : "off");
        if (c.rt) {
            add("rt_quality", rtQualityName(c.rtQuality));
        }

        add("resolution", c.resolution);
        add("aspect", c.aspect);
        add("msaa", c.msaa);
        add("downsample", std::to_string(c.downsample));
        add("hdr", c.hdr ? "on" : "off");
        add("crt", c.crt.empty() ? std::string("off") : ddCrtPresetName(c.crt));
        add("beam", c.beam ? "on" : "off");
        add("rt_venue", c.venue);
        for (const std::string &t : c.extraTags) {
            const std::string key = t.substr(0, t.find(':'));
            bool builtIn = false;
            for (const char *k : builtInKeys) {
                builtIn = builtIn || (key == k);
            }

            if (!builtIn && (std::find(tags.begin(), tags.end(), t) == tags.end())) {
                tags.push_back(t);
            }
        }

        return tags;
    }

    std::string ddTagChanges(const std::vector<std::string> &before, const std::vector<std::string> &after) {
        auto keyOf = [](const std::string &t) { return t.substr(0, t.find(':')); };
        auto find = [&keyOf](const std::vector<std::string> &tags, const std::string &key) -> std::string {
            for (const std::string &t : tags) {
                if (keyOf(t) == key) {
                    return t;
                }
            }

            return key + ":(none)";
        };

        std::vector<std::string> keys;
        for (const std::vector<std::string> *list : { &after, &before }) {
            for (const std::string &t : *list) {
                const std::string k = keyOf(t);
                if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
                    keys.push_back(k);
                }
            }
        }

        std::string out;
        for (const std::string &k : keys) {
            const std::string b = find(before, k);
            const std::string a = find(after, k);
            if (a != b) {
                out += (out.empty() ? "" : ", ") + b + " -> " + a;
            }
        }

        return out;
    }

    void DdSession::setIdentity(const std::string &gameKey, const std::vector<std::string> &extra) {
        game = gameKey;
        extraTags = extra;
    }

    DdSession::Decision DdSession::noteContext(const DdContext &context, double nowMs) {
        Decision d;
        DdContext c = context;
        c.game = game;
        c.extraTags = extraTags;
        c.venue = c.rt ? venue : std::string();
        if (haveContext && (c == current)) {
            return d;
        }

        const std::vector<std::string> before = currentTags;
        current = c;
        currentTags = ddContextTags(c);
        if (!haveContext) {
            haveContext = true;
            startMs = nowMs;
            d.event = Event::SessionStart;
        }
        else if ((nowMs - startMs) >= (DdSettleSeconds * 1000.0)) {
            d.event = Event::SettingsChanged;
            d.changes = ddTagChanges(before, currentTags);
        }

        return d;
    }

    DdSession::Decision DdSession::noteVenue(const std::string &newVenue) {
        Decision d;
        if (newVenue == venue) {
            return d;
        }

        venue = newVenue;
        if (haveContext && current.rt) {
            const std::vector<std::string> before = currentTags;
            current.venue = venue;
            currentTags = ddContextTags(current);
            d.event = Event::VenueChanged;
            d.changes = ddTagChanges(before, currentTags);
        }

        return d;
    }

    std::vector<std::string> DdSession::tags() const {
        if (haveContext) {
            return currentTags;
        }

        const std::string t = ddTag("game", game);
        return t.empty() ? std::vector<std::string>() : std::vector<std::string>{ t };
    }

    static std::string ddLower(std::string s) {
        for (char &c : s) {
            c = ((c >= 'A') && (c <= 'Z')) ? char(c - 'A' + 'a') : c;
        }

        return s;
    }

    static bool ddEndsWith(const std::string &s, const std::string &suffix) {
        return (s.size() >= suffix.size()) && (s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
    }

    std::string ddGameFromExe(const std::string &exeName) {
        const size_t slash = exeName.find_last_of("/\\");
        std::string name = ddLower((slash == std::string::npos) ? exeName : exeName.substr(slash + 1));
        if (ddEndsWith(name, ".exe")) {
            name.resize(name.size() - 4);
        }

        if (ddEndsWith(name, "recompiled")) {
            name.resize(name.size() - 10);
        }

        return (name == "wcw") ? std::string("wt") : name;
    }

    std::vector<std::string> ddParseTags(const char *list) {
        std::vector<std::string> tags;
        if (list == nullptr) {
            return tags;
        }

        std::string all(list);
        size_t pos = 0;
        while (pos <= all.size()) {
            size_t end = all.find(',', pos);
            if (end == std::string::npos) {
                end = all.size();
            }

            const std::string entry = all.substr(pos, end - pos);
            const size_t colon = entry.find(':');
            if (colon != std::string::npos) {
                std::string t = ddTag(entry.substr(0, colon), entry.substr(colon + 1));
                if (!t.empty() && (std::find(tags.begin(), tags.end(), t) == tags.end())) {
                    tags.push_back(std::move(t));
                }
            }

            pos = end + 1;
        }

        return tags;
    }

    // Lowercases, replaces characters outside allowed with '_' and collapses runs of '_'.
    static std::string ddSanitize(const std::string &s, const char *allowedPunct) {
        std::string out;
        for (char c : ddLower(s)) {
            const bool ok = ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '_') || ((c != '\0') && (strchr(allowedPunct, c) != nullptr));
            const char r = ok ? c : '_';
            if ((r == '_') && !out.empty() && (out.back() == '_')) {
                continue;
            }

            out.push_back(r);
        }

        return out;
    }

    static std::string ddTrim(const std::string &s, const char *chars) {
        const size_t a = s.find_first_not_of(chars);
        if (a == std::string::npos) {
            return std::string();
        }

        return s.substr(a, s.find_last_not_of(chars) - a + 1);
    }

    static bool ddStartsWithLetter(const std::string &s) {
        return !s.empty() && (s[0] >= 'a') && (s[0] <= 'z');
    }

    std::string ddMetricName(const std::string &name) {
        std::string n = ddSanitize(name, ".");
        return ddStartsWithLetter(n) ? n : ("aki_" + n);
    }

    std::string ddTag(const std::string &key, const std::string &value) {
        std::string v = ddTrim(value, " \t\r\n");
        if (v.empty()) {
            return std::string();
        }

        std::string k = ddTrim(ddSanitize(key, "-./"), "_");
        if (!ddStartsWithLetter(k)) {
            k = "t_" + k;
        }

        v = ddTrim(ddSanitize(v, "-:./"), "_");
        if (v.empty()) {
            return std::string();
        }

        std::string t = k + ":" + v;
        if (t.size() > 200) {
            t.resize(200);
        }

        return t;
    }

    std::string ddFormatValue(double value) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.6f", value);
        std::string s(buf);
        if (s.find('.') != std::string::npos) {
            while (s.back() == '0') {
                s.pop_back();
            }

            if (s.back() == '.') {
                s.pop_back();
            }
        }

        return (s == "-0") ? std::string("0") : s;
    }

    std::string ddGaugeLine(const std::string &name, double value, const std::vector<std::string> &tags) {
        if (!std::isfinite(value)) {
            return std::string();
        }

        std::string line = ddMetricName(name) + ":" + ddFormatValue(value) + "|g";
        std::string joined;
        for (const std::string &t : tags) {
            if (!t.empty()) {
                joined += (joined.empty() ? "" : ",") + t;
            }
        }

        if (!joined.empty()) {
            line += "|#" + joined;
        }

        return line;
    }

    std::string ddEventLine(const std::string &title, const std::string &text, const std::vector<std::string> &tags,
        const std::string &alertType, const std::string &aggregationKey)
    {
        std::string ti, tx;
        for (char c : title) {
            if (c != '\r') {
                ti.push_back((c == '\n') ? ' ' : c);
            }
        }

        for (char c : text) {
            if (c == '\n') {
                tx += "\\n";
            }
            else if (c != '\r') {
                tx.push_back(c);
            }
        }

        std::string line = "_e{" + std::to_string(ti.size()) + "," + std::to_string(tx.size()) + "}:" + ti + "|" + tx;
        if (!aggregationKey.empty()) {
            line += "|k:" + aggregationKey;
        }

        line += "|s:aki-game";
        if (!alertType.empty()) {
            line += "|t:" + alertType;
        }

        std::string joined;
        for (const std::string &t : tags) {
            if (!t.empty()) {
                joined += (joined.empty() ? "" : ",") + t;
            }
        }

        if (!joined.empty()) {
            line += "|#" + joined;
        }

        return line;
    }

    std::vector<std::string> ddPackets(const std::vector<std::string> &lines, size_t maxBytes) {
        std::vector<std::string> packets;
        std::string cur;
        for (const std::string &l : lines) {
            if (l.empty()) {
                continue;
            }

            if (cur.empty()) {
                cur = l;
            }
            else if (cur.size() + 1 + l.size() <= maxBytes) {
                cur += "\n" + l;
            }
            else {
                packets.push_back(cur);
                cur = l;
            }
        }

        if (!cur.empty()) {
            packets.push_back(cur);
        }

        return packets;
    }
};
