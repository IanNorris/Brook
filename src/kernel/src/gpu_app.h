#pragma once

// gpu_app.h — userspace GPU (virgl) access interface.
//
// A 3D-capable display driver (virtio-gpu with virgl) registers these ops so the
// core kernel's syscall layer can give userspace apps a private, per-process
// virgl context for hardware-accelerated rendering (OpenGL-style command streams)
// WITHOUT exposing the compositor's own context. Each app gets its own virgl
// context + resource id space; the app submits virgl command-stream dwords and
// the kernel relays them to the host, exactly as the compositor already does.
//
// All ops are absent (GpuAppGet() == nullptr) unless a driver has registered AND
// proven its 3D path. Handles are opaque; ctxId/resId namespaces are managed by
// the driver. Functions return <0 (negative errno) on failure, >=0 on success.

#include <stdint.h>

namespace brook {

struct GpuAppOps {
    const char* name;

    // Create a new private virgl context for an app (pid for bookkeeping/logging).
    // Returns a context id (>0) or <0 on failure.
    int32_t (*CtxCreate)(uint32_t pid);

    // Destroy a context and reclaim its resources. Safe to call on exit.
    void (*CtxDestroy)(int32_t ctxId);

    // Create a 3D resource in the context. `format`/`bind` are virgl values
    // (VIRGL_FORMAT_*, VIRGL_BIND_*). `target` is a PIPE_* resource target:
    // 0 (PIPE_BUFFER) creates a 1-D linear buffer (w = byte length, h ignored),
    // anything else a 2-D texture. The distinction is mandatory — virglrenderer
    // rejects a resource whose bind flags imply a buffer but whose target is a
    // texture (Mesa's glReadPixels staging buffer). Returns the resource's
    // host-global id (>0) — the app uses this SAME id both in later ops and
    // inside the virgl command streams it submits — or <0.
    int32_t (*ResourceCreate3D)(int32_t ctxId, uint32_t target, uint32_t format,
                                uint32_t bind, uint32_t w, uint32_t h);

    // Back a resource with guest memory at kernel-virtual `vaddr` (size `bytes`),
    // described to the host page-by-page (scatter-gather). The buffer may be a
    // window VFB (so host-rendered pixels land straight in the window) or a
    // vertex buffer the app fills. Returns 0 or <0.
    int32_t (*ResourceAttachUser)(int32_t ctxId, int32_t resId,
                                  uint64_t vaddr, uint32_t bytes);

    // Create a vertex-buffer resource backed by a copy of `bytes` from `src`
    // (kernel-readable; the caller validated it), and push it to the host. Used
    // for app-provided vertex data without translating user page tables. Returns
    // the new buffer's host-global id (>0, usable in SET_VERTEX_BUFFERS) or <0.
    // The backing is freed when the context is destroyed.
    int32_t (*BufferUpload)(int32_t ctxId, const void* src, uint32_t bytes);

    // Submit `n` virgl command dwords (already validated/copied by the caller)
    // to the context. Returns 0 or <0.
    int32_t (*Submit3D)(int32_t ctxId, const uint32_t* dwords, uint32_t n);

    // Transfer a resource box guest<->host. dir: 0 = TO host, 1 = FROM host.
    int32_t (*Transfer3D)(int32_t ctxId, int32_t resId, int dir,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t texW, uint32_t texH);

    // Fetch a host 3D capability set (virgl_caps blob) via the virtio-gpu
    // GET_CAPSET command. `capsetId` is a VIRTIO_GPU_CAPSET_* value (1=VIRGL,
    // 2=VIRGL2), `version` the requested capset version. Copies up to `maxBytes`
    // of the blob into `out` (kernel-writable). Returns the number of bytes
    // written (>0) or <0 on failure. Needed so unmodified Mesa's DRM GET_CAPS
    // ioctl can obtain a real capset and bind the hardware virgl screen instead
    // of falling back to software (llvmpipe).
    int32_t (*GetCapset)(uint32_t capsetId, uint32_t version,
                         void* out, uint32_t maxBytes);
};

// Register the app-GPU ops (called by the display driver once its 3D path is
// proven). extern "C" so loadable driver modules can import it.
extern "C" void GpuAppRegister(const GpuAppOps* ops);

// Get the active app-GPU ops, or nullptr if none registered.
const GpuAppOps* GpuAppGet();

} // namespace brook
