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

// Multi-layer runs that end early

// Replays a sequence of run lengths through the workload queue's publishing: a run's generated frame waits for
// the longest of the last 8 runs; a run that ends shorter is published when the next run starts, if
// publishShortRun allows. Returns, per run, whether its generated frame was shown.
static std::vector<bool> replayRuns(const std::vector<uint32_t> &lengths) {
    std::vector<bool> shown;
    std::vector<uint32_t> history;
    uint32_t previous = 0;
    for (uint32_t length : lengths) {
        uint32_t expected = 1;
        for (size_t i = (history.size() > 8) ? history.size() - 8 : 0; i < history.size(); i++) {
            expected = std::max(expected, history[i]);
        }

        shown.push_back((length >= expected) || publishShortRun(length, previous));
        history.push_back(length);
        previous = length;
    }

    return shown;
}

TEST_CASE("short runs: a run cut short after longer runs drops its generated frame (Revenge attract flashes)") {
    // Run lengths from Revenge's attract mode (WCW_RUN_LOG, 60 fps). Both cut-short runs flashed in the flicker
    // log: a 1-layer run after 3-layer runs (generated 55.9 against real 48.6) and a 3-layer run after a 5-layer
    // one (generated 6.4 against real 8.2).
    const std::vector<bool> a = replayRuns({ 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1 });
    CHECK(!a[8]);
    const std::vector<bool> b = replayRuns({ 3, 3, 3, 3, 3, 3, 3, 5, 3, 3, 3, 3 });
    CHECK(!b[8]);
}

TEST_CASE("short runs: a scene's steady layer count keeps its generated frames") {
    // After the first cut-short run, the scene keeps 1 (or 3) layers: those runs match the previous one and are
    // shown, though still shorter than the recent maximum.
    const std::vector<bool> a = replayRuns({ 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1 });
    CHECK(a[9] && a[10] && a[11]);
    const std::vector<bool> b = replayRuns({ 3, 3, 3, 3, 3, 3, 3, 5, 3, 3, 3, 3 });
    CHECK(b[9] && b[10] && b[11]);
    for (bool s : replayRuns({ 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 })) {
        CHECK(s);
    }
}

TEST_CASE("short runs: publishShortRun") {
    CHECK(!publishShortRun(1, 3));
    CHECK(!publishShortRun(3, 5));
    CHECK(publishShortRun(3, 3));
    CHECK(publishShortRun(4, 3));
    // No previous run yet.
    CHECK(publishShortRun(1, 0));
}

TEST_CASE("trace area: only the pass that darkens the misses traces full screen (No Mercy arenas ran at ~7 fps)") {
    // A No Mercy arena frame with outside darkness: the arena pass darkens the misses, the ring, wrestler and
    // small passes don't, and must stay within their geometry instead of tracing the whole target.
    CHECK(traceArea(true, 0.55f) == TraceArea::FullScreen);
    CHECK(traceArea(false, 0.55f) == TraceArea::Geometry);
}

TEST_CASE("trace area: without outside darkness, passes trace only their receivers") {
    CHECK(traceArea(false, 0.0f) == TraceArea::Receivers);
    // darkenMisses alone (no darkness to apply) doesn't widen the pass.
    CHECK(traceArea(true, 0.0f) == TraceArea::Receivers);
}

TEST_CASE("generated frames: the real frame (current weight 1) is not generated, interpolated ones are") {
    CHECK(!generatedFrame(1.0f));
    CHECK(generatedFrame(0.5f));
    CHECK(generatedFrame(0.25f));
    CHECK(generatedFrame(0.75f));
    CHECK(generatedFrame(0.0f));
}

