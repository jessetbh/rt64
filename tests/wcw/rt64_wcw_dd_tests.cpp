//
// RT64
//

// [wcw dd] Unit tests for the in-game Datadog metrics logic (WCW_DD_METRICS, aki-roadmap#125): the window aggregation
// (rates, percentiles, 1% low, hitches, GPU medians), the context tags and their change text, and the DogStatsD lines,
// which must match AkiTools lib\ddmetrics.ps1 (the expected strings below are that script's output). Built and run
// with the other wcw tests on every rt64 build.

#include "common/rt64_wcw_logic.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "wcw_test.h"

using namespace RT64::WcwLogic;

static double metric(const std::vector<std::pair<std::string, double>> &m, const std::string &name, double missing = -1.0) {
    for (const auto &p : m) {
        if (p.first == name) {
            return p.second;
        }
    }

    return missing;
}

static bool hasMetric(const std::vector<std::pair<std::string, double>> &m, const std::string &name) {
    return metric(m, name, -12345.0) != -12345.0;
}

TEST_CASE("dd: WCW_DD_METRICS is on only when set, not empty and not 0") {
    CHECK(!envSwitchOn(nullptr));
    CHECK(!envSwitchOn(""));
    CHECK(!envSwitchOn("0"));
    CHECK(envSwitchOn("1"));
    CHECK(envSwitchOn("yes"));
}

TEST_CASE("dd window: steady 60 fps gives 60 presents/s, 16.7 ms percentiles and no hitches") {
    DdFrameWindow w;
    w.start(0.0);
    const double frameMs = 1000.0 / 60.0;
    for (int i = 0; i < 600; i++) {
        w.addPresent(i * frameMs, false, false);
    }

    const DdFrameStats s = w.take(10000.0, 60);
    CHECK_NEAR(s.seconds, 10.0, 1e-9);
    CHECK_EQ(s.presents, 600u);
    CHECK_NEAR(s.presentsPerS, 60.0, 1e-9);
    CHECK_NEAR(s.realFps, 60.0, 1e-9);
    CHECK_NEAR(s.interpolatedPerS, 0.0, 1e-9);
    CHECK(s.hasIntervals);
    CHECK_NEAR(s.p50Ms, frameMs, 1e-6);
    CHECK_NEAR(s.p99Ms, frameMs, 1e-6);
    CHECK_NEAR(s.low1PctFps, 60.0, 1e-6);
    CHECK_EQ(s.hitches, 0u);
}

TEST_CASE("dd window: generated and repeated presents split real fps from presents/s") {
    DdFrameWindow w;
    w.start(0.0);
    // 120 presents/s for 1 s, every other one generated; 3 repeated real frames.
    for (int i = 0; i < 120; i++) {
        w.addPresent(i * (1000.0 / 120.0), (i % 2) == 1, (i == 10) || (i == 20) || (i == 30));
    }

    const DdFrameStats s = w.take(1000.0, 120);
    CHECK_NEAR(s.presentsPerS, 120.0, 1e-9);
    CHECK_NEAR(s.interpolatedPerS, 60.0, 1e-9);
    CHECK_NEAR(s.realFps, 60.0, 1e-9);
    CHECK_EQ(s.repeated, 3u);
    CHECK_NEAR(s.repeatedPerMin, 180.0, 1e-9);
}

TEST_CASE("dd window: a stall is a hitch, shows in max, p99 and the 1% low") {
    DdFrameWindow w;
    w.start(0.0);
    double t = 0.0;
    // 199 intervals of 16.67 ms and one of 100 ms.
    for (int i = 0; i < 200; i++) {
        w.addPresent(t, false, false);
        t += (i == 100) ? 100.0 : (1000.0 / 60.0);
    }

    const DdFrameStats s = w.take(t, 60);
    CHECK_EQ(s.hitches, 1u);
    CHECK_NEAR(s.maxMs, 100.0, 1e-9);
    CHECK_NEAR(s.p50Ms, 1000.0 / 60.0, 1e-6);
    // 199 intervals: the slowest 1% is one interval, the stall.
    CHECK_NEAR(s.low1PctFps, 10.0, 1e-9);
    CHECK_NEAR(s.hitchesPerMin, 60.0 / s.seconds, 1e-9);
}

