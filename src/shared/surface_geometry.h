#ifndef BROOK_SURFACE_GEOMETRY_H
#define BROOK_SURFACE_GEOMETRY_H
#include <stdint.h>
/* Source coordinates are Wayland 24.8 surface units, before destination
 * scaling. A resolved rectangle is in transformed buffer units (also 24.8).
 * This state is copied into each immutable surface commit. */
struct brook_geometry_state {
    int32_t scale,transform;
    int source_set,destination_set;
    int32_t x,y,w,h,dw,dh;
};
struct brook_geometry {
    uint32_t sx,sy,sw,sh,w,h,transform;
};
enum { BROOK_GEOMETRY_OK=0, BROOK_GEOMETRY_SIZE=1, BROOK_GEOMETRY_BOUNDS=2, BROOK_GEOMETRY_BUFFER_SIZE=3 };
static inline int brook_geometry_resolve(const struct brook_geometry_state *s,
        uint32_t bw,uint32_t bh,struct brook_geometry *out) {
    *out=(struct brook_geometry){0,0,0,0,0,0,0};
    if (s->source_set && !s->destination_set && ((s->w|s->h)&255)) return BROOK_GEOMETRY_SIZE;
    if (!bw || !bh) return BROOK_GEOMETRY_OK;
    const uint32_t scale=s->scale?s->scale:1;
    if (s->scale<0 || s->transform<0 || s->transform>7 || bw>16384 || bh>16384 ||
        bw%scale || bh%scale) return BROOK_GEOMETRY_BUFFER_SIZE;
    uint32_t tw=(s->transform&1)?bh:bw,th=(s->transform&1)?bw:bh;
    int64_t x=0,y=0,w=(int64_t)tw*256,h=(int64_t)th*256;
    if (s->source_set) {
        x=(int64_t)s->x*scale;y=(int64_t)s->y*scale;
        w=(int64_t)s->w*scale;h=(int64_t)s->h*scale;
        if (x<0 || y<0 || w<=0 || h<=0 || x+w>(int64_t)tw*256 || y+h>(int64_t)th*256)
            return BROOK_GEOMETRY_BOUNDS;
        if (!s->destination_set && ((s->w|s->h)&255)) return BROOK_GEOMETRY_SIZE;
    }
    const int64_t dw=s->destination_set?(int64_t)s->dw:(s->source_set?(int64_t)s->w/256:tw/scale);
    const int64_t dh=s->destination_set?(int64_t)s->dh:(s->source_set?(int64_t)s->h/256:th/scale);
    if (dw<=0 || dh<=0 || dw>16384 || dh>16384) return BROOK_GEOMETRY_SIZE;
    *out=(struct brook_geometry){(uint32_t)x,(uint32_t)y,(uint32_t)w,(uint32_t)h,(uint32_t)dw,(uint32_t)dh,(uint32_t)s->transform};
    return BROOK_GEOMETRY_OK;
}
/* Pixel-center nearest sampling. All products fit in signed 64 bits at the
 * compositor's 16384 limit. Apply the transform the client applied to its
 * buffer (inverse of the compositor's buffer-to-surface operation). */
static inline void brook_geometry_pixel(const struct brook_geometry *g,
        uint32_t bw,uint32_t bh,uint32_t x,uint32_t y,uint32_t *px,uint32_t *py) {
    const int64_t tw=(g->transform&1)?bh:bw,th=(g->transform&1)?bw:bh;
    int64_t u=(int64_t)g->sx*2*g->w+(int64_t)(2*x+1)*g->sw;
    int64_t v=(int64_t)g->sy*2*g->h+(int64_t)(2*y+1)*g->sh;
    int64_t ud=(int64_t)g->w*512*tw,vd=(int64_t)g->h*512*th;
    if (g->transform&4) u=ud-u;
    int64_t a=u,ad=ud,b=v,bd=vd;
    switch(g->transform&3) {
    case 1:a=v;ad=vd;b=ud-u;bd=ud;break;
    case 2:a=ud-u;b=vd-v;break;
    case 3:a=vd-v;ad=vd;b=u;bd=ud;break;
    }
    *px=(uint32_t)(a*bw/ad);*py=(uint32_t)(b*bh/bd);
    if (*px>=bw) *px=bw-1;
    if (*py>=bh) *py=bh-1;
}
#endif
