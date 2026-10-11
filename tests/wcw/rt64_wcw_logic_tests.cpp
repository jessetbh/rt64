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

// Ray-traced ambient occlusion

TEST_CASE("ao: Off traces no occlusion rays") {
    for (uint32_t samples : { 0u, 2u, 8u }) {
        const AoSettings off = aoLookSettings(AoLevel::Off, samples);
        CHECK_EQ(off.samples, 0u);
        CHECK_EQ(off.strength, 0.0f);
        CHECK_EQ(off.characterStrength, 0.0f);
    }

    CHECK_EQ(aoLookSettings(AoLevel::Subtle, 0).samples, 0u);
}

TEST_CASE("ao: Strong is stronger and reaches further than Subtle") {
    const AoSettings subtle = aoLookSettings(AoLevel::Subtle, 2);
    const AoSettings strong = aoLookSettings(AoLevel::Strong, 2);
    CHECK(strong.strength > subtle.strength);
    CHECK(strong.characterStrength > subtle.characterStrength);
    CHECK(strong.radius > subtle.radius);
    CHECK(strong.characterRadius >= subtle.characterRadius);
    CHECK(strong.gain > subtle.gain);
    CHECK((strong.strength <= 1.0f) && (strong.characterStrength <= 1.0f));
}

TEST_CASE("ao: the ray count doesn't change the look") {
    for (AoLevel level : { AoLevel::Subtle, AoLevel::Strong }) {
        const AoSettings few = aoLookSettings(level, 1);
        const AoSettings many = aoLookSettings(level, 8);
        CHECK_EQ(few.samples, 1u);
        CHECK_EQ(many.samples, 8u);
        CHECK_EQ(few.strength, many.strength);
        CHECK_EQ(few.radius, many.radius);
        CHECK_EQ(few.characterRadius, many.characterRadius);
        CHECK_EQ(few.gain, many.gain);
    }
}

TEST_CASE("ao: wrestlers aren't halved any more (the old 0.5x left their AO invisible)") {
    for (AoLevel level : { AoLevel::Subtle, AoLevel::Strong }) {
        const AoSettings settings = aoLookSettings(level, 2);
        CHECK(settings.characterStrength >= 0.8f * settings.strength);
        CHECK(settings.characterRadius > 0.0f);
        CHECK(settings.characterRadius <= settings.radius);
    }
}

TEST_CASE("ao: reach suits the AKI world scale and contrast stays below the blotchy range") {
    // A 25-40 unit reach only caught surfaces that almost touch (rings are several hundred units across);
    // a gain of 3 turned sampling noise into dark blotches on the wrestlers.
    for (AoLevel level : { AoLevel::Subtle, AoLevel::Strong }) {
        const AoSettings settings = aoLookSettings(level, 2);
        CHECK(settings.radius >= 40.0f);
        CHECK((settings.gain >= 1.0f) && (settings.gain <= 2.5f));
    }
}

TEST_CASE("ao: wrestlers stay below the muddy range at the normal camera distance") {
    // Owner review of a CPU match: wrestler strength 0.85 with gain 2.5 and a 45-unit reach darkened whole bodies
    // instead of creases, and an 80-unit world reach left grime along the ring edges. Keep the AO in creases and
    // contacts: moderate strength and gain, short reach.
    for (AoLevel level : { AoLevel::Subtle, AoLevel::Strong }) {
        const AoSettings settings = aoLookSettings(level, 2);
        CHECK(settings.characterStrength <= 0.65f);
        CHECK(settings.characterGain <= 2.0f);
        CHECK(settings.characterRadius <= 35.0f);
        CHECK(settings.radius <= 65.0f);
    }
}

TEST_CASE("ao cycle: alternates every period on wall-clock boundaries, on first") {
    CHECK(aoCycleOn(0, 2000));
    CHECK(aoCycleOn(1999, 2000));
    CHECK(!aoCycleOn(2000, 2000));
    CHECK(!aoCycleOn(3999, 2000));
    CHECK(aoCycleOn(4000, 2000));
    CHECK(aoCycleOn(123456789, 0));
}

// Ray tracing quality presets (the AO menu was folded into them)

TEST_CASE("quality: AO comes with the level (none on Performance, Subtle on Balanced and Quality, Strong on Ultra)") {
    CHECK_EQ(aoSettings(uint32_t(RtQuality::Performance)).samples, 0u);
    const AoSettings balanced = aoSettings(uint32_t(RtQuality::Balanced));
    const AoSettings quality = aoSettings(uint32_t(RtQuality::Quality));
    const AoSettings ultra = aoSettings(uint32_t(RtQuality::Ultra));
    CHECK(balanced.samples > 0);
    CHECK_EQ(balanced.strength, aoLookSettings(AoLevel::Subtle, 2).strength);
    CHECK_EQ(quality.strength, balanced.strength);
    CHECK_EQ(ultra.strength, aoLookSettings(AoLevel::Strong, 2).strength);
}

TEST_CASE("quality: each level traces at least as many rays as the one below") {
    for (uint32_t q = 1; q <= uint32_t(RtQuality::Ultra); q++) {
        const RtPreset lower = rtPreset(q - 1);
        const RtPreset higher = rtPreset(q);
        CHECK(higher.samplesPerLight >= lower.samplesPerLight);
        CHECK(higher.aoSamples >= lower.aoSamples);
        CHECK(rtTraceScale(higher, 2160) >= rtTraceScale(lower, 2160));
    }
}