TEST_CASE("dd window: a game steadily below the target (30 fps on 60) has no hitches") {
    DdFrameWindow w;
    w.start(0.0);
    for (int i = 0; i < 300; i++) {
        w.addPresent(i * (1000.0 / 30.0), false, false);
    }

    const DdFrameStats s = w.take(10000.0, 60);
    CHECK_EQ(s.hitches, 0u);
    // ...but a 3x stall at 30 fps is one.
    DdFrameWindow w2;
    w2.start(0.0);
    double t = 0.0;
    for (int i = 0; i < 100; i++) {
        w2.addPresent(t, false, false);
        t += (i == 50) ? 100.0 : (1000.0 / 30.0);
    }

    CHECK_EQ(w2.take(t, 60).hitches, 1u);
}

TEST_CASE("dd window: the first interval spans the window boundary; take starts the next window") {
    DdFrameWindow w;
    w.start(0.0);
    w.addPresent(990.0, false, false);
    const DdFrameStats a = w.take(1000.0, 60);
    CHECK_EQ(a.presents, 1u);
    CHECK(!a.hasIntervals);
    w.addPresent(1010.0, false, false);
    const DdFrameStats b = w.take(2000.0, 60);
    CHECK_EQ(b.presents, 1u);
    CHECK(b.hasIntervals);
    CHECK_NEAR(b.p50Ms, 20.0, 1e-9);
    CHECK_NEAR(b.seconds, 1.0, 1e-9);
    // No presents at all (a hang): rates are 0, no percentiles.
    const DdFrameStats c = w.take(12000.0, 60);
    CHECK_EQ(c.presents, 0u);
    CHECK_NEAR(c.presentsPerS, 0.0, 1e-9);
    CHECK(!c.hasIntervals);
}

TEST_CASE("dd window: GPU medians and p95 from the timing rows") {
    DdFrameWindow w;
    w.start(0.0);
    for (int i = 1; i <= 20; i++) {
        w.addGpuRender(double(i), 0.5, 2.0);
    }

    w.addGpuPresent(1.0);
    w.addGpuPresent(3.0);
    w.addGpuPresent(2.0);
    const DdFrameStats s = w.take(1000.0, 60);
    CHECK_EQ(s.gpuRenderRows, 20u);
    CHECK_NEAR(s.gpuRenderMs, 10.0, 1e-9);
    CHECK_NEAR(s.gpuRenderP95Ms, 19.0, 1e-9);
    CHECK_NEAR(s.rtBuildMs, 0.5, 1e-9);
    CHECK_NEAR(s.rtTraceMs, 2.0, 1e-9);
    CHECK_NEAR(s.gpuPresentMs, 2.0, 1e-9);
    // Cleared for the next window.
    CHECK_EQ(w.take(2000.0, 60).gpuRenderRows, 0u);
}

TEST_CASE("dd: nearest-rank percentile") {
    CHECK_NEAR(ddPercentile({}, 0.5), 0.0, 1e-9);
    CHECK_NEAR(ddPercentile({ 5.0 }, 0.99), 5.0, 1e-9);
    CHECK_NEAR(ddPercentile({ 4.0, 1.0, 3.0, 2.0 }, 0.5), 2.0, 1e-9);
    CHECK_NEAR(ddPercentile({ 4.0, 1.0, 3.0, 2.0 }, 1.0), 4.0, 1e-9);
    CHECK_NEAR(ddPercentile({ 4.0, 1.0, 3.0, 2.0 }, 0.0), 1.0, 1e-9);
}

TEST_CASE("dd: fps target is the set rate, else the VI rate (Original)") {
    CHECK_EQ(ddFpsTarget(120, 60), 120u);
    CHECK_EQ(ddFpsTarget(0, 60), 60u);
    CHECK_EQ(ddFpsTarget(0, 0), 0u);
}

