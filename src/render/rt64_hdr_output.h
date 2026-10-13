//
// RT64
//

#pragma once

#include "common/rt64_plume.h"
#include "common/rt64_wcw_logic.h"

namespace RT64 {
    // [wcw hdr] Optional HDR presentation. When enabled (and Windows HDR is on for the window's
    // display), the swap chain is created as scRGB: R16G16B16A16_FLOAT, linear, 1.0 = 80 nits.
    // Everything drawn to it converts its SDR (sRGB) output to linear light at the paper white
    // brightness (sdrScale), which the user's HDR Picture choice sets from the display's figures and
    // the CRT filter's state (WcwLogic::hdrPaperWhite). Turning HDR on or off takes effect on the next
    // launch; the picture choice applies immediately.
    struct HdrOutput {
        struct Settings {
            bool enabled = false;
            WcwLogic::HdrPicture picture = WcwLogic::HdrPicture::Auto;
            // Paper white for HdrPicture::Custom, in nits.
            float customNits = 200.0f;
        };

        struct DisplayInfo {
            bool hdrEnabled = false;
            float maxLuminance = 0.0f;
            float maxFullFrameLuminance = 0.0f;
            // Windows' "SDR content brightness" for the display, in nits (0 if unknown).
            float sdrWhiteNits = 0.0f;
        };

        // For the settings readout.
        struct Status {
            bool active = false;
            WcwLogic::HdrDisplay display;
            WcwLogic::HdrCrt crt;
            float paperWhiteNits = 0.0f;
        };

        // Thread-safe. Test overrides: WCW_HDR=1/0/force, WCW_HDR_PAPER=<nits> (replaces the picture choice).
        static void setUserSettings(const Settings &settings);
        static Settings userSettings();

        // Whether Windows HDR is on for the display showing the window, its peak brightness and
        // SDR white level (Windows only).
        static DisplayInfo queryDisplay(RenderWindow window);

        // Called once the swap chain exists, with the display it was created for.
        static void setActive(bool active, const DisplayInfo &display);

        // The CRT filter reports whether it's on, its preset's mask strength and the beam simulator,
        // which Auto and CRT detail use. Thread-safe; cheap when nothing changed.
        static void setCrtState(const WcwLogic::HdrCrt &crt);

        // Thread-safe snapshot of the display, CRT state and resulting paper white.
        static Status status();

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
