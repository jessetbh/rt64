//
// RT64
//

#pragma once

#include "common/rt64_plume.h"

namespace RT64 {
    // [wcw diag] aki-roadmap#4: WCW_DIAG_SERIAL_GPU=1 keeps rt64's queues from running on the GPU at the same time. A
    // thread takes a process-wide lock before it submits and releases it after it waits for that submit (RenderWorker
    // does both; submits outside it call these). Waiting for the lock gives up after 500 ms (logged). Diagnostic only.
    void wcwSerialGpuBegin();
    void wcwSerialGpuEnd();

    struct RenderWorker {
        RenderDevice *device = nullptr;
        std::string name;
        std::unique_ptr<RenderCommandQueue> commandQueue;
        std::unique_ptr<RenderCommandList> commandList;
        std::unique_ptr<RenderCommandFence> commandFence;

        RenderWorker(RenderDevice *device, const std::string &name, RenderCommandListType commandListType);
        ~RenderWorker();
        void execute();
        void wait();
    };

    // RAII convenience class for aiding opening, closing and execution of command lists inside a scope.

    struct RenderWorkerExecution {
        RenderWorker *worker;

        RenderWorkerExecution(RenderWorker *worker);
        ~RenderWorkerExecution();
    };
};
