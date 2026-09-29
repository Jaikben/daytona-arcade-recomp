// Screen composition, as MAME's model2 screen_update: the back 2D layers
// (opaque, background pen underneath), the 3D layer where it drew, the front
// 2D layers where they are not transparent. The 2D layers come from the CPU
// at 496x384; the 3D layer is the GPU's 512x512 (times the render scale),
// of which the top-left 496x384 is visible.
Texture2D<float4> back : register(t0, space2);
Texture2D<float4> layer3d : register(t1, space2);
Texture2D<float4> front : register(t2, space2);
SamplerState back_s : register(s0, space2);
SamplerState layer3d_s : register(s1, space2);
SamplerState front_s : register(s2, space2);

struct PSIn {
    float2 uv : TEXCOORD0;
    float4 pos : SV_Position;
};
float4 main(PSIn i) : SV_Target0 {
    const float4 f = front.Sample(front_s, i.uv);
    if (f.a > 0.0) return float4(f.rgb, 1.0);
    const float4 l = layer3d.Sample(layer3d_s, i.uv * float2(496.0 / 512.0, 384.0 / 512.0));
    if (l.a > 0.0) return float4(l.rgb, 1.0);
    return float4(back.Sample(back_s, i.uv).rgb, 1.0);
}
