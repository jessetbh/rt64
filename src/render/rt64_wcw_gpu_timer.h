//
// RT64
//

#pragma once

#include <memory>
#include <vector>

#include "common/rt64_plume.h"
#include "common/rt64_wcw_perf.h"

// [wcw perf] GPU time per named pass (WCW_PERF_LOG=1, aki-roadmap#82). A timer brackets one command list: begin() after
// the list begins, WcwGpuScope markers while it records, finish() before it ends, and collect() once the GPU has run it.
// The workload and present threads already wait for each list's GPU work, so collect() reads finished queries and never
// stalls. Each recording thread has one active timer; scopes on that thread stamp into its command list only.
namespace RT64 {
    struct WcwGpuTimer {
        static constexpr uint32_t Capacity = 512;

        std::unique_ptr<RenderQueryPool> pool;
        RenderCommandList *commandList = nullptr;
        WcwPerf::MarkerBudget budget;
        std::vector<WcwPerf::Marker> markers;
        bool recorded = false;

        // Creates the query pool when WCW_PERF_LOG is set and the device supports timestamps.
        void setup(RenderDevice *device);
        bool available() const { return pool != nullptr; }

        // Starts timing commandList on this thread.
        void begin(RenderCommandList *commandList);

        // Closes every open pass and stops timing; call before the command list ends.
        void finish();

        // After the GPU finished the list: fills row.passMs, gpuMs and droppedMarkers. False if nothing was timed.
        bool collect(WcwPerf::GpuRow &row);

        void push(WcwPerf::GpuPass pass);
        void pop();

        // The timer recording on this thread for this command list, or null.
        static WcwGpuTimer *active(const RenderCommandList *commandList);
    };

    // Times a pass over a scope on the thread's active timer. No-op when perf logging is off.
    struct WcwGpuScope {
        WcwGpuTimer *timer;

        WcwGpuScope(const RenderCommandList *commandList, WcwPerf::GpuPass pass);
        ~WcwGpuScope();

        // Ends this pass and starts another in its place.
        void switchTo(WcwPerf::GpuPass pass);

        WcwGpuScope(const WcwGpuScope &) = delete;
        WcwGpuScope &operator=(const WcwGpuScope &) = delete;
    };
};
