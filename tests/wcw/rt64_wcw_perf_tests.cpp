//
// RT64
//

// [wcw perf] Unit tests for common/rt64_wcw_perf (WCW_PERF_LOG): pass and phase names, the stamp budget, per-pass time
// accounting and the CSV rows. Built and run with the other wcw tests on every rt64 build.

#include "common/rt64_wcw_perf.h"

#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "wcw_test.h"

using namespace RT64::WcwPerf;

static size_t csvColumns(const std::string &line) {
    size_t count = 1;
    for (char c : line) {
        count += (c == ',') ? 1 : 0;
    }

    return count;
}

static std::vector<std::string> csvFields(const std::string &line) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string f;
    while (std::getline(ss, f, ',')) {
        fields.push_back(f);
    }

    return fields;
}

static bool isIdentifier(const char *name) {
    if ((name == nullptr) || (name[0] == '\0')) {
        return false;
    }

    for (const char *c = name; *c != '\0'; c++) {
        if (!(((*c >= 'a') && (*c <= 'z')) || ((*c >= '0') && (*c <= '9')) || (*c == '_'))) {
            return false;
        }
    }

    return true;
}

TEST_CASE("perf: every GPU pass and CPU phase has a unique lowercase identifier") {
    std::set<std::string> names;
    for (uint32_t i = 0; i < GpuPassCount; i++) {
        const char *name = gpuPassName(GpuPass(i));
        CHECK(isIdentifier(name));
        CHECK(std::string(name) != "unknown");
        names.insert(name);
    }

    CHECK_EQ(names.size(), size_t(GpuPassCount));

    std::set<std::string> phases;
    for (uint32_t i = 0; i < CpuPhaseCount; i++) {
        const char *name = cpuPhaseName(CpuPhase(i));
        CHECK(isIdentifier(name));
        CHECK(std::string(name) != "unknown");
        CHECK(isKnownCpuPhaseName(name));
        phases.insert(name);
    }

    CHECK_EQ(phases.size(), size_t(CpuPhaseCount));
}

TEST_CASE("perf: the phase names ultramodern reports are known") {
    for (const char *name : { "game_frame", "game_busy", "rsp_audio", "rsp_task", "dl_submit" }) {
        CHECK(isKnownCpuPhaseName(name));
    }

    CHECK(!isKnownCpuPhaseName("not_a_phase"));
    CHECK(!isKnownCpuPhaseName(nullptr));
}

TEST_CASE("perf: nested passes count only their own time") {
    // raster [0,10) with rt_trace [2,6) inside it, rt_denoise [4,5) inside that.
    const Marker markers[] = {
        { GpuPass::Raster, true }, { GpuPass::RtTrace, true }, { GpuPass::RtDenoise, true },
        { GpuPass::Other, false }, { GpuPass::Other, false }, { GpuPass::Other, false }
    };
    const uint64_t ns[] = { 0, 2000000, 4000000, 5000000, 6000000, 10000000 };
    std::array<double, GpuPassCount> ms;
    const double span = exclusivePassTimes(markers, ns, std::size(markers), ms);
    CHECK_NEAR(span, 10.0, 1e-9);
    CHECK_NEAR(ms[uint32_t(GpuPass::Raster)], 6.0, 1e-9);
    CHECK_NEAR(ms[uint32_t(GpuPass::RtTrace)], 3.0, 1e-9);
    CHECK_NEAR(ms[uint32_t(GpuPass::RtDenoise)], 1.0, 1e-9);
    CHECK_NEAR(ms[uint32_t(GpuPass::Other)], 0.0, 1e-9);
}

