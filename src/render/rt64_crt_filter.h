//
// RT64
//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/rt64_plume.h"

#include "rt64_vi_renderer.h"

namespace RT64 {
    struct RenderWorker;

    // [wcw crt] Optional CRT presentation filter: runs a RetroArch slang shader preset
    // (crt-guest-advanced-ntsc, ...) through librashader in place of the VI blit. librashader.dll
    // is loaded on first use; the presets and shaders ship next to the executable in crt/ (see
    // contrib/crt/NOTICE.md).
    //
    // The frame RT64 rendered (usually supersampled) is first box-filtered down to the VI's
    // scanline space (e.g. 320x240) so the shaders see a native-resolution source with one row
    // per scanline, then the preset draws it into the TV rectangle of the swap chain. UI drawn
    // after the present (menus, inspector) stays unfiltered.
    //
    // With the beam simulator on (Blur Busters CRT Beam Simulator, 120 Hz+ displays), the preset
    // draws into an offscreen copy of the window instead, and every display refresh presents one
    // sub-frame of a simulated 60 Hz rolling scan of it. The present queue keeps presenting those
    // sub-frames (renderBeamSubframe) while it waits for the game's next frame. A new game frame
    // takes effect when the simulated scan restarts at the top, like a VI framebuffer swap.
    struct CrtFilter {
        struct Settings {
            bool enabled = false;
            // Preset name (crt/presets/<name>.slangp next to the executable) or a path to a .slangp.
            std::string preset;
            // Source width = scanlines * aspect * this (1 = 320 for 4:3, 2 = 640).
            uint32_t sourceWidthScale = 1;
            // Shader parameter overrides applied on top of the preset.
            std::vector<std::pair<std::string, float>> parameters;
            bool beamSimulator = false;
        };

        struct RenderParams {
            RenderWorker *presentWorker = nullptr;
            RenderTexture *swapChainTexture = nullptr;
            const RenderFramebuffer *swapChainFramebuffer = nullptr;
            // Describes the input frame; outputWidth/outputHeight are filled in by the filter.
            VIRenderer::RenderParams vi;
        };

        // Thread-safe; picked up on the next present. WCW_CRT=<preset> overrides it
        // (WCW_CRT_BEAM=1 adds the beam simulator).
        static void setUserSettings(const Settings &settings);
        static Settings userSettings();

        CrtFilter(RenderInterface *renderInterface, RenderDevice *device, RenderShaderFormat shaderFormat);
        ~CrtFilter();

        // True when the filter should replace the VI blit this frame.
        bool active();

        // Display conditions the beam simulator depends on, updated by the present queue before
        // each present: it needs vsync and a display of at least twice the simulated CRT's rate. The
        // CRT runs at 60 Hz, or at the interpolated framerate (contentRate, e.g. 120) when the display
        // is fast enough for that, so interpolated frames each get a scan of their own.
        void setDisplayConditions(uint32_t refreshRate, bool vsyncEnabled, bool interpolating, uint32_t contentRate);

        // True when the beam simulator is running, so the present queue should present a sub-frame
        // every display refresh (renderBeamSubframe) instead of waiting for the game's next frame.
        bool beamActive();

        // Draws the frame. Returns false (having recorded nothing) when the preset can't be used,
        // in which case the caller falls back to the regular VI blit.
        bool render(VIRenderer &viRenderer, const RenderParams &p);

        // Draws the next beam simulator sub-frame of the last frame into the swap chain. Returns
        // false (having recorded nothing) when there is nothing to present.
        bool renderBeamSubframe(RenderWorker *presentWorker, RenderTexture *swapChainTexture, const RenderFramebuffer *swapChainFramebuffer, uint32_t windowWidth, uint32_t windowHeight);

        // True when renderBeamSubframe has a frame to draw at this window size.
        bool hasBeamFrame(uint32_t windowWidth, uint32_t windowHeight);

        // Display refreshes that went by without a beam sub-frame (a present that missed its vblank);
        // keeps the simulated scan in phase with the display.
        void skipBeamRefreshes(uint32_t count);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
};
