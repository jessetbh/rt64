//
// RT64
//

#include "rt64_wcw_perf.h"

#include "rt64_wcw_logic.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace RT64::WcwPerf {
    const char *gpuPassName(GpuPass pass) {
        switch (pass) {
        case GpuPass::Other: return "other";
        case GpuPass::Setup: return "setup";
        case GpuPass::FbOps: return "fb_ops";
        case GpuPass::Raster: return "raster";
        case GpuPass::RtBuild: return "rt_build";
        case GpuPass::RtTrace: return "rt_trace";
        case GpuPass::RtDenoise: return "rt_denoise";
        case GpuPass::RtComposite: return "rt_composite";
        case GpuPass::InterpPrime: return "interp_prime";
        case GpuPass::Resolve: return "resolve";
        case GpuPass::Vi: return "vi";
        case GpuPass::Crt: return "crt";
        case GpuPass::Ui: return "ui";
        case GpuPass::Diag: return "diag";
        default: return "unknown";
        }
    }

    const char *cpuPhaseName(CpuPhase phase) {
        switch (phase) {
        case CpuPhase::Rt64Record: return "rt64_record";
        case CpuPhase::Rt64GpuWait: return "rt64_gpu_wait";
        case CpuPhase::PacingWait: return "pacing_wait";
        case CpuPhase::Acquire: return "acquire";
        case CpuPhase::PresentRecord: return "present_record";
        case CpuPhase::PresentGpuWait: return "present_gpu_wait";
        case CpuPhase::PrePresentSleep: return "pre_present_sleep";
        case CpuPhase::SwapChainWait: return "swapchain_wait";
        case CpuPhase::PresentCall: return "present_call";
        case CpuPhase::PresentInterval: return "present_interval";
        default: return "unknown";
        }
    }

    bool isKnownCpuPhaseName(const char *name) {
        static const char *gameNames[] = { "game_frame", "game_busy", "rsp_audio", "rsp_task", "dl_submit" };
        if (name == nullptr) {
            return false;
        }

        for (const char *n : gameNames) {
            if (strcmp(n, name) == 0) {
                return true;
            }
        }

        for (uint32_t i = 0; i < CpuPhaseCount; i++) {
            if (strcmp(cpuPhaseName(CpuPhase(i)), name) == 0) {
                return true;
            }
        }

        return false;
    }

    MarkerBudget::MarkerBudget(uint32_t capacity) {
        reset(capacity);
    }

    void MarkerBudget::reset(uint32_t capacity) {
        this->capacity = capacity;
        usedStamps = 0;
        openStamped = 0;
        droppedPushes = 0;
        stack.clear();
    }

    bool MarkerBudget::push() {
        // Room for this push, its pop and the pops of every stamped pass still open.
        const bool stamp = (usedStamps + 2 + openStamped) <= capacity;
        stack.push_back(stamp);
        if (stamp) {
            usedStamps++;
            openStamped++;
        }
        else {
            droppedPushes++;
        }

        return stamp;
    }

    bool MarkerBudget::pop() {
        if (stack.empty()) {
            return false;
        }

        const bool stamp = stack.back();
        stack.pop_back();
        if (stamp) {
            usedStamps++;
            openStamped--;
        }

        return stamp;
    }

    bool MarkerBudget::swap() {
        if (stack.empty()) {
            return push();
        }

        const bool stamp = stack.back() && ((usedStamps + 1 + openStamped) <= capacity);
        if (stamp) {
            usedStamps++;
        }
        else if (stack.back()) {
            droppedPushes++;
        }

        return stamp;
    }

    double exclusivePassTimes(const Marker *markers, const uint64_t *stampsNs, size_t count, std::array<double, GpuPassCount> &outMs) {
        outMs.fill(0.0);
        if (count < 2) {
            return 0.0;
        }

        std::vector<GpuPass> open;
        for (size_t i = 0; i + 1 < count; i++) {
            const Marker &m = markers[i];
            if (m.swap) {
                if (open.empty()) {
                    open.push_back(m.pass);
                }
                else {
                    open.back() = m.pass;
                }
            }
            else if (m.push) {
                open.push_back(m.pass);
            }
            else if (!open.empty()) {
                open.pop_back();
            }

            const GpuPass current = open.empty() ? GpuPass::Other : open.back();
            const uint64_t a = stampsNs[i];
            const uint64_t b = stampsNs[i + 1];
            const double ms = (b > a) ? double(b - a) / 1000000.0 : 0.0;
            const uint32_t column = std::min(uint32_t(current), GpuPassCount - 1);
            outMs[column] += ms;
        }

        const uint64_t first = stampsNs[0];
        const uint64_t last = stampsNs[count - 1];
        return (last > first) ? double(last - first) / 1000000.0 : 0.0;
    }

    std::string gpuCsvHeader() {
        std::string h = "t_ms,row,source,present,game_frame,present_id,gen";
        for (uint32_t i = 0; i < GpuPassCount; i++) {
            h += ",";
            h += gpuPassName(GpuPass(i));
        }

        h += ",gpu_ms,dropped";
        return h;
    }

    std::string gpuCsvRow(const GpuRow &row) {
        char buf[64];
        std::string s;
        snprintf(buf, sizeof(buf), "%.3f,%llu,%s,%llu,%llu,%llu,%d", row.tMs, (unsigned long long)row.row, row.present ? "present" : "render",
            (unsigned long long)row.presentIndex, (unsigned long long)row.gameFrame, (unsigned long long)row.presentId, row.generated ? 1 : 0);
        s += buf;
        for (uint32_t i = 0; i < GpuPassCount; i++) {
            snprintf(buf, sizeof(buf), ",%.4f", row.passMs[i]);
            s += buf;
        }

        snprintf(buf, sizeof(buf), ",%.4f,%u", row.gpuMs, row.droppedMarkers);
        s += buf;
        return s;
    }

    std::string cpuCsvHeader() {
        return "t_ms,phase,ms,game_frame,present";
    }

    std::string cpuCsvRow(double startMs, const char *phase, double durationMs, uint64_t gameFrame, uint64_t presentIndex) {
        // Phase names are identifiers; anything that would break the CSV becomes '_'.
        std::string name = (phase != nullptr) ? phase : "unknown";
        for (char &c : name) {
            if ((c == ',') || (c == '"') || (c == '\n') || (c == '\r')) {
                c = '_';
            }
        }

        char buf[128];
        snprintf(buf, sizeof(buf), "%.3f,%s,%.4f,%llu,%llu", startMs, name.c_str(), std::max(durationMs, 0.0),
            (unsigned long long)gameFrame, (unsigned long long)presentIndex);
        return buf;
    }

    // Sink.

    namespace {
        struct Sink {
            std::mutex mutex;
            FILE *gpuFile = nullptr;
            FILE *cpuFile = nullptr;
            bool opened = false;
            Clock::time_point lastFlush;
        };

        Sink &sink() {
            static Sink s;
            return s;
        }

        std::atomic<uint64_t> presents{ 0 };
        std::atomic<uint64_t> gameFrames{ 0 };
        std::atomic<uint64_t> gpuRows{ 0 };

        void flushAtExit() {
            flush();
        }

        // Opens both files on first use; the caller holds the mutex.
        void openFiles(Sink &s) {
            if (s.opened) {
                return;
            }

            s.opened = true;
            s.gpuFile = fopen("wcw_gpu_passes.csv", "w");
            s.cpuFile = fopen("wcw_cpu_phases.csv", "w");
            for (FILE *f : { s.gpuFile, s.cpuFile }) {
                if (f != nullptr) {
                    setvbuf(f, nullptr, _IOFBF, 1 << 20);
                }
            }

            if (s.gpuFile != nullptr) {
                fprintf(s.gpuFile, "%s\n", gpuCsvHeader().c_str());
            }

            if (s.cpuFile != nullptr) {
                fprintf(s.cpuFile, "%s\n", cpuCsvHeader().c_str());
            }

            s.lastFlush = Clock::now();
            atexit(flushAtExit);
            fprintf(stderr, "[wcw perf] WCW_PERF_LOG: writing wcw_gpu_passes.csv and wcw_cpu_phases.csv\n");
        }

        // Flushes about once a second so a run that ends abruptly keeps most of its rows; the caller holds the mutex.
        void maybeFlush(Sink &s) {
            const Clock::time_point now = Clock::now();
            if ((now - s.lastFlush) >= std::chrono::seconds(1)) {
                s.lastFlush = now;
                if (s.gpuFile != nullptr) {
                    fflush(s.gpuFile);
                }

                if (s.cpuFile != nullptr) {
                    fflush(s.cpuFile);
                }
            }
        }
    }

    bool enabled() {
        static const bool on = []() {
            const char *e = getenv("WCW_PERF_LOG");
            return (e != nullptr) && (e[0] != '\0') && (strcmp(e, "0") != 0);
        }();
        return on;
    }

    bool timingEnabled() {
        static const bool on = enabled() || WcwLogic::envSwitchOn(getenv("WCW_DD_METRICS"));
        return on;
    }

    double clockMs(Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(t.time_since_epoch()).count();
    }

    void cpuPhaseNamed(const char *phase, Clock::time_point start, Clock::time_point end) {
        if (!enabled()) {
            return;
        }

        if ((phase != nullptr) && (strcmp(phase, "game_frame") == 0)) {
            gameFrames.fetch_add(1, std::memory_order_relaxed);
        }

        const std::string line = cpuCsvRow(clockMs(start), phase, std::chrono::duration<double, std::milli>(end - start).count(),
            gameFrames.load(std::memory_order_relaxed), presents.load(std::memory_order_relaxed));
        Sink &s = sink();
        std::scoped_lock lock(s.mutex);
        openFiles(s);
        if (s.cpuFile != nullptr) {
            fputs(line.c_str(), s.cpuFile);
            fputc('\n', s.cpuFile);
        }

        maybeFlush(s);
    }

    void cpuPhase(CpuPhase phase, Clock::time_point start, Clock::time_point end) {
        cpuPhaseNamed(cpuPhaseName(phase), start, end);
    }

    void cpuPhaseNs(const char *phase, int64_t startNs, int64_t endNs) {
        cpuPhaseNamed(phase, Clock::time_point(std::chrono::nanoseconds(startNs)), Clock::time_point(std::chrono::nanoseconds(endNs)));
    }

    uint64_t notePresent() {
        return presents.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint64_t presentCount() {
        return presents.load(std::memory_order_relaxed);
    }

    uint64_t gameFrameCount() {
        return gameFrames.load(std::memory_order_relaxed);
    }

    void gpuRow(GpuRow row) {
        if (!enabled()) {
            return;
        }

        row.row = gpuRows.fetch_add(1, std::memory_order_relaxed);
        row.gameFrame = gameFrames.load(std::memory_order_relaxed);
        const std::string line = gpuCsvRow(row);
        Sink &s = sink();
        std::scoped_lock lock(s.mutex);
        openFiles(s);
        if (s.gpuFile != nullptr) {
            fputs(line.c_str(), s.gpuFile);
            fputc('\n', s.gpuFile);
        }

        maybeFlush(s);
    }

    void flush() {
        Sink &s = sink();
        std::scoped_lock lock(s.mutex);
        if (s.gpuFile != nullptr) {
            fflush(s.gpuFile);
        }

        if (s.cpuFile != nullptr) {
            fflush(s.cpuFile);
        }
    }
};
