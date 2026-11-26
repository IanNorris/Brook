#define _GNU_SOURCE
#include "../../shared/brook_debug_frame.h"
#include "../../shared/wm_input_abi.h"
#include "../../shared/wm_window_state.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "CAPTURE_EXCLUSION_FAIL line=%d errno=%d\n", __LINE__, errno); return 1; } } while (0)

struct window {
    uint32_t id, stride;
    uint32_t *pixels;
};

static long request(const struct window *window, uint64_t incarnation, uint32_t flags)
{
    struct BrookDebugFrameRequest args = {flags, window->id, incarnation};
    return syscall(BROOK_SYS_DEBUG_FRAME, BROOK_DEBUG_FRAME_REQUEST, 0, &args, sizeof(args), 0, 0);
}

static int wait_frame(long token, struct BrookDebugFrameInfo *info)
{
    for (unsigned i = 0; i < 400; ++i) {
        if (syscall(BROOK_SYS_DEBUG_FRAME, 1, token, info, sizeof(*info), 0, 0) != 0)
            return 0;
        if (info->state != 1) return info->state == 2;
        usleep(25000);
    }
    return 0;
}

int main(void)
{
    struct window secret = {0}, control = {0};
    CHECK(syscall(506, 16, 16, "protected", &secret, 1, 0) == 0);
    CHECK(syscall(506, 16, 16, "control", &control, 1, 0) == 0);
    CHECK(secret.id && control.id && secret.pixels && control.pixels);
    struct BrookWindowState secret_state = {0}, control_state = {0};
    CHECK(syscall(BROOK_SYS_WM_WINDOW_STATE, secret.id, 0, &secret_state, sizeof(secret_state), 0, 0) == 0);
    CHECK(syscall(BROOK_SYS_WM_WINDOW_STATE, control.id, 0, &control_state, sizeof(control_state), 0, 0) == 0);
    for (unsigned y = 0; y < 16; ++y) {
        for (unsigned x = 0; x < 16; ++x) {
            secret.pixels[y * secret.stride + x] = 0xffff0000;
            control.pixels[y * control.stride + x] = 0xff00ff00;
        }
    }
    CHECK(syscall(508, secret.id, 0, 0, 0, 0, 0) == 0);
    CHECK(syscall(508, control.id, 0, 0, 0, 0, 0) == 0);
    long before = syscall(BROOK_SYS_DEBUG_FRAME, 0, 0, 0, 0, 0, 0);
    CHECK(before > 0);
    struct BrookDebugFrameInfo info = {0};
    CHECK(wait_frame(before, &info));
    uint64_t secret_offset = ((secret_state.client_y + 8) * (uint64_t)info.stride + (secret_state.client_x + 8)) * sizeof(uint32_t);
    uint32_t pixel = 0;
    CHECK(syscall(BROOK_SYS_DEBUG_FRAME, 2, before, &pixel, sizeof(pixel), secret_offset, 0) == sizeof(pixel));
    CHECK((pixel & 0x00ffffff) == 0x00ff0000);
    CHECK(syscall(BROOK_SYS_WM_EXCLUDE_CAPTURE, secret.id, 0, 0, 0, 0, 0) == 0);
    CHECK(syscall(BROOK_SYS_DEBUG_FRAME, 2, before, &pixel, sizeof(pixel), secret_offset, 0) == -1 && errno == EACCES);
    CHECK(syscall(BROOK_SYS_DEBUG_FRAME, 3, before, 0, 0, 0, 0) == 0);
    CHECK(request(&secret, secret_state.incarnation, BROOK_DEBUG_FRAME_FLAG_WINDOW_BUFFER) == -1 && errno == EACCES);
    CHECK(request(&secret, secret_state.incarnation, BROOK_DEBUG_FRAME_FLAG_WINDOW_CROP) == -1 && errno == EACCES);
    long token = request(&control, control_state.incarnation, BROOK_DEBUG_FRAME_FLAG_WINDOW_BUFFER);
    CHECK(token > 0);
    CHECK(syscall(BROOK_SYS_DEBUG_FRAME, 2, token, &pixel, sizeof(pixel), 0, 0) == sizeof(pixel));
    CHECK(pixel == 0xff00ff00);
    CHECK(syscall(BROOK_SYS_DEBUG_FRAME, 3, token, 0, 0, 0, 0) == 0);
    token = syscall(BROOK_SYS_DEBUG_FRAME, 0, 0, 0, 0, 0, 0);
    CHECK(token > 0);
    CHECK(wait_frame(token, &info));
    uint64_t control_offset = ((control_state.client_y + 8) * (uint64_t)info.stride + (control_state.client_x + 8)) * sizeof(pixel);
    CHECK(syscall(BROOK_SYS_DEBUG_FRAME, 2, token, &pixel, sizeof(pixel), secret_offset, 0) == sizeof(pixel));
    CHECK(pixel == 0xff000000);
    CHECK(syscall(BROOK_SYS_DEBUG_FRAME, 2, token, &pixel, sizeof(pixel), control_offset, 0) == sizeof(pixel));
    CHECK((pixel & 0x00ffffff) == 0x0000ff00);
    CHECK(syscall(BROOK_SYS_DEBUG_FRAME, 3, token, 0, 0, 0, 0) == 0);
    puts("CAPTURE_EXCLUSION_PASS window=refused crop=refused desktop=masked control=intact");
    return 0;
}
