// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels, Ville Linde, Aaron Giles
//
// Model 2 3D layer, pixel stage: MAME's model2rd.ipp scanline shaders and
// bilinear texel fetch (transplanted in src/runtime/raster.cpp, BSD-3), per
// pixel. Integer arithmetic as the CPU reference does it; the first polygon
// to write a pixel keeps it (the depth test on draw order does MAME's fill
// check). Solid polygons arrive with their colour computed on the CPU.
//
// a: flags, lumabase, colour (solid: 0xRRGGBB; textured: palette word), texlod
//    flags: renderer 0-1, checker 2, mirror x/y 3-4, wrap x/y 5-6, utex 7,
//           utexminlod 8-9, luma 16-23
// b: texwidth | texheight << 16, texx | texy << 16, utexx | utexy << 16,
//    max mip level | first sheet << 8
// c: clip minx, maxx, miny, maxy (inclusive, 3D-layer pixels)
StructuredBuffer<uint> texram : register(t0, space2); // texture RAM 0 then 1, 0x80000 dwords each
StructuredBuffer<uint> vmem : register(t1, space2);   // colour table (0xc000 bytes), luma (0x8000), gamma (0x100)

struct PSIn {
    float3 p : TEXCOORD0;
    nointerpolation uint4 a : TEXCOORD1;
    nointerpolation uint4 b : TEXCOORD2;
    nointerpolation uint4 c : TEXCOORD3;
    nointerpolation float scale : TEXCOORD4;
    float4 pos : SV_Position;
};

static const uint kLuma = 0xc000, kGamma = 0x14000;

uint rbyte(uint addr) { return (vmem[addr >> 2] >> ((addr & 3) * 8)) & 0xff; }
uint colorxlat(uint index) { return rbyte(index * 2) | (rbyte(index * 2 + 1) << 8); }
uint gamma(uint i) { return rbyte(kGamma + i); }

static const uint s_log2_table[128] = {
    0,   2,   5,   8,   11,  14,  16,  19,  22,  25,  27,  30,  33,  35,  38,  40,  43,  46,  48,  51,  53,  56,
    58,  61,  63,  65,  68,  70,  73,  75,  77,  80,  82,  84,  87,  89,  91,  93,  96,  98,  100, 102, 104, 106,
    109, 111, 113, 115, 117, 119, 121, 123, 125, 127, 129, 132, 134, 136, 138, 140, 141, 143, 145, 147, 149, 151,
    153, 155, 157, 159, 161, 162, 164, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 189, 191,
    193, 194, 196, 198, 200, 201, 203, 205, 206, 208, 209, 211, 213, 214, 216, 218, 219, 221, 222, 224, 225, 227,
    229, 230, 232, 233, 235, 236, 238, 239, 241, 242, 244, 245, 247, 248, 250, 251, 253, 254};

int fast_log2(float value) {
    if (value < 0.0) return 0;
    uint ival = asuint(value) >> 16;
    int e = int(ival >> 7) - 127;
    return (e << 8) | int(s_log2_table[ival & 127]);
}

// x86 cvttss2si: NaN and out of range give INT32_MIN.
int to_s32(float f) {
    if (!(f >= -2147483648.0 && f < 2147483648.0)) return int(0x80000000u);
    return int(f);
}

uint LERP(uint x, uint y, uint a) { return (x + (((y - x) * a) >> 8)) & 0x00ff00ffu; }

uint get_texel(uint base_x, uint base_y, int x, int y, uint sheet) {
    int x2 = int(base_x) + x;
    int y2 = int(base_y) + y;
    if (x2 >= 1024) { // sheets are mapped as 2048x1024 but stored as 1024x2048
        x2 -= 1024;
        y2 ^= 1024;
    }
    uint offset = uint(((y2 / 2) * 512) + (x2 / 2));
    uint texel = texram[sheet * 0x80000u + ((offset >> 1) & 0x7ffffu)];
    if (offset & 1) texel >>= 16;
    if ((y & 1) == 0) texel >>= 8;
    if ((x & 1) == 0) texel >>= 4;
    return texel & 0x0f;
}

