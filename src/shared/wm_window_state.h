#ifndef BROOK_WM_WINDOW_STATE_H
#define BROOK_WM_WINDOW_STATE_H
#include <stdint.h>

/* WM_WINDOW_STATE (0xB08): (window id, expected incarnation, output, size, 0, 0).
 * Only the calling provider's windows are visible. Expected incarnation zero
 * discovers the current identity; nonzero rejects a reused slot with ESTALE.
 * Coordinates are desktop pixels at the CLIENT origin, including chrome offset.
 * This snapshot neither maps pixels nor changes focus, geometry or visibility.
 */
enum {
    BROOK_SYS_WM_WINDOW_STATE = 0xB08,
    BROOK_SYS_WM_PLACE_WINDOW = 0xB09,
    BROOK_WINDOW_STATE_VERSION = 1,
    BROOK_WINDOW_STATE_VISIBLE = 1u << 0,
    BROOK_WINDOW_STATE_FOCUSED = 1u << 1,
    BROOK_WINDOW_STATE_MINIMIZED = 1u << 2,
    BROOK_WINDOW_STATE_MAXIMIZED = 1u << 3,
    BROOK_WINDOW_STATE_CLIENT_CHROME = 1u << 4,
    BROOK_WINDOW_STATE_FOCUSABLE = 1u << 5
};
typedef struct BrookWindowState {
    uint32_t version;
    uint32_t window;
    uint64_t incarnation;
    int32_t client_x, client_y;
    uint32_t width, height;
    uint32_t flags;
    uint32_t reserved;
} BrookWindowState;
/* WM_PLACE_WINDOW: (window, nonzero incarnation, signed client x, signed client
 * y, 0, 0). Moves only the caller's matching window; preserves size and focus. */
#ifdef __cplusplus
static_assert(sizeof(BrookWindowState) == 40, "window state ABI");
#else
_Static_assert(sizeof(BrookWindowState) == 40, "window state ABI");
#endif
#endif
