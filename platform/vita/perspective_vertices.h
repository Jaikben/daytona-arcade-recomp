#pragma once

#include <vita2d.h>
#include <cmath>

namespace vita {

struct PerspectivePoint { float x, y, u, v, q; };

// Encode homogeneous positions for the existing libvita2d WVP shader.
// clip.w = depth gives GPU perspective interpolation without microtriangles.
inline bool perspective_vertex(vita2d_texture_vertex &out, const PerspectivePoint &p,
                               float source_w, float source_h) {
    if (!(p.q > 0) || !(source_w > 0) || !(source_h > 0)) return false;
    const float depth = 1.0f / p.q;
    out = {(p.x / 480.0f - 1.0f) * depth,
           (1.0f - p.y / 272.0f) * depth, depth,
           p.u / source_w, p.v / source_h};
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z) &&
           std::isfinite(out.u) && std::isfinite(out.v);
}
inline constexpr float perspective_matrix[16] = {
    1,0,0,0, 0,1,0,0, 0,0,0.5f,1, 0,0,0,0
};

// Preserve GPU18's interpolation and triangle order. A tessellated triangle
// repeats its lattice points up to six times; calculate each point only once
// in cached CPU memory before copying it into the GPU's vertex pool.
inline bool emit_perspective_triangle(vita2d_texture_vertex *out,
        const PerspectivePoint &a, const PerspectivePoint &b, const PerspectivePoint &c,
        int subdiv, float source_w, float source_h) {
    if (subdiv != 1 && subdiv != 2 && subdiv != 4 && subdiv != 8) return false;
    bool valid = true;
    auto evaluate = [&](int ib, int ic, vita2d_texture_vertex &dst) {
        const float wb = float(ib) / float(subdiv);
        const float wc = float(ic) / float(subdiv);
        const float wa = 1.0f - wb - wc;
        const float q = wa * a.q + wb * b.q + wc * c.q;
        dst.x = wa * a.x + wb * b.x + wc * c.x;
        dst.y = wa * a.y + wb * b.y + wc * c.y;
        dst.z = 0.5f;
        dst.u = (wa * a.u * a.q + wb * b.u * b.q + wc * c.u * c.q) / q / source_w;
        dst.v = (wa * a.v * a.q + wb * b.v * b.q + wc * c.v * c.q) / q / source_h;
        if (!std::isfinite(dst.x) || !std::isfinite(dst.y) ||
            !std::isfinite(dst.u) || !std::isfinite(dst.v)) valid = false;
    };
    if (subdiv == 1) {
        evaluate(0, 0, out[0]);
        evaluate(1, 0, out[1]);
        evaluate(0, 1, out[2]);
        return valid;
    }
    vita2d_texture_vertex grid[9][9];
    for (int row = 0; row <= subdiv; ++row)
        for (int col = 0; col <= subdiv - row; ++col)
            evaluate(col, row, grid[row][col]);
    for (int row = 0; row < subdiv; ++row) {
        for (int col = 0; col < subdiv - row; ++col) {
            *out++ = grid[row][col];
            *out++ = grid[row][col + 1];
            *out++ = grid[row + 1][col];
            if (col < subdiv - row - 1) {
                *out++ = grid[row][col + 1];
                *out++ = grid[row + 1][col + 1];
                *out++ = grid[row + 1][col];
            }
        }
    }
    return valid;
}

} // namespace vita
