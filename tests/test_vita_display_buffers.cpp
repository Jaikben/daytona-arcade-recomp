#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <vector>

// Exercise the exact adapter compiled into libvita2d with ordered API mocks.
static int vita2d_initialized = 1, drawing = 0, system_app_mode = 0;
static int vblank_wait = 1, _vita2d_context = 7;
static unsigned frontBufferIndex = 0, backBufferIndex = 0;
static constexpr unsigned DISPLAY_HEIGHT = 2, DISPLAY_STRIDE_IN_PIXELS = 2;
static unsigned char pixels[3][16]{};
static void *displayBufferData[] = {pixels[0], pixels[1], pixels[2]};
struct vita2d_display_data { void *address; };
static std::vector<int> calls;
static void sceGxmFinish(int context) { assert(context == 7); calls.push_back(1); }
static void sceGxmDisplayQueueFinish() { calls.push_back(2); }
static void sceDisplayWaitVblankStart() { calls.push_back(4); }
static void display_callback(const vita2d_display_data *data) {
    assert(data->address == displayBufferData[backBufferIndex] || data->address == pixels[0]);
    calls.push_back(3);
    if (vblank_wait) sceDisplayWaitVblankStart();
}
#include "../platform/vita/display_buffers.inc"

int main() {
    for (int invalid : {0, 4, 99}) assert(daytona_vita2d_set_buffer_count(invalid) == -1);
    drawing = 1; assert(daytona_vita2d_set_buffer_count(2) == -1); drawing = 0;
    system_app_mode = 1; assert(daytona_vita2d_set_buffer_count(2) == -1); system_app_mode = 0;
    vita2d_initialized = 0; assert(daytona_vita2d_set_buffer_count(2) == -1); vita2d_initialized = 1;
    assert(calls.empty());
    for (unsigned from = 1; from <= 3; ++from) {
        for (unsigned to = 1; to <= 3; ++to) {
            for (int wait = 0; wait <= 1; ++wait) {
                activeDisplayBuffers = from;
                frontBufferIndex = from - 1;
                backBufferIndex = from == 1 ? 0 : (frontBufferIndex + 1) % from;
                vblank_wait = wait;
                std::memset(pixels[frontBufferIndex], 42, 16);
                calls.clear();
                assert(daytona_vita2d_set_buffer_count(to) == 0);
                assert(activeDisplayBuffers == to);
                if (from == to) { assert(calls.empty()); continue; }
                assert((calls == std::vector<int>{1, 2, 3, 4}));
                assert(frontBufferIndex == 0 && backBufferIndex == (to == 1 ? 0 : 1));
                for (auto byte : pixels[0]) assert(byte == 42);
                calls.clear();
                assert(daytona_vita2d_present_single() == (to == 1));
                if (to == 1) {
                    assert(calls[0] == 1 && calls[1] == 3); // GPU finish before scanout
                    assert(calls.size() == (wait ? 3u : 2u));
                    assert(frontBufferIndex == backBufferIndex);
                } else assert(calls.empty()); // normal GXM swap path
            }
        }
    }
}
