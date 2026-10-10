//
// RT64
//

// Volk must be included before anything else that pulls in the Vulkan headers.
#include "plume_vulkan.h"

#if defined(_WIN32)
#   include "plume_d3d12.h"
#   define LIBRA_RUNTIME_D3D12
#endif
#define LIBRA_RUNTIME_VULKAN
#include "librashader/include/librashader_ld.h"

#include "rt64_crt_filter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>

#include "rt64_render_worker.h"

namespace RT64 {
    // Settings shared with the UI thread.

    static std::mutex crtSettingsMutex;
    static CrtFilter::Settings crtSettings;
    static uint64_t crtSettingsVersion = 1;

    void CrtFilter::setUserSettings(const Settings &settings) {
        std::scoped_lock lock(crtSettingsMutex);
        crtSettings = settings;
        crtSettingsVersion++;
    }

    CrtFilter::Settings CrtFilter::userSettings() {
        std::scoped_lock lock(crtSettingsMutex);
        return crtSettings;
    }

    static uint64_t userSettingsVersion() {
        std::scoped_lock lock(crtSettingsMutex);
        return crtSettingsVersion;
    }

    // Debug/test override: WCW_CRT=<preset> enables the filter regardless of the menu,
    // WCW_CRT=0 forces it off. WCW_CRT_SRCW=<n> overrides the source width scale and
    // WCW_CRT_BEAM=1 enables the beam simulator.
    static bool envSettings(CrtFilter::Settings &settings) {
        const char *env = getenv("WCW_CRT");
        if ((env == nullptr) || (env[0] == '\0')) {
            return false;
        }

        settings = CrtFilter::Settings();
        settings.enabled = (strcmp(env, "0") != 0);
        settings.preset = env;
        if (const char *srcw = getenv("WCW_CRT_SRCW")) {
            settings.sourceWidthScale = uint32_t(std::max(atoi(srcw), 1));
        }

        if (const char *beam = getenv("WCW_CRT_BEAM")) {
            settings.beamSimulator = (atoi(beam) != 0);
        }

        return true;
    }

