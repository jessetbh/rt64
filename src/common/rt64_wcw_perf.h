//
// RT64
//

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// [wcw perf] Performance instrumentation (WCW_PERF_LOG=1, aki-roadmap#82): GPU time per named render pass in
// wcw_gpu_passes.csv and CPU frame phases in wcw_cpu_phases.csv. The pass and phase names, the stamp budget, the
// per-pass time accounting and the CSV rows are pure logic, unit tested in tests/wcw; the sink at the bottom writes
// the files. Nothing here runs unless the variable is set.
namespace RT64::WcwPerf {
    // GPU passes. Each is one column of wcw_gpu_passes.csv, in this order. Time goes to the innermost open pass, so
    // nested passes don't count twice (the ray-traced shadow pass inside a raster pass counts only as rt_trace).
    enum class GpuPass : uint8_t {
        // GPU time between markers with no pass open.
        Other,
        // Buffer uploads and the RSP / vertex compute passes before the framebuffers.
        Setup,
        // Framebuffer operations: copies and reinterpretations between targets, reads from RDRAM.
        FbOps,
        // The raster passes of each framebuffer pair.
        Raster,
        // Ray tracing: BLAS/TLAS builds, the shadow and AO trace, the temporal resolve (denoise), the composite.
        RtBuild,
        RtTrace,
        RtDenoise,
        RtComposite,
        // Frame interpolation: copying the live target into the generated frames' targets before they're drawn.
        InterpPrime,
        // Present: resolving or downsampling the frame to show.
        Resolve,
        // Present: the VI blit, or the CRT filter in its place. HDR output is part of whichever ran.
        Vi,
        Crt,
        // Present: the UI (menus, overlays) and the debug inspector.
        Ui,
        // Present: diagnostic readbacks (WCW_PRESENT_LUM).
        Diag,
        Count
    };

    constexpr uint32_t GpuPassCount = uint32_t(GpuPass::Count);
    const char *gpuPassName(GpuPass pass);

    // CPU phases, one row each in wcw_cpu_phases.csv. The game-side phases come from ultramodern by name (see
    // cpuPhaseName for the list); these are the ones rt64 measures itself.
    enum class CpuPhase : uint8_t {
        // Workload thread: recording one rendered frame (real or generated), then waiting for its GPU work.
        Rt64Record,
        Rt64GpuWait,
        // Present thread, in order: the even-pacing wait between generated frames, acquiring the swap chain image,
        // recording the present, waiting for its GPU work, the sleep before present, the swap chain wait, Present.
        PacingWait,
        Acquire,
        PresentRecord,
        PresentGpuWait,
        PrePresentSleep,
        SwapChainWait,
        PresentCall,
        // The time from one Present call to the next.
        PresentInterval,
        Count
    };

    constexpr uint32_t CpuPhaseCount = uint32_t(CpuPhase::Count);
    const char *cpuPhaseName(CpuPhase phase);

    // The phase names ultramodern reports (game_frame, game_busy, rsp_audio, rsp_task, dl_submit). Unknown names
    // are written as they are.
    bool isKnownCpuPhaseName(const char *name);

    // A pass marker: a push opens a pass, a pop closes the innermost one. Each stamped marker has a GPU timestamp.
    struct Marker {
        GpuPass pass = GpuPass::Other;
        bool push = false;
    };

    // Keeps a frame's markers within the query pool's capacity: a push is stamped only if its pop, and the pops of
    // every pass still open, fit too. A pop is stamped only if its push was, so stamped markers always balance.
    class MarkerBudget {
    public:
        explicit MarkerBudget(uint32_t capacity = 0);
        void reset(uint32_t capacity);
        // Whether to stamp this push / pop.
        bool push();
        bool pop();
        uint32_t used() const { return usedStamps; }
        // Passes still open, stamped or not.
        uint32_t depth() const { return uint32_t(stack.size()); }
        // Pushes refused for lack of room since the last reset.
        uint32_t dropped() const { return droppedPushes; }
    private:
        uint32_t capacity = 0;
        uint32_t usedStamps = 0;
        uint32_t openStamped = 0;
        uint32_t droppedPushes = 0;
        std::vector<bool> stack;
    };

    // Splits the time between stamps over the passes: each interval goes to the innermost pass open at its start
    // (Other when none is). Pops with nothing open are ignored; stamps that go backwards count as 0 ms. stampsNs[i]
    // is the GPU time of markers[i] in nanoseconds. Returns the span from the first to the last stamp, in ms.
    double exclusivePassTimes(const Marker *markers, const uint64_t *stampsNs, size_t count, std::array<double, GpuPassCount> &outMs);

    // wcw_gpu_passes.csv: one row per rendered frame (source "render") and per present (source "present").
    struct GpuRow {
        double tMs = 0.0;
        uint64_t row = 0;
        bool present = false;
        uint64_t presentIndex = 0;
        uint64_t gameFrame = 0;
        uint64_t presentId = 0;
        bool generated = false;
        std::array<double, GpuPassCount> passMs = {};
        double gpuMs = 0.0;
        uint32_t droppedMarkers = 0;
    };

    std::string gpuCsvHeader();
    std::string gpuCsvRow(const GpuRow &row);

    // wcw_cpu_phases.csv: one row per phase occurrence (start time, duration, the game frame and present counts).
    std::string cpuCsvHeader();
    std::string cpuCsvRow(double startMs, const char *phase, double durationMs, uint64_t gameFrame, uint64_t presentIndex);

    // Sink (not pure). WCW_PERF_LOG=1 enables it; the files go to the working directory.
    bool enabled();
    using Clock = std::chrono::steady_clock;
    // Milliseconds since the steady clock's epoch, the time base of the other wcw_*.csv logs.
    double clockMs(Clock::time_point t);
    void cpuPhase(CpuPhase phase, Clock::time_point start, Clock::time_point end);
    void cpuPhaseNamed(const char *phase, Clock::time_point start, Clock::time_point end);
    // The ultramodern hook's signature: a phase name and its start and end in steady clock nanoseconds.
    void cpuPhaseNs(const char *phase, int64_t startNs, int64_t endNs);
    // Counts a Present call: the present index the rows carry.
    uint64_t notePresent();
    uint64_t presentCount();
    uint64_t gameFrameCount();
    void gpuRow(GpuRow row);
    void flush();

    // Times one CPU phase over a scope.
    struct CpuScope {
        CpuPhase phase;
        Clock::time_point start;
        bool active;
        explicit CpuScope(CpuPhase phase) : phase(phase), active(enabled()) {
            if (active) {
                start = Clock::now();
            }
        }

        ~CpuScope() {
            if (active) {
                cpuPhase(phase, start, Clock::now());
            }
        }
    };
};