TEST_CASE("perf: time between passes goes to other, and the columns add up to the span") {
    // setup [0,1), gap [1,3), fb_ops [3,7) with raster [4,6) inside, gap [7,8), fb_ops [8,9).
    const Marker markers[] = {
        { GpuPass::Setup, true }, { GpuPass::Other, false },
        { GpuPass::FbOps, true }, { GpuPass::Raster, true }, { GpuPass::Other, false }, { GpuPass::Other, false },
        { GpuPass::FbOps, true }, { GpuPass::Other, false }
    };
    const uint64_t ns[] = { 0, 1000000, 3000000, 4000000, 6000000, 7000000, 8000000, 9000000 };
    std::array<double, GpuPassCount> ms;
    const double span = exclusivePassTimes(markers, ns, std::size(markers), ms);
    CHECK_NEAR(ms[uint32_t(GpuPass::Setup)], 1.0, 1e-9);
    CHECK_NEAR(ms[uint32_t(GpuPass::Other)], 3.0, 1e-9);
    CHECK_NEAR(ms[uint32_t(GpuPass::FbOps)], 3.0, 1e-9);
    CHECK_NEAR(ms[uint32_t(GpuPass::Raster)], 2.0, 1e-9);
    double sum = 0.0;
    for (double v : ms) {
        sum += v;
    }

    CHECK_NEAR(sum, span, 1e-9);
    CHECK_NEAR(span, 9.0, 1e-9);
}

TEST_CASE("perf: a switch from one pass to another splits the time") {
    // rt_trace [0,3), then switchTo(rt_composite): pop + push at the same stamp, [3,4).
    const Marker markers[] = {
        { GpuPass::RtTrace, true }, { GpuPass::Other, false }, { GpuPass::RtComposite, true }, { GpuPass::Other, false }
    };
    const uint64_t ns[] = { 0, 3000000, 3000000, 4000000 };
    std::array<double, GpuPassCount> ms;
    exclusivePassTimes(markers, ns, std::size(markers), ms);
    CHECK_NEAR(ms[uint32_t(GpuPass::RtTrace)], 3.0, 1e-9);
    CHECK_NEAR(ms[uint32_t(GpuPass::RtComposite)], 1.0, 1e-9);
    CHECK_NEAR(ms[uint32_t(GpuPass::Other)], 0.0, 1e-9);
}

TEST_CASE("perf: stray pops, backwards stamps and short timelines don't break the accounting") {
    // A pop with nothing open is ignored; a stamp before its predecessor counts as 0 ms.
    const Marker markers[] = {
        { GpuPass::Other, false }, { GpuPass::Raster, true }, { GpuPass::Other, false }, { GpuPass::Vi, true }, { GpuPass::Other, false }
    };
    const uint64_t ns[] = { 0, 1000000, 5000000, 4000000, 6000000 };
    std::array<double, GpuPassCount> ms;
    const double span = exclusivePassTimes(markers, ns, std::size(markers), ms);
    CHECK_NEAR(ms[uint32_t(GpuPass::Other)], 1.0, 1e-9);
    CHECK_NEAR(ms[uint32_t(GpuPass::Raster)], 4.0, 1e-9);
    CHECK_NEAR(ms[uint32_t(GpuPass::Vi)], 2.0, 1e-9);
    CHECK_NEAR(span, 6.0, 1e-9);

    // Fewer than two stamps: no time at all.
    std::array<double, GpuPassCount> one;
    one.fill(7.0);
    CHECK_NEAR(exclusivePassTimes(markers, ns, 1, one), 0.0, 1e-9);
    CHECK_NEAR(one[uint32_t(GpuPass::Raster)], 0.0, 1e-9);
}

