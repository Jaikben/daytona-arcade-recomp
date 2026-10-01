// Included in the pinned public libvita2d source, after its private state.
// Storage stays allocated for all three buffers; only the active ring changes.
static unsigned int activeDisplayBuffers = 3;

int daytona_vita2d_set_buffer_count(unsigned int count)
{
    if (count < 1 || count > 3 || !vita2d_initialized || drawing || system_app_mode) return -1;
    if (count == activeDisplayBuffers) return 0;
    sceGxmFinish(_vita2d_context);
    sceGxmDisplayQueueFinish();
    if (frontBufferIndex != 0)
        memcpy(displayBufferData[0], displayBufferData[frontBufferIndex],
               DISPLAY_HEIGHT * DISPLAY_STRIDE_IN_PIXELS * 4);
    vita2d_display_data data;
    data.address = displayBufferData[0];
    display_callback(&data);
    if (!vblank_wait) sceDisplayWaitVblankStart();
    frontBufferIndex = 0;
    backBufferIndex = count == 1 ? 0 : 1;
    activeDisplayBuffers = count;
    return 0;
}

static int daytona_vita2d_present_single(void)
{
    if (activeDisplayBuffers != 1 || system_app_mode) return 0;
    // Never enqueue a swap with the same old/new sync object. Wait for the
    // GPU, then display the sole buffer directly. Scanout can still tear.
    sceGxmFinish(_vita2d_context);
    vita2d_display_data data;
    data.address = displayBufferData[backBufferIndex];
    display_callback(&data);
    frontBufferIndex = backBufferIndex;
    return 1;
}
