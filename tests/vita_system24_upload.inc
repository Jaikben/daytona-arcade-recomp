// Tests the exact CPU upload function used by the Vita GXM renderer, with
// padded GPU-like strides and skipped presentations. No Vita SDK is required.
void system24_upload_tests() {
    Memory mem;
    std::fill(mem.tile.begin(), mem.tile.end(), 0);
    std::fill(mem.chars.begin(), mem.chars.end(), 0x11);
    std::fill(mem.pal.begin(), mem.pal.end(), 0);
    std::fill(mem.xlat.begin(), mem.xlat.end(), 0xff);
    auto video = std::make_unique<rt::Video>(mem.tile.data(), mem.chars.data());
    video->set_external_3d(true);
    video->enable_write_tracking();
    for (unsigned i = 0; i < 0x2000; ++i) video->palette_w(i, mem.pal.data(), mem.xlat.data());
    constexpr size_t stride = 520;
    constexpr uint32_t sentinel = 0x19a55a91u;
    std::array<std::vector<uint32_t>, 8> current, fresh;
    for (auto &texture : current) texture.assign(stride * 512, sentinel);
    for (auto &texture : fresh) texture.assign(stride * 512, sentinel);
    uint64_t previous = UINT64_MAX;
    auto frame = [&] {
        video->tile_memory_w();
        video->character_memory_w();
        video->screen_update({}, 0, mem.view());
    };
    auto upload = [&] {
        unsigned count = 0;
        for (int layer = 0; layer < 4; ++layer) {
            count += vita::upload_system24_layer(*video, layer, previous,
                current[size_t(layer)].data(), stride * sizeof(uint32_t),
                current[size_t(layer + 4)].data(), stride * sizeof(uint32_t));
            vita::upload_system24_layer(*video, layer, UINT64_MAX,
                fresh[size_t(layer)].data(), stride * sizeof(uint32_t),
                fresh[size_t(layer + 4)].data(), stride * sizeof(uint32_t));
        }
        previous = video->system24_texture_generation();
        for (size_t plane = 0; plane < current.size(); ++plane) {
            compare(current[plane].data(), fresh[plane].data(), current[plane].size(),
                    int(plane), "incremental GXM tile upload");
            for (size_t y = 0; y < 512; ++y)
                for (size_t x = 512; x < stride; ++x)
                    require(current[plane][y * stride + x] == sentinel, "GPU stride padding unchanged");
        }
        return count;
    };
    frame();
    require(upload() == 16384, "initial GXM upload initializes every tile");
    require(current[0][0] == 0xffffffff && current[4][0] == 0,
            "category 0 tile goes behind 3D");
    frame();
    require(upload() == 0, "unchanged GXM texture has no writes");
    for (unsigned i = 0; i < 17; ++i) mem.word(i * 13u, 1);
    frame();
    require(upload() == 17, "17 changes upload only 17 tiles, not eight full textures");
    mem.chars[32] ^= 0x11;
    frame();
    require(upload() == 17, "character changes upload every referencing tile");
    mem.word(0x1000 + 8, 1);
    frame(); // The host skips presenting this emulated frame.
    mem.word(0x3000 + 12, 1);
    frame();
    require(upload() == 2, "dirty tiles survive skipped presentation");
    mem.word(0x5000, 43);
    mem.word(0x6000, 0xa55a);
    frame();
    require(upload() == 0, "scroll and masks do not recopy texture pixels");
    mem.word(0x2000, 0x8000);
    frame();
    require(upload() == 1, "category change uploads one tile");
    require(current[2][0] == 0xffffffff && current[6][0] == 0xffffffff,
            "normal opaque background ignores category");
    mem.word(0x5006, 0x2000);
    frame();
    require(upload() == 8192, "normal-to-split updates background pair alpha");
    require(current[2][0] == 0 && current[6][0] == 0xffffffff,
            "split opaque background filters category 1");
    mem.word(0x5006, 0x6000);
    frame();
    require(upload() == 0, "split-to-split does not rewrite unchanged alpha");
    mem.word(0x5006, 0);
    frame();
    require(upload() == 8192, "split-to-normal restores category 1 background");
    require(current[2][0] == 0xffffffff, "normal category restored");
    std::fill(mem.xlat.begin(), mem.xlat.end(), 0);
    video->palette_w(1, mem.pal.data(), mem.xlat.data());
    frame();
    require(upload() == 16384, "palette update recolors all cached tile planes");
    require(current[2][0] == 0xff000000, "updated palette is visible");
    video->palette_w(1, mem.pal.data(), mem.xlat.data());
    frame();
    require(upload() == 0, "same palette value does not invalidate uploads");
    require(vita::upload_system24_layer(*video, 0, UINT64_MAX, current[0].data(), 128,
                                        current[4].data(), stride * sizeof(uint32_t)) == 0,
            "invalid texture stride rejected");
    std::puts("PASS: exact Vita tile uploader: sparse writes, glyphs, skipped frames, palettes, split categories, padded strides");
}
