//
// RT64
//

#pragma once

#include <atomic>
#include <cstdlib>

namespace RT64 {
    // [wcw] The graphics menu's "2D Texture Edges" option: Original (point-sampled art as the N64 draws it) or Smooth
    // (sharp bilinear, shared/rt64_wcw_sharp_texel.h). Read by the framebuffer renderer into FrameParams every frame,
    // so it takes effect on the next frame. WCW_SMOOTH_2D=0/1 overrides it.
    struct WcwTextureEdges {
        static std::atomic<bool> &flag() {
            static std::atomic<bool> value(false);
            return value;
        }

        static void setSmooth(bool smooth) {
            flag() = smooth;
        }

        static bool smooth() {
            static const char *envText = getenv("WCW_SMOOTH_2D");
            return (envText != nullptr) ? (envText[0] == '1') : flag().load();
        }
    };
};
