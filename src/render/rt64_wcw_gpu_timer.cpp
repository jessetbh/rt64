//
// RT64
//

#include "rt64_wcw_gpu_timer.h"

namespace RT64 {
    static thread_local WcwGpuTimer *activeTimer = nullptr;

    void WcwGpuTimer::setup(RenderDevice *device) {
        if (!WcwPerf::enabled() || (device == nullptr) || !device->getCapabilities().queryPools) {
            return;
        }

        pool = device->createQueryPool(Capacity);
        markers.reserve(Capacity);
    }

    void WcwGpuTimer::begin(RenderCommandList *commandList) {
        recorded = false;
        if (pool == nullptr) {
            return;
        }

        this->commandList = commandList;
        markers.clear();
        budget.reset(Capacity);
        commandList->resetQueryPool(pool.get(), 0, Capacity);
        activeTimer = this;
    }

    void WcwGpuTimer::push(WcwPerf::GpuPass pass) {
        if (budget.push()) {
            commandList->writeTimestamp(pool.get(), uint32_t(markers.size()));
            markers.push_back({ pass, true });
        }
    }

    void WcwGpuTimer::pop() {
        if (budget.pop()) {
            commandList->writeTimestamp(pool.get(), uint32_t(markers.size()));
            markers.push_back({ WcwPerf::GpuPass::Other, false });
        }
    }

    void WcwGpuTimer::swap(WcwPerf::GpuPass pass) {
        if (budget.swap()) {
            commandList->writeTimestamp(pool.get(), uint32_t(markers.size()));
            markers.push_back({ pass, false, true });
        }
    }

    void WcwGpuTimer::finish() {
        if ((pool == nullptr) || (activeTimer != this)) {
            return;
        }

        // Close what a scope left open (none should be).
        while (budget.depth() > 0) {
            pop();
        }

        activeTimer = nullptr;
        commandList = nullptr;
        recorded = (markers.size() >= 2);
    }

    bool WcwGpuTimer::collect(WcwPerf::GpuRow &row) {
        if (!recorded) {
            return false;
        }

        recorded = false;
        pool->queryResults();
        row.gpuMs = WcwPerf::exclusivePassTimes(markers.data(), pool->getResults(), markers.size(), row.passMs);
        row.droppedMarkers = budget.dropped();
        return true;
    }

    WcwGpuTimer *WcwGpuTimer::active(const RenderCommandList *commandList) {
        WcwGpuTimer *timer = activeTimer;
        return ((timer != nullptr) && (timer->commandList == commandList)) ? timer : nullptr;
    }

    WcwGpuScope::WcwGpuScope(const RenderCommandList *commandList, WcwPerf::GpuPass pass) {
        timer = WcwPerf::enabled() ? WcwGpuTimer::active(commandList) : nullptr;
        if (timer != nullptr) {
            timer->push(pass);
        }
    }

    WcwGpuScope::~WcwGpuScope() {
        // The timer may have finished while this scope was open; its pops were already written.
        if ((timer != nullptr) && (WcwGpuTimer::active(timer->commandList) == timer)) {
            timer->pop();
        }
    }

    void WcwGpuScope::switchTo(WcwPerf::GpuPass pass) {
        if ((timer != nullptr) && (WcwGpuTimer::active(timer->commandList) == timer)) {
            timer->swap(pass);
        }
    }
};
