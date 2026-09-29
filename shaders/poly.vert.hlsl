// Model 2 3D layer, vertex stage: the polygons arrive already projected to
// 3D-layer pixels (rt::prepare_gpu_frame, src/runtime/raster.cpp), with w = 1,
// so every parameter interpolates linearly in screen space exactly as the
// CPU rasterizer steps it.
struct VSIn {
    float4 pos : TEXCOORD0;  // x, y (3D-layer pixels), draw-order depth, scale
    float3 p : TEXCOORD1;    // 1/z, u/z, v/z
    uint4 a : TEXCOORD2;
    uint4 b : TEXCOORD3;
    uint4 c : TEXCOORD4;
};
struct VSOut {
    float3 p : TEXCOORD0;
    nointerpolation uint4 a : TEXCOORD1;
    nointerpolation uint4 b : TEXCOORD2;
    nointerpolation uint4 c : TEXCOORD3;
    nointerpolation float scale : TEXCOORD4;
    float4 pos : SV_Position;
};
VSOut main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos.x / 256.0 - 1.0, 1.0 - i.pos.y / 256.0, i.pos.z, 1.0); // 512x512 layer
    o.p = i.p;
    o.a = i.a;
    o.b = i.b;
    o.c = i.c;
    o.scale = i.pos.w;
    return o;
}
