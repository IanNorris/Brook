#pragma once

// gpu_compositor.h — GPU composition interface (GL DRAW / textured-quad based).
//
// A 3D-capable display driver (virtio-gpu with virgl) registers a set of
// composition ops here. The window compositor uses them, when enabled, to
// composite window content on the GPU: each window's pixel buffer becomes a
// host texture, and one textured-quad draw per window composes it into a scanout
// render-target. The CPU never blits window pixels into the final framebuffer —
// changed window content moves guest->host as device DMA (TRANSFER_TO_HOST_3D).
//
// All ops are absent (GpuCompositorGet() == nullptr) unless a driver has
// registered AND proven its 3D path, and the compositor only activates the GPU
// path when DrawSupported() is true; otherwise the CPU compositor path is used
// and is completely untouched.

#include <stdint.h>
#include "gpu_scene.h"

namespace brook {

// Opaque per-texture handle. 0 == invalid.
typedef uint32_t GpuTexId;
// Canonical virgl formats supported by retained capture. These values describe
// the resource created by DRM, independently of a Wayland buffer declaration.
inline constexpr bool GpuCaptureFormatSupported(uint32_t format) {
    return format == 1 || format == 2; // B8G8R8A8_UNORM / B8G8R8X8_UNORM
}

// Special source handle for DrawQuad: sample the blurred backdrop (the scene as
// composited up to the most recent BlurBarrier, downsampled + gaussian-blurred)
// at the quad's destination rect in screen space. Used to paint frosted glass
// behind a window's titlebar. Only valid in the phase after BlurBarrier().
static constexpr GpuTexId GPU_TEX_BLUR_BACKDROP = 0xFFFFFFFFu;
enum class GpuCaptureResult : int32_t {
    Uncertain = -1, // A GPU operation failed: keep the source lease/quarantine.
    Rejected = 0,  // Rejected before any GPU commands; no source read started.
    Complete = 1,  // Source read completed and compositor source detached.
};

struct GpuCompositorOps {
    const char* name;

    // Create a sampler-view texture of (w,h). If `alpha` is false the texture is
    // B8G8R8X8 (opaque content); if true it is B8G8R8A8 so alpha is preserved
    // for cursor/chrome and Wayland SHM content, including CSD shadows.
    // The caller must also enable blending when drawing alpha content. Backing is the
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
    // The compositor composes each layer through the GL pipeline via DrawQuad
    // (enables per-window opacity). The GPU path is only used when DrawSupported()
    // returns true; otherwise the CPU compositor path handles composition.

    // True if the GL DRAW pipeline is set up and ready.
    bool (*DrawSupported)();

    // Draw a src texture region as a textured quad into the scanout at a dst
    // region (scaled), with optional src-alpha blend and a uniform opacity
    // (0..255; 255 = opaque). Recorded per call; the batch is composed +
    // presented by EndFrame.
    void (*DrawQuad)(GpuTexId src,
                     uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                     uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh,
                     uint32_t opacity, bool alphaBlend);

    // Mark the backdrop barrier: everything drawn so far this frame (wallpaper +
    // window content) is the "backdrop". At present time the compositor snapshots
    // the scanout at this point, downsamples + gaussian-blurs it, and makes it
    // available to subsequent DrawQuad(GPU_TEX_BLUR_BACKDROP, ...) calls so a
    // window can show frosted glass. `strength` is the app-requested blur amount
    // (a window's blurRadius; the compositor passes the max across glass windows)
    // and scales how wide the blur is. No-op if blur is unsupported.
    void (*BlurBarrier)(uint32_t strength);

    // Owned-capture prerequisites. Only the compositor thread may call these,
    // outside BeginFrame/EndFrame assembly. Destinations have no guest backing.
    // XRGB destinations force alpha one; ARGB destinations preserve source alpha.
    // Creation publishes a handle only after resource/attachment/view creation.
    // Failed teardown retains an unusable quarantined metadata slot; never
    // fall back to DestroyTexture on an owned texture.
    GpuTexId (*CreateOwnedTexture)(uint32_t w, uint32_t h, bool alpha);
    bool (*DestroyOwnedTexture)(GpuTexId tex);
    // Caller pins the canonical source/producer and validates its dimensions.
    // srcFormat is canonical virgl B8G8R8A8_UNORM(1) or B8G8R8X8_UNORM(2),
    // never an unchecked client declaration. Unsupported formats reject.
    // Success fences producer, copies, fences compositor, then detaches source.
    // An uncertain result must never authorize buffer release or backing free.
    GpuCaptureResult (*CaptureExternal)(GpuTexId dst, int32_t producerCtx,
                                      uint32_t srcGres, uint32_t srcFormat, uint32_t srcW, uint32_t srcH);
    // Compose retained owned sources into a distinct owned ARGB staging target.
    // Caller pins every source and destination, outside frame assembly. Invalid
    // lists reject before commands; uncertain completion requires retaining all
    // inputs. Complete includes a fence and restoration of desktop GPU state.
    GpuCaptureResult (*ComposeOwnedLayers)(GpuTexId dst,const GpuSceneLayer*,uint32_t count);
    // Draw a retained premultiplied root during desktop frame assembly. Opacity
    // scales RGB and alpha together; existing DrawQuad retains its legacy mode.
    void (*DrawPremultipliedQuad)(GpuTexId src,uint32_t sx,uint32_t sy,uint32_t sw,uint32_t sh,
                                 uint32_t dx,uint32_t dy,uint32_t dw,uint32_t dh,uint32_t opacity);
};

// Register the GPU compositor ops (called by the display driver once its 3D
// path is proven). extern "C" so loadable driver modules can import it.
extern "C" void GpuCompositorRegister(const GpuCompositorOps* ops);

// Get the active GPU compositor ops, or nullptr if none registered.
const GpuCompositorOps* GpuCompositorGet();

} // namespace brook