TEST_CASE("quality: Ultra traces every pixel with more rays than Quality") {
    const RtPreset quality = rtPreset(uint32_t(RtQuality::Quality));
    const RtPreset ultra = rtPreset(uint32_t(RtQuality::Ultra));
    CHECK_EQ(rtTraceScale(ultra, 2160), 1.0f);
    CHECK(ultra.samplesPerLight > quality.samplesPerLight);
    CHECK(ultra.aoSamples > quality.aoSamples);
}

TEST_CASE("quality: trace resolution at 1080p and 4K") {
    // Performance caps at 720 lines and Balanced at 1080, both at most half resolution; Quality and Ultra are full.
    CHECK_NEAR(rtTraceScale(rtPreset(0), 1080), 0.5f, 1e-6f);
    CHECK_NEAR(rtTraceScale(rtPreset(1), 1080), 0.5f, 1e-6f);
    CHECK_NEAR(rtTraceScale(rtPreset(0), 2160), 720.0f / 2160.0f, 1e-6f);
    CHECK_NEAR(rtTraceScale(rtPreset(1), 2160), 0.5f, 1e-6f);
    CHECK_EQ(rtTraceScale(rtPreset(2), 2160), 1.0f);
}

TEST_CASE("quality: out-of-range levels clamp to Ultra") {
    CHECK_EQ(rtPreset(9).samplesPerLight, rtPreset(uint32_t(RtQuality::Ultra)).samplesPerLight);
    CHECK_EQ(aoSettings(9).samples, aoSettings(uint32_t(RtQuality::Ultra)).samples);
}

// Resolve: separate shadow and AO cleanup

TEST_CASE("denoise: shadows get a tighter filter than AO (crisp shadow edges, smooth AO)") {
    const DenoiseSettings d = denoiseSettings();
    CHECK(d.shadowRadius < d.aoRadius);
    CHECK(d.shadowRadius >= 1);
    CHECK(d.aoRadius <= DenoiseMaxRadius);
}

TEST_CASE("denoise: AO accumulates longer than shadows, and both keep some history") {
    const DenoiseSettings d = denoiseSettings();
    CHECK(d.aoAlpha <= d.shadowAlpha);
    CHECK(d.aoAlpha > 0.0f && d.shadowAlpha < 1.0f);
}

// Blue noise (ray tracer sample jitter)

static const std::vector<float> &blueTile() {
    static const std::vector<float> tile = blueNoiseTile(BlueNoiseSize);
    return tile;
}

TEST_CASE("blue noise: every rank appears exactly once") {
    const std::vector<float> &tile = blueTile();
    const uint32_t count = BlueNoiseSize * BlueNoiseSize;
    CHECK_EQ(tile.size(), size_t(count));
    std::vector<int> seen(count, 0);
    for (float v : tile) {
        CHECK(v > 0.0f && v < 1.0f);
        const int rank = int(std::lround(v * float(count) - 0.5f));
        CHECK(rank >= 0 && rank < int(count));
        if (rank >= 0 && rank < int(count)) {
            seen[rank]++;
        }
    }

    CHECK(std::all_of(seen.begin(), seen.end(), [](int n) { return n == 1; }));
}

TEST_CASE("blue noise: the same size always gives the same tile") {
    CHECK(blueNoiseTile(BlueNoiseSize) == blueTile());
}

TEST_CASE("blue noise: sizes that aren't powers of two give an all-zero tile") {
    const std::vector<float> tile = blueNoiseTile(48);
    CHECK_EQ(tile.size(), size_t(48 * 48));
    CHECK(std::all_of(tile.begin(), tile.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("blue noise: 4x4 neighbourhoods average close to 0.5 (no low-frequency blotches)") {
    // White noise: the mean of 16 uniform values has a standard deviation of about 0.072. Blotchy noise is what
    // the old interleaved gradient noise turned into after filtering; blue noise keeps every block near 0.5.
    const std::vector<float> &tile = blueTile();
    double sumSq = 0.0, worst = 0.0;
    int blocks = 0;
    for (uint32_t by = 0; by < BlueNoiseSize; by += 4) {
        for (uint32_t bx = 0; bx < BlueNoiseSize; bx += 4) {
            double mean = 0.0;
            for (uint32_t y = 0; y < 4; y++) {
                for (uint32_t x = 0; x < 4; x++) {
                    mean += tile[(by + y) * BlueNoiseSize + bx + x];
                }
            }

            mean /= 16.0;
            sumSq += (mean - 0.5) * (mean - 0.5);
            worst = std::max(worst, std::abs(mean - 0.5));
            blocks++;
        }
    }

    CHECK(std::sqrt(sumSq / blocks) < 0.04);
    CHECK(worst < 0.15);
}

TEST_CASE("blue noise: the lowest 10% of values never sit next to each other") {
    // Any threshold of a blue noise tile is an evenly spread point set: at 10% density no two points touch,
    // diagonals included, even across the tile's wrap-around edges.
    const std::vector<float> &tile = blueTile();
    const int n = int(BlueNoiseSize);
    std::vector<std::pair<int, int>> points;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            if (tile[y * n + x] < 0.1f) {
                points.emplace_back(x, y);
            }
        }
    }

    int touching = 0;
    for (size_t i = 0; i < points.size(); i++) {
        for (size_t j = i + 1; j < points.size(); j++) {
            int dx = std::abs(points[i].first - points[j].first), dy = std::abs(points[i].second - points[j].second);
            dx = std::min(dx, n - dx);
            dy = std::min(dy, n - dy);
            if ((dx <= 1) && (dy <= 1)) {
                touching++;
            }
        }
    }

    CHECK_EQ(touching, 0);
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
