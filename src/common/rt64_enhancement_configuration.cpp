//
// RT64
//

#include "rt64_enhancement_configuration.h"

namespace RT64 {
    // EnhancementConfiguration
    
    EnhancementConfiguration::EnhancementConfiguration() {
        framebuffer.reinterpretFixULS = true;
        // [wcw fix] was SkipBuffering. SkipBuffering redirects the present to the framebuffer
        // the game is CURRENTLY drawing into (latency optimization, assumes one gfx task per
        // visual frame). WCW builds each frame from MULTIPLE gfx tasks (separate clear + draw
        // tasks, up to ~4-5/frame in cinematics), so SkipBuffering presented the buffer between
        // the clear-task and the draw-tasks -> 1 in 3 presents was pure black (measured via
        // swapchain readback; visible as constant black flicker). Console mode presents the VI
        // origin buffer, which the game only swaps in once its content is final.
        presentation.mode = Presentation::Mode::Console;
        presentation.removeBlackBorders = true;
        rect.fixRectLR = true;
        f3dex.forceBranch = false;
        s2dex.fixBilerpMismatch = true;
        s2dex.framebufferFastPath = true;
        textureLOD.scale = false;
    }
};