TEST_CASE("dd metrics: only gauges with data, RT ms only while ray tracing runs") {
    DdFrameStats s;
    DdProcessSample p;
    auto m = ddWindowMetrics(s, p);
    CHECK(hasMetric(m, "aki.game.active"));
    CHECK(!hasMetric(m, "aki.game.presents_per_s"));
    CHECK(!hasMetric(m, "aki.game.pacing.p99_ms"));
    CHECK(!hasMetric(m, "aki.game.perf.gpu_render_ms"));
    CHECK(!hasMetric(m, "aki.game.memory.private_mb"));
    CHECK(!hasMetric(m, "aki.game.memory.vram_mb"));

    s.seconds = 10.0;
    s.presentsPerS = 60.0;
    s.hasIntervals = true;
    s.p99Ms = 17.0;
    s.gpuRenderRows = 5;
    s.gpuRenderMs = 4.0;
    p.has = true;
    p.privateMb = 900.0;
    p.handles = 512;
    p.hasVram = true;
    p.vramMb = 1500.0;
    m = ddWindowMetrics(s, p);
    CHECK_NEAR(metric(m, "aki.game.presents_per_s"), 60.0, 1e-9);
    CHECK_NEAR(metric(m, "aki.game.pacing.p99_ms"), 17.0, 1e-9);
    CHECK_NEAR(metric(m, "aki.game.perf.gpu_render_ms"), 4.0, 1e-9);
    CHECK(!hasMetric(m, "aki.game.perf.rt_trace_ms"));
    CHECK(!hasMetric(m, "aki.game.perf.gpu_present_ms"));
    CHECK_NEAR(metric(m, "aki.game.memory.private_mb"), 900.0, 1e-9);
    CHECK_NEAR(metric(m, "aki.game.process.handles"), 512.0, 1e-9);
    CHECK_NEAR(metric(m, "aki.game.memory.vram_mb"), 1500.0, 1e-9);
    s.rtTraceMs = 2.5;
    m = ddWindowMetrics(s, p);
    CHECK_NEAR(metric(m, "aki.game.perf.rt_trace_ms"), 2.5, 1e-9);
    CHECK(hasMetric(m, "aki.game.perf.rt_build_ms"));
    // Every name is already a valid Datadog metric name.
    for (const auto &e : m) {
        CHECK(ddMetricName(e.first) == e.first);
    }
}

TEST_CASE("dd context: tags use ddmetrics.ps1 keys; rt_quality only with RT on") {
    DdContext c;
    c.game = "nomercy";
    c.fpsTarget = 120;
    c.framerate = "manual";
    c.rt = false;
    c.rtQuality = 2;
    c.resolution = "WindowIntegerScale";
    c.aspect = "Expand";
    c.msaa = "None";
    c.extraTags = { "conversion:btbw" };
    std::vector<std::string> t = ddContextTags(c);
    const std::vector<std::string> expected = { "game:nomercy", "fps_target:120", "framerate:manual", "rt:off", "resolution:windowintegerscale",
        "aspect:expand", "msaa:none", "downsample:1", "hdr:off", "crt:off", "beam:off", "conversion:btbw" };
    CHECK(t == expected);
    c.rt = true;
    c.crt = "crt-guest-advanced-ntsc";
    c.venue = "Raw";
    t = ddContextTags(c);
    CHECK(std::find(t.begin(), t.end(), "rt_quality:quality") != t.end());
    CHECK(std::find(t.begin(), t.end(), "crt:crt-guest-advanced-ntsc") != t.end());
    CHECK(std::find(t.begin(), t.end(), "rt_venue:raw") != t.end());
    CHECK(std::string(rtQualityName(3)) == "ultra");
    CHECK(std::string(rtQualityName(9)) == "unknown");
}

TEST_CASE("dd context: equality covers every field (a settings change is never missed)") {
    DdContext a;
    DdContext b;
    CHECK(a == b);
    b.fpsTarget = 60;
    CHECK(a != b);
    b = a;
    b.downsample = 2;
    CHECK(a != b);
    b = a;
    b.beam = true;
    CHECK(a != b);
    b = a;
    b.venue = "Raw";
    CHECK(a != b);
    b = a;
    b.extraTags = { "x:y" };
    CHECK(a != b);
    b = a;
    b.rtQuality = 1;
    CHECK(a != b);
}

