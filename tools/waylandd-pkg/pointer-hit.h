#ifndef BROOK_POINTER_HIT_H
#define BROOK_POINTER_HIT_H
/* Pointer hit-testing over a window's surface tree (root plus subsurfaces).
 * Layers are listed bottom to top, as surface_layers() produces them; each
 * carries its offset from the root surface and its committed size, both in
 * logical surface units, plus its input region (surface-local logical).
 * Positions are Wayland 24.8 fixed relative to the root surface. The topmost
 * layer whose bounds and input region contain the point receives it. */
#include <stdint.h>
/* Requires pointer-region.h (no include guard) to be included first. */

struct pointer_hit_layer {
    const struct pointer_region *region; /* NULL means infinite */
    int64_t x,y;                         /* logical offset from the root */
    uint32_t w,h;                        /* logical size; 0 = unmapped */
    void *user;
};

static int32_t pointer_hit_floor(int64_t fixed) {
    return (int32_t)(fixed>=0?fixed/256:-((-fixed+255)/256));
}

/* Returns the chosen layer index or -1; lx and ly get surface-local 24.8. */
static int pointer_hit_pick(const struct pointer_hit_layer *layers,unsigned count,
                            int64_t fx,int64_t fy,int64_t *lx,int64_t *ly) {
    for (unsigned i=count;i-->0;) {
        const struct pointer_hit_layer *l=&layers[i];
        if (!l->w || !l->h) continue;
        int64_t rx=fx-l->x*256,ry=fy-l->y*256;
        if (rx<0 || ry<0 || rx>=(int64_t)l->w*256 || ry>=(int64_t)l->h*256) continue;
        if (l->region && !pointer_region_contains(l->region,pointer_hit_floor(rx),pointer_hit_floor(ry))) continue;
        *lx=rx;*ly=ry;return (int)i;
    }
    return -1;
}

/* Implicit grab: while any button is held, events keep going to the surface
 * that received the press, if it is still part of the tree. */
static int pointer_hit_grab_index(const struct pointer_hit_layer *layers,unsigned count,const void *grab) {
    if (!grab) return -1;
    for (unsigned i=0;i<count;++i) if (layers[i].user==grab) return (int)i;
    return -1;
}
#endif
