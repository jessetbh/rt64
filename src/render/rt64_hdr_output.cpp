//
// RT64
//

#include "rt64_hdr_output.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#   include <dxgi1_6.h>
#endif

namespace RT64 {
    static std::mutex hdrSettingsMutex;
    static HdrOutput::Settings hdrSettings;
    static WcwLogic::HdrDisplay hdrDisplay;
    static WcwLogic::HdrCrt hdrCrt;
    static std::atomic<bool> hdrActive = false;
    static std::atomic<float> hdrPaperWhite = WcwLogic::HdrUnknownSdrWhiteNits;
    static std::atomic<float> hdrPeakNits = 0.0f;

    static bool envForced() {
        const char *env = getenv("WCW_HDR");
        return (env != nullptr) && (strcmp(env, "force") == 0);
    }

    // Paper white from the picture choice, the display and the CRT filter (WcwLogic::hdrPaperWhite). Call with the lock held.
    static void updatePaperWhite() {
        float paperWhite = WcwLogic::hdrPaperWhite(hdrSettings.picture, hdrSettings.customNits, hdrDisplay, hdrCrt);
        if (const char *env = getenv("WCW_HDR_PAPER")) {
            paperWhite = std::clamp(float(atof(env)), WcwLogic::HdrMinPaperWhiteNits, WcwLogic::HdrMaxPaperWhiteNits);
        }

        if (std::abs(paperWhite - hdrPaperWhite.load()) > 0.5f) {
            hdrPaperWhite = paperWhite;
            if (hdrActive) {
                fprintf(stderr, "[wcw hdr] Paper white %.0f nits (picture %u, CRT %s, beam %s).\n", paperWhite, uint32_t(hdrSettings.picture),
                    hdrCrt.on ? "on" : "off", hdrCrt.beam ? "on" : "off");
            }
        }
    }

    void HdrOutput::setUserSettings(const Settings &settings) {
        std::scoped_lock lock(hdrSettingsMutex);
        hdrSettings = settings;
        updatePaperWhite();
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

        return settings;
    }

#   if defined(_WIN32)
    // Windows' "SDR content brightness" for the monitor (DISPLAYCONFIG_SDR_WHITE_LEVEL), in nits.
    static float querySdrWhiteNits(HMONITOR monitor) {
        MONITORINFOEXW monitorInfo = {};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!GetMonitorInfoW(monitor, &monitorInfo)) {
            return 0.0f;
        }

        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
            return 0.0f;
        }

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
            return 0.0f;
        }

        for (UINT32 i = 0; i < pathCount; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
            sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = paths[i].sourceInfo.adapterId;
            sourceName.header.id = paths[i].sourceInfo.id;
            if ((DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) || (wcscmp(sourceName.viewGdiDeviceName, monitorInfo.szDevice) != 0)) {
                continue;
            }

            DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel = {};
            whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            whiteLevel.header.size = sizeof(whiteLevel);
            whiteLevel.header.adapterId = paths[i].targetInfo.adapterId;
            whiteLevel.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&whiteLevel.header) == ERROR_SUCCESS) {
                // SDRWhiteLevel is in units of 1/1000 of 80 nits.
                return float(whiteLevel.SDRWhiteLevel) * 80.0f / 1000.0f;
            }
        }

        return 0.0f;
    }
#   endif

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
        info.sdrWhiteNits = querySdrWhiteNits(monitor);
#   endif
        if (envForced()) {
            info.hdrEnabled = true;
        }

        return info;
    }

    void HdrOutput::setActive(bool active, const DisplayInfo &display) {
        std::scoped_lock lock(hdrSettingsMutex);
        hdrActive = active;
        hdrPeakNits = display.maxLuminance;
        hdrDisplay.peakNits = display.maxLuminance;
        hdrDisplay.fullFrameNits = display.maxFullFrameLuminance;
        hdrDisplay.sdrWhiteNits = display.sdrWhiteNits;
        updatePaperWhite();
    }

    void HdrOutput::setCrtState(const WcwLogic::HdrCrt &crt) {
        std::scoped_lock lock(hdrSettingsMutex);
        if ((crt.on == hdrCrt.on) && (crt.beam == hdrCrt.beam) && (std::abs(crt.baseMaskStrength - hdrCrt.baseMaskStrength) < 1e-4f)) {
            return;
        }

        hdrCrt = crt;
        updatePaperWhite();
    }

    HdrOutput::Status HdrOutput::status() {
        std::scoped_lock lock(hdrSettingsMutex);
        Status status;
        status.active = hdrActive;
        status.display = hdrDisplay;
        status.crt = hdrCrt;
        status.paperWhiteNits = hdrPaperWhite;
        return status;
    }

    bool HdrOutput::active() {
        return hdrActive;
    }

    float HdrOutput::peakNits() {
        return hdrPeakNits;
    }

    float HdrOutput::paperWhiteNits() {
        return hdrPaperWhite;
    }

    float HdrOutput::sdrScale() {
        return hdrPaperWhite / 80.0f;
    }

    RenderFormat HdrOutput::swapChainFormat() {
        return hdrActive ? RenderFormat::R16G16B16A16_FLOAT : RenderFormat::B8G8R8A8_UNORM;
    }
};
