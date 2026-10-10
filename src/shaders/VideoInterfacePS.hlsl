//
// RT64
//

#include "shared/rt64_video_interface.h"

[[vk::push_constant]] ConstantBuffer<VideoInterfaceCB> gConstants : register(b0);
Texture2D<float4> gInput : register(t1);
SamplerState gSampler : register(s2);

// Limit texture sampling to the area the VI can sample of the texture.

float4 SampleInput(float2 uv) {
    const float2 LowerRight = gConstants.videoResolution / gConstants.textureResolution;
    const float2 HalfPixel = float2(0.5f, 0.5f) / gConstants.textureResolution;
    float2 outsideBorder = step(LowerRight, uv);
    float4 sampledColor = gInput.SampleLevel(gSampler, clamp(uv, HalfPixel, LowerRight - HalfPixel), 0);
    float4 gammaCorrectedColor = pow(sampledColor, gConstants.gamma);
    gammaCorrectedColor.rgb *= max(1.0f - outsideBorder.x - outsideBorder.y, 0.0f);
    gammaCorrectedColor.a = 1.0f;
    return gammaCorrectedColor;
}

//
// Sourced from https://www.shadertoy.com/view/csX3RH
//
float4 PixelAntialiasing(float2 uv) {
    float2 uvTexspace = uv * gConstants.videoResolution;
    float2 seam = floor(uvTexspace + 0.5f);
    uvTexspace = (uvTexspace - seam) / fwidth(uvTexspace) + seam;
    uvTexspace = clamp(uvTexspace, seam - 0.5f, seam + 0.5f);
    return SampleInput(uvTexspace / gConstants.textureResolution);
}

// [wcw crt] Box filter over the input texels covered by this output pixel. Builds the CRT filter's
// native-resolution source (e.g. 320x240) out of RT64's supersampled frame. Each bilinear tap
// averages a 2x2 texel quad, so n taps per axis cover 2n texels.
float4 BoxDownsample(float2 uv) {
    const float2 texelPos = uv * gConstants.videoResolution;
    const float2 footprint = max(fwidth(texelPos), 1.0f);
    const int2 taps = clamp(int2(ceil(footprint * 0.5f)), 1, 8);
    float4 sum = 0.0f;
    for (int y = 0; y < taps.y; y++) {
        for (int x = 0; x < taps.x; x++) {
            const float2 offset = ((float2(x, y) + 0.5f) / float2(taps) - 0.5f) * footprint;
            sum += SampleInput((texelPos + offset) / gConstants.textureResolution);
        }
    }

    return sum / float(taps.x * taps.y);
}

float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
#if defined(BOX_DOWNSAMPLE)
    return BoxDownsample(uv);
#elif defined(PIXEL_ANTIALIASING)
    return PixelAntialiasing(uv);
#else
    return SampleInput((uv / gConstants.textureResolution) * gConstants.videoResolution);
#endif
}