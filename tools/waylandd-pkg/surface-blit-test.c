/* Bit-exactness proof and benchmark for surface-blit.h against the original
 * waylandd CPU composition loop (copied verbatim below as the reference).
 *   ./surface-blit-test          correctness only (used by the nix checkPhase)
 *   ./surface-blit-test --bench  also time a 3200x1800 browser-like scene */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "surface-blit.h"

static uint32_t ref_pixel_over(uint32_t src, uint32_t dst) {
    unsigned a = src >> 24, inv = 255 - a;
    if (a == 255) return src;
    if (!a) return dst;
    uint32_t out = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        unsigned v = ((src >> shift) & 255) + (((dst >> shift) & 255) * inv + 127) / 255;
        out |= (v > 255 ? 255 : v) << shift;
    }
    return out;
}
static uint32_t ref_pixel_straight(uint32_t px) {
    unsigned a = px >> 24;
    if (!a || a == 255) return a ? px : 0;
    uint32_t out = px & 0xff000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned v = (((px >> shift) & 255) * 255 + a / 2) / a;
        out |= (v > 255 ? 255 : v) << shift;
    }
    return out;
}
/* The original loop from surface_render. */
static void ref_draw(uint32_t *dst, uint32_t dst_w, uint32_t dst_h, const struct brook_geometry *physical,
                     const uint32_t *pixels, uint32_t bw, uint32_t bh, int64_t dx, int64_t dy) {
    int64_t x0 = dx < 0 ? -dx : 0, y0 = dy < 0 ? -dy : 0, x1 = physical->w, y1 = physical->h;
    if (dx + x1 > dst_w) x1 = (int64_t)dst_w - dx;
    if (dy + y1 > dst_h) y1 = (int64_t)dst_h - dy;
    for (int64_t y = y0; y < y1; ++y) for (int64_t x = x0; x < x1; ++x) {
        uint32_t *out = &dst[(size_t)(dy + y) * dst_w + dx + x];
        uint32_t px, py; brook_geometry_pixel(physical, bw, bh, x, y, &px, &py);
        *out = ref_pixel_over(pixels[(size_t)py * bw + px], *out);
    }
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static uint32_t rnd_pixel(int opaque) {
    uint32_t r = rnd();
    unsigned sel = rnd() % 8;
    uint32_t a = opaque || sel < 3 ? 255 : sel == 3 ? 0 : rnd() & 255;
    return (r & 0x00ffffffu) | (a << 24);
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void exhaustive(void) {
    for (uint32_t t = 0; t <= 255u * 255u; ++t) assert(brook_div255_round(t) == (t + 127) / 255);
    /* Every (alpha, source channel, destination channel) combination. */
    for (uint32_t a = 0; a < 256; ++a) for (uint32_t c = 0; c < 256; ++c) for (uint32_t d = 0; d < 256; ++d) {
        uint32_t s = (a << 24) | (c << 16) | (c << 8) | c, t = (d << 24) | (d << 16) | (d << 8) | d;
        assert(brook_pixel_over_fast(s, t) == ref_pixel_over(s, t));
        uint32_t s2 = (a << 24) | (c << 16) | (d << 8) | (c ^ d);
        assert(brook_pixel_over_fast(s2, ~t) == ref_pixel_over(s2, ~t));
    }
    for (uint64_t p = 0; p < (1ull << 32); p += 4093) assert(brook_pixel_straight_fast((uint32_t)p) == ref_pixel_straight((uint32_t)p));
    for (uint32_t a = 0; a < 256; ++a) for (uint32_t c = 0; c < 65536; ++c) {
        uint32_t p = (a << 24) | (c << 8) | (c & 255);
        assert(brook_pixel_straight_fast(p) == ref_pixel_straight(p));
    }
    puts("SURFACE_BLIT_EXHAUSTIVE_OK");
}

static void random_scenes(void) {
    enum { W = 173, H = 97 };
    static uint32_t a[W * H], b[W * H], img[300 * 200];
    for (int iter = 0; iter < 4000; ++iter) {
        memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b));
        unsigned layers = 1 + rnd() % 4;
        for (unsigned l = 0; l < layers; ++l) {
            uint32_t bw = 1 + rnd() % 300, bh = 1 + rnd() % 200;
            int opaque = rnd() % 3 == 0;
            for (uint32_t i = 0; i < bw * bh; ++i) img[i] = rnd_pixel(opaque);
            struct brook_geometry g;
            g.transform = rnd() % 8;
            uint32_t tw = (g.transform & 1) ? bh : bw, th = (g.transform & 1) ? bw : bh;
            /* Source rect in 24.8 within the transformed buffer, including fractional cases. */
            uint32_t sx = rnd() % (tw * 256), sy = rnd() % (th * 256);
            uint32_t sw = 1 + rnd() % (tw * 256 - sx), sh = 1 + rnd() % (th * 256 - sy);
            if (rnd() % 3 == 0) { sx = sy = 0; sw = tw * 256; sh = th * 256; }
            g.sx = sx; g.sy = sy; g.sw = sw; g.sh = sh;
            g.w = 1 + rnd() % (2 * W); g.h = 1 + rnd() % (2 * H);
            if (rnd() % 4 == 0) { g.w = tw; g.h = th; }
            int64_t dx = (int64_t)(rnd() % (2 * W)) - W / 2, dy = (int64_t)(rnd() % (2 * H)) - H / 2;
            unsigned flags = (opaque ? BROOK_BLIT_OPAQUE : 0) | (l == 0 ? BROOK_BLIT_DST_CLEAR : 0);
            ref_draw(a, W, H, &g, img, bw, bh, dx, dy);
            assert(brook_blit_draw(b, W, H, &g, img, bw, bh, dx, dy, flags));
            if (memcmp(a, b, sizeof(a))) {
                fprintf(stderr, "mismatch iter=%d layer=%u t=%u buf=%ux%u src=%u,%u %ux%u dst=%ux%u at %lld,%lld flags=%u\n",
                        iter, l, g.transform, bw, bh, sx, sy, sw, sh, g.w, g.h, (long long)dx, (long long)dy, flags);
                assert(0);
            }
        }
        uint32_t outA[W * H], outB[W * H];
        for (int i = 0; i < W * H; ++i) outA[i] = ref_pixel_straight(a[i]);
        brook_straight_rows(outB, W, b, W, W, H);
        assert(!memcmp(outA, outB, sizeof(outA)));
    }
    puts("SURFACE_BLIT_RANDOM_OK");
}

