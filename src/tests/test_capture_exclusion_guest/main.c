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

static uint64_t pixel_offset(const struct BrookWindowState *state,
                             const struct BrookDebugFrameInfo *info)
{
    return (((uint64_t)state->client_y + state->height / 2) * info->stride +
            (uint64_t)state->client_x + state->width / 2) * sizeof(uint32_t);
}

static int read_pixel(long token, const struct BrookWindowState *state,
                      const struct BrookDebugFrameInfo *info, uint32_t *pixel)
{
    if (state->client_x < 0 || state->client_y < 0 ||
        (uint64_t)state->client_x + state->width / 2 >= info->width ||
        (uint64_t)state->client_y + state->height / 2 >= info->height)
        return 0;
    return syscall(BROOK_SYS_DEBUG_FRAME, 2, token, pixel, sizeof(*pixel),
                   pixel_offset(state, info), 0) == sizeof(*pixel);
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
    // Leave room for scaled chrome and keep both clients clear of the pointer.
    CHECK(syscall(BROOK_SYS_WM_PLACE_WINDOW, secret.id, secret_state.incarnation, 200, 200, 0, 0) == 0);
    CHECK(syscall(BROOK_SYS_WM_PLACE_WINDOW, control.id, control_state.incarnation, 400, 200, 0, 0) == 0);
    CHECK(syscall(BROOK_SYS_WM_WINDOW_STATE, secret.id, secret_state.incarnation, &secret_state, sizeof(secret_state), 0, 0) == 0);
    CHECK(syscall(BROOK_SYS_WM_WINDOW_STATE, control.id, control_state.incarnation, &control_state, sizeof(control_state), 0, 0) == 0);
    for (unsigned y = 0; y < 16; ++y) {
        for (unsigned x = 0; x < 16; ++x) {
            secret.pixels[y * secret.stride + x] = 0xffff00ff;
            control.pixels[y * control.stride + x] = 0xff00ff00;
        }
    }
    CHECK(syscall(508, secret.id, 0, 0, 0, 0, 0) == 0);
    CHECK(syscall(508, control.id, 0, 0, 0, 0, 0) == 0);
    long before = 0;
    struct BrookDebugFrameInfo info = {0};
    uint32_t pixel = 0, visible_secret = 0, visible_control = 0;
    // A hardware capture can initially see an older scanout. Require both
    // sentinels in the same frame before exercising the exclusion transition.
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        before = syscall(BROOK_SYS_DEBUG_FRAME, 0, 0, 0, 0, 0, 0);
        CHECK(before > 0 && wait_frame(before, &info));
        CHECK(read_pixel(before, &secret_state, &info, &visible_secret));
        CHECK(read_pixel(before, &control_state, &info, &visible_control));
        if ((visible_secret & 0x00ffffff) == 0x00ff00ff &&
            (visible_control & 0x00ffffff) == 0x0000ff00)
            break;
        CHECK(syscall(BROOK_SYS_DEBUG_FRAME, 3, before, 0, 0, 0, 0) == 0);
        before = 0;
        usleep(25000);
    }
    if (!before)
        fprintf(stderr, "Capture never showed both windows: secret=%08x control=%08x\n",
                visible_secret, visible_control);
    CHECK(before > 0);
    uint64_t secret_offset = pixel_offset(&secret_state, &info);
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
    CHECK(read_pixel(token, &secret_state, &info, &pixel));
    CHECK(pixel == 0xff000000);
    CHECK(read_pixel(token, &control_state, &info, &pixel));
    CHECK((pixel & 0x00ffffff) == 0x0000ff00);
    CHECK(syscall(BROOK_SYS_DEBUG_FRAME, 3, token, 0, 0, 0, 0) == 0);
    // The parent can latch after a popup and its own child already exist.
    // Give each popup an origin outside the parent's rectangle.
    struct window browser = {0}, popup = {0}, child = {0}, late = {0};
    struct BrookWindowState popup_state = {0}, child_state = {0}, late_state = {0};
    CHECK(syscall(506, 16, 16, "Firefox", &browser, 1, 0) == 0);
    CHECK(syscall(506, 16, 16, "suggestions", &popup, 3, 0) == 0);
    CHECK(syscall(506, 16, 16, "submenu", &child, 3, 0) == 0);
    CHECK(syscall(514, popup.id, browser.id, 72, 0, 0, 0) == 0);
    CHECK(syscall(514, child.id, popup.id, 36, 0, 0, 0) == 0);
    CHECK(syscall(0xB01, browser.id, "firefox", 0, 0, 0, 0) == 0);
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x) {
            popup.pixels[y * popup.stride + x] = 0xffaa00bb;
            child.pixels[y * child.stride + x] = 0xffbb00aa;
        }
    CHECK(syscall(508, popup.id, 0, 0, 0, 0, 0) == 0);
    CHECK(syscall(508, child.id, 0, 0, 0, 0, 0) == 0);
    CHECK(syscall(BROOK_SYS_WM_WINDOW_STATE, popup.id, 0, &popup_state, sizeof(popup_state), 0, 0) == 0);
    CHECK(syscall(BROOK_SYS_WM_WINDOW_STATE, child.id, 0, &child_state, sizeof(child_state), 0, 0) == 0);
    token = request(&popup, popup_state.incarnation, BROOK_DEBUG_FRAME_FLAG_WINDOW_BUFFER);
    CHECK(token > 0 && syscall(BROOK_SYS_DEBUG_FRAME, 3, token, 0, 0, 0, 0) == 0);
    static const char title[] =
        "A very long title with a site name that would push a private browsing suffix beyond the display limit"
        " — Mozilla Firefox Private Browsing";
    CHECK(syscall(509, browser.id, title, 0, 0, 0, 0) == 0);
    CHECK(request(&popup, popup_state.incarnation, BROOK_DEBUG_FRAME_FLAG_WINDOW_BUFFER) == -1 && errno == EACCES);
    CHECK(request(&child, child_state.incarnation, BROOK_DEBUG_FRAME_FLAG_WINDOW_CROP) == -1 && errno == EACCES);
    CHECK(syscall(506, 16, 16, "late popup", &late, 3, 0) == 0);
    CHECK(syscall(514, late.id, browser.id, 135, 0, 0, 0) == 0);
    CHECK(syscall(BROOK_SYS_WM_WINDOW_STATE, late.id, 0, &late_state, sizeof(late_state), 0, 0) == 0);
    CHECK(request(&late, late_state.incarnation, BROOK_DEBUG_FRAME_FLAG_WINDOW_BUFFER) == -1 && errno == EACCES);
    token = syscall(BROOK_SYS_DEBUG_FRAME, 0, 0, 0, 0, 0, 0);
    CHECK(token > 0 && wait_frame(token, &info));
    const struct BrookWindowState states[] = {popup_state, child_state};
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        uint64_t offset = ((states[i].client_y + 8) * (uint64_t)info.stride +
                           states[i].client_x + 8) * sizeof(pixel);
        CHECK(syscall(BROOK_SYS_DEBUG_FRAME, 2, token, &pixel, sizeof(pixel), offset, 0) == sizeof(pixel));
        CHECK(pixel == 0xff000000);
    }
    CHECK(syscall(BROOK_SYS_DEBUG_FRAME, 3, token, 0, 0, 0, 0) == 0);
    puts("CAPTURE_EXCLUSION_PASS window=refused crop=refused desktop=masked control=intact long_title=matched popup_tree=masked");
    return 0;
}
