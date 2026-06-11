// drmclear.c — Milestone 1 validation for Brook's virtio-gpu DRM render node.
//
// Hand-rolls the virtio-gpu DRM uABI (no libdrm dependency, mirroring drmprobe)
// to exercise the M1 resource/submit path end to end against the REAL host
// virglrenderer:
//   open renderD128 -> CONTEXT_INIT -> RESOURCE_CREATE (render target) ->
//   EXECBUFFER (a virgl stream that makes a surface over the RT and CLEARs it
//   to a known colour) -> TRANSFER_FROM_HOST -> WAIT -> RESOURCE_INFO ->
//   GEM_CLOSE.
//
// The host virglrenderer validates the command stream and resource references,
// so a clean run is real external evidence the DRM<->virgl mapping is correct
// (not merely self-consistent). Pixel readback needs VIRTGPU_MAP (M1b); for now
// success = every ioctl returns 0 and the host accepts the clear. Prints
// DRMCLEAR_OK on success.

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define IOWR(nr, size) ((3u << 30) | ((unsigned)(size) << 16) | ('d' << 8) | (nr))

struct drm_virtgpu_context_set_param { uint64_t param, value; };
struct drm_virtgpu_context_init { uint32_t num_params, pad; uint64_t ctx_set_params; };
struct drm_virtgpu_resource_create {
    uint32_t target, format, bind, width, height, depth, array_size;
    uint32_t last_level, nr_samples, flags, bo_handle, res_handle, size, stride;
};
struct drm_virtgpu_resource_info { uint32_t bo_handle, res_handle, size, blob_mem; };
struct drm_virtgpu_3d_box { uint32_t x, y, z, w, h, d; };
struct drm_virtgpu_3d_transfer { uint32_t bo_handle; struct drm_virtgpu_3d_box box;
    uint32_t level, offset, stride, layer_stride; };
struct drm_virtgpu_execbuffer {
    uint32_t flags, size; uint64_t command, bo_handles;
    uint32_t num_bo_handles; int32_t fence_fd;
    uint32_t ring_idx, syncobj_stride, num_in_syncobjs, num_out_syncobjs;
    uint64_t in_syncobjs, out_syncobjs;
};
struct drm_virtgpu_3d_wait { uint32_t handle, flags; };
struct drm_gem_close { uint32_t handle, pad; };

#define IOCTL_CONTEXT_INIT       IOWR(0x4b, sizeof(struct drm_virtgpu_context_init))
#define IOCTL_RESOURCE_CREATE    IOWR(0x44, sizeof(struct drm_virtgpu_resource_create))
#define IOCTL_RESOURCE_INFO      IOWR(0x45, sizeof(struct drm_virtgpu_resource_info))
#define IOCTL_TRANSFER_FROM_HOST IOWR(0x46, sizeof(struct drm_virtgpu_3d_transfer))
#define IOCTL_EXECBUFFER         IOWR(0x42, sizeof(struct drm_virtgpu_execbuffer))
#define IOCTL_WAIT               IOWR(0x48, sizeof(struct drm_virtgpu_3d_wait))
#define IOCTL_GEM_CLOSE          IOWR(0x09, sizeof(struct drm_gem_close))

// virgl command encoding (mirrors gltri.c / the kernel driver).
#define VIRGL_CCMD_CREATE_OBJECT        1
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5
#define VIRGL_CCMD_CLEAR                7
#define VIRGL_OBJECT_SURFACE           8
#define VIRGL_FORMAT_B8G8R8X8_UNORM     2
#define VIRGL_BIND_RENDER_TARGET     (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW      (1u << 3)
#define PIPE_CLEAR_COLOR0            (1u << 2)
#define PIPE_TEXTURE_2D                 2

#define W 64
#define H 64
#define H_SURF 1

static uint32_t cmd0(uint32_t cmd, uint32_t obj, uint32_t len) {
    return (cmd & 0xff) | ((obj & 0xff) << 8) | (len << 16);
}
static uint32_t f32bits(float f) { union { float f; uint32_t u; } u; u.f = f; return u.u; }

