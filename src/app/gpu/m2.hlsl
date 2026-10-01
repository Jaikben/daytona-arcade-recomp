// Shaders for the hardware (SDL_GPU) renderer: one source for every backend.
// scripts/build_shaders.py compiles it with DXC to SPIR-V (Vulkan) and DXIL
// (Direct3D 12), and SPIRV-Cross turns the SPIR-V into MSL (Metal); the result
// is src/app/gpu/shaders_gen.h. Resource spaces follow SDL_GPU: vertex
// uniforms space1; fragment textures and samplers space2.

struct Screen { float2 size; float2 pad; }; // the render target, in pixels
[[vk::binding(0, 1)]] cbuffer ScreenUBO : register(b0, space1) { Screen screen; };

// Model 2 polygons, already projected to screen pixels (model2_3d_project).
// depth: the polygon's place in the hardware's draw order; the depth test keeps
// the earlier one, as the rasterizer's "first to fill a pixel wins". oz: 1/z,
// u/z, v/z at the vertex (render_one's p[0..2]), interpolated linearly in
// screen space as the rasterizer's plane equations are. The rest is the
// polygon's texture state, decoded on the CPU as render_one does:
//   info.x: renderer (bits 0-1), checker 2, wrap x 3, wrap y 4, mirror x 5,
//           mirror y 6, microtexture 7, its min LOD 8-9, sheets swapped 10,
//           max mip level 12-16, log2(width / 32) 17-19, log2(height / 32) 20-22
//   info.y: texx | texy << 16      info.z: utexx | utexy << 16
//   info.w: lumabase | luma << 16  more.x: texlod (int)  more.y: palette colour (RGB555)
struct PolyIn {
    float2 pos : TEXCOORD0;
    float depth : TEXCOORD1;
    float3 oz : TEXCOORD2;
    float4 color : TEXCOORD3;
    uint4 info : TEXCOORD4;
    uint2 more : TEXCOORD5;
};
struct PolyOut {
    float4 pos : SV_Position;
    noperspective float3 oz : TEXCOORD0;
    nointerpolation float4 color : TEXCOORD1;
    nointerpolation uint4 info : TEXCOORD2;
    nointerpolation uint2 more : TEXCOORD3;
};

PolyOut vs_poly(PolyIn i) {
    PolyOut o;
    o.pos = float4(i.pos.x / screen.size.x * 2.0 - 1.0, 1.0 - i.pos.y / screen.size.y * 2.0, i.depth, 1.0);
    o.oz = i.oz;
    o.color = i.color;
    o.info = i.info;
    o.more = i.more;
    return o;
}

// Fragment data (read-only storage buffers, space2, as SDL_GPU orders them):
// texture RAM (sheet 0 then sheet 1, 0x80000 dwords each, as the rasterizer
// reads them), the luma RAM (one entry per dword, low byte), and the colour
// translation table with the rasterizer's gamma already applied.
[[vk::binding(0, 2)]] StructuredBuffer<uint> texram : register(t0, space2);
[[vk::binding(1, 2)]] StructuredBuffer<uint> lumaram : register(t1, space2);
[[vk::binding(2, 2)]] StructuredBuffer<uint> xlat : register(t2, space2);

static const uint log2_table[128] = {0, 2, 5, 8, 11, 14, 16, 19, 22, 25, 27, 30, 33, 35, 38, 40, 43, 46, 48, 51, 53, 56, 58, 61, 63, 65, 68, 70, 73, 75, 77, 80, 82, 84, 87, 89, 91, 93, 96, 98, 100, 102, 104, 106, 109, 111, 113, 115, 117, 119, 121, 123, 125, 127, 129, 132, 134, 136, 138, 140, 141, 143, 145, 147, 149, 151, 153, 155, 157, 159, 161, 162, 164, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 189, 191, 193, 194, 196, 198, 200, 201, 203, 205, 206, 208, 209, 211, 213, 214, 216, 218, 219, 221, 222, 224, 225, 227, 229, 230, 232, 233, 235, 236, 238, 239, 241, 242, 244, 245, 247, 248, 250, 251, 253, 254};

// raster.cpp fast_log2
int fast_log2(float value) {
    if (value < 0.0) return 0;
    const uint ival = asuint(value) >> 16;
    const int e = int(ival >> 7) - 127;
    return (e << 8) | int(log2_table[ival & 127u]);
}

// x86 cvttss2si: NaN and out of range give INT32_MIN (raster.cpp to_s32)
int to_s32(float f) {
    if (!(f >= -2147483648.0 && f < 2147483648.0)) return int(0x80000000u);
    return int(f);
}

uint lerp8(uint x, uint y, uint a) { return (x + (((y - x) * a) >> 8)) & 0x00ff00ffu; } // raster.cpp LERP

// raster.cpp get_texel
uint get_texel(uint base_x, uint base_y, int x, int y, uint sheet) {
    int x2 = int(base_x) + x;
    int y2 = int(base_y) + y;
    if (x2 >= 1024) { // sheets are mapped 2048x1024, stored 1024x2048
        x2 -= 1024;
        y2 ^= 1024;
    }
    const uint offset = uint((y2 / 2) * 512 + (x2 / 2));
    uint texel = texram[sheet * 0x80000u + ((offset >> 1) & 0x7ffffu)];
    if (offset & 1u) texel >>= 16;
    if ((y & 1) == 0) texel >>= 8;
    if ((x & 1) == 0) texel >>= 4;
    return texel & 0x0fu;
}