TEST_CASE("dd context: tag change text lists changed, added and removed keys") {
    const std::vector<std::string> before = { "game:wt", "rt:off", "fps_target:60" };
    const std::vector<std::string> after = { "game:wt", "rt:on", "rt_quality:ultra", "fps_target:60" };
    CHECK(ddTagChanges(before, after) == "rt:off -> rt:on, rt_quality:(none) -> rt_quality:ultra");
    CHECK(ddTagChanges(after, before) == "rt:on -> rt:off, rt_quality:ultra -> rt_quality:(none)");
    CHECK(ddTagChanges(before, before).empty());
}

TEST_CASE("dd: game key from the executable name (AkiTools games.psd1)") {
    CHECK(ddGameFromExe("NoMercyRecompiled.exe") == "nomercy");
    CHECK(ddGameFromExe("C:\\games\\WCWRecompiled.exe") == "wt");
    CHECK(ddGameFromExe("/x/y/RevengeRecompiled") == "revenge");
    CHECK(ddGameFromExe("Vpw2Recompiled.EXE") == "vpw2");
    CHECK(ddGameFromExe("Wm2kRecompiled.exe") == "wm2k");
    CHECK(ddGameFromExe("Vpw64Recompiled.exe") == "vpw64");
    CHECK(ddGameFromExe("SomethingElse.exe") == "somethingelse");
}

TEST_CASE("dd: WCW_DD_TAGS parsing") {
    CHECK(ddParseTags(nullptr).empty());
    const std::vector<std::string> t = ddParseTags("conversion:BTBW, owner:Fire Ant,novalue:,nocolon,conversion:btbw");
    const std::vector<std::string> expected = { "conversion:btbw", "owner:fire_ant" };
    CHECK(t == expected);
}

TEST_CASE("dd: names, tags and values match ddmetrics.ps1") {
    CHECK(ddTag("owner", "Fire Ant") == "owner:fire_ant");
    CHECK(ddTag("Git RT64", "  crt-guest-advanced/NTSC.slangp ") == "git_rt64:crt-guest-advanced/ntsc.slangp");
    CHECK(ddTag("_9key", "__a__b__") == "t_9key:a_b");
    CHECK(ddTag("k", "###").empty());
    CHECK(ddTag("k", "").empty());
    CHECK(ddTag("k", "C:\\Games\\x y") == "k:c:_games_x_y");
    CHECK(ddTag("k", std::string(300, 'a')).size() == 200);
    CHECK(ddMetricName("aki.game.Pacing-P99 ms") == "aki.game.pacing_p99_ms");
    CHECK(ddMetricName("9lives") == "aki_9lives");
    CHECK(ddFormatValue(60.8) == "60.8");
    CHECK(ddFormatValue(16.6666666) == "16.666667");
    CHECK(ddFormatValue(2.0) == "2");
    CHECK(ddFormatValue(-0.0000001) == "0");
    CHECK(ddFormatValue(1234567.25) == "1234567.25");
}

TEST_CASE("dd: gauge and event lines match ddmetrics.ps1 (source aki-game)") {
    CHECK(ddGaugeLine("aki.run.presents_per_s", 60.8, { "game:nomercy", "rt:on" }) == "aki.run.presents_per_s:60.8|g|#game:nomercy,rt:on");
    CHECK(ddGaugeLine("aki.game.x", 1.0, {}) == "aki.game.x:1|g");
    CHECK(ddGaugeLine("aki.game.x", std::numeric_limits<double>::infinity(), {}).empty());
    CHECK(ddEventLine("Title", "a\nb", { "game:wt" }, "error", "key1") == "_e{5,4}:Title|a\\nb|k:key1|s:aki-game|t:error|#game:wt");
    CHECK(ddEventLine("T\r\nx", "", {}, "", "") == "_e{3,0}:T x||s:aki-game");
}

TEST_CASE("dd: packets join lines under the size limit, a long line goes alone") {
    const std::vector<std::string> lines = { "aaaa", "bbbb", "", "cccc", std::string(20, 'd'), "ee" };
    const std::vector<std::string> p = ddPackets(lines, 10);
    const std::vector<std::string> expected = { "aaaa\nbbbb", "cccc", std::string(20, 'd'), "ee" };
    CHECK(p == expected);
    CHECK(ddPackets({}, 10).empty());
    for (const std::string &packet : ddPackets(std::vector<std::string>(500, "aki.game.presents_per_s:60|g|#game:nomercy"))) {
        CHECK(packet.size() <= DdMaxPacketBytes);
    }
}