int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0) { fprintf(stderr, "DRMCLEAR: open failed\n"); return 1; }
    fprintf(stderr, "DRMCLEAR: opened fd=%d\n", fd);

    // CONTEXT_INIT (capset = VIRGL2).
    struct drm_virtgpu_context_set_param cp = { 0x0001 /*CAPSET_ID*/, 2 /*VIRGL2*/ };
    struct drm_virtgpu_context_init ci = { 1, 0, (uint64_t)(uintptr_t)&cp };
    if (ioctl(fd, IOCTL_CONTEXT_INIT, &ci) < 0) { fprintf(stderr, "DRMCLEAR: CONTEXT_INIT failed\n"); return 2; }
    fprintf(stderr, "DRMCLEAR: CONTEXT_INIT ok\n");

    // RESOURCE_CREATE a 2D render-target/sampler resource.
    struct drm_virtgpu_resource_create rc;
    memset(&rc, 0, sizeof(rc));
    rc.target = PIPE_TEXTURE_2D;
    rc.format = VIRGL_FORMAT_B8G8R8X8_UNORM;
    rc.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW;
    rc.width = W; rc.height = H; rc.depth = 1; rc.array_size = 1; rc.nr_samples = 0;
    if (ioctl(fd, IOCTL_RESOURCE_CREATE, &rc) < 0) { fprintf(stderr, "DRMCLEAR: RESOURCE_CREATE failed\n"); return 3; }
    fprintf(stderr, "DRMCLEAR: RESOURCE_CREATE bo=%u res=%u size=%u stride=%u\n",
            rc.bo_handle, rc.res_handle, rc.size, rc.stride);
    uint32_t bo = rc.bo_handle, res = rc.res_handle;

    // EXECBUFFER: create a surface over the RT, bind it, and CLEAR to a colour.
    uint32_t dw[64]; uint32_t n = 0;
    dw[n++] = cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    dw[n++] = H_SURF; dw[n++] = res; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
    dw[n++] = 0; dw[n++] = 0;
    dw[n++] = cmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    dw[n++] = 1; dw[n++] = 0; dw[n++] = H_SURF;
    dw[n++] = cmd0(VIRGL_CCMD_CLEAR, 0, 8);
    dw[n++] = PIPE_CLEAR_COLOR0;
    dw[n++] = f32bits(0.20f); dw[n++] = f32bits(0.40f); dw[n++] = f32bits(0.80f); dw[n++] = f32bits(1.0f);
    dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;

    struct drm_virtgpu_execbuffer eb;
    memset(&eb, 0, sizeof(eb));
    eb.size = n * 4;
    eb.command = (uint64_t)(uintptr_t)dw;
    eb.bo_handles = (uint64_t)(uintptr_t)&bo;
    eb.num_bo_handles = 1;
    eb.fence_fd = -1;
    if (ioctl(fd, IOCTL_EXECBUFFER, &eb) < 0) { fprintf(stderr, "DRMCLEAR: EXECBUFFER failed\n"); return 4; }
    fprintf(stderr, "DRMCLEAR: EXECBUFFER ok (%u dwords)\n", n);

    // TRANSFER_FROM_HOST the cleared RT.
    struct drm_virtgpu_3d_transfer tr;
    memset(&tr, 0, sizeof(tr));
    tr.bo_handle = bo;
    tr.box.w = W; tr.box.h = H; tr.box.d = 1;
    if (ioctl(fd, IOCTL_TRANSFER_FROM_HOST, &tr) < 0) { fprintf(stderr, "DRMCLEAR: TRANSFER_FROM_HOST failed\n"); return 5; }
    fprintf(stderr, "DRMCLEAR: TRANSFER_FROM_HOST ok\n");

    struct drm_virtgpu_3d_wait w = { bo, 0 };
    if (ioctl(fd, IOCTL_WAIT, &w) < 0) { fprintf(stderr, "DRMCLEAR: WAIT failed\n"); return 6; }
    fprintf(stderr, "DRMCLEAR: WAIT ok\n");

    struct drm_virtgpu_resource_info ri; memset(&ri, 0, sizeof(ri)); ri.bo_handle = bo;
    if (ioctl(fd, IOCTL_RESOURCE_INFO, &ri) < 0) { fprintf(stderr, "DRMCLEAR: RESOURCE_INFO failed\n"); return 7; }
    fprintf(stderr, "DRMCLEAR: RESOURCE_INFO res=%u size=%u\n", ri.res_handle, ri.size);

    struct drm_gem_close gc = { bo, 0 };
    if (ioctl(fd, IOCTL_GEM_CLOSE, &gc) < 0) { fprintf(stderr, "DRMCLEAR: GEM_CLOSE failed\n"); return 8; }
    fprintf(stderr, "DRMCLEAR: GEM_CLOSE ok\n");

    fprintf(stderr, "DRMCLEAR_OK\n");
    close(fd);
    return 0;
}
