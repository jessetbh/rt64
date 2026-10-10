//
// RT64
//

#pragma once

#include "common/rt64_user_configuration.h"
#include "hle/rt64_vi.h"

#include "rt64_descriptor_sets.h"
#include "rt64_shader_library.h"

namespace RT64 {
    struct VIRenderer {
        std::unique_ptr<VideoInterfaceDescriptorSet> descriptorSet;
        const RenderSampler *descriptorSetSampler = nullptr;

        struct RenderParams {
            RenderDevice *device = nullptr;
            RenderCommandList *commandList = nullptr;
            RenderTexture *texture = nullptr;
            const RenderSwapChain *swapChain = nullptr;
            const ShaderLibrary *shaderLibrary = nullptr;
            RenderFormat textureFormat = RenderFormat::UNKNOWN;
            hlslpp::float2 resolutionScale;
            uint32_t downsamplingScale = 0;
            uint32_t textureWidth = 0;
            uint32_t textureHeight = 0;
            UserConfiguration::Filtering filtering = UserConfiguration::Filtering::Linear;
            const VI *vi = nullptr;
            bool removeBlackBorders = false;

            // [wcw crt] When set, draw into a render target of this size instead of the swap chain,
            // box-filtering the (supersampled) input down to it. Used to build the CRT filter's
            // native-resolution source image.
            uint32_t outputWidth = 0;
            uint32_t outputHeight = 0;
        };

        VIRenderer();
        ~VIRenderer();
        void render(const RenderParams &p);
        static void getViewportAndScissor(const RenderSwapChain *swapChain, const VI &vi, hlslpp::float2 resolutionScale, uint32_t downsamplingScale, bool removeBlackBorders, RenderViewport &viewport, RenderRect &scissor);
        static void getViewportAndScissor(hlslpp::float2 windowSize, const VI &vi, hlslpp::float2 resolutionScale, uint32_t downsamplingScale, bool removeBlackBorders, RenderViewport &viewport, RenderRect &scissor);

        // [wcw crt] Size of the virtual SD TV (scanline space) and of the virtual HD TV for the current VI.
        static void getTVSize(const VI &vi, hlslpp::float2 resolutionScale, uint32_t downsamplingScale, bool removeBlackBorders, hlslpp::float2 &sdSize, hlslpp::float2 &hdSize);

        // [wcw crt] The whole virtual HD TV fitted (pillarboxed/letterboxed) into the window.
        static RenderViewport getTVRect(hlslpp::float2 windowSize, hlslpp::float2 hdSize);
    };
};