    static std::filesystem::path programDirectory() {
#   if defined(_WIN32)
        wchar_t buffer[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if ((length > 0) && (length < MAX_PATH)) {
            return std::filesystem::path(buffer).parent_path();
        }
#   endif
        return std::filesystem::current_path();
    }

    static std::filesystem::path resolvePresetPath(const std::string &preset) {
        std::filesystem::path path = std::filesystem::u8path(preset);
        if (path.has_extension() || path.has_parent_path()) {
            return path.is_absolute() ? path : (programDirectory() / path);
        }

        return programDirectory() / "crt" / "presets" / (preset + ".slangp");
    }

    // Fractional-ratio variant of the Blur Busters CRT Beam Simulator (contrib/crt/shaders).
    static const char *BeamSimulatorPreset = "aki-beam";

    // The simulated CRT refreshes at the N64's 60 Hz.
    static const double BeamCrtRate = 60.0;

    // Swap chain textures don't record their VkFormat (plume only sets it for textures it allocates).
    static VkFormat vulkanImageFormat(const VulkanTexture *texture) {
        if (texture->imageFormat != VK_FORMAT_UNDEFINED) {
            return texture->imageFormat;
        }

        switch (texture->desc.format) {
        case RenderFormat::B8G8R8A8_UNORM:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case RenderFormat::R8G8B8A8_UNORM:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case RenderFormat::R16G16B16A16_FLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }

    // CrtFilter::Impl

    enum class CrtBackend {
        None,
        D3D12,
        Vulkan
    };

    // A librashader filter chain on whichever backend RT64 runs on.
    struct LibraChain {
#   if defined(_WIN32)
        libra_d3d12_filter_chain_t d3d12 = nullptr;
#   endif
        libra_vk_filter_chain_t vk = nullptr;
        // Set when creating or running the chain failed; it isn't retried until the settings change.
        bool failed = false;

        bool ready() const {
#       if defined(_WIN32)
            if (d3d12 != nullptr) {
                return !failed;
            }
#       endif
            return (vk != nullptr) && !failed;
        }
    };

    // A color target the filter renders into.
    struct CrtTarget {
        std::unique_ptr<RenderTexture> texture;
        std::unique_ptr<RenderFramebuffer> framebuffer;
        uint32_t width = 0;
        uint32_t height = 0;

        // Returns true when the target had to be (re)created.
        bool ensure(RenderDevice *device, uint32_t newWidth, uint32_t newHeight, const char *name) {
            if ((texture != nullptr) && (width == newWidth) && (height == newHeight)) {
                return false;
            }

            framebuffer.reset();
            texture = device->createTexture(RenderTextureDesc::ColorTarget(newWidth, newHeight, RenderFormat::B8G8R8A8_UNORM));
            texture->setName(name);
            const RenderTexture *colorTarget = texture.get();
            framebuffer = device->createFramebuffer(RenderFramebufferDesc(&colorTarget, 1));
            width = newWidth;
            height = newHeight;
            return true;
        }
    };

    struct CrtFilter::Impl {
        RenderInterface *renderInterface = nullptr;
        RenderDevice *device = nullptr;
        CrtBackend backend = CrtBackend::None;

        // librashader is loaded on first use. If loading fails the filter stays off.
        bool libraryTried = false;
        libra_instance_t libra = {};

        uint64_t appliedVersion = 0;
        Settings settings;
        std::string guestPreset;
        LibraChain guestChain;
        LibraChain beamChain;
        std::unique_ptr<RenderWorker> setupWorker;

        // Native-resolution input of the preset.
        CrtTarget sourceTarget;
        size_t frameCount = 0;

        // Beam simulator: the preset's output (window sized), double buffered so a new game frame
        // waits for the simulated scan to restart at the top.
        CrtTarget guestTargets[2];
        uint32_t frontTarget = 0;
        uint32_t pendingTarget = 0;
        bool pendingFrame = false;
        bool haveFrontFrame = false;
        uint64_t refreshCounter = 0;
        uint64_t lastCrtRefresh = UINT64_MAX;
        uint32_t displayRefreshRate = 0;
        bool displayVsync = false;
        bool displayInterpolating = false;
        // Display refreshes per simulated CRT refresh (e.g. 2.75 at 165 Hz), 0 when unavailable.
        double beamRatio = 0.0;
        std::string beamUnavailableReason;

        ~Impl() {
            freeChain(guestChain);
            freeChain(beamChain);
        }

        void logError(const char *what, libra_error_t error) const {
            char *message = nullptr;
            if ((error != nullptr) && (libra.error_write(error, &message) == 0) && (message != nullptr)) {
                fprintf(stderr, "[wcw crt] %s failed: %s\n", what, message);
                libra.error_free_string(&message);
            }
            else {
                fprintf(stderr, "[wcw crt] %s failed.\n", what);
            }

            if (error != nullptr) {
                libra.error_free(&error);
            }
        }

        bool loadLibrary() {
            if (!libraryTried) {
                libraryTried = true;
                libra = librashader_load_instance();
                if (!libra.instance_loaded) {
                    fprintf(stderr, "[wcw crt] librashader.dll could not be loaded; the CRT filter is unavailable.\n");
                }
                else if (libra.instance_abi_version() != LIBRASHADER_CURRENT_ABI) {
                    fprintf(stderr, "[wcw crt] librashader ABI %zu does not match the expected %d; the CRT filter is unavailable.\n", size_t(libra.instance_abi_version()), LIBRASHADER_CURRENT_ABI);
                    libra.instance_loaded = false;
                }
            }

            return libra.instance_loaded;
        }

        // The present queue waits for the GPU after every present, so nothing in flight references
        // a chain by the time it's freed here.
        void freeChain(LibraChain &chain) {
            if (libra.instance_loaded) {
#           if defined(_WIN32)
                if (chain.d3d12 != nullptr) {
                    libra.d3d12_filter_chain_free(&chain.d3d12);
                }
#           endif
                if (chain.vk != nullptr) {
                    libra.vk_filter_chain_free(&chain.vk);
                }
            }

            chain = LibraChain();
        }

        void setParameter(LibraChain &chain, const std::string &name, float value) {
            libra_error_t error = nullptr;
#       if defined(_WIN32)
            if (chain.d3d12 != nullptr) {
                error = libra.d3d12_filter_chain_set_param(&chain.d3d12, name.c_str(), value);
            }
#       endif
            if (chain.vk != nullptr) {
                error = libra.vk_filter_chain_set_param(&chain.vk, name.c_str(), value);
            }

            if (error != nullptr) {
                logError(("Setting parameter " + name).c_str(), error);
            }
        }

        // Builds a filter chain. Shader compilation happens here (cached by librashader after the
        // first time), so this stalls the present thread once per preset change.
        bool createChain(LibraChain &chain, const std::string &preset) {
            freeChain(chain);
            chain.failed = true;
            if (!loadLibrary()) {
                return false;
            }

            const std::filesystem::path presetPath = resolvePresetPath(preset);
            const std::string presetPathU8 = presetPath.u8string();
            libra_shader_preset_t libraPreset = nullptr;
            libra_preset_opt_t presetOptions = {};
            presetOptions.version = LIBRASHADER_CURRENT_VERSION;
            presetOptions.original_aspect_uniforms = true;
            presetOptions.frametime_uniforms = true;
            libra_error_t error = libra.preset_create_with_options(presetPathU8.c_str(), nullptr, &presetOptions, &libraPreset);
            if (error != nullptr) {
                logError(("Loading preset " + presetPathU8).c_str(), error);
                return false;
            }

            if (setupWorker == nullptr) {
                setupWorker = std::make_unique<RenderWorker>(device, "CRT Filter Setup", RenderCommandListType::DIRECT);
            }

            setupWorker->commandList->begin();
#       if defined(_WIN32)
            if (backend == CrtBackend::D3D12) {
                D3D12Device *d3d12Device = static_cast<D3D12Device *>(device);
                D3D12CommandList *d3d12CommandList = static_cast<D3D12CommandList *>(setupWorker->commandList.get());
                filter_chain_d3d12_opt_t options = {};
                options.version = LIBRASHADER_CURRENT_VERSION;
                error = libra.d3d12_filter_chain_create_deferred(&libraPreset, d3d12Device->d3d, d3d12CommandList->d3d, &options, &chain.d3d12);
            }
#       endif
            if (backend == CrtBackend::Vulkan) {
                VulkanDevice *vulkanDevice = static_cast<VulkanDevice *>(device);
                VulkanCommandList *vulkanCommandList = static_cast<VulkanCommandList *>(setupWorker->commandList.get());
                VulkanCommandQueue *vulkanQueue = static_cast<VulkanCommandQueue *>(setupWorker->commandQueue.get());
                libra_device_vk_t vulkan = {};
                vulkan.physical_device = vulkanDevice->physicalDevice;
                vulkan.instance = vulkanDevice->renderInterface->instance;
                vulkan.device = vulkanDevice->vk;
                vulkan.queue = vulkanQueue->queue->vk;
                vulkan.entry = vkGetInstanceProcAddr;
                filter_chain_vk_opt_t options = {};
                options.version = LIBRASHADER_CURRENT_VERSION;
                error = libra.vk_filter_chain_create_deferred(&libraPreset, vulkan, vulkanCommandList->vk, &options, &chain.vk);
            }

            setupWorker->commandList->end();
            setupWorker->execute();
            setupWorker->wait();

            if (libraPreset != nullptr) {
                libra.preset_free(&libraPreset);
            }

            if (error != nullptr) {
                logError(("Creating the filter chain for " + presetPathU8).c_str(), error);
                freeChain(chain);
                chain.failed = true;
                return false;
            }

            chain.failed = false;
            fprintf(stderr, "[wcw crt] Loaded preset %s\n", presetPathU8.c_str());
            return true;
        }

        // Records one run of a chain. librashader binds its own pipeline state, so the command
        // list's cached state is invalidated afterwards and the caller rebinds its framebuffer.
        bool runChain(LibraChain &chain, RenderCommandList *commandList, RenderTexture *input, uint32_t inputWidth, uint32_t inputHeight,
            RenderTexture *output, uint32_t outputWidth, uint32_t outputHeight, const libra_viewport_t &viewport,
            size_t chainFrameCount, uint32_t totalSubframes, uint32_t currentSubframe)
        {
            libra_error_t error = nullptr;
#       if defined(_WIN32)
            if (chain.d3d12 != nullptr) {
                D3D12CommandList *d3d12CommandList = static_cast<D3D12CommandList *>(commandList);
                libra_image_d3d12_t inputImage = {};
                inputImage.image_type = LIBRA_D3D12_IMAGE_TYPE_RESOURCE;
                inputImage.handle.resource = static_cast<D3D12Texture *>(input)->d3d;
                libra_image_d3d12_t outputImage = {};
                outputImage.image_type = LIBRA_D3D12_IMAGE_TYPE_RESOURCE;
                outputImage.handle.resource = static_cast<D3D12Texture *>(output)->d3d;
                frame_d3d12_opt_t options = {};
                options.version = LIBRASHADER_CURRENT_VERSION;
                options.frame_direction = 1;
                options.total_subframes = totalSubframes;
                options.current_subframe = currentSubframe;
                options.frames_per_second = 60.0f;
                options.frametime_delta = 17;
                error = libra.d3d12_filter_chain_frame(&chain.d3d12, d3d12CommandList->d3d, chainFrameCount, inputImage, outputImage, &viewport, nullptr, &options);

                // librashader binds its own descriptor heaps, root signatures and render targets.
                d3d12CommandList->descriptorHeapsSet = false;
                d3d12CommandList->activeTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
                d3d12CommandList->activeGraphicsPipeline = nullptr;
                d3d12CommandList->activeGraphicsPipelineLayout = nullptr;
                d3d12CommandList->activeComputePipelineLayout = nullptr;
            }
#       endif
            if (chain.vk != nullptr) {
                VulkanCommandList *vulkanCommandList = static_cast<VulkanCommandList *>(commandList);

                // librashader must not be called inside a render pass; plume leaves one open after a
                // clear or draw until the next barrier or framebuffer change.
                vulkanCommandList->endActiveRenderPass();

                const VulkanTexture *vulkanInput = static_cast<const VulkanTexture *>(input);
                const VulkanTexture *vulkanOutput = static_cast<const VulkanTexture *>(output);
                libra_image_vk_t inputImage = { vulkanInput->vk, vulkanImageFormat(vulkanInput), inputWidth, inputHeight };
                libra_image_vk_t outputImage = { vulkanOutput->vk, vulkanImageFormat(vulkanOutput), outputWidth, outputHeight };
                frame_vk_opt_t options = {};
                options.version = LIBRASHADER_CURRENT_VERSION;
                options.frame_direction = 1;
                options.total_subframes = totalSubframes;
                options.current_subframe = currentSubframe;
                options.frames_per_second = 60.0f;
                options.frametime_delta = 17;
                error = libra.vk_filter_chain_frame(&chain.vk, vulkanCommandList->vk, chainFrameCount, inputImage, outputImage, &viewport, nullptr, &options);
                vulkanCommandList->activeGraphicsPipelineLayout = nullptr;
                vulkanCommandList->activeComputePipelineLayout = nullptr;
            }

            if (error != nullptr) {
                // The chain is still referenced by this frame's commands; it's freed on the next rebuild.
                logError("Rendering the CRT filter", error);
                chain.failed = true;
                return false;
            }

            return true;
        }

        void applyGuestParameters() {
            for (const auto &parameter : settings.parameters) {
                setParameter(guestChain, parameter.first, parameter.second);
            }
        }

        // Re-reads the settings and (re)builds the chains when they changed.
        void refreshSettings() {
            static Settings envOverride;
            static const bool envOverridden = envSettings(envOverride);
            const uint64_t version = envOverridden ? UINT64_MAX : userSettingsVersion();
            if (version == appliedVersion) {
                return;
            }

            const Settings next = envOverridden ? envOverride : userSettings();
            appliedVersion = version;
            settings = next;
            if (!settings.enabled || (backend == CrtBackend::None)) {
                return;
            }

            try {
                if ((settings.preset != guestPreset) || (!guestChain.ready())) {
                    guestPreset = settings.preset;
                    frameCount = 0;
                    haveFrontFrame = false;
                    pendingFrame = false;
                    if (createChain(guestChain, settings.preset)) {
                        applyGuestParameters();
                    }
                }
                else {
                    applyGuestParameters();
                }

                if (settings.beamSimulator && !beamChain.ready()) {
                    beamRatio = 0.0;
                    createChain(beamChain, BeamSimulatorPreset);
                }
            }
            catch (const std::exception &e) {
                fprintf(stderr, "[wcw crt] Setting up preset %s threw: %s\n", settings.preset.c_str(), e.what());
                guestChain.failed = true;
            }
        }

        bool guestActive() const {
            return settings.enabled && guestChain.ready();
        }

        // Display refreshes per simulated CRT refresh, or 0 when the display can't run the beam simulator.
        double computeBeamRatio() {
            std::string reason;
            double ratio = double(displayRefreshRate) / BeamCrtRate;
            if (!displayVsync) {
                reason = "it needs V-Sync";
            }
            else if (displayInterpolating) {
                reason = "it doesn't work with frame interpolation (set the framerate to Original)";
            }
            else if (ratio < 1.95) {
                reason = "the display refresh rate (" + std::to_string(displayRefreshRate) + " Hz) must be 120 Hz or higher";
            }

            if (!reason.empty()) {
                ratio = 0.0;
            }

            if (reason != beamUnavailableReason) {
                if (!reason.empty()) {
                    fprintf(stderr, "[wcw crt] Beam simulator inactive: %s.\n", reason.c_str());
                }
                else {
                    fprintf(stderr, "[wcw crt] Beam simulator active: %u Hz display, %.3f refreshes per 60 Hz CRT refresh.\n", displayRefreshRate, ratio);
                }

                beamUnavailableReason = reason;
            }

            return ratio;
        }

        bool beamActive() {
            if (!guestActive() || !settings.beamSimulator || !beamChain.ready()) {
                return false;
            }

            const double ratio = computeBeamRatio();
            if (ratio != beamRatio) {
                beamRatio = ratio;
                if (ratio > 0.0) {
                    setParameter(beamChain, "HZ_RATIO", float(ratio));
                    // Blur Busters' recommendation: ~0.5 at 120 Hz, ~0.7 at 240 Hz and up.
                    setParameter(beamChain, "GAIN_VS_BLUR", float(std::clamp(0.5 + (ratio - 2.0) * 0.1, 0.5, 0.7)));
                }
            }

            return beamRatio > 0.0;
        }

        // The shader's own CRT refresh counter (it nudges an even integer ratio off by 0.001 to
        // protect LCDs from image retention, LCD_ANTI_RETENTION_TOGGLE on by default).
        uint64_t crtRefreshIndex(uint64_t refresh) const {
            double ratio = beamRatio;
            if ((std::floor(ratio) == ratio) && (std::fmod(ratio, 2.0) == 0.0)) {
                ratio += 0.001;
            }

            return uint64_t(std::floor(double(refresh) / ratio));
        }

        // Draws the next sub-frame of the front frame over the whole swap chain texture.
        bool presentBeamSubframe(RenderCommandList *commandList, RenderTexture *swapChainTexture, const RenderFramebuffer *swapChainFramebuffer, uint32_t windowWidth, uint32_t windowHeight) {
            const uint64_t crtRefresh = crtRefreshIndex(refreshCounter);
            const bool scanStart = (crtRefresh != lastCrtRefresh);
            lastCrtRefresh = crtRefresh;
            if (pendingFrame && scanStart) {
                frontTarget = pendingTarget;
                pendingFrame = false;
            }

            CrtTarget &front = guestTargets[frontTarget];
            commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(swapChainTexture, RenderTextureLayout::COLOR_WRITE));

            libra_viewport_t viewport = { 0.0f, 0.0f, windowWidth, windowHeight };
            const bool ok = runChain(beamChain, commandList, front.texture.get(), front.width, front.height, swapChainTexture, windowWidth, windowHeight,
                viewport, size_t(refreshCounter), uint32_t(std::lround(beamRatio)), 1);

            refreshCounter++;
            commandList->setFramebuffer(swapChainFramebuffer);
            return ok;
        }
    };

    // CrtFilter

    CrtFilter::CrtFilter(RenderInterface *renderInterface, RenderDevice *device, RenderShaderFormat shaderFormat) {
        impl = std::make_unique<Impl>();
        impl->renderInterface = renderInterface;
        impl->device = device;
        switch (shaderFormat) {
#   if defined(_WIN32)
        case RenderShaderFormat::DXIL:
            impl->backend = CrtBackend::D3D12;
            break;
#   endif
        case RenderShaderFormat::SPIRV:
            impl->backend = CrtBackend::Vulkan;
            break;
        default:
            impl->backend = CrtBackend::None;
            break;
        }
    }

    CrtFilter::~CrtFilter() { }

    bool CrtFilter::active() {
        impl->refreshSettings();
        return impl->guestActive();
    }

    void CrtFilter::setDisplayConditions(uint32_t refreshRate, bool vsyncEnabled, bool interpolating) {
        impl->displayRefreshRate = refreshRate;
        impl->displayVsync = vsyncEnabled;
        impl->displayInterpolating = interpolating;
    }

    bool CrtFilter::beamActive() {
        impl->refreshSettings();
        return impl->beamActive();
    }

    bool CrtFilter::render(VIRenderer &viRenderer, const RenderParams &p) {
        if (!active() || (p.vi.texture == nullptr) || (p.vi.vi == nullptr)) {
            return false;
        }

        // Source: one row per scanline, width from the TV's aspect ratio.
        hlslpp::float2 sdSize, hdSize;
        VIRenderer::getTVSize(*p.vi.vi, p.vi.resolutionScale, p.vi.downsamplingScale, p.vi.removeBlackBorders, sdSize, hdSize);
        const float lines = std::max(float(sdSize.y), 1.0f);
        const float aspect = float(hdSize.x) / std::max(float(hdSize.y), 1.0f);
        const uint32_t sourceHeight = uint32_t(lines);
        const uint32_t sourceWidth = uint32_t(std::lround(lines * aspect)) * std::max(impl->settings.sourceWidthScale, 1U);
        if ((sourceWidth == 0) || (sourceHeight == 0)) {
            return false;
        }

        impl->sourceTarget.ensure(impl->device, sourceWidth, sourceHeight, "CRT Filter Source");

        RenderCommandList *commandList = p.presentWorker->commandList.get();
        RenderTexture *sourceTexture = impl->sourceTarget.texture.get();
        commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(sourceTexture, RenderTextureLayout::COLOR_WRITE));
        commandList->setFramebuffer(impl->sourceTarget.framebuffer.get());
        commandList->clearColor();

        VIRenderer::RenderParams sourceParams = p.vi;
        sourceParams.outputWidth = sourceWidth;
        sourceParams.outputHeight = sourceHeight;
        viRenderer.render(sourceParams);
        commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(sourceTexture, RenderTextureLayout::SHADER_READ));

