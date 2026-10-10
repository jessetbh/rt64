//
// RT64
//

#pragma once

#include "common/rt64_plume.h"

namespace RT64 {
    // [wcw hdr] Optional HDR presentation. When enabled (and Windows HDR is on for the window's
    // display), the swap chain is created as scRGB: R16G16B16A16_FLOAT, linear, 1.0 = 80 nits.
    // Everything drawn to it converts its SDR (sRGB) output to linear light at the paper white
    // brightness (sdrScale), which follows Windows' "SDR content brightness" setting for the
    // display times the user's HDR Brightness. Turning HDR on or off takes effect on the next
    // launch; brightness applies immediately.
    struct HdrOutput {
        struct Settings {
            bool enabled = false;
            // Multiplier on Windows' SDR content brightness (1.0 = match the desktop).
            float brightness = 1.0f;
        };

        struct DisplayInfo {
            bool hdrEnabled = false;
            float maxLuminance = 0.0f;
            float maxFullFrameLuminance = 0.0f;
            // Windows' "SDR content brightness" for the display, in nits (0 if unknown).
            float sdrWhiteNits = 0.0f;
        };

        // Thread-safe. Test overrides: WCW_HDR=1/0/force, WCW_HDR_PAPER=<nits>.
        static void setUserSettings(const Settings &settings);
        static Settings userSettings();

        // Whether Windows HDR is on for the display showing the window, its peak brightness and
        // SDR white level (Windows only).
        static DisplayInfo queryDisplay(RenderWindow window);

        // Called once the swap chain exists, with the display it was created for.
        static void setActive(bool active, const DisplayInfo &display);

        // True when the swap chain is scRGB.
        static bool active();

        // Peak brightness the display reports, in nits (0 if unknown).
        static float peakNits();

        // Paper white in nits: the brightness of SDR white (game picture, menus).
        static float paperWhiteNits();

        // Scale from SDR linear [0, 1] to scRGB: paper white / 80 nits.
        static float sdrScale();

        // Format of the swap chain's color targets.
        static RenderFormat swapChainFormat();
    };
};
