#ifndef BROOK_SURFACE_BLIT_H
#define BROOK_SURFACE_BLIT_H
/* CPU scene composition kernels for waylandd.
 *
 * Bit-exact replacements for the original per-pixel loop in surface_render:
 *     out = pixel_over(src[brook_geometry_pixel(g,x,y)], out)
 * That loop paid two 64-bit divisions per destination pixel for the nearest
 * sample and a division by 255 per channel for the blend. Here:
 *   - brook_geometry_pixel is separable (one source axis follows x, the other
 *     follows y, for every transform), so the sample offsets are computed once
 *     per column and once per row of each draw;
 *   - (t+127)/255 is replaced by an exact shift form (proved exhaustively in
 *     surface-blit-test.c);
 *   - opaque sources (XRGB snapshots carry alpha 0xff) and the first layer
 *     over the cleared composite reduce to copies, and an unscaled identity
 *     draw copies whole rows.
 * Pure functions with no waylandd state, so the host test can compare them
 * against the original implementation. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "surface-geometry.h"

/* floor((t+127)/255) for 0 <= t <= 255*255. */
static inline uint32_t brook_div255_round(uint32_t t) {
    t += 128;
    return (t + (t >> 8)) >> 8;
}
static inline uint32_t brook_pixel_over_fast(uint32_t src, uint32_t dst) {
    const uint32_t a = src >> 24;
    if (a == 255) return src;
    if (!a) return dst;
    const uint32_t inv = 255 - a;
    /* Two channels per 32-bit word in 16-bit lanes. Per lane t <= 65153 and
     * t + (t >> 8) <= 65407, so no lane carries into the next; the result is
     * brook_div255_round of each channel (exhaustively checked in the test). */
    uint32_t rb = (dst & 0x00ff00ffu) * inv + 0x00800080u;
    uint32_t ag = ((dst >> 8) & 0x00ff00ffu) * inv + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    ag = ((ag + ((ag >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    /* Saturating add of the source channels (lanes <= 510). */
    rb += src & 0x00ff00ffu;
    ag += (src >> 8) & 0x00ff00ffu;
    uint32_t m = rb & 0x01000100u; rb = (rb | (m - (m >> 8))) & 0x00ff00ffu;
    m = ag & 0x01000100u; ag = (ag | (m - (m >> 8))) & 0x00ff00ffu;
    return rb | (ag << 8);
}

/* FORCE_ALPHA: an ARGB source whose opaque region covers the surface
 * (wl_surface.set_opaque_region). Like other compositors, ignore its alpha:
 * copy the colour channels and write alpha 255, so a client that leaves
 * alpha < 255 in its framebuffer (a GL game's blend effects) never makes
 * the window see-through. */
enum { BROOK_BLIT_OPAQUE = 1, BROOK_BLIT_DST_CLEAR = 2, BROOK_BLIT_FORCE_ALPHA = 4 };

/* Composite one draw into `dst` (dst_w x dst_h, stride dst_w). `physical` is
 * the view geometry with w/h already multiplied by the output scale; (dx,dy)
 * the scaled destination origin. Flags: OPAQUE when every source pixel has
 * alpha 255, DST_CLEAR when the covered destination is still all zero.
 * Returns 0 only on allocation failure (nothing written in that case). */
static int brook_blit_draw(uint32_t *dst, uint32_t dst_w, uint32_t dst_h,
                           const struct brook_geometry *physical, const uint32_t *pixels,
                           uint32_t bw, uint32_t bh, int64_t dx, int64_t dy, unsigned flags) {
    int64_t x0 = dx < 0 ? -dx : 0, y0 = dy < 0 ? -dy : 0, x1 = physical->w, y1 = physical->h;
    if (dx + x1 > dst_w) x1 = (int64_t)dst_w - dx;
    if (dy + y1 > dst_h) y1 = (int64_t)dst_h - dy;
    if (x1 <= x0 || y1 <= y0) return 1;
    const size_t cols = (size_t)(x1 - x0), rows = (size_t)(y1 - y0);
    size_t stackCols[1024], stackRows[1024];
    size_t *colOff = cols <= 1024 ? stackCols : malloc(cols * sizeof(size_t));
    size_t *rowOff = rows <= 1024 ? stackRows : malloc(rows * sizeof(size_t));
    if (!colOff || !rowOff) {
        if (colOff && colOff != stackCols) free(colOff);
        if (rowOff && rowOff != stackRows) free(rowOff);
        return 0;
    }
    const int swapped = physical->transform & 1;
    int contiguous = !physical->transform;
    for (size_t i = 0; i < cols; ++i) {
        uint32_t px, py;
        brook_geometry_pixel(physical, bw, bh, (uint32_t)(x0 + (int64_t)i), 0, &px, &py);
        colOff[i] = swapped ? (size_t)py * bw : px;
        if (i && colOff[i] != colOff[i - 1] + 1) contiguous = 0;
    }
    for (size_t j = 0; j < rows; ++j) {
        uint32_t px, py;
        brook_geometry_pixel(physical, bw, bh, 0, (uint32_t)(y0 + (int64_t)j), &px, &py);
        rowOff[j] = swapped ? px : (size_t)py * bw;
    }
    for (size_t j = 0; j < rows; ++j) {
        uint32_t *out = dst + (size_t)(dy + y0 + (int64_t)j) * dst_w + (size_t)(dx + x0);
        const uint32_t *src = pixels + rowOff[j];
        if (contiguous) {
            const uint32_t *s = src + colOff[0];
            if (flags & BROOK_BLIT_FORCE_ALPHA)
                for (size_t i = 0; i < cols; ++i) out[i] = s[i] | 0xff000000u;
            else if (flags & BROOK_BLIT_OPAQUE) memcpy(out, s, cols * 4);
            else if (flags & BROOK_BLIT_DST_CLEAR)
                for (size_t i = 0; i < cols; ++i) out[i] = (s[i] >> 24) ? s[i] : 0;
            else for (size_t i = 0; i < cols; ++i) out[i] = brook_pixel_over_fast(s[i], out[i]);
        } else if (flags & BROOK_BLIT_FORCE_ALPHA) {
            for (size_t i = 0; i < cols; ++i) out[i] = src[colOff[i]] | 0xff000000u;
        } else if (flags & BROOK_BLIT_OPAQUE) {
            for (size_t i = 0; i < cols; ++i) out[i] = src[colOff[i]];
        } else if (flags & BROOK_BLIT_DST_CLEAR) {
            for (size_t i = 0; i < cols; ++i) { const uint32_t s = src[colOff[i]]; out[i] = (s >> 24) ? s : 0; }
        } else {
            for (size_t i = 0; i < cols; ++i) out[i] = brook_pixel_over_fast(src[colOff[i]], out[i]);
        }
    }
    if (colOff != stackCols) free(colOff);
    if (rowOff != stackRows) free(rowOff);
    return 1;
}

/* Straight-alpha conversion of a premultiplied composite row (the original
 * pixel_straight), with opaque and transparent pixels on the fast branch. */
/* floor(n/a) == (n * ceil(2^32/a)) >> 32 exactly for n < 2^16, a <= 255:
 * the reciprocal's error is below a, so n*error < 2^24 never reaches 2^32. */
static const uint64_t *brook_straight_recip(void) {
    static uint64_t table[256];
    if (!table[1]) for (uint32_t a = 1; a < 256; ++a) table[a] = ((1ull << 32) + a - 1) / a;
    return table;
}
static inline uint32_t brook_pixel_straight_fast(uint32_t px) {
    const uint32_t a = px >> 24;
    if (a == 255) return px;
    if (!a) return 0;
    const uint64_t r = brook_straight_recip()[a];
    uint32_t out = px & 0xff000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        uint32_t v = (uint32_t)(((uint64_t)(((px >> shift) & 255) * 255 + a / 2) * r) >> 32);
        out |= (v > 255 ? 255 : v) << shift;
    }
    return out;
}
static void brook_straight_rows(uint32_t *dst, size_t dst_stride, const uint32_t *src, size_t src_stride,
                                uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; ++y) {
        uint32_t *out = dst + (size_t)y * dst_stride;
        const uint32_t *in = src + (size_t)y * src_stride;
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t p = in[x];
            out[x] = (p >> 24) == 255 ? p : brook_pixel_straight_fast(p);
        }
    }
}

/* A single aligned, untransformed source crop covers the entire destination.
 * It already supplies the final premultiplied pixels, so there is no need for
 * a cleared composite or a second pass through that composite. */
static inline const uint32_t *brook_native_crop(const struct brook_geometry *g,
        const uint32_t *pixels, uint32_t bw, uint32_t bh, uint32_t scale,
        uint32_t w, uint32_t h) {
    if (!pixels || !w || !h || g->transform || ((g->sx | g->sy) & 255) ||
        (uint64_t)g->w * scale != w || (uint64_t)g->h * scale != h ||
        g->sw != (uint64_t)w * 256 || g->sh != (uint64_t)h * 256) return NULL;
    const uint32_t x = g->sx / 256, y = g->sy / 256;
    if (x >= bw || y >= bh || w > bw - x || h > bh - y) return NULL;
    return pixels + (size_t)y * bw + x;
}

/* opaque: 0 converts premultiplied alpha, 1 copies (every source alpha is
 * already 255), BROOK_BLIT_FORCE_ALPHA copies with alpha forced to 255. */
static inline void brook_publish_native_rows(uint32_t *dst, size_t dst_stride,
        const uint32_t *src, size_t src_stride, uint32_t w, uint32_t h, int opaque) {
    if (!opaque) {
        brook_straight_rows(dst, dst_stride, src, src_stride, w, h);
        return;
    }
    if (opaque == BROOK_BLIT_FORCE_ALPHA) {
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t *out = dst + (size_t)y * dst_stride;
            const uint32_t *in = src + (size_t)y * src_stride;
            for (uint32_t x = 0; x < w; ++x) out[x] = in[x] | 0xff000000u;
        }
        return;
    }
    if (src_stride == w && dst_stride == w) {
        memcpy(dst, src, (size_t)w * h * 4);
        return;
    }
    for (uint32_t y = 0; y < h; ++y)
        memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, (size_t)w * 4);
}
#endif
