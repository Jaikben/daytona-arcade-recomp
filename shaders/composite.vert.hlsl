// Fullscreen triangle for the screen composition pass.
struct VSOut {
    float2 uv : TEXCOORD0;
    float4 pos : SV_Position;
};
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    const float2 t = float2(float((id << 1) & 2), float(id & 2));
    o.uv = t;
    o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    return o;
}
