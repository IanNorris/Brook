static void VgpuPanicPresent()
{
    if (!ControlQueueEmergencySafe()) return;
    if (!g_fbBacking || g_fbW == 0 || g_fbH == 0) return;

    // Revert scanout to the 2D framebuffer the panic was drawn into. Idempotent
    // if it was already RESOURCE_FB (non-composite path).
    SetScanout(0, RESOURCE_FB, g_fbW, g_fbH);

    // Push the whole framebuffer to the host and scan it out.
    TransferToHost2D(RESOURCE_FB, 0, 0, g_fbW, g_fbH, 0);
    ResourceFlush(RESOURCE_FB, 0, 0, g_fbW, g_fbH);
}

