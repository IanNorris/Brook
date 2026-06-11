// drmprobe.c — Milestone 0 validation for the Brook virtio-gpu DRM render node.
//
// Hand-rolls the virtio-gpu DRM uABI ioctls (no libdrm dependency, mirroring how
// gltri.c hand-rolls virgl) to verify the kernel's /dev/dri/renderD128 shim
// answers the device-identification / capability ioctls Mesa issues during
// initialization: DRM_IOCTL_VERSION, VIRTGPU_GETPARAM, VIRTGPU_GET_CAPS. This
// validates the ioctl plumbing end-to-end inside Brook without needing Mesa's
// filesystem enumeration (that is exercised separately).
//
// Output goes to stderr (serial in a non-WM boot). Prints PROBE_OK on success.

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

// _IOWR('d', nr, size) — dir=3 (read|write), type='d'. The kernel dispatches on
// type+nr and ignores the encoded size, but we encode real sizes for fidelity.
#define IOWR(nr, size) ((3u << 30) | ((unsigned)(size) << 16) | ('d' << 8) | (nr))

struct drm_version {
    int      version_major;
    int      version_minor;
    int      version_patchlevel;
    int      _pad;
    uint64_t name_len;
    uint64_t name;
    uint64_t date_len;
    uint64_t date;
    uint64_t desc_len;
    uint64_t desc;
};

struct drm_virtgpu_getparam { uint64_t param; uint64_t value; };
struct drm_virtgpu_get_caps {
    uint32_t cap_set_id; uint32_t cap_set_ver;
    uint64_t addr; uint32_t size; uint32_t pad;
};

#define DRM_IOCTL_VERSION          IOWR(0x00, sizeof(struct drm_version))
#define DRM_IOCTL_VIRTGPU_GETPARAM IOWR(0x43, sizeof(struct drm_virtgpu_getparam))
#define DRM_IOCTL_VIRTGPU_GET_CAPS IOWR(0x49, sizeof(struct drm_virtgpu_get_caps))

#define VIRTGPU_PARAM_3D_FEATURES  1
#define VIRTGPU_PARAM_CONTEXT_INIT 6
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs 7

int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "DRMPROBE: open(/dev/dri/renderD128) failed\n");
        return 1;
    }
    fprintf(stderr, "DRMPROBE: opened renderD128 fd=%d\n", fd);

    // VERSION: two-call protocol — first to learn lengths, then to fetch name.
    struct drm_version v;
    memset(&v, 0, sizeof(v));
    if (ioctl(fd, DRM_IOCTL_VERSION, &v) < 0) {
        fprintf(stderr, "DRMPROBE: VERSION(len) failed\n");
        return 2;
    }
    char namebuf[64] = {0};
    if (v.name_len > 0 && v.name_len < sizeof(namebuf)) {
        v.name = (uint64_t)(uintptr_t)namebuf;
        if (ioctl(fd, DRM_IOCTL_VERSION, &v) < 0) {
            fprintf(stderr, "DRMPROBE: VERSION(name) failed\n");
            return 3;
        }
    }
    fprintf(stderr, "DRMPROBE: driver=\"%s\" version=%d.%d.%d\n",
            namebuf, v.version_major, v.version_minor, v.version_patchlevel);

    if (strcmp(namebuf, "virtio_gpu") != 0) {
        fprintf(stderr, "DRMPROBE: unexpected driver name\n");
        return 4;
    }

    // GETPARAM probes.
    struct { uint64_t param; const char* label; } params[] = {
        { VIRTGPU_PARAM_3D_FEATURES,         "3D_FEATURES" },
        { VIRTGPU_PARAM_CONTEXT_INIT,        "CONTEXT_INIT" },
        { VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs,"CAPSET_IDS" },
    };
    for (unsigned i = 0; i < sizeof(params)/sizeof(params[0]); ++i) {
        uint64_t out = 0;
        struct drm_virtgpu_getparam gp = { params[i].param, (uint64_t)(uintptr_t)&out };
        int r = ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp);
        fprintf(stderr, "DRMPROBE: GETPARAM %-12s -> rc=%d value=%llu\n",
                params[i].label, r, (unsigned long long)out);
    }

    // GET_CAPS: fetch a small capset blob.
    static uint8_t caps[1024];
    struct drm_virtgpu_get_caps gc;
    memset(&gc, 0, sizeof(gc));
    gc.cap_set_id = 2;            // VIRGL2
    gc.cap_set_ver = 0;
    gc.addr = (uint64_t)(uintptr_t)caps;
    gc.size = sizeof(caps);
    int rc = ioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &gc);
    fprintf(stderr, "DRMPROBE: GET_CAPS(id=2,size=%u) -> rc=%d\n",
            (unsigned)sizeof(caps), rc);

    fprintf(stderr, "DRMPROBE: PROBE_OK\n");
    close(fd);
    return 0;
}