        // Output: the whole TV rectangle of the window (pillarboxed like the VI blit).
        const uint32_t windowWidth = p.vi.swapChain->getWidth();
        const uint32_t windowHeight = p.vi.swapChain->getHeight();
        const hlslpp::float2 windowSize = hlslpp::float2(float(windowWidth), float(windowHeight));
        const RenderViewport tvRect = VIRenderer::getTVRect(windowSize, hdSize);
        libra_viewport_t viewport;
        viewport.x = std::round(tvRect.x);
        viewport.y = std::round(tvRect.y);
        viewport.width = uint32_t(std::max(std::lround(tvRect.width), 1L));
        viewport.height = uint32_t(std::max(std::lround(tvRect.height), 1L));

        impl->frameCount++;
        if (impl->beamActive()) {
            // Render the preset into the back buffer; it becomes visible when the scan restarts.
            Impl &s = *impl;
            for (CrtTarget &target : s.guestTargets) {
                if (target.ensure(s.device, windowWidth, windowHeight, "CRT Filter Beam Input")) {
                    s.haveFrontFrame = false;
                    s.pendingFrame = false;
                }
            }

            const uint32_t back = s.pendingFrame ? s.pendingTarget : (s.haveFrontFrame ? (1 - s.frontTarget) : s.frontTarget);
            CrtTarget &backTarget = s.guestTargets[back];
            commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(backTarget.texture.get(), RenderTextureLayout::COLOR_WRITE));
            commandList->setFramebuffer(backTarget.framebuffer.get());
            commandList->clearColor();
            const bool ok = s.runChain(s.guestChain, commandList, sourceTexture, sourceWidth, sourceHeight, backTarget.texture.get(), windowWidth, windowHeight,
                viewport, s.frameCount, 1, 1);
            commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(backTarget.texture.get(), RenderTextureLayout::SHADER_READ));
            if (ok) {
                if (s.haveFrontFrame) {
                    s.pendingTarget = back;
                    s.pendingFrame = true;
                }
                else {
                    s.frontTarget = back;
                    s.haveFrontFrame = true;
                }
            }

            s.presentBeamSubframe(commandList, p.swapChainTexture, p.swapChainFramebuffer, windowWidth, windowHeight);
        }
        else {
            impl->haveFrontFrame = false;
            impl->pendingFrame = false;
            commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(p.swapChainTexture, RenderTextureLayout::COLOR_WRITE));
            impl->runChain(impl->guestChain, commandList, sourceTexture, sourceWidth, sourceHeight, p.swapChainTexture, windowWidth, windowHeight,
                viewport, impl->frameCount, 1, 1);

            // Rebind the swap chain for whatever draws next (menus, inspector).
            commandList->setFramebuffer(p.swapChainFramebuffer);
        }

        return true;
    }

    void CrtFilter::skipBeamRefreshes(uint32_t count) {
        impl->refreshCounter += count;
    }

    bool CrtFilter::renderBeamSubframe(RenderWorker *presentWorker, RenderTexture *swapChainTexture, const RenderFramebuffer *swapChainFramebuffer, uint32_t windowWidth, uint32_t windowHeight) {
        if (!beamActive() || !impl->haveFrontFrame) {
            return false;
        }

        const CrtTarget &front = impl->guestTargets[impl->frontTarget];
        if ((front.width != windowWidth) || (front.height != windowHeight)) {
            return false;
        }

        return impl->presentBeamSubframe(presentWorker->commandList.get(), swapChainTexture, swapChainFramebuffer, windowWidth, windowHeight);
    }
};
