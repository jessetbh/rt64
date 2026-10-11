//
// RT64
//

#include "rt64_wcw_logic.h"

#include <algorithm>

namespace RT64::WcwLogic {
    bool shadowCasterFresh(uint64_t frameCounter, uint64_t builtFrame) {
        return (frameCounter >= builtFrame) && ((frameCounter - builtFrame) <= ShadowCasterStaleFrames);
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
