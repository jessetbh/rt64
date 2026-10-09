//
// RT64
//

// [wcw rt] Composites the hybrid ray tracing output onto the framebuffer's color target:
// alpha blended for the debug views, multiplied for shadows (pipeline blend state).

Texture2D<float4> gRtOutput : register(t0);

float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
    return gRtOutput.Load(int3(pos.xy, 0));
}