// raster.cpp fetch_bilinear_texel; level -1 is the microtexture
uint fetch_texel(uint4 info, int level, int u, int v, bool translucent) {
    const uint flags = info.x;
    const bool swap = (flags >> 10) & 1u;
    uint tex_width, tex_height, tex_x, tex_y, sheet;
    if (level == -1) {
        tex_width = 128u;
        tex_height = 128u;
        tex_x = info.z & 0xffffu;
        tex_y = info.z >> 16;
        sheet = swap ? 0u : 1u; // texsheet[1]
        const uint sh = 1u << ((flags >> 8) & 3u);
        u <<= sh;
        v <<= sh;
    } else {
        const uint texx = info.y & 0xffffu, texy = info.y >> 16;
        tex_width = (32u << ((flags >> 17) & 7u)) >> uint(level);
        tex_height = (32u << ((flags >> 20) & 7u)) >> uint(level);
        tex_x = ((texx - 2048u) >> uint(level)) & 2047u;
        tex_y = ((texy - 1024u) >> uint(level)) & 1023u;
        sheet = ((level & 1) != 0) != swap ? 1u : 0u; // texsheet[level & 1]
        u >>= level;
        v >>= level;
    }
    if (((flags >> 5) & 1u) && (u & int(tex_width << 8)) != 0) u = ~u;
    if (((flags >> 6) & 1u) && (v & int(tex_height << 8)) != 0) v = ~v;
    u -= 0x80;
    v -= 0x80;
    uint ufrac = uint(u) & 0xffu;
    uint vfrac = uint(v) & 0xffu;
    int u0 = int(uint(u >> 8) & (tex_width - 1u));
    int u1 = int(uint(u0 + 1) & (tex_width - 1u));
    int v0 = int(uint(v >> 8) & (tex_height - 1u));
    int v1 = int(uint(v0 + 1) & (tex_height - 1u));
    if (!((flags >> 3) & 1u) && u1 == 0) {
        if (ufrac >= 0x80u) { u0 = u1; u1++; ufrac = 0u; }
        else { u1 = u0; u0--; ufrac = 0x100u; }
    }
    if (!((flags >> 4) & 1u) && v1 == 0) {
        if (vfrac >= 0x80u) { v0 = 0; v1++; vfrac = 0u; }
        else { v1 = v0; v0--; vfrac = 0x100u; }
    }
    uint t00 = get_texel(tex_x, tex_y, u0, v0, sheet) << 4;
    uint t01 = get_texel(tex_x, tex_y, u1, v0, sheet) << 4;
    uint t10 = get_texel(tex_x, tex_y, u0, v1, sheet) << 4;
    uint t11 = get_texel(tex_x, tex_y, u1, v1, sheet) << 4;
    if (translucent) {
        if (t00 != 0xf0u) t00 |= 0x00800000u;
        if (t01 != 0xf0u) t01 |= 0x00800000u;
        if (t10 != 0xf0u) t10 |= 0x00800000u;
        if (t11 != 0xf0u) t11 |= 0x00800000u;
        if (t00 == 0xf0u) t00 = t01 & 0xffu;
        if (t01 == 0xf0u) t01 = t00 & 0xffu;
        if (t10 == 0xf0u) t10 = t11 & 0xffu;
        if (t11 == 0xf0u) t11 = t10 & 0xffu;
    }
    uint t0x = lerp8(t00, t01, ufrac);
    uint t1x = lerp8(t10, t11, ufrac);
    if (translucent) {
        if (t0x == 0xf0u) t0x = t1x & 0xffu;
        if (t1x == 0xf0u) t1x = t0x & 0xffu;
    }
    return lerp8(t0x, t1x, vfrac);
}

float4 ps_poly(PolyOut i) : SV_Target {
    const uint flags = i.info.x;
    if ((flags >> 2) & 1u) { // checker: the rasterizer draws pixels with (x ^ y) odd
        const uint2 p = uint2(i.pos.xy);
        if (((p.x ^ p.y) & 1u) == 0u) discard;
    }
    const uint renderer = flags & 3u;
    if (renderer < 2u) return i.color; // solid: its palette colour, from the CPU

    // raster.cpp draw_tex_span, one pixel
    const bool translucent = renderer == 3u;
    const int max_level = int((flags >> 12) & 31u);
    const float z = 1.0 / i.oz.x;
    const int mml = -int(i.more.x) + fast_log2(z);
    const int level = clamp(mml >> 7, 0, max_level);
    const int u = to_s32(i.oz.y * z * 256.0);
    const int v = to_s32(i.oz.z * z * 256.0);
    uint t = fetch_texel(i.info, level, u, v, translucent);
    if (mml > 0 && level < max_level) {
        const uint t2 = fetch_texel(i.info, level + 1, u, v, translucent);
        t = lerp8(t, t2, uint((mml & 127) << 1));
    } else if (((flags >> 7) & 1u) && mml < 0) {
        const uint t2 = fetch_texel(i.info, -1, u, v, translucent);
        t = lerp8(t, t2, uint(min(-mml >> ((flags >> 8) & 3u), 127)));
    }
    if (translucent) {
        if (t < 0x00400000u) discard;
        t &= 0xffu;
    }
    const uint lumabase = i.info.w & 0xffffu, pluma = i.info.w >> 16;
    uint luma = ((lumaram[lumabase + (t >> 1)] & 0xffu) * pluma / 256u) & 0xffu;
    luma = min(luma, 0x3fu);
    const uint c = i.more.y;
    const uint cr = ((c >> 0) & 0x1fu) << 8, cg = 0x2000u + (((c >> 5) & 0x1fu) << 8), cb = 0x4000u + (((c >> 10) & 0x1fu) << 8);
    return float4(float(xlat[cr + luma]) / 255.0, float(xlat[cg + luma]) / 255.0, float(xlat[cb + luma]) / 255.0, 1.0);
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
