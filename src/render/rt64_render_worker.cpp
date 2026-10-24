//
// RT64
//

#include "rt64_render_worker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace RT64 {
    // [wcw diag] Serialized GPU submissions (aki-roadmap#4).

    static std::timed_mutex wcwSerialGpuMutex;
    static thread_local bool wcwSerialGpuHeld = false;

    static bool wcwSerialGpuEnabled() {
        static const bool enabled = []() {
            const bool on = (getenv("WCW_DIAG_SERIAL_GPU") != nullptr);
            if (on) {
                fprintf(stderr, "[wcw diag4] serial GPU submissions on\n");
            }

            return on;
        }();

        return enabled;
    }

    void wcwSerialGpuBegin() {
        if (!wcwSerialGpuEnabled() || wcwSerialGpuHeld) {
            return;
        }

        if (wcwSerialGpuMutex.try_lock_for(std::chrono::milliseconds(500))) {
            wcwSerialGpuHeld = true;
        }
        else {
            static std::atomic<uint32_t> timeouts{ 0 };
            const uint32_t count = ++timeouts;
            if ((count <= 10) || ((count % 100) == 0)) {
                fprintf(stderr, "[wcw diag4] serial GPU lock timed out (%u)\n", count);
            }
        }
    }

    void wcwSerialGpuEnd() {
        if (wcwSerialGpuHeld) {
            wcwSerialGpuHeld = false;
            wcwSerialGpuMutex.unlock();
        }
    }

    // RenderWorker

    RenderWorker::RenderWorker(RenderDevice *device, const std::string &name, RenderCommandListType commandListType) {
        assert(device != nullptr);

        this->device = device;
        this->name = name;

        commandQueue = device->createCommandQueue(commandListType);
        commandList = commandQueue->createCommandList();
        // [wcw] Named so a device-loss report (WCW_DRED) says which worker's commands hung the GPU.
        commandQueue->setName(name + " queue");
        commandList->setName(name);
        commandFence = device->createCommandFence();
    }

    RenderWorker::~RenderWorker() { }

    void RenderWorker::execute() {
        wcwSerialGpuBegin();
        commandQueue->executeCommandLists(commandList.get(), commandFence.get());
    }

    void RenderWorker::wait() {
        commandQueue->waitForCommandFence(commandFence.get());
        wcwSerialGpuEnd();
    }

    // RenderWorkerExecution

    RenderWorkerExecution::RenderWorkerExecution(RenderWorker *worker) {
        assert(worker != nullptr);

        this->worker = worker;
        worker->commandList->begin();
    }

    RenderWorkerExecution::~RenderWorkerExecution() {
        worker->commandList->end();
        worker->execute();
        worker->wait();
    }
};