uint fetch_bilinear_texel(uint4 a, uint4 b, bool translucent, int miplevel, int u, int v) {
    const uint flags = a.x;
    const uint texwidth = b.x & 0xffff, texheight = b.x >> 16, texx = b.y & 0xffff, texy = b.y >> 16;
    const uint sheet0 = (b.w >> 8) & 1;
    uint tex_width, tex_height, tex_x, tex_y, sheet;
    if (miplevel == -1) { // microtexture
        tex_width = 128;
        tex_height = 128;
        tex_x = b.z & 0xffff;
        tex_y = b.z >> 16;
        sheet = sheet0 ^ 1;
        const uint utexminlod = (flags >> 8) & 3;
        u <<= 1u << utexminlod;
        v <<= 1u << utexminlod;
    } else {
        tex_width = texwidth >> miplevel;
        tex_height = texheight >> miplevel;
        tex_x = ((texx - 2048u) >> miplevel) & 2047u;
        tex_y = ((texy - 1024u) >> miplevel) & 1023u;
        sheet = sheet0 ^ uint(miplevel & 1);
        u >>= miplevel;
        v >>= miplevel;
    }
    if (((flags >> 3) & 1) && (u & int(tex_width << 8))) u = ~u;
    if (((flags >> 4) & 1) && (v & int(tex_height << 8))) v = ~v;
    u -= 0x80;
    v -= 0x80;
    uint ufrac = uint(u) & 0xff;
    uint vfrac = uint(v) & 0xff;
    uint u0 = uint(u >> 8) & (tex_width - 1);
    uint u1 = (u0 + 1) & (tex_width - 1);
    uint v0 = uint(v >> 8) & (tex_height - 1);
    uint v1 = (v0 + 1) & (tex_height - 1);
    if (!((flags >> 5) & 1) && u1 == 0) {
        if (ufrac >= 0x80) { u0 = u1; u1++; ufrac = 0; }
        else { u1 = u0; u0--; ufrac = 0x100; }
    }
    if (!((flags >> 6) & 1) && v1 == 0) {
        if (vfrac >= 0x80) { v0 = 0; v1++; vfrac = 0; }
        else { v1 = v0; v0--; vfrac = 0x100; }
    }
    uint tex00 = get_texel(tex_x, tex_y, int(u0), int(v0), sheet) << 4;
    uint tex01 = get_texel(tex_x, tex_y, int(u1), int(v0), sheet) << 4;
    uint tex10 = get_texel(tex_x, tex_y, int(u0), int(v1), sheet) << 4;
    uint tex11 = get_texel(tex_x, tex_y, int(u1), int(v1), sheet) << 4;
    if (translucent) {
        if (tex00 != 0xf0) tex00 |= 0x00800000;
        if (tex01 != 0xf0) tex01 |= 0x00800000;
        if (tex10 != 0xf0) tex10 |= 0x00800000;
        if (tex11 != 0xf0) tex11 |= 0x00800000;
        if (tex00 == 0x000000f0) tex00 = tex01 & 0xff;
        if (tex01 == 0x000000f0) tex01 = tex00 & 0xff;
        if (tex10 == 0x000000f0) tex10 = tex11 & 0xff;
        if (tex11 == 0x000000f0) tex11 = tex10 & 0xff;
    }
    uint tex0x = LERP(tex00, tex01, ufrac);
    uint tex1x = LERP(tex10, tex11, ufrac);
    if (translucent) {
        if (tex0x == 0x000000f0) tex0x = tex1x & 0xff;
        if (tex1x == 0x000000f0) tex1x = tex0x & 0xff;
    }
    return LERP(tex0x, tex1x, vfrac);
}

float4 main(PSIn i) : SV_Target0 {
    const int2 pix = int2(floor(i.pos.xy / i.scale)); // 3D-layer pixel (the scale is the render resolution multiplier)
    if (pix.x < int(i.c.x) || pix.x > int(i.c.y) || pix.y < int(i.c.z) || pix.y > int(i.c.w)) discard;
    const uint flags = i.a.x;
    if (((flags >> 2) & 1) && ((pix.x ^ pix.y) & 1) == 0) discard; // checkerboard polygons: every other pixel
    const uint renderer = flags & 3;
    if (renderer < 2) { // solid
        const uint c = i.a.z;
        return float4(float((c >> 16) & 0xff), float((c >> 8) & 0xff), float(c & 0xff), 255.0) / 255.0;
    }
    const bool translucent = renderer == 3;
    precise float ooz = i.p.x;
    precise float uoz = i.p.y;
    precise float voz = i.p.z;
    precise float z = 1.0 / ooz;
    const int mml = -int(i.a.w) + fast_log2(z);
    const int max_level = int(i.b.w & 0xff);
    const int level = clamp(mml >> 7, 0, max_level);
    precise float uf = uoz * z * 256.0;
    precise float vf = voz * z * 256.0;
    const int u = to_s32(uf);
    const int v = to_s32(vf);
    uint t = fetch_bilinear_texel(i.a, i.b, translucent, level, u, v);
    if (mml > 0 && level < max_level) {
        const uint t2 = fetch_bilinear_texel(i.a, i.b, translucent, level + 1, u, v);
        t = LERP(t, t2, uint((mml & 127) << 1));
    } else if (((flags >> 7) & 1) && mml < 0) {
        const uint t2 = fetch_bilinear_texel(i.a, i.b, translucent, -1, u, v);
        t = LERP(t, t2, uint(min(-mml >> ((flags >> 8) & 3), 127)));
    }
    if (translucent) {
        if (t < 0x00400000) discard;
        t &= 0xff;
    }
    const uint colorbase = i.a.z;
    const uint luma_in = (flags >> 16) & 0xff;
    uint luma = (rbyte(kLuma + ((i.a.y + (t >> 1)) & 0x7fff)) * luma_in / 256) & 0xff;
    luma = min(luma, 0x3fu);
    const uint tr = gamma(colorxlat(0x0000 / 2 + (((colorbase >> 0) & 0x1f) << 8) + luma) & 0xff);
    const uint tg = gamma(colorxlat(0x4000 / 2 + (((colorbase >> 5) & 0x1f) << 8) + luma) & 0xff);
    const uint tb = gamma(colorxlat(0x8000 / 2 + (((colorbase >> 10) & 0x1f) << 8) + luma) & 0xff);
    return float4(float(tr), float(tg), float(tb), 255.0) / 255.0;
}
