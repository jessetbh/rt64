//
// RT64
//

#include "rt64_hdr_output.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#   include <dxgi1_6.h>
#endif

namespace RT64 {
    static std::mutex hdrSettingsMutex;
    static HdrOutput::Settings hdrSettings;
    static std::atomic<bool> hdrActive = false;
    static std::atomic<float> hdrScale = 2.5f;

    // Test overrides: WCW_HDR=1/0 (enabled), WCW_HDR=force (scRGB even with Windows HDR off; Windows
    // composites it onto the SDR desktop), WCW_HDR_PAPER=<nits>.
    static bool envForced() {
        const char *env = getenv("WCW_HDR");
        return (env != nullptr) && (strcmp(env, "force") == 0);
    }

    void HdrOutput::setUserSettings(const Settings &settings) {
        std::scoped_lock lock(hdrSettingsMutex);
        hdrSettings = settings;
        float paperWhite = settings.paperWhiteNits;
        if (const char *env = getenv("WCW_HDR_PAPER")) {
            paperWhite = float(atof(env));
        }

        hdrScale = std::clamp(paperWhite, 80.0f, 1000.0f) / 80.0f;
    }

    HdrOutput::Settings HdrOutput::userSettings() {
        Settings settings;
        {
            std::scoped_lock lock(hdrSettingsMutex);
            settings = hdrSettings;
        }

        if (const char *env = getenv("WCW_HDR")) {
            settings.enabled = (strcmp(env, "0") != 0);
        }

        if (const char *env = getenv("WCW_HDR_PAPER")) {
            settings.paperWhiteNits = float(atof(env));
        }

        return settings;
    }

    HdrOutput::DisplayInfo HdrOutput::queryDisplay(RenderWindow window) {
        DisplayInfo info;
#   if defined(_WIN32)
        const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        IDXGIFactory1 *factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return info;
        }

        bool found = false;
        IDXGIAdapter1 *adapter = nullptr;
        for (UINT a = 0; !found && (factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND); a++) {
            IDXGIOutput *output = nullptr;
            for (UINT o = 0; !found && (adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND); o++) {
                IDXGIOutput6 *output6 = nullptr;
                DXGI_OUTPUT_DESC1 desc = {};
                if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))) && SUCCEEDED(output6->GetDesc1(&desc)) && (desc.Monitor == monitor)) {
                    found = true;
                    info.hdrEnabled = (desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                    info.maxLuminance = desc.MaxLuminance;
                    info.maxFullFrameLuminance = desc.MaxFullFrameLuminance;
                }

                if (output6 != nullptr) {
                    output6->Release();
                }

                output->Release();
            }

            adapter->Release();
        }

        factory->Release();
#   endif
        if (envForced()) {
            info.hdrEnabled = true;
        }

        return info;
    }

    void HdrOutput::setActive(bool active) {
        hdrActive = active;
    }

    bool HdrOutput::active() {
        return hdrActive;
    }

    float HdrOutput::sdrScale() {
        return hdrScale;
    }

    RenderFormat HdrOutput::swapChainFormat() {
        return hdrActive ? RenderFormat::R16G16B16A16_FLOAT : RenderFormat::B8G8R8A8_UNORM;
    }
};
