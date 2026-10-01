// Shaders for the hardware (SDL_GPU) renderer: one source for every backend.
// scripts/build_shaders.py compiles it with DXC to SPIR-V (Vulkan) and DXIL
// (Direct3D 12), and SPIRV-Cross turns the SPIR-V into MSL (Metal); the result
// is src/app/gpu/shaders_gen.h. Resource spaces follow SDL_GPU: vertex
// uniforms space1; fragment textures and samplers space2.

struct Screen { float2 size; float2 pad; }; // the render target, in pixels
[[vk::binding(0, 1)]] cbuffer ScreenUBO : register(b0, space1) { Screen screen; };

// Model 2 polygons, already projected to screen pixels (model2_3d_project).
// depth: the polygon's place in the hardware's draw order; the depth test keeps
// the earlier one, as the rasterizer's "first to fill a pixel wins".
struct PolyIn {
    float2 pos : TEXCOORD0;
    float depth : TEXCOORD1;
    float4 color : TEXCOORD2;
    float checker : TEXCOORD3; // 1: draw every other pixel (x ^ y odd)
};
struct PolyOut {
    float4 pos : SV_Position;
    float4 color : TEXCOORD0;
    nointerpolation float checker : TEXCOORD1;
};

PolyOut vs_poly(PolyIn i) {
    PolyOut o;
    o.pos = float4(i.pos.x / screen.size.x * 2.0 - 1.0, 1.0 - i.pos.y / screen.size.y * 2.0, i.depth, 1.0);
    o.color = i.color;
    o.checker = i.checker;
    return o;
}

float4 ps_poly(PolyOut i) : SV_Target {
    if (i.checker > 0.5) {
        const uint2 p = uint2(i.pos.xy);
        if (((p.x ^ p.y) & 1u) == 0u) discard;
    }
    return i.color;
}

// A CPU-made layer (tilemaps) as a textured quad; pixels that are 0 are holes.
struct QuadIn {
    float2 pos : TEXCOORD0;
    float2 uv : TEXCOORD1;
};
struct QuadOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// SDL_GPU's Vulkan backend takes a combined image sampler in set 2.
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D<float4> layer_tex : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState layer_smp : register(s0, space2);

QuadOut vs_quad(QuadIn i) {
    QuadOut o;
    o.pos = float4(i.pos.x / screen.size.x * 2.0 - 1.0, 1.0 - i.pos.y / screen.size.y * 2.0, 0.0, 1.0);
    o.uv = i.uv;
    return o;
}

float4 ps_quad(QuadOut i) : SV_Target {
    const float4 c = layer_tex.Sample(layer_smp, i.uv);
    if (all(c == 0.0)) discard;
    return float4(c.rgb, 1.0);
}