TEST_CASE("generated frames: only the real frame traces at 60 and 120 fps (every displayed frame traced: ~6 fps)") {
    // Replays a pass through the interpolation weights of a few game frames and counts its traces.
    for (uint32_t targetRate : { 60u, 120u }) {
        InterpolationTicks ticks;
        bool haveTrace = false;
        uint32_t reuses = 0, traces = 0, displayed = 0;
        const uint32_t gameFrames = 10;
        for (uint32_t g = 0; g < gameFrames; g++) {
            const uint32_t frames = ticks.beginGameFrame(targetRate, 30, 0);
            for (uint32_t f = 0; f < frames; f++) {
                const FrameWeights weights = ticks.advance(targetRate, 30);
                displayed++;
                if (traceRender(generatedFrame(weights.cur), haveTrace, reuses)) {
                    traces++;
                    haveTrace = true;
                    reuses = 0;
                }
                else {
                    reuses++;
                }
            }
        }

        CHECK_EQ(displayed, gameFrames * (targetRate / 30));
        // One trace per game frame; the very first generated frame traces too, having nothing to reuse.
        CHECK(traces <= gameFrames + 1);
        CHECK(traces >= gameFrames);
    }
}

TEST_CASE("generated frames: a pass with no trace yet traces, even on a generated frame") {
    CHECK(traceRender(true, false, 0));
    CHECK(traceRender(false, false, 0));
    CHECK(traceRender(false, true, 0));
    CHECK(!traceRender(true, true, 0));
}

TEST_CASE("generated frames: shadows don't freeze when real frames are skipped under load") {
    // 120 fps: 3 generated frames between real ones reuse the trace; a 4th in a row (the real frame was skipped)
    // traces again.
    CHECK(!traceRender(true, true, 1));
    CHECK(!traceRender(true, true, 2));
    CHECK(traceRender(true, true, MaxGeneratedReuse));
    CHECK(traceRender(true, true, MaxGeneratedReuse + 5));
}

TEST_CASE("generated frames: two passes sharing a slot don't reuse each other's trace (generated frames ~5% darker)") {
    // Two No Mercy passes of similar scenes map to one slot. On the real frame each traces in turn, so the slot
    // holds the second pass's trace; on a generated frame the first pass must trace, not composite that trace.
    const uint32_t passA[] = { 12, 40, 2, 2, 600 };
    const uint32_t passB[] = { 12, 40, 2, 2, 598 };
    const uint64_t slotTrace = passSignature(passB, 5);
    CHECK(passSignature(passA, 5) != slotTrace);
    CHECK(traceRender(true, passSignature(passA, 5) == slotTrace, 0));
    CHECK(!traceRender(true, passSignature(passB, 5) == slotTrace, 0));
}

TEST_CASE("generated frames: a trace is reused only with the same outside darkness choice (misses darkened twice)") {
    // The arena pass darkened the misses on the real frame's target. On a generated target it is again the
    // darkening pass: reuse. If that target picked another pass to darken, the arena trace (darkness baked in)
    // must not be composited, or the misses darken twice.
    CHECK(reusableTrace(true, 7, 7, true, true));
    CHECK(reusableTrace(true, 7, 7, false, false));
    CHECK(!reusableTrace(true, 7, 7, true, false));
    CHECK(!reusableTrace(true, 7, 7, false, true));
    CHECK(!reusableTrace(true, 7, 8, true, true));
    CHECK(!reusableTrace(false, 7, 7, true, true));
}

