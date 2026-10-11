//
// RT64
//

// [wcw] Unit tests for common/rt64_wcw_logic (frame interpolation, present pacing, CRT beam simulator, ray-traced
// shadows). Built and run as part of every rt64 build (RT64_WCW_TESTS); a failure fails the build. Each test names
// the bug it guards against where there was one.

#include "common/rt64_wcw_logic.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "wcw_test.h"

using namespace RT64::WcwLogic;

// Ray-traced shadows

TEST_CASE("shadow casters stay fresh for 2 game frames, then age out") {
    CHECK(shadowCasterFresh(10, 10));
    CHECK(shadowCasterFresh(11, 10));
    CHECK(shadowCasterFresh(12, 10));
    CHECK(!shadowCasterFresh(13, 10));
    CHECK(!shadowCasterFresh(9, 10));
}

TEST_CASE("shadow casters survive every render of a game frame at 120 fps (shadows vanished at 120)") {
    // At 120 fps every workload renders 4 times per game frame. Freshness is counted in game frames, so the
    // wrestlers' scene (built on frame 100) must still cast shadows in all 4 renders of frames 100 to 102.
    const uint64_t built = 100;
    for (uint64_t frame = 100; frame <= 102; frame++) {
        for (int render = 0; render < 4; render++) {
            CHECK(shadowCasterFresh(frame, built));
        }
    }
}

// CRT beam simulator

TEST_CASE("beam: 120 fps on a 280 Hz display simulates a 120 Hz CRT") {
    const BeamDecision d = decideBeam(280, true, true, 120);
    CHECK(d.unavailable == BeamUnavailable::None);
    CHECK_NEAR(d.crtRate, 120.0, 1e-9);
    CHECK_NEAR(d.ratio, 280.0 / 120.0, 1e-9);
}

TEST_CASE("beam: 60 fps (or no interpolation) keeps a 60 Hz CRT") {
    const BeamDecision smooth = decideBeam(280, true, true, 60);
    CHECK_NEAR(smooth.crtRate, 60.0, 1e-9);
    CHECK_NEAR(smooth.ratio, 280.0 / 60.0, 1e-9);
    const BeamDecision original = decideBeam(280, true, false, 0);
    CHECK_NEAR(original.crtRate, 60.0, 1e-9);
    CHECK_NEAR(original.ratio, 280.0 / 60.0, 1e-9);
}

TEST_CASE("beam: 120 fps on a 144 Hz display falls back to a 60 Hz CRT") {
    // 144 / 120 is under 2 refreshes per frame, so each interpolated frame can't get its own scan.
    const BeamDecision d = decideBeam(144, true, true, 120);
    CHECK(d.unavailable == BeamUnavailable::None);
    CHECK_NEAR(d.crtRate, 60.0, 1e-9);
    CHECK_NEAR(d.ratio, 2.4, 1e-9);
}

TEST_CASE("beam: unavailable without V-Sync or below 120 Hz") {
    const BeamDecision noVsync = decideBeam(280, false, true, 120);
    CHECK(noVsync.unavailable == BeamUnavailable::NeedsVSync);
    CHECK_EQ(noVsync.ratio, 0.0);
    const BeamDecision slow = decideBeam(60, true, false, 0);
    CHECK(slow.unavailable == BeamUnavailable::RefreshRateTooLow);
    CHECK_EQ(slow.ratio, 0.0);
    CHECK(decideBeam(120, true, false, 0).unavailable == BeamUnavailable::None);
}

// Present pacing

