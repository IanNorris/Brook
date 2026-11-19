#ifndef BROOK_CHROME_SHADOW_H
#define BROOK_CHROME_SHADOW_H
/* The window shadow nine-slice (compositor refactor stage 1.3), shared by
 * the kernel (virgl compose, chrome_material.h) and the user-mode renderer
 * (VKRENDER), so both draw the same shadow from the same 64 px source.
 * C and C++. Straight alpha: alpha << 24 | BROOK_CHROME_SHADE.
 *
 * Slicing: source columns/rows 0..23, 24..39, 40..63 (corner, edge,
 * corner). The destination is the surface's outer rectangle grown by
 * shadowSpread on every side; corners are shadowCorner output pixels square
 * and the edges stretch. The centre is never drawn. */
#include <stdint.h>
#include "brook_theme.h"

enum { BROOK_CHROME_SHADOW_SIZE = 64, BROOK_CHROME_SHADOW_CORNER = 24, BROOK_CHROME_SHADOW_EDGE = 16 };

/* Pixel-centre circle coverage: the first column of row inside a rounded
 * corner of radius (0 when row is past the corner). */
static inline unsigned brook_chrome_corner_inset(unsigned radius, unsigned row) {
    unsigned x;
    if (!radius || row >= radius) return 0;
    {
        const int dy = (int)(2 * (radius - row) - 1);
        for (x = 0; x < radius; ++x) {
            const int dx = (int)(2 * (radius - x) - 1);
            if (dx * dx + dy * dy <= (int)(4 * radius * radius)) return x;
        }
    }
    return radius;
}

/* The centre is a rounded, transparent hole: shading never darkens the
 * window through its own translucent title bar. */
static inline uint32_t brook_chrome_shadow_pixel(unsigned x, unsigned y) {
    static const uint8_t opacity[] = {136, 113, 92, 73, 56, 42, 30, 21, 14, 9, 5, 3, 2, 1, 0, 0, 0};
    unsigned dx, dy, distance, alpha;
    if (x >= 64 || y >= 64) return 0;
    if (x >= 16 && x < 48 && y >= 16 && y < 48) {
        const unsigned row = y < 32 ? y - 16 : 47 - y;
        const unsigned inset = brook_chrome_corner_inset(y < 32 ? 7 : 2, row);
        if (x >= 16 + inset && x < 48 - inset) return 0;
    }
    dx = x < 16 ? 16 - x : x >= 48 ? x - 47 : 0;
    dy = y < 16 ? 16 - y : y >= 48 ? y - 47 : 0;
    distance = (dx > dy ? dx : dy) + (dx < dy ? dx : dy) / 2;
    alpha = distance < 17 ? opacity[distance] : 0;
    if (y < 16) alpha = alpha * 3 / 4;
    return alpha ? (alpha << 24) | BROOK_CHROME_SHADE : 0;
}

/* One of the eight nine-slice pieces (index 0..7, row-major, centre
 * skipped) of a shadow around (x, y, width, height) grown by spread, with
 * corner-sized corners. Returns 0 when the shadowed rectangle is smaller
 * than two corners (no shadow), else fills the source and destination. */
struct BrookChromeShadowPiece { uint32_t sx, sy, sw, sh; int32_t dx, dy; uint32_t dw, dh; };
static inline int brook_chrome_shadow_piece(int32_t x, int32_t y, uint32_t width, uint32_t height,
                                            uint32_t spread, uint32_t corner, unsigned index,
                                            struct BrookChromeShadowPiece *out) {
    static const uint32_t source[3] = {0, 24, 40}, span[3] = {24, 16, 24};
    const int64_t left = (int64_t)x - spread, top = (int64_t)y - spread;
    const int64_t w = (int64_t)width + 2 * (int64_t)spread, h = (int64_t)height + 2 * (int64_t)spread;
    unsigned col, row;
    int64_t dx[3], dy[3], dw[3], dh[3];
    if (index >= 8 || !corner || w < 2 * (int64_t)corner || h < 2 * (int64_t)corner) return 0;
    if (index >= 4) ++index; /* skip the centre */
    col = index % 3; row = index / 3;
    dx[0] = left; dx[1] = left + corner; dx[2] = left + w - corner;
    dy[0] = top; dy[1] = top + corner; dy[2] = top + h - corner;
    dw[0] = corner; dw[1] = w - 2 * (int64_t)corner; dw[2] = corner;
    dh[0] = corner; dh[1] = h - 2 * (int64_t)corner; dh[2] = corner;
    if (dw[col] <= 0 || dh[row] <= 0) return 0;
    out->sx = source[col]; out->sy = source[row]; out->sw = span[col]; out->sh = span[row];
    out->dx = (int32_t)dx[col]; out->dy = (int32_t)dy[row]; out->dw = (uint32_t)dw[col]; out->dh = (uint32_t)dh[row];
    return 1;
}
#endif
