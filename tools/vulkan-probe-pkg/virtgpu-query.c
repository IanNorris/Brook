// Small raw DRM control shared by Linux reference and Brook negative probes.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <virtgpu_drm.h>

int main(int argc, char** argv)
{
    if (argc > 2) return 2;
    int fd = open(argc == 2 ? argv[1] : "/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open render node"); return 1; }
    uint64_t values[9] = {0};
    int errors[9] = {0};
    for (uint64_t i = 1; i <= 8; ++i) {
        struct drm_virtgpu_getparam param = { .param = i, .value = (uintptr_t)&values[i] };
        if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &param)) errors[i] = errno;
    }
    int ready = 1;
    const unsigned required[] = {1, 2, 3, 4, 6, 7};
    for (unsigned i = 0; i < sizeof(required)/sizeof(required[0]); ++i)
        if (errors[required[i]] || !values[required[i]]) ready = 0;
    if (!(values[7] & (1ull << 4))) ready = 0;
    int contextError = 0;
    int contextAttempted = ready;
    if (ready) {
        struct drm_virtgpu_context_set_param parameters[] = {
            { .param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID, .value = 4 },
            { .param = VIRTGPU_CONTEXT_PARAM_NUM_RINGS, .value = 64 },
            { .param = VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK, .value = 0 },
        };
        struct drm_virtgpu_context_init context = { .num_params = 3,
            .ctx_set_params = (uintptr_t)parameters };
        if (ioctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &context)) { contextError = errno; ready = 0; }
    }
    printf("{\"passed\":%s,\"params\":[", ready ? "true" : "false");
    for (unsigned i = 1; i <= 8; ++i)
        printf("%s{\"id\":%u,\"value\":%llu,\"errno\":%d}", i == 1 ? "" : ",",
               i, (unsigned long long)values[i], errors[i]);
    printf("],\"context_attempted\":%s,\"context_errno\":%d}\n", contextAttempted ? "true" : "false", contextError);
    close(fd);
    return ready ? 0 : 1;
}