// Simulates the present loop's timing for one configuration using the real pacing rules: present 0 of a game frame
// goes out when the frame arrives, later presents wait for their even target, optionally sleep a frame period past
// the previous present, and with the beam simulator wait for the next vblank. Returns the presents per second.
static double simulatePresentsPerSecond(uint32_t refreshRate, uint32_t targetRate, uint32_t viRate, bool beamActive) {
    using namespace std::chrono;
    const auto refresh = duration_cast<steady_clock::duration>(duration<double>(1.0 / refreshRate));
    const auto period = duration_cast<steady_clock::duration>(duration<double>(1.0 / targetRate));
    const auto gameFrame = duration_cast<steady_clock::duration>(duration<double>(1.0 / viRate));
    const steady_clock::time_point epoch{};
    auto nextVBlank = [&](steady_clock::time_point t) {
        const auto n = (t - epoch + refresh - steady_clock::duration(1)) / refresh;
        return epoch + refresh * n;
    };

    const int framesToPresent = int(framesPerGameFrame(targetRate, viRate, 0));
    const int gameFrames = 300;
    bool havePrevious = false;
    steady_clock::time_point previous = epoch;
    int presents = 0;
    for (int f = 0; f < gameFrames; f++) {
        // A game frame arrives every gameFrame; when the presents fall behind, its first present waits for them.
        steady_clock::time_point frameStart = std::max(epoch + gameFrame * f, previous);
        for (int i = 0; i < framesToPresent; i++) {
            steady_clock::time_point t = (i == 0) ? frameStart : std::max(evenPresentTarget(frameStart, i, framesToPresent, viRate), previous);
            if (sleepFramePeriodBeforePresent(havePrevious, targetRate, viRate, beamActive)) {
                t = std::max(t, previous + period);
            }

            if (beamActive) {
                t = nextVBlank(t);
            }

            // A game frame whose presents run past the next game frame's arrival loses the rest (the present
            // thread moves on to the newer frame).
            if ((i > 0) && (t >= epoch + gameFrame * (f + 1))) {
                break;
            }

            previous = t;
            havePrevious = true;
            presents++;
        }
    }

    return presents / (duration<double>(gameFrame).count() * gameFrames);
}

TEST_CASE("pacing: 120 fps with the beam simulator on a 280 Hz display keeps 120 presents/s (it ran at ~92)") {
    CHECK_NEAR(simulatePresentsPerSecond(280, 120, 30, true), 120.0, 3.0);
}

TEST_CASE("pacing: 60 fps with the beam simulator on a 280 Hz display keeps 60 presents/s") {
    CHECK_NEAR(simulatePresentsPerSecond(280, 60, 30, true), 60.0, 1.5);
}

TEST_CASE("pacing: without the beam simulator 120 and 60 fps keep their rates") {
    CHECK_NEAR(simulatePresentsPerSecond(280, 120, 30, false), 120.0, 3.0);
    CHECK_NEAR(simulatePresentsPerSecond(280, 60, 30, false), 60.0, 1.5);
}

TEST_CASE("pacing: presents are spread evenly over the game frame") {
    const std::chrono::steady_clock::time_point start{};
    const auto gameFrame = std::chrono::microseconds(1000000 / 30);
    for (int i = 0; i < 4; i++) {
        CHECK((evenPresentTarget(start, i, 4, 30) - start) == (gameFrame * i) / 4);
    }

    CHECK(!sleepFramePeriodBeforePresent(false, 120, 30, false));
    CHECK(!sleepFramePeriodBeforePresent(true, 0, 30, false));
    CHECK(!sleepFramePeriodBeforePresent(true, 30, 30, false));
    CHECK(sleepFramePeriodBeforePresent(true, 120, 30, false));
    CHECK(!sleepFramePeriodBeforePresent(true, 120, 30, true));
}

// Frame interpolation

TEST_CASE("interpolation: frames per game frame") {
    CHECK_EQ(framesPerGameFrame(60, 30, 0), 2u);
    CHECK_EQ(framesPerGameFrame(120, 30, 0), 4u);
    CHECK_EQ(framesPerGameFrame(144, 30, 0), 5u);
    CHECK_EQ(framesPerGameFrame(30, 30, 0), 2u);
    CHECK_EQ(framesPerGameFrame(120, 30, 3), 3u);
}

