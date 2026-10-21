// Actual CPU-only compose builder, also compiled by boundary tests.
static uint32_t BuildComposePacket(GpuCommandStorage& storage, int32_t W, int32_t H)
{
    if (g_controlQueuePoisoned || !storage.bytes ||
        (storage.state!=GpuCommandStorageState::Writable &&
         storage.state!=GpuCommandStorageState::Completed)) return 0;
    auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(storage.bytes + CMD_REQ_OFF);
    memset(sub, 0, sizeof(*sub));
    sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D; sub->hdr.ctx_id = COMP_CTX_ID;
    uint32_t* dw = ComposeDwBase(storage.bytes); uint32_t n = 0;

    // Framebuffer + clear (opaque black; wallpaper quad covers it).
    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    dw[n++] = 1; dw[n++] = 0; dw[n++] = COMP_SCANOUT_SURF;
    dw[n++] = VirglCmd0(VIRGL_CCMD_CLEAR, 0, 8);
    dw[n++] = PIPE_CLEAR_COLOR0;
    dw[n++] = F32Bits(0.0f); dw[n++] = F32Bits(0.0f); dw[n++] = F32Bits(0.0f); dw[n++] = F32Bits(1.0f);
    dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
    // Viewport: clip [-1,1] -> [0,W]x[0,H]. scale=(W/2,H/2), translate=(W/2,H/2),
    // built from integer dims via IntToF32Bits (no runtime FP).
    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7);
    dw[n++] = 0;
    dw[n++] = GpuViewportHalfBits(W); dw[n++] = GpuViewportHalfBits(H); dw[n++] = F32Bits(1.0f);
    dw[n++] = GpuViewportHalfBits(W); dw[n++] = GpuViewportHalfBits(H); dw[n++] = F32Bits(0.0f);
    // State bound once for the whole frame.
    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_RASTERIZER, 1);      dw[n++] = DRAW_RAST;
    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_DSA, 1);             dw[n++] = DRAW_DSA;
    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 1); dw[n++] = DRAW_VE;
    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = DRAW_VS; dw[n++] = PIPE_SHADER_VERTEX;
    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = DRAW_FS; dw[n++] = PIPE_SHADER_FRAGMENT;
    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SAMPLER_STATES, 0, 3);
    dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = DRAW_SAMP;

    uint32_t lastFragment=DRAW_FS;
    uint32_t lastBlend = 0;
    uint32_t lastSamp  = DRAW_SAMP;   // nearest bound above
    uint32_t barrierCursor = 0;       // next barrier to process (indices ascending)
    for (uint32_t i = 0; i < g_drawQuadCount; ++i)
    {
        // At each backdrop barrier landing on this quad, snapshot the scanout
        // (everything drawn so far), downsample it into BLUR_A, run the separable
        // gaussian (H: A->B, V: B->A), then restore the scanout framebuffer/
        // viewport/shader/sampler so the following backdrop quad (which samples
        // BLUR_A via GPU_TEX_BLUR_BACKDROP) composites normally. Multiple barriers
        // per frame (one per glass window, back-to-front) each re-snapshot the
        // current scene, so a glass window frosts exactly what is beneath it —
        // including lower glass windows (BRO-185). One stream, in order — the host
        // serializes it.
        while (barrierCursor < g_blurBarrierCount &&
               g_blurBarrierIdx[barrierCursor] == i && g_blurReady)
        {
            uint32_t barrierStrength = g_blurBarrierStrength[barrierCursor];
            ++barrierCursor;
            // Iterations scale with the app-requested blur strength (a window's
            // blurRadius): each extra H/V gaussian pair widens the effective blur.
            uint32_t iters = (barrierStrength + 3) / 4;   // radius 4->1, 8->2, ...
            if (iters < 1) iters = 1;
            if (iters > 4) iters = 4;                     // cap cost
            if (n + 128 + iters * 64 > COMPOSE_DWORD_CAPACITY) return 0;
            else
            {
                const int32_t bw = static_cast<int32_t>(COMP_BLUR_W);
                const int32_t bh = static_cast<int32_t>(COMP_BLUR_H);
                // Downsample BLIT scanout -> BLUR_A (linear).
                uint32_t s0 = (VIRGL_BLIT_MASK_RGBA & 0xFF) | ((VIRGL_TEX_FILTER_LINEAR & 0x3) << 8);
                dw[n++] = VirglCmd0(VIRGL_CCMD_BLIT, 0, 21);
                dw[n++] = s0; dw[n++] = 0; dw[n++] = 0;
                dw[n++] = COMP_BLUR_A_RES; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
                dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
                dw[n++] = COMP_BLUR_W; dw[n++] = COMP_BLUR_H; dw[n++] = 1;
                dw[n++] = COMP_SCANOUT_RES; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
                dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
                dw[n++] = static_cast<uint32_t>(W); dw[n++] = static_cast<uint32_t>(H); dw[n++] = 1;
                // Opaque blend + linear sampler + blur vertex buffer + blur viewport.
                dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_BLEND, 1); dw[n++] = DRAW_BLEND;
                dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SAMPLER_STATES, 0, 3);
                dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = DRAW_SAMP_LIN;
                dw[n++] = VirglCmd0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7);
                dw[n++] = 0;
                dw[n++] = IntToF32Bits(bw / 2); dw[n++] = IntToF32Bits(bh / 2); dw[n++] = F32Bits(1.0f);
                dw[n++] = IntToF32Bits(bw / 2); dw[n++] = IntToF32Bits(bh / 2); dw[n++] = F32Bits(0.0f);
                dw[n++] = VirglCmd0(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3);
                dw[n++] = 20; dw[n++] = 0; dw[n++] = COMP_BLURVTX_RES;
                // `iters` separable passes; each pair leaves the result in BLUR_A.
                for (uint32_t it = 0; it < iters; ++it)
                {
                    // H pass: BLUR_A -> BLUR_B.
                    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
                    dw[n++] = 1; dw[n++] = 0; dw[n++] = BLUR_B_SURF;
                    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = DRAW_FS_BLURH; dw[n++] = PIPE_SHADER_FRAGMENT;
                    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_SAMPLER_VIEWS, 0, 3);
                    dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = g_blurAView;
                    dw[n++] = VirglCmd0(VIRGL_CCMD_DRAW_VBO, 0, 12);
                    dw[n++] = 0; dw[n++] = 4; dw[n++] = PIPE_PRIM_TRIANGLE_STRIP; dw[n++] = 0;
                    dw[n++] = 1; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
                    dw[n++] = 0; dw[n++] = 0; dw[n++] = 3; dw[n++] = 0;
                    // V pass: BLUR_B -> BLUR_A.
                    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
                    dw[n++] = 1; dw[n++] = 0; dw[n++] = BLUR_A_SURF;
                    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = DRAW_FS_BLURV; dw[n++] = PIPE_SHADER_FRAGMENT;
                    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_SAMPLER_VIEWS, 0, 3);
                    dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = g_blurBView;
                    dw[n++] = VirglCmd0(VIRGL_CCMD_DRAW_VBO, 0, 12);
                    dw[n++] = 0; dw[n++] = 4; dw[n++] = PIPE_PRIM_TRIANGLE_STRIP; dw[n++] = 0;
                    dw[n++] = 1; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
                    dw[n++] = 0; dw[n++] = 0; dw[n++] = 3; dw[n++] = 0;
                }
                // Restore scanout framebuffer + viewport + chrome shader + nearest.
                dw[n++] = VirglCmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
                dw[n++] = 1; dw[n++] = 0; dw[n++] = COMP_SCANOUT_SURF;
                dw[n++] = VirglCmd0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7);
                dw[n++] = 0;
                dw[n++] = GpuViewportHalfBits(W); dw[n++] = GpuViewportHalfBits(H); dw[n++] = F32Bits(1.0f);
                dw[n++] = GpuViewportHalfBits(W); dw[n++] = GpuViewportHalfBits(H); dw[n++] = F32Bits(0.0f);
                dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = DRAW_FS; dw[n++] = PIPE_SHADER_FRAGMENT;
                dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SAMPLER_STATES, 0, 3);
                dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = DRAW_SAMP;
                lastFragment=DRAW_FS;
                lastBlend = DRAW_BLEND;   // blur bound opaque
                lastSamp  = DRAW_SAMP;
            }
        }

        const DrawQuadRec& q = g_drawQuads[i];
        // At most 30 dwords/quad; bail if we'd overflow the compose region (last page
        // is the response buffer).
        if (n + 32 > COMPOSE_DWORD_CAPACITY) return 0;
        if (q.blend != lastBlend) {
            dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_BLEND, 1);
            dw[n++] = q.blend;
            lastBlend = q.blend;
        }
        const uint32_t fragment=(q.flags&QUAD_PREMULTIPLIED)?DRAW_FS_PM:DRAW_FS;
        if (fragment!=lastFragment) {
            dw[n++]=VirglCmd0(VIRGL_CCMD_BIND_SHADER,0,2);
            dw[n++]=fragment;dw[n++]=PIPE_SHADER_FRAGMENT;lastFragment=fragment;
        }
        // Frosted-glass backdrop quads sample the (upscaled) blur RT with linear
        // filtering; everything else stays nearest (crisp 1:1 textures/masks).
        uint32_t wantSamp = (q.flags & QUAD_SAMPLE_BLUR) ? DRAW_SAMP_LIN : DRAW_SAMP;
        if (wantSamp != lastSamp) {
            dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SAMPLER_STATES, 0, 3);
            dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = wantSamp;
            lastSamp = wantSamp;
        }
        dw[n++] = VirglCmd0(VIRGL_CCMD_SET_SAMPLER_VIEWS, 0, 3);
        dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = q.sview;
        dw[n++] = VirglCmd0(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3);
        dw[n++] = 20; dw[n++] = i * 80; dw[n++] = COMP_DRAW_VBUF_RES;
        dw[n++] = VirglCmd0(VIRGL_CCMD_DRAW_VBO, 0, 12);
        dw[n++] = 0; dw[n++] = 4; dw[n++] = PIPE_PRIM_TRIANGLE_STRIP; dw[n++] = 0;
        dw[n++] = 1; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 3; dw[n++] = 0;
    }
    sub->size = n * 4;
    return sizeof(VirtioGpuCmdSubmit)+n*4;
}
