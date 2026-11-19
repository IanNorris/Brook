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
#include "desktop_layers_abi.h"

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

enum { BROOK_CHROME_SHADOW_VARIANT_COUNT = 7,
       BROOK_CHROME_SHADOW_ATLAS_HEIGHT = BROOK_CHROME_SHADOW_VARIANT_COUNT * BROOK_CHROME_SHADOW_SIZE };

static inline unsigned brook_chrome_shadow_tile_y(unsigned kind, unsigned flags) {
    if (kind < BROOK_DESKTOP_SHADOW_WINDOW_ACTIVE || kind > BROOK_DESKTOP_SHADOW_PANEL) return 0;
    return (kind - 1 + ((flags & BROOK_DESKTOP_SURFACE_LIGHT_SHADOW) ? 3 : 0)) * BROOK_CHROME_SHADOW_SIZE;
}

/* Integer square root for the baked corner distance. */
static inline unsigned brook_chrome_shadow_sqrt(unsigned value) {
    unsigned root = 0, bit = 1u << 16;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else root >>= 1;
        bit >>= 2;
    }
    return root;
}

/* A separately baked ambient and contact pair per surface class. Measure
 * distance from the rounded contour, including inside the corner cut-outs.
 * Blend the side profile into the top/bottom profile along the arc so the
 * shadow reads as downcast without a seam at the nine-slice joins. */
static inline uint32_t brook_chrome_shadow_variant_pixel(unsigned kind, int light, unsigned x, unsigned y) {
    if (kind == BROOK_DESKTOP_SHADOW_WINDOW) return brook_chrome_shadow_pixel(x, y);
    if (kind < BROOK_DESKTOP_SHADOW_WINDOW_ACTIVE || kind > BROOK_DESKTOP_SHADOW_PANEL ||
        !x || !y || x >= 63 || y >= 63) return 0;
    const unsigned panel = kind == BROOK_DESKTOP_SHADOW_PANEL;
    const unsigned radius = panel ? 12 : y < 32 ? 8 : 2;
    const unsigned px = x < 32 ? x : 63 - x, py = y < 32 ? y : 63 - y;
    const unsigned corner = 16 + radius;
    const unsigned dx = px < corner ? 4 * (corner - px) - 2 : 0;
    const unsigned dy = py < corner ? 4 * (corner - py) - 2 : 0;
    const unsigned r = radius * 4;
    const unsigned squared = dx * dx + dy * dy;
    if (squared <= r * r) return 0;
    const unsigned distance = brook_chrome_shadow_sqrt(squared) - r;

    /* Reach in source pixels and peak coverage at the material edge. */
    static const uint8_t reach[3][3] = {{8, 14, 16}, {5, 9, 13}, {7, 12, 16}};
    static const uint8_t ambient[3][3] = {{13, 47, 113}, {9, 29, 69}, {12, 39, 94}};
    static const uint8_t contact[3][3] = {{4, 18, 52}, {3, 12, 40}, {4, 15, 46}};
    static const uint8_t contactReach[] = {5, 3, 3};
    const unsigned recipe = kind - BROOK_DESKTOP_SHADOW_WINDOW_ACTIVE;
    const unsigned vertical = y < 32 ? 0 : 2;
    const unsigned normal = dx + dy;
    const unsigned fade = 4 * (reach[recipe][1] * dx + reach[recipe][vertical] * dy) / normal;
    const unsigned peak = (ambient[recipe][1] * dx + ambient[recipe][vertical] * dy) / normal;
    const unsigned contactPeak = (contact[recipe][1] * dx + contact[recipe][vertical] * dy) / normal;
    const unsigned near = 4 * contactReach[recipe];
    const unsigned remain = distance < fade ? fade - distance : 0;
    const unsigned ambientAlpha = peak * remain * remain / (fade * fade);
    const unsigned contactAlpha = distance < near ? contactPeak * (near - distance) / near : 0;
    const unsigned alpha = contactAlpha + (255 - contactAlpha) * ambientAlpha / 255;
    return alpha ? (alpha << 24) | (light ? 0x142238u : 0x030a16u) : 0;
}

static inline uint32_t brook_chrome_shadow_atlas_pixel(unsigned x, unsigned y) {
    const unsigned tile = y / BROOK_CHROME_SHADOW_SIZE;
    if (tile >= BROOK_CHROME_SHADOW_VARIANT_COUNT) return 0;
    return brook_chrome_shadow_variant_pixel(tile > 3 ? tile - 2 : tile + 1,
                                            tile > 3, x, y % BROOK_CHROME_SHADOW_SIZE);
}

/* One of the eight nine-slice pieces (index 0..7, row-major, centre
 * skipped) of a shadow around (x, y, width, height) grown by spread, with
 * corner-sized corners. Returns 0 when the shadowed rectangle is smaller
 * than two corners (no shadow), else fills the source and destination. */
struct BrookChromeShadowPiece { uint32_t sx, sy, sw, sh; int32_t dx, dy; uint32_t dw, dh; };
static inline int brook_chrome_shadow_piece_with_slice(int32_t x, int32_t y, uint32_t width, uint32_t height,
                                            uint32_t spread, uint32_t corner, unsigned index, uint32_t slice,
                                            struct BrookChromeShadowPiece *out) {
    const uint32_t source[3] = {0, slice, 64 - slice}, span[3] = {slice, 64 - 2 * slice, slice};
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
static inline int brook_chrome_shadow_piece(int32_t x, int32_t y, uint32_t width, uint32_t height,
                                            uint32_t spread, uint32_t corner, unsigned index,
                                            struct BrookChromeShadowPiece *out) {
    return brook_chrome_shadow_piece_with_slice(x, y, width, height, spread, corner, index, 24, out);
}
static inline int brook_chrome_shadow_variant_piece(int32_t x, int32_t y, uint32_t width, uint32_t height,
                                                    uint32_t spread, uint32_t corner, unsigned index,
                                                    unsigned kind, unsigned flags, struct BrookChromeShadowPiece *out) {
    if (!brook_chrome_shadow_piece_with_slice(x, y, width, height, spread, corner, index,
                                            kind == BROOK_DESKTOP_SHADOW_WINDOW ? 24 : 28, out)) return 0;
    out->sy += brook_chrome_shadow_tile_y(kind, flags);
    return 1;
}
#endif
