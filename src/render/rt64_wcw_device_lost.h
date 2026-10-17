//
// RT64
//

#pragma once

#include "plume_render_interface.h"

#include "common/rt64_wcw_logic.h"

namespace RT64 {
    // [wcw] Whether a renderer thread skips the GPU work of its next piece of work: the device is lost
    // (plume::wcwDeviceLostFlag, see WcwLogic::deviceLostWork). Bookkeeping still runs so the quit is not blocked.
    inline bool wcwSkipGpuWork() {
        return WcwLogic::deviceLostWork(plume::wcwDeviceLostFlag().load()) == WcwLogic::DeviceLostWork::SkipGpuWork;
    }
};
