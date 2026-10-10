//
// RT64
//

#pragma once

#include "common/rt64_plume.h"

namespace RT64 {
    // [wcw hdr] Optional HDR presentation. When enabled (and Windows HDR is on for the window's
    // display), the swap chain is created as scRGB: R16G16B16A16_FLOAT, linear, 1.0 = 80 nits.
    // Everything drawn to it converts its SDR (sRGB) output to linear light at the paper white
    // brightness (sdrScale). Turning HDR on or off takes effect on the next launch; the
    // brightness values apply immediately.
    struct HdrOutput {
        struct Settings {
            bool enabled = false;
            // Brightness of SDR white (menus, the game picture), in nits.
            float paperWhiteNits = 200.0f;
        };

        struct DisplayInfo {
            bool hdrEnabled = false;
            float maxLuminance = 0.0f;
            float maxFullFrameLuminance = 0.0f;
        };

        // Thread-safe. WCW_HDR=1/0 overrides the enabled state.
        static void setUserSettings(const Settings &settings);
        static Settings userSettings();

        // Whether Windows HDR is on for the display showing the window (Windows only).
        static DisplayInfo queryDisplay(RenderWindow window);

        // Called once the swap chain exists.
        static void setActive(bool active);

        // True when the swap chain is scRGB.
        static bool active();

        // Scale from SDR linear [0, 1] to scRGB: paper white / 80 nits.
        static float sdrScale();

        // Format of the swap chain's color targets.
        static RenderFormat swapChainFormat();
    };
};