TEST_CASE("generated frames: a pass's signature is stable across the renders of a game frame") {
    const uint32_t pass[] = { 24, 8, 8, 130 };
    CHECK_EQ(passSignature(pass, 4), passSignature(pass, 4));
    // Order and count matter: the same draws in another order, or one draw fewer, are another pass.
    const uint32_t reordered[] = { 8, 24, 8, 130 };
    CHECK(passSignature(pass, 4) != passSignature(reordered, 4));
    CHECK(passSignature(pass, 4) != passSignature(pass, 3));
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

TEST_CASE("quality: AO comes with the level (none on Performance, Subtle on Balanced, Strong on Quality and Ultra)") {
    CHECK_EQ(aoSettings(uint32_t(RtQuality::Performance)).samples, 0u);
    const AoSettings balanced = aoSettings(uint32_t(RtQuality::Balanced));
    const AoSettings quality = aoSettings(uint32_t(RtQuality::Quality));
    const AoSettings ultra = aoSettings(uint32_t(RtQuality::Ultra));
    CHECK(balanced.samples > 0);
    CHECK_EQ(balanced.strength, aoLookSettings(AoLevel::Subtle, 2).strength);
    CHECK_EQ(quality.strength, aoLookSettings(AoLevel::Strong, 2).strength);
    CHECK_EQ(ultra.strength, quality.strength);
}

TEST_CASE("quality: Strong AO always gets at least 4 rays (at 2 it mottles)") {
    for (uint32_t q = 0; q <= uint32_t(RtQuality::Ultra); q++) {
        if (rtPreset(q).ao == AoLevel::Strong) {
            CHECK(rtPreset(q).aoSamples >= 4);
        }
    }
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

// Wrestler sheen

TEST_CASE("sheen: highlights and outlines brighten without blowing out") {
    // The shadow factor is stored up to 2. Neither term alone may double a fully lit wrestler (factor 1), and the
    // two together (they peak in different places: the highlight faces the light, the outline is the silhouette)
    // stay close to that ceiling. The owner found 0.3 / 0.35 too faint and 0.65 / 0.7 too much.
    const SheenSettings s = sheenSettings();
    CHECK(s.specular > 0.35f && s.specular < 0.65f);
    CHECK(s.rim > 0.35f && s.rim < 0.7f);
    CHECK(s.specular + s.rim <= 1.1f);
}

TEST_CASE("sheen: a glossy highlight and a thin outline, not a wash") {
    // A low exponent spreads the highlight over the whole lit side (looks like a brightness bump, not shine); a low
    // rim power brightens the whole body instead of its outline.
    const SheenSettings s = sheenSettings();
    CHECK(s.shininess >= 8.0f);
    CHECK(s.rimPower >= 2.0f);
}

// Colored light

static float luminance(const std::array<float, 3> &c) {
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

TEST_CASE("light color: white stays white (presets without colors look exactly as before)") {
    const std::array<float, 3> white = normalizedLightColor(1.0f, 1.0f, 1.0f);
    CHECK_NEAR(white[0], 1.0f, 1e-5f);
    CHECK_NEAR(white[1], 1.0f, 1e-5f);
    CHECK_NEAR(white[2], 1.0f, 1e-5f);
    const std::array<float, 3> grey = normalizedLightColor(0.3f, 0.3f, 0.3f);
    CHECK_NEAR(grey[0], 1.0f, 1e-5f);
}

TEST_CASE("light color: a tint keeps the light's brightness") {
    const std::array<float, 3> warm = normalizedLightColor(1.0f, 0.9f, 0.75f);
    CHECK_NEAR(luminance(warm), 1.0f, 1e-4f);
    CHECK(warm[0] > warm[1] && warm[1] > warm[2]);
    const std::array<float, 3> cool = normalizedLightColor(0.8f, 0.9f, 1.0f);
    CHECK_NEAR(luminance(cool), 1.0f, 1e-4f);
    CHECK(cool[2] > cool[0]);
}

TEST_CASE("light color: saturated colors are capped, black falls back to white") {
    const std::array<float, 3> red = normalizedLightColor(1.0f, 0.0f, 0.0f);
    CHECK(red[0] <= MaxLightChannel);
    CHECK_EQ(red[1], 0.0f);
    const std::array<float, 3> black = normalizedLightColor(0.0f, 0.0f, 0.0f);
    CHECK_EQ(black[0], 1.0f);
    const std::array<float, 3> negative = normalizedLightColor(-1.0f, -1.0f, -1.0f);
    CHECK_EQ(negative[2], 1.0f);
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

// HDR output

// The owner's monitor: 455 nits peak, 265 full frame, Windows SDR white 240.
static const HdrDisplay OwnerHdr = { 455.0f, 265.0f, 240.0f };
static const HdrDisplay BrightHdr = { 1000.0f, 600.0f, 240.0f };
static const HdrDisplay Hdr400 = { 400.0f, 400.0f, 300.0f };
static const HdrCrt CrtOn = { true, 0.3f, false };
static const HdrCrt CrtBeam = { true, 0.3f, true };
static const HdrCrt CrtOff = { false, 0.3f, false };

TEST_CASE("hdr mask: unchanged from the original adaptive mask (owner's 150% and 100% logged values)") {
    // The CRT filter logged 0.55 / gain 1.26 at 360 nits paper white and 0.87 / gain 1.90 at 240 nits.
    HdrMask at150 = hdrMask(360.0f, 455.0f, 0.3f, false);
    CHECK_NEAR(at150.strength, 0.55, 0.01);
    CHECK_NEAR(at150.gain, 1.26, 0.01);
    HdrMask at100 = hdrMask(240.0f, 455.0f, 0.3f, false);
    CHECK_NEAR(at100.strength, 0.87, 0.01);
    CHECK_NEAR(at100.gain, 1.90, 0.01);
    CHECK_NEAR(at100.beamScale, 1.0, 1e-6);
}

TEST_CASE("hdr mask: never weaker than the preset, never past full, and the gain keeps the picture as bright as SDR") {
    for (float paper : { 80.0f, 200.0f, 455.0f, 900.0f }) {
        for (bool beam : { false, true }) {
            const HdrMask mask = hdrMask(paper, 455.0f, 0.3f, beam);
            CHECK(mask.strength >= 0.3f - 1e-6f);
            CHECK(mask.strength <= 1.0f + 1e-6f);
            // The stronger mask's light loss times the gain equals the preset's own.
            CHECK_NEAR((1.0f - mask.strength * 2.0f / 3.0f) * mask.gain, 1.0f - 0.3f * 2.0f / 3.0f, 1e-5);
            CHECK(mask.beamScale >= 1.0f);
            CHECK(mask.beamScale <= 4.0f);
        }
    }

    // No headroom (paper white at or above peak): the preset as is.
    CHECK_NEAR(hdrMask(455.0f, 455.0f, 0.3f, false).strength, 0.3, 1e-6);
    // An unknown peak counts as 1000 nits.
    CHECK_NEAR(hdrMask(240.0f, 0.0f, 0.3f, false).strength, hdrMask(240.0f, 1000.0f, 0.3f, false).strength, 1e-6);
}

TEST_CASE("hdr auto: on the owner's monitor with the CRT filter it lands where the owner tuned by eye (80% = 192 nits)") {
    const float paper = hdrPaperWhite(HdrPicture::Auto, 0.0f, OwnerHdr, CrtOn);
    CHECK_NEAR(paper, 190.0, 5.0);
    CHECK(hdrMask(paper, OwnerHdr.peakNits, 0.3f, false).strength > 0.98f);
    CHECK_EQ(int(hdrAdvice(paper, OwnerHdr, CrtOn)), int(HdrAdvice::Good));
}

TEST_CASE("hdr auto: without the CRT filter it matches the desktop (Windows SDR white)") {
    CHECK_NEAR(hdrPaperWhite(HdrPicture::Auto, 0.0f, OwnerHdr, CrtOff), 240.0, 1e-3);
    CHECK_NEAR(hdrPaperWhite(HdrPicture::Auto, 0.0f, BrightHdr, CrtOff), 240.0, 1e-3);
    // Windows doesn't report SDR white: 200 nits.
    CHECK_NEAR(hdrPaperWhite(HdrPicture::Auto, 0.0f, HdrDisplay{ 455.0f, 265.0f, 0.0f }, CrtOff), 200.0, 1e-3);
}

TEST_CASE("hdr auto: a bright display gets the full mask at desktop brightness, never brighter than the desktop") {
    const float paper = hdrPaperWhite(HdrPicture::Auto, 0.0f, BrightHdr, CrtOn);
    CHECK_NEAR(paper, 240.0, 1e-3);
    CHECK(hdrMask(paper, BrightHdr.peakNits, 0.3f, false).strength > 0.99f);
}

TEST_CASE("hdr auto: an HDR400 panel with the CRT filter stays at a comfortable brightness instead of chasing the mask") {
    const float paper = hdrPaperWhite(HdrPicture::Auto, 0.0f, Hdr400, CrtOn);
    CHECK(paper >= HdrComfortNits - 1e-3f);
    CHECK(paper <= Hdr400.sdrWhiteNits + 1e-3f);
}

TEST_CASE("hdr auto: the beam simulator shares the headroom, so Auto holds the comfort floor on the owner's monitor") {
    CHECK_NEAR(hdrPaperWhite(HdrPicture::Auto, 0.0f, OwnerHdr, CrtBeam), HdrComfortNits, 1e-3);
    // CRT detail goes as low as its floor for the mask.
    CHECK_NEAR(hdrPaperWhite(HdrPicture::CrtDetail, 0.0f, OwnerHdr, CrtBeam), HdrCrtDetailFloorNits, 1e-3);
}

TEST_CASE("hdr picture: Brighter >= Balanced >= CRT detail, on every display") {
    for (const HdrDisplay &display : { OwnerHdr, BrightHdr, Hdr400, HdrDisplay{} }) {
        for (const HdrCrt &crt : { CrtOn, CrtBeam, CrtOff }) {
            const float brighter = hdrPaperWhite(HdrPicture::Brighter, 0.0f, display, crt);
            const float balanced = hdrPaperWhite(HdrPicture::Balanced, 0.0f, display, crt);
            const float detail = hdrPaperWhite(HdrPicture::CrtDetail, 0.0f, display, crt);
            CHECK(brighter >= balanced - 1e-3f);
            CHECK(balanced >= detail - 1e-3f);
            CHECK(detail >= HdrMinPaperWhiteNits);
        }
    }
}

TEST_CASE("hdr picture: Custom uses the user's nits, clamped to the valid range") {
    CHECK_NEAR(hdrPaperWhite(HdrPicture::Custom, 190.0f, OwnerHdr, CrtOn), 190.0, 1e-3);
    CHECK_NEAR(hdrPaperWhite(HdrPicture::Custom, 10.0f, OwnerHdr, CrtOn), HdrMinPaperWhiteNits, 1e-3);
    CHECK_NEAR(hdrPaperWhite(HdrPicture::Custom, 5000.0f, OwnerHdr, CrtOn), HdrMaxPaperWhiteNits, 1e-3);
}

TEST_CASE("hdr advice: the owner's old 150% (360 nits) was brighter than the monitor holds full-screen") {
    CHECK_EQ(int(hdrAdvice(360.0f, OwnerHdr, CrtOn)), int(HdrAdvice::TooBright));
    CHECK_EQ(int(hdrAdvice(240.0f, OwnerHdr, CrtOn)), int(HdrAdvice::Good));
}

TEST_CASE("hdr advice: little headroom with the CRT filter asks for less brightness, and very dim is flagged") {
    // 1.2x headroom: the mask barely moves.
    CHECK_EQ(int(hdrAdvice(250.0f, HdrDisplay{ 300.0f, 0.0f, 250.0f }, CrtOn)), int(HdrAdvice::LowHeadroom));
    // Without the CRT filter headroom doesn't matter.
    CHECK_EQ(int(hdrAdvice(250.0f, HdrDisplay{ 300.0f, 0.0f, 250.0f }, CrtOff)), int(HdrAdvice::Good));
    CHECK_EQ(int(hdrAdvice(100.0f, OwnerHdr, CrtOff)), int(HdrAdvice::Dim));
}

// HUD Placement

namespace {
    // A 320-pixel-wide game screen (centre 160) and a 480-pixel anamorphic one (WCW's 480x240).
    std::vector<HudAnchor> anchors320(const std::vector<HudBox> &boxes) {
        return hudAnchors(boxes, 0.0f, 320.0f);
    }

    // A meter drawn as segments 'step' pixels apart.
    void addSegments(std::vector<HudBox> &boxes, float left, float right, float top, float bottom, float step, float segmentWidth) {
        for (float x = left; x + segmentWidth <= right; x += step) {
            boxes.push_back({ x, top, x + segmentWidth, bottom });
        }
    }

    bool allAre(const std::vector<HudAnchor> &anchors, size_t begin, size_t end, HudAnchor anchor) {
        for (size_t i = begin; i < end; i++) {
            if (anchors[i] != anchor) {
                return false;
            }
        }

        return true;
    }

    // rt64's rect conversion for an origin (convertViewportRect), as normalized device x of the wide output.
    float rectOriginNdc(float x, float fbWidth, float centredWidthFraction, float placement, HudAnchor anchor) {
        float origin = fbWidth / 2.0f;
        if (anchor == HudAnchor::Left) {
            origin = 0.0f * placement + (fbWidth / 2.0f) * (1.0f - placement);
        }
        else if (anchor == HudAnchor::Right) {
            origin = fbWidth * placement + (fbWidth / 2.0f) * (1.0f - placement);
        }

        const float wideX = origin + (x - origin) * centredWidthFraction;
        return (wideX / fbWidth) * 2.0f - 1.0f;
    }
}

TEST_CASE("hud: two corner meters anchor to their own edges (WM2000 attitude meters)") {
    std::vector<HudBox> boxes;
    addSegments(boxes, 16.0f, 120.0f, 200.0f, 210.0f, 6.0f, 4.0f);
    const size_t leftCount = boxes.size();
    addSegments(boxes, 200.0f, 304.0f, 200.0f, 210.0f, 6.0f, 4.0f);
    // Name plates above the meters.
    boxes.push_back({ 16.0f, 186.0f, 80.0f, 196.0f });
    boxes.push_back({ 240.0f, 186.0f, 304.0f, 196.0f });

    const auto anchors = anchors320(boxes);
    CHECK(allAre(anchors, 0, leftCount, HudAnchor::Left));
    CHECK(allAre(anchors, leftCount, boxes.size() - 2, HudAnchor::Right));
    CHECK(anchors[boxes.size() - 2] == HudAnchor::Left);
    CHECK(anchors[boxes.size() - 1] == HudAnchor::Right);
}

TEST_CASE("hud: a centred timer stays in one piece (digits left of the centre don't anchor left)") {
    // "00:26": five glyphs across the centre line.
    std::vector<HudBox> boxes;
    for (int i = 0; i < 5; i++) {
        boxes.push_back({ 136.0f + i * 10.0f, 12.0f, 144.0f + i * 10.0f, 24.0f });
    }

    const auto anchors = anchors320(boxes);
    CHECK(allAre(anchors, 0, boxes.size(), HudAnchor::Centre));
}

TEST_CASE("hud: an element near the centre stays centred even without crossing it") {
    // A short label just left of the centre line, alone.
    const auto anchors = anchors320({ { 140.0f, 100.0f, 158.0f, 110.0f } });
    CHECK(anchors[0] == HudAnchor::Centre);
}

TEST_CASE("hud: a meter's end segment near the centre moves with the meter (no tearing)") {
    std::vector<HudBox> boxes;
    addSegments(boxes, 20.0f, 154.0f, 200.0f, 210.0f, 6.0f, 4.0f);
    // Alone, the last segment would be in the centre band.
    CHECK(anchors320({ boxes.back() })[0] == HudAnchor::Centre);
    const auto anchors = anchors320(boxes);
    CHECK(allAre(anchors, 0, boxes.size(), HudAnchor::Left));
}

TEST_CASE("hud: an element across the centre stays, and draws far apart are separate elements") {
    std::vector<HudBox> boxes = {
        { 60.0f, 20.0f, 260.0f, 40.0f },  // banner across the centre
        { 10.0f, 220.0f, 40.0f, 230.0f }, // bottom-left icon, far below the banner
        { 280.0f, 220.0f, 310.0f, 230.0f } // bottom-right icon
    };

    const auto anchors = anchors320(boxes);
    CHECK(anchors[0] == HudAnchor::Centre);
    CHECK(anchors[1] == HudAnchor::Left);
    CHECK(anchors[2] == HudAnchor::Right);

    // Touching the banner joins it: the icon then stays with the banner.
    boxes[1] = { 40.0f, 42.0f, 56.0f, 52.0f };
    CHECK(anchors320(boxes)[1] == HudAnchor::Centre);
}

TEST_CASE("hud: the anamorphic 480-wide framebuffer uses its own centre") {
    const auto anchors = hudAnchors({ { 20.0f, 200.0f, 150.0f, 210.0f }, { 330.0f, 200.0f, 460.0f, 210.0f }, { 225.0f, 10.0f, 255.0f, 20.0f } }, 0.0f, 480.0f);
    CHECK(anchors[0] == HudAnchor::Left);
    CHECK(anchors[1] == HudAnchor::Right);
    CHECK(anchors[2] == HudAnchor::Centre);
}

TEST_CASE("hud: a full 2D screen (too many draws) and an empty screen are left alone") {
    std::vector<HudBox> boxes;
    for (size_t i = 0; i <= HudMaxBoxes; i++) {
        boxes.push_back({ 0.0f, 0.0f, 4.0f, 4.0f });
    }

    CHECK(allAre(anchors320(boxes), 0, boxes.size(), HudAnchor::Centre));
    CHECK(anchors320({}).empty());
    CHECK(hudAnchors({ { 0.0f, 0.0f, 4.0f, 4.0f } }, 10.0f, 10.0f)[0] == HudAnchor::Centre);
}

TEST_CASE("hud: shift reaches the window edge at Expand, a 16:9 edge when clamped, nothing at Original or 4:3") {
    // 16:9 window: the 4:3 picture is 0.75 of the width.
    const float s169 = (4.0f / 3.0f) / (16.0f / 9.0f);
    CHECK_NEAR(hudShiftNdc(HudAnchor::Left, s169, 1.0f), -0.25, 1e-5);
    CHECK_NEAR(hudShiftNdc(HudAnchor::Right, s169, 1.0f), 0.25, 1e-5);
    CHECK_NEAR(hudShiftNdc(HudAnchor::Centre, s169, 1.0f), 0.0, 1e-6);
    // The 4:3 picture's left edge (-s) lands on the window edge.
    CHECK_NEAR(-s169 + hudShiftNdc(HudAnchor::Left, s169, 1.0f), -1.0, 1e-5);

    // 21:9 window with HUD Placement 16:9: rt64's percentage is (16:9 - 4:3) / (21:9 - 4:3).
    const float s219 = (4.0f / 3.0f) / (21.0f / 9.0f);
    const float clamp169 = (16.0f / 9.0f - 4.0f / 3.0f) / (21.0f / 9.0f - 4.0f / 3.0f);
    CHECK_NEAR(-s219 + hudShiftNdc(HudAnchor::Left, s219, clamp169), -(16.0f / 9.0f) / (21.0f / 9.0f), 1e-5);
    CHECK_NEAR(s219 + hudShiftNdc(HudAnchor::Right, s219, clamp169), (16.0f / 9.0f) / (21.0f / 9.0f), 1e-5);

    CHECK(!hudPlacementActive(s169, 0.0f));
    CHECK(!hudPlacementActive(1.0f, 1.0f));
    CHECK(hudPlacementActive(s169, 1.0f));
    CHECK_NEAR(hudShiftNdc(HudAnchor::Left, s169, 0.0f), 0.0, 1e-6);
    CHECK_NEAR(hudShiftNdc(HudAnchor::Right, 1.0f, 1.0f), 0.0, 1e-6);
}

TEST_CASE("hud: triangles move exactly like rects with rt64's G_EX origins (both parts of an element stay together)") {
    const float fbWidth = 320.0f;
    const float placements[] = { 1.0f, 0.4444f, 0.25f };
    const float fractions[] = { 0.75f, 0.5714f };
    const HudAnchor sides[] = { HudAnchor::Left, HudAnchor::Right, HudAnchor::Centre };
    for (float e : placements) {
        for (float s : fractions) {
            for (HudAnchor a : sides) {
                for (float x : { 0.0f, 37.0f, 160.0f, 290.0f }) {
                    // A triangle vertex: its 4:3 position compressed into the centre, then shifted.
                    const float triNdc = ((x / fbWidth) * 2.0f - 1.0f) * s + hudShiftNdc(a, s, e);
                    CHECK_NEAR(triNdc, rectOriginNdc(x, fbWidth, s, e, a), 1e-4);
                }
            }
        }
    }
}

// Widescreen

TEST_CASE("widescreen: a pass whose scissor matches its framebuffer covers the screen (both pixel aspects)") {
    CHECK(framebufferPassCoversScreen(320, 240, 320, 240));
    CHECK(framebufferPassCoversScreen(480, 240, 480, 240));
}

TEST_CASE("widescreen: No Mercy's story screens (480x240 scissor, fb measured 480x196) cover the screen (dialog text jumped)") {
    CHECK(framebufferPassCoversScreen(480, 240, 480, 196));
    CHECK(framebufferPassCoversScreen(480, 240, 480, 198));
}

TEST_CASE("widescreen: a sub-rectangle of another shape doesn't (split screen, a texture rendered into part of it)") {
    CHECK(!framebufferPassCoversScreen(240, 240, 480, 240));
    CHECK(!framebufferPassCoversScreen(480, 120, 480, 240));
    CHECK(!framebufferPassCoversScreen(64, 64, 480, 240));
}

TEST_CASE("widescreen: the ratio test is unchanged (a same-shape scissor counts, as before)") {
    CHECK(framebufferPassCoversScreen(64, 32, 480, 240));
    CHECK(framebufferPassCoversScreen(300, 240, 320, 240));
}

TEST_CASE("widescreen: empty or unknown sizes don't cover the screen") {
    CHECK(!framebufferPassCoversScreen(0, 240, 480, 240));
    CHECK(!framebufferPassCoversScreen(480, 240, 0, 0));
}

static uint32_t packXY(float x, float y) {
    return (uint32_t(uint16_t(int16_t(std::lround(x * 4.0f)))) << 16) | uint32_t(uint16_t(int16_t(std::lround(y * 4.0f))));
}

static float unpackX(uint32_t v) {
    return float(int16_t((v >> 16) & 0xFFFF)) / 4.0f;
}

TEST_CASE("widescreen: a game-set screen position is compressed like transformed vertices (story flow lines were stretched)") {
    // 480-wide N64 screen, viewport centered at 240, Expand 16:9 (projRatioScale 0.75): a flow-line end at x = 338 lands
    // 98 * 0.75 from the center, as the boxes it joins do; y stays.
    const uint32_t v = modifiedScreenXYForAspect(packXY(338.0f, 61.25f), 240.0f, 0.75f);
    CHECK(std::abs(unpackX(v) - (240.0f + 98.0f * 0.75f)) <= 0.25f);
    CHECK((v & 0xFFFF) == (packXY(338.0f, 61.25f) & 0xFFFF));
}

TEST_CASE("widescreen: the viewport center and both sides follow the same rule") {
    CHECK(unpackX(modifiedScreenXYForAspect(packXY(240.0f, 0.0f), 240.0f, 0.75f)) == 240.0f);
    CHECK(std::abs(unpackX(modifiedScreenXYForAspect(packXY(40.0f, 0.0f), 240.0f, 0.75f)) - 90.0f) <= 0.25f);
    // A viewport centered elsewhere (AKI's menus use a 3x screen viewport, still centered on the screen; others may not).
    CHECK(std::abs(unpackX(modifiedScreenXYForAspect(packXY(100.0f, 0.0f), 60.0f, 0.5f)) - 80.0f) <= 0.25f);
}

TEST_CASE("widescreen: at Original (ratio 1) a modify is untouched, bit for bit") {
    for (uint32_t v : { packXY(338.0f, 61.25f), packXY(-12.75f, 239.5f), 0xFFFF8000u, 0x7FFF0001u }) {
        CHECK(modifiedScreenXYForAspect(v, 240.0f, 1.0f) == v);
    }
}

TEST_CASE("widescreen: negative screen x and the int16 range survive the conversion") {
    CHECK(std::abs(unpackX(modifiedScreenXYForAspect(packXY(-100.0f, 5.0f), 240.0f, 0.75f)) - (240.0f - 340.0f * 0.75f)) <= 0.25f);
    CHECK(unpackX(modifiedScreenXYForAspect(packXY(8000.0f, 0.0f), 0.0f, 2.0f)) == 8191.75f);
}

int main() {
    return WcwTest::runAll("rt64 wcw tests");
}