/* 3200x1800 output: an ARGB page layer at 2x (1600x900 -> 3200x1800) and an
 * opaque 1280x720 video subsurface scaled to 2560x1440, like YouTube in a
 * maximised software-rendered browser. */
static void bench(void) {
    enum { W = 3200, H = 1800 };
    uint32_t *a = calloc((size_t)W * H, 4), *b = calloc((size_t)W * H, 4), *vfb = calloc((size_t)W * H, 4);
    uint32_t *page = malloc(1600 * 900 * 4), *video = malloc(1280 * 720 * 4), *opaque = malloc(1600 * 900 * 4);
    for (int i = 0; i < 1600 * 900; ++i) page[i] = rnd_pixel(0);
    for (int i = 0; i < 1280 * 720; ++i) video[i] = rnd_pixel(1);
    for (int i = 0; i < 1600 * 900; ++i) opaque[i] = rnd_pixel(1);
    struct brook_geometry gp = {0, 0, 1600 * 256, 900 * 256, W, H, 0};
    struct brook_geometry gv = {0, 0, 1280 * 256, 720 * 256, 2560, 1440, 0};
    const int reps = 5;
    double t0 = now_s();
    for (int r = 0; r < reps; ++r) {
        memset(a, 0, (size_t)W * H * 4);
        ref_draw(a, W, H, &gp, page, 1600, 900, 0, 0);
        ref_draw(a, W, H, &gv, video, 1280, 720, 320, 180);
        for (size_t i = 0; i < (size_t)W * H; ++i) vfb[i] = ref_pixel_straight(a[i]);
    }
    double t1 = now_s();
    double st[4] = {0};
    for (int r = 0; r < reps; ++r) {
        double s0 = now_s();
        memset(b, 0, (size_t)W * H * 4);
        double s1 = now_s();
        brook_blit_draw(b, W, H, &gp, page, 1600, 900, 0, 0, BROOK_BLIT_DST_CLEAR);
        double s2 = now_s();
        brook_blit_draw(b, W, H, &gv, video, 1280, 720, 320, 180, BROOK_BLIT_OPAQUE);
        double s3 = now_s();
        brook_straight_rows(vfb, W, b, W, W, H);
        double s4 = now_s();
        st[0] += s1 - s0; st[1] += s2 - s1; st[2] += s3 - s2; st[3] += s4 - s3;
    }
    double t2 = now_s();
    printf("SURFACE_BLIT_STAGES clear_ms=%.1f page_ms=%.1f video_ms=%.1f straight_ms=%.1f\n",
           st[0] * 1e3 / reps, st[1] * 1e3 / reps, st[2] * 1e3 / reps, st[3] * 1e3 / reps);
    assert(!memcmp(a, b, (size_t)W * H * 4));
    /* Unscaled full-window opaque layer (the common windowed case at scale 1). */
    struct brook_geometry g1 = {0, 0, 1600 * 256, 900 * 256, 1600, 900, 0};
    double t3 = now_s();
    for (int r = 0; r < reps; ++r) { memset(a, 0, (size_t)W * H * 4); ref_draw(a, W, H, &g1, opaque, 1600, 900, 0, 0); }
    double t4 = now_s();
    for (int r = 0; r < reps; ++r) { memset(b, 0, (size_t)W * H * 4); brook_blit_draw(b, W, H, &g1, opaque, 1600, 900, 0, 0, BROOK_BLIT_OPAQUE | BROOK_BLIT_DST_CLEAR); }
    double t5 = now_s();
    assert(!memcmp(a, b, (size_t)W * H * 4));
    printf("SURFACE_BLIT_BENCH scaled_two_layer_3200x1800 old_ms=%.1f new_ms=%.1f speedup=%.1fx\n",
           (t1 - t0) * 1e3 / reps, (t2 - t1) * 1e3 / reps, (t1 - t0) / (t2 - t1));
    printf("SURFACE_BLIT_BENCH unscaled_opaque_1600x900 old_ms=%.1f new_ms=%.1f speedup=%.1fx\n",
           (t4 - t3) * 1e3 / reps, (t5 - t4) * 1e3 / reps, (t4 - t3) / (t5 - t4));
    /* Typical page: opaque except ~2% antialiased or shadow pixels. */
    for (int i = 0; i < 1600 * 900; ++i) page[i] = rnd() % 50 ? rnd_pixel(1) : rnd_pixel(0);
    double t6 = now_s();
    for (int r = 0; r < reps; ++r) {
        memset(a, 0, (size_t)W * H * 4);
        ref_draw(a, W, H, &gp, page, 1600, 900, 0, 0);
        ref_draw(a, W, H, &gv, video, 1280, 720, 320, 180);
        for (size_t i = 0; i < (size_t)W * H; ++i) vfb[i] = ref_pixel_straight(a[i]);
    }
    double t7 = now_s();
    for (int r = 0; r < reps; ++r) {
        memset(b, 0, (size_t)W * H * 4);
        brook_blit_draw(b, W, H, &gp, page, 1600, 900, 0, 0, BROOK_BLIT_DST_CLEAR);
        brook_blit_draw(b, W, H, &gv, video, 1280, 720, 320, 180, BROOK_BLIT_OPAQUE);
        brook_straight_rows(vfb, W, b, W, W, H);
    }
    double t8 = now_s();
    assert(!memcmp(a, b, (size_t)W * H * 4));
    printf("SURFACE_BLIT_BENCH typical_two_layer_3200x1800 old_ms=%.1f new_ms=%.1f speedup=%.1fx\n",
           (t7 - t6) * 1e3 / reps, (t8 - t7) * 1e3 / reps, (t7 - t6) / (t8 - t7));
    free(a); free(b); free(vfb); free(page); free(video); free(opaque);
}

int main(int argc, char **argv) {
    exhaustive();
    random_scenes();
    if (argc > 1 && !strcmp(argv[1], "--bench")) bench();
    return 0;
}