static void checkIntegerMultipleWeights(uint32_t targetRate, uint32_t viRate) {
    InterpolationTicks ticks;
    const uint32_t n = targetRate / viRate;
    for (int gameFrame = 0; gameFrame < 50; gameFrame++) {
        CHECK_EQ(ticks.beginGameFrame(targetRate, viRate, 0), n);
        for (uint32_t i = 0; i < n; i++) {
            const FrameWeights w = ticks.advance(targetRate, viRate);
            CHECK_NEAR(w.prev, float(i) / n, 1e-6);
            CHECK_NEAR(w.cur, float(i + 1) / n, 1e-6);
        }

        CHECK(ticks.logical >= ticks.display);
    }
}

TEST_CASE("interpolation: 60 fps shows a blend and the real frame per game frame") {
    checkIntegerMultipleWeights(60, 30);
}

TEST_CASE("interpolation: 120 fps shows three blends and the real frame per game frame") {
    // The last displayed frame of each game frame has weight 1: the real frame, not a blend.
    checkIntegerMultipleWeights(120, 30);
}

TEST_CASE("interpolation: 144 fps alternates 4 and 5 frames and keeps pace with the game") {
    InterpolationTicks ticks;
    uint32_t total = 0;
    const int gameFrames = 300;
    for (int f = 0; f < gameFrames; f++) {
        const uint32_t frames = ticks.beginGameFrame(144, 30, 0);
        CHECK((frames == 4) || (frames == 5));
        float lastCur = -1.0f;
        for (uint32_t i = 0; i < frames; i++) {
            const FrameWeights w = ticks.advance(144, 30);
            CHECK((w.prev >= 0.0f) && (w.cur <= 1.0f) && (w.prev < w.cur));
            CHECK(w.cur > lastCur);
            lastCur = w.cur;
        }

        CHECK((ticks.logical - ticks.display) <= int64_t(144 + 30));
        total += frames;
    }

    CHECK_NEAR(double(total) / (gameFrames / 30.0), 144.0, 1.0);
}

TEST_CASE("interpolation: skipped frames advance the display ticks") {
    InterpolationTicks ticks;
    CHECK_EQ(ticks.beginGameFrame(120, 30, 0), 4u);
    ticks.advance(120, 30);
    ticks.skip(30, 3);
    CHECK_EQ(ticks.display, int64_t(120));
    CHECK_EQ(ticks.beginGameFrame(120, 30, 0), 4u);
    ticks.reset();
    CHECK_EQ(ticks.logical, int64_t(0));
    CHECK_EQ(ticks.display, int64_t(0));
}

TEST_CASE("priming: only the first layer of a run primes (Revenge flicker)") {
    CHECK(primeGeneratedTargets(true, true, false, 1));
    CHECK(!primeGeneratedTargets(true, true, false, 2));
    CHECK(!primeGeneratedTargets(true, true, false, 3));
    CHECK(!primeGeneratedTargets(false, true, false, 1));
    CHECK(!primeGeneratedTargets(true, false, false, 1));
    CHECK(!primeGeneratedTargets(true, true, true, 1));
}

TEST_CASE("matrix slots: double-buffered slots 0x40 apart share one identity") {
    MatrixSlotIdentity identity(0x40);
    const uint32_t a = identity.canonical(0x80101000);
    CHECK_EQ(a, 0x80101000u);
    CHECK_EQ(identity.canonical(0x80101040), a);
    CHECK_EQ(identity.canonical(0x80101000), a);
    CHECK_EQ(identity.canonical(0x80200000), 0x80200000u);
    // The partner can be either side: a slot 0x40 below a known one pairs with it too.
    CHECK_EQ(identity.canonical(0x801FFFC0), 0x80200000u);
    CHECK_EQ(identity.canonical(0x80300000), 0x80300000u);
}

TEST_CASE("matrix slots: a stride of 0 disables the pairing") {
    MatrixSlotIdentity identity(0);
    CHECK_EQ(identity.canonical(0x80101000), 0x80101000u);
    CHECK_EQ(identity.canonical(0x80101040), 0x80101040u);
}

int main() {
    return WcwTest::runAll("rt64 wcw tests");
}
