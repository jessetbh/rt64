//
// RT64
//

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// [wcw] Decision logic for frame interpolation, present pacing, the CRT beam simulator and ray-traced shadows, kept
// free of renderer state so it can be unit tested (tests/wcw). The renderer calls these functions; a change to any
// of these rules belongs here, with a test.
namespace RT64::WcwLogic {
    // Ray-traced shadows

    // Other scenes stay shadow casters for this many game frames after their last build. Counted in game frames,
    // not workloads: frame interpolation renders every workload once per displayed frame (4 times per game frame
    // at 120 fps), which aged the wrestlers' scene out before the floor's pass used it.
    constexpr uint64_t ShadowCasterStaleFrames = 2;

    // Whether a scene last built on game frame builtFrame still casts shadows on game frame frameCounter.
    bool shadowCasterFresh(uint64_t frameCounter, uint64_t builtFrame);

    // CRT beam simulator

    // The simulated CRT's refresh rate without frame interpolation.
    constexpr double BeamCrtRate = 60.0;

    enum class BeamUnavailable {
        None,
        NeedsVSync,
        RefreshRateTooLow
    };

    struct BeamDecision {
        // The simulated CRT's refresh rate.
        double crtRate = BeamCrtRate;
        // Display refreshes per simulated CRT refresh; 0 when the beam simulator can't run.
        double ratio = 0.0;
        BeamUnavailable unavailable = BeamUnavailable::None;
    };

    // With frame interpolation above 60 fps (e.g. 120), the simulated CRT refreshes at that rate when the display has
    // at least two refreshes per frame, so every interpolated frame gets a scan of its own. Otherwise it stays at
    // 60 Hz and each scan shows the latest frame. contentRate is the interpolated framerate (0 without interpolation).
    BeamDecision decideBeam(uint32_t displayRefreshRate, bool vsync, bool interpolating, uint32_t contentRate);

    // Present pacing

    // The time present i of a game frame's framesToPresent presents is due: spread evenly over the game frame.
    std::chrono::steady_clock::time_point evenPresentTarget(std::chrono::steady_clock::time_point frameStart, int i, int framesToPresent, uint32_t viOriginalRate);

    // Whether a present first sleeps one frame period (at targetRate) past the previous present. Not with the beam
    // simulator: its presents are paced to their even targets and then wait for a vblank, and sleeping a full
    // period first rounded every 120 fps frame up to 3 refreshes at 280 Hz (presents fell to ~92/s).
    bool sleepFramePeriodBeforePresent(bool havePreviousPresent, uint32_t targetRate, uint32_t viOriginalRate, bool beamActive);

    // Frame interpolation

    // Displayed frames per game frame: display rate / game rate, rounded, at least 2 (2 for a 30 fps game at 60,
    // 4 at 120). capOverride (WCW_HFR_CAP) replaces it when non-zero.
    uint32_t framesPerGameFrame(uint32_t targetRate, uint32_t viOriginalRate, uint32_t capOverride);

    struct FrameWeights {
        float prev = 0.0f;
        float cur = 1.0f;
    };

    // Tick accounting that turns a display rate and a game rate into displayed frames and interpolation weights.
    // Each game frame adds targetRate logical ticks; each displayed frame adds viOriginalRate display ticks.
    struct InterpolationTicks {
        int64_t logical = 0;
        int64_t display = 0;

        void reset();

        // Starts a game frame and returns how many frames to display for it (capped at framesPerGameFrame).
        uint32_t beginGameFrame(uint32_t targetRate, uint32_t viOriginalRate, uint32_t capOverride);

        // Advances one displayed frame and returns its weights. The game frame's last displayed frame has a
        // current weight of 1: the real frame.
        FrameWeights advance(uint32_t targetRate, uint32_t viOriginalRate);

        // Skips displayed frames (not rendered).
        void skip(uint32_t viOriginalRate, uint32_t frames);
    };

    // Generated-frame priming: copying the game's buffer into a run's generated targets happens once per run, before
    // its first layer draws the blended frame. Priming after the blend applied translucent full-screen layers
    // (fades) twice; priming every layer replaced the earlier layers' draws with the blend (Revenge flicker).
    bool primeGeneratedTargets(bool primeEnabled, bool generateInterpolatedFrames, bool usingMSAA, uint32_t runLength);

    // Matrix identity across frames

    // AKI engines double-buffer their uploaded matrices: an object's matrix alternates between two slots 'stride'
    // bytes apart from frame to frame. Both slots get one identity: an address whose partner (address +/- stride) was
    // already seen takes the partner's identity. A stride of 0 disables the pairing.
    class MatrixSlotIdentity {
    public:
        explicit MatrixSlotIdentity(uint32_t stride);
        uint32_t canonical(uint32_t address);

    private:
        uint32_t stride;
        std::unordered_map<uint32_t, uint32_t> identities;
        std::mutex identitiesMutex;
    };
};
