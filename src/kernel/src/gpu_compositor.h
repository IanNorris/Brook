#pragma once

// gpu_compositor.h — GPU composition interface (BLIT-based).
//
// A 3D-capable display driver (virtio-gpu with virgl) registers a set of
// composition ops here. The window compositor uses them, when enabled, to
// composite window content on the GPU: each window's pixel buffer becomes a
// host texture, and one BLIT per window draws it into a scanout render-target.
// The CPU never blits window pixels into the final framebuffer — changed window
// content moves guest->host as device DMA (TRANSFER_TO_HOST_3D) only.
//
// All ops are absent (GpuCompositorGet() == nullptr) unless a driver has
// registered AND proven its 3D path, so the CPU compositor path is the default
// and is completely untouched when GPU composition is off.

#include <stdint.h>

namespace brook {

// Opaque per-texture handle. 0 == invalid.
typedef uint32_t GpuTexId;

struct GpuCompositorOps {
    const char* name;

    // Create a sampler-view texture of (w,h). If `alpha` is false the texture is
    // B8G8R8X8 (opaque, window/desktop content); if true it is B8G8R8A8 so the
    // source alpha is honoured by an alpha-blended Blit (cursor). Backing is the
    // w*h*4 buffer at kernel virtual address `backingVaddr` (may be physically
    // scattered, e.g. a VmmAllocPages VFB). Returns 0 on failure.
    GpuTexId (*CreateTexture)(uint32_t w, uint32_t h, uint64_t backingVaddr, bool alpha);

    // Destroy a texture created by CreateTexture.
    void (*DestroyTexture)(GpuTexId tex);

    // Upload a dirty rect of the texture's backing into the host texture
    // (device DMA, not a CPU copy). Call after the window has rendered.
    void (*UpdateTexture)(GpuTexId tex, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h);

    // Begin a composited frame: bind + clear the scanout render-target to
    // `clearArgb` (0xAARRGGBB; alpha ignored, opaque clear).
    void (*BeginFrame)(uint32_t clearArgb);

    // BLIT a src texture region into the scanout RT at a dst region (scaled).
    // alphaBlend = blend over existing scanout content (for chrome/cursor).
    void (*Blit)(GpuTexId src,
                 uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                 uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh,
                 bool alphaBlend);

    // Present the composed scanout RT (flush). Ends the frame.
    void (*EndFrame)();

    // Scanout dimensions (== display size).
    void (*GetSize)(uint32_t* w, uint32_t* h);

    // Capture a downscaled thumbnail of the last presented scanout into `out`
    // (BGRA, up to `maxPixels`). BLITs the full scanout RT into a small
    // thumbnail render-target, reads it back, and copies it out. Sets *outW/*outH
    // to the thumbnail dimensions and returns the pixel count (0 on failure).
    // Used for in-guest visual verification without a host display.
    uint32_t (*CaptureThumb)(uint32_t* out, uint32_t maxPixels,
                             uint32_t* outW, uint32_t* outH);

    // Capture the FULL presented scanout at native resolution (1:1, no
    // downscale). BLITs the scanout RT into a full-size readback RT, reads it
    // back, and returns a pointer to the driver-internal BGRA pixel buffer (do
    // not free). Sets *outW/*outH to the scanout dimensions. Returns nullptr on
    // failure. For full-res visual / text-fidelity verification.
    const uint32_t* (*CaptureFull)(uint32_t* outW, uint32_t* outH);

    // --- DRAW (textured-quad) composition path ---------------------------
    // Optional. When DrawSupported() returns true, the compositor may use
    // DrawQuad instead of Blit to compose each layer through the GL pipeline
    // (enables per-window opacity). Falls back to Blit when absent/false.

    // True if the GL DRAW path is set up and active (opt/gpudraw + pipeline OK).
    bool (*DrawSupported)();

    // Draw a src texture region as a textured quad into the scanout at a dst
    // region (scaled), with optional src-alpha blend and a uniform opacity
    // (0..255; 255 = opaque). Recorded per call; the batch is composed +
    // presented by EndFrame. Coordinates match Blit.
    void (*DrawQuad)(GpuTexId src,
                     uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                     uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh,
                     uint32_t opacity, bool alphaBlend);
};

// Register the GPU compositor ops (called by the display driver once its 3D
// path is proven). extern "C" so loadable driver modules can import it.
extern "C" void GpuCompositorRegister(const GpuCompositorOps* ops);

// Get the active GPU compositor ops, or nullptr if none registered.
const GpuCompositorOps* GpuCompositorGet();

} // namespace brook
