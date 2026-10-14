//
// RT64
//

#include "rt64_wcw_logic.h"

#include <algorithm>
#include <cmath>

namespace RT64::WcwLogic {
    bool shadowCasterFresh(uint64_t frameCounter, uint64_t builtFrame) {
        return (frameCounter >= builtFrame) && ((frameCounter - builtFrame) <= ShadowCasterStaleFrames);
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
            // Strong mottled the mat next to the wrestlers).
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

    uint32_t framesPerGameFrame(uint32_t targetRate, uint32_t viOriginalRate, uint32_t capOverride) {
        if (capOverride > 0) {
            return capOverride;
        }

        return std::max(2u, (targetRate + viOriginalRate / 2) / viOriginalRate);
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

    bool primeGeneratedTargets(bool primeEnabled, bool generateInterpolatedFrames, bool usingMSAA, uint32_t runLength) {
        return primeEnabled && generateInterpolatedFrames && !usingMSAA && (runLength == 1);
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
};