TEST_CASE("perf: the marker budget never stamps more than the pool holds and always balances") {
    // Capacity 6: push A, push B, push C fit (3 used, 3 pops owed); push D would need 3 + 1 + 1 + 3 = 8 > 6.
    MarkerBudget budget(6);
    CHECK(budget.push());
    CHECK(budget.push());
    CHECK(budget.push());
    CHECK(!budget.push());
    CHECK_EQ(budget.dropped(), 1u);
    CHECK_EQ(budget.depth(), 4u);
    // D's pop isn't stamped; C's, B's and A's are.
    CHECK(!budget.pop());
    CHECK(budget.pop());
    CHECK(budget.pop());
    CHECK(budget.pop());
    CHECK_EQ(budget.used(), 6u);
    CHECK_EQ(budget.depth(), 0u);
    // Nothing open: a pop is ignored.
    CHECK(!budget.pop());
    CHECK_EQ(budget.used(), 6u);
}

TEST_CASE("perf: the marker budget keeps balanced stamps over many sibling passes") {
    // 100 sibling passes in a pool of 64: stamp pairs until full, never an orphan push.
    MarkerBudget budget(64);
    uint32_t stampedPushes = 0, stampedPops = 0;
    budget.push();
    stampedPushes++;
    for (int i = 0; i < 100; i++) {
        stampedPushes += budget.push() ? 1 : 0;
        stampedPops += budget.pop() ? 1 : 0;
    }

    stampedPops += budget.pop() ? 1 : 0;
    CHECK_EQ(stampedPushes, stampedPops);
    CHECK(budget.used() <= 64u);
    CHECK_EQ(budget.used(), stampedPushes + stampedPops);
    CHECK(budget.dropped() > 0u);

    // Reset starts a fresh frame.
    budget.reset(64);
    CHECK_EQ(budget.used(), 0u);
    CHECK_EQ(budget.dropped(), 0u);
    CHECK(budget.push());
}

TEST_CASE("perf: GPU rows have one column per header name, passes in enum order") {
    const std::string header = gpuCsvHeader();
    const std::vector<std::string> names = csvFields(header);
    CHECK_EQ(names.size(), size_t(7 + GpuPassCount + 2));
    CHECK(names[0] == "t_ms");
    CHECK(names[2] == "source");
    CHECK(names[6] == "gen");
    for (uint32_t i = 0; i < GpuPassCount; i++) {
        CHECK(names[7 + i] == gpuPassName(GpuPass(i)));
    }

    CHECK(names[names.size() - 2] == "gpu_ms");

    GpuRow row;
    row.tMs = 1234.5;
    row.row = 7;
    row.present = true;
    row.presentIndex = 42;
    row.gameFrame = 21;
    row.presentId = 99;
    row.generated = true;
    row.passMs[uint32_t(GpuPass::Crt)] = 0.25;
    row.gpuMs = 0.5;
    const std::string line = gpuCsvRow(row);
    CHECK_EQ(csvColumns(line), names.size());
    const std::vector<std::string> fields = csvFields(line);
    CHECK(fields[0] == "1234.500");
    CHECK(fields[1] == "7");
    CHECK(fields[2] == "present");
    CHECK(fields[3] == "42");
    CHECK(fields[4] == "21");
    CHECK(fields[5] == "99");
    CHECK(fields[6] == "1");
    CHECK(fields[7 + uint32_t(GpuPass::Crt)] == "0.2500");

    row.present = false;
    row.generated = false;
    const std::vector<std::string> renderFields = csvFields(gpuCsvRow(row));
    CHECK(renderFields[2] == "render");
    CHECK(renderFields[6] == "0");
}

TEST_CASE("perf: CPU rows match their header and keep the CSV intact") {
    CHECK(cpuCsvHeader() == "t_ms,phase,ms,game_frame,present");
    const std::string line = cpuCsvRow(10.5, "present_call", 0.125, 3, 9);
    CHECK(line == "10.500,present_call,0.1250,3,9");
    // Names that would break the CSV are sanitised; negative durations clamp to 0.
    CHECK(cpuCsvRow(1.0, "a,b\"c", -2.0, 0, 0) == "1.000,a_b_c,0.0000,0,0");
    CHECK(cpuCsvRow(1.0, nullptr, 1.0, 0, 0) == "1.000,unknown,1.0000,0,0");
}
