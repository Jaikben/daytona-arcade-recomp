#pragma once

#include "runtime/video.h"
#include <vita2d.h>
#include <cstdint>
#include <vector>

namespace vita {

class GpuFastRenderer {
public:
    GpuFastRenderer();
    ~GpuFastRenderer();
    GpuFastRenderer(const GpuFastRenderer &) = delete;
    GpuFastRenderer &operator=(const GpuFastRenderer &) = delete;

    bool ok() const { return background_ && foreground_; }
    void reset_materials();
    void reap_retired();
    void shutdown();
    void draw(rt::Video &video);
    void draw_exact(rt::Video &video);
    double last_gpu_ms() const { return last_gpu_ms_; }
    std::size_t cached_bytes() const { return cached_bytes_; }
    std::size_t cached_materials() const { return materials_.size(); }
    std::size_t retired_bytes() const { return retired_bytes_; }
    unsigned pool_drops() const { return pool_drops_; }
    unsigned min_pool_free() const { return min_pool_free_; }

private:
    struct MaterialKey {
        uint16_t h0 = 0, h1 = 0, h2 = 0, h3 = 0;
        uint8_t luma = 0;
        bool translucent = false;
        bool operator==(const MaterialKey &o) const {
            return h0 == o.h0 && h1 == o.h1 && h2 == o.h2 && h3 == o.h3 &&
                   luma == o.luma && translucent == o.translucent;
        }
    };
    struct Material {
        MaterialKey key;
        vita2d_texture *texture = nullptr;
        uint32_t source_w = 1, source_h = 1;
        uint32_t tex_w = 1, tex_h = 1;
        std::size_t bytes = 0;
        uint64_t stamp = 0;
    };

    vita2d_texture *background_ = nullptr;
    vita2d_texture *foreground_ = nullptr;
    std::vector<Material> materials_;
    std::vector<vita2d_texture *> retired_;
    std::vector<std::size_t> order_;
    std::size_t cached_bytes_ = 0, retired_bytes_ = 0;
    uint64_t stamp_ = 0;
    double last_gpu_ms_ = 0.0;
    unsigned pool_drops_ = 0, min_pool_free_ = 0;
    bool shutdown_ = false;
    uint8_t gamma_[256]{};

    static constexpr std::size_t kCacheLimit = 40u * 1024u * 1024u;
    static constexpr uint32_t kTextureLimit = 512;

    static uint16_t le16(const uint8_t *base, uint32_t index);
    static uint32_t swap_rb(uint32_t argb);
    void upload_layer(vita2d_texture *texture, const std::vector<uint32_t> &pixels);
    Material *material_for(const rt::GeoPoly &poly, const rt::VideoMem &mem);
    Material build_material(const rt::GeoPoly &poly, const rt::VideoMem &mem);
    void evict_for(std::size_t bytes);
    uint32_t shade_texel(const rt::GeoPoly &poly, const rt::VideoMem &mem, uint8_t texel) const;
    uint32_t solid_color(const rt::GeoPoly &poly, const rt::VideoMem &mem) const;
    void draw_polygons(rt::Video &video);
};

} // namespace vita
