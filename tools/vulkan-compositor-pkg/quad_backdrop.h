#pragma once
#include "quad_plan.h"

/* Glass backdrop blur (opt-in: BROOK_RENDER_BLUR=1).
 *
 * Each visible glass rectangle S gets a tile in one shared atlas image with
 * a mip chain; tiles isolate the stacks below overlapping windows and may
 * use different strengths. A tile's blur depth D (coarse cell G = 2^D
 * desktop pixels) comes from the surface record's level. Its capture
 * rectangle P is S padded by QUAD_BACKDROP_PAD_CELLS cells and snapped
 * outward to the G grid, then clipped to the screen.
 *
 * Mip 0 holds P at 1/B (B = 2^base, normally 2): every texel is the exact
 * premultiplied BxB box of the composited stack (coverage-weighted at layer
 * edges). Dual-Kawase downsamples (5 bilinear taps) build mips 1..D-base,
 * then dual-Kawase upsamples (8 bilinear taps) carry the result back up to
 * mip QUAD_BACKDROP_FINAL (1/2B), which the glass draw samples with one
 * bilinear tap (stopping there instead of at mip 0 costs no visible
 * smoothness at these depths and skips the largest pass). Every tap is
 * clamped to the tile's valid texels, so tiles never bleed and the screen
 * edge behaves as clamp-to-edge.
 *
 * Shimmer: P's origin lies on the G grid, so every level samples the same
 * desktop positions wherever S is; the filter's support (under 4G) does not
 * reach past the padding, so the blurred backdrop at a desktop pixel inside
 * S depends only on the content around it, not on where S is. Moving a
 * glass window over static content leaves it unchanged.
 *
 * Atlas placement depends only on the tiles' sizes and order, so a dragged
 * window keeps its place, and each tile is recaptured only when its own key
 * (placement, P and every sampled input) changes: a window dragged over
 * static content recaptures once per G pixels of travel. Coordinates are
 * desktop pixels unless named atlas. */
enum {
    QUAD_BACKDROP_WIDTH=2048, QUAD_BACKDROP_HEIGHT=2048, QUAD_BACKDROP_MIPS=6, QUAD_BACKDROP_TILES=24,
    QUAD_BACKDROP_PAD_CELLS=4, QUAD_BACKDROP_ALIGN=1<<(QUAD_BACKDROP_MIPS-1), QUAD_BACKDROP_FINAL=1
};
struct QuadBackdropTile {
    struct QuadRect screen;   /* S: the glass rectangle */
    struct QuadRect capture;  /* P: what mip 0 holds */
    struct QuadRect atlas;    /* P's mip-0 texels: origin, then ceil(P/B) texels */
    uint16_t layer,below;
    uint8_t depth,base,mips,unused;
    uint32_t radius;          /* blur level from the surface record */
    uint64_t key;             /* placement, P and every sampled input */
};
enum { QUAD_GPU_ATLAS, QUAD_GPU_GLASS, QUAD_GPU_FRAME, QUAD_GPU_SHELL, QUAD_GPU_CLASSES };
struct QuadBackdrop {
    uint32_t count;
    uint64_t key,damageKey;
    uint8_t used[BROOK_VK_MAX_LAYERS],chromeClass[BROOK_VK_MAX_LAYERS];
    struct QuadBackdropTile tiles[QUAD_BACKDROP_TILES];
};

/* Theme strengths (brook_appearance_blur_strength): Light 2, Moderate 8,
 * Heavy 16. The result is close to a Gaussian with sigma about G: 15, 31
 * and 62 desktop pixels (7.5, 15 and 31 logical pixels at 200%). */
static inline unsigned quad_backdrop_depth(unsigned level){return level<=3?4u:level<=11?5u:6u;}

static inline void quad_backdrop_add(struct QuadBackdrop *p,struct QuadRect screen,
                                    unsigned layer,unsigned below,unsigned radius)
{
    if(!radius || quad_rect_empty(screen) || p->count>QUAD_BACKDROP_TILES)return;
    if(p->count==QUAD_BACKDROP_TILES){++p->count;return;}
    p->tiles[p->count++]=(struct QuadBackdropTile){.screen=screen,.layer=layer,
        .below=below,.radius=radius>24?24:radius};
}

static inline const uint32_t *quad_backdrop_layer(const uint32_t *scene,unsigned layer)
{
    return scene+BROOK_VK_SCENE_HEADER+layer*BROOK_VK_LAYER_WORDS;
}

static inline uint64_t quad_backdrop_mix(uint64_t key,uint64_t value)
{
    return (key^value)*UINT64_C(1099511628211);
}
static inline uint64_t quad_backdrop_mix_rect(uint64_t key,struct QuadRect r)
{
    key=quad_backdrop_mix(key,(uint32_t)r.x0);key=quad_backdrop_mix(key,(uint32_t)r.y0);
    key=quad_backdrop_mix(key,(uint32_t)r.x1);return quad_backdrop_mix(key,(uint32_t)r.y1);
}

/* Plan only layers intersecting the capture. Exact opaque coverage skips
 * older inputs, avoiding needless recapture when a hidden client commits. */
static inline unsigned quad_backdrop_bottom(const uint32_t *scene,const struct QuadRect *holes,
                                            const struct QuadBackdropTile *tile)
{
    for(unsigned i=tile->below;i>1;){
        --i;
        const uint32_t *l=quad_backdrop_layer(scene,i);
        if(quad_layer_replaces(l) && quad_rect_contains(quad_layer_bounds(l,scene[0],scene[1]),tile->capture) &&
           (!holes || quad_rect_empty(quad_rect_intersect(holes[i],tile->capture))))return i;
    }
    return 0;
}

static inline int32_t quad_backdrop_floor(int32_t v,int32_t g){return v>=0?v/g*g:-((-v+g-1)/g*g);}
static inline int32_t quad_backdrop_ceil(int32_t v,int32_t g){return -quad_backdrop_floor(-v,g);}

/* P's size before clipping: whole cells covering S and at least the
 * padding on both sides for any grid phase of S. It depends only on S's
 * size, so P changes once per cell crossed. */
static inline int32_t quad_backdrop_span(int32_t size,unsigned depth)
{
    const int32_t g=1<<depth;
    return quad_backdrop_ceil(size+2*QUAD_BACKDROP_PAD_CELLS*g+g-1,g);
}
/* S padded and snapped to the coarse grid, within the screen. */
static inline struct QuadRect quad_backdrop_capture(struct QuadRect s,unsigned depth,uint32_t width,uint32_t height)
{
    const int32_t g=1<<depth,pad=QUAD_BACKDROP_PAD_CELLS*g;
    const int32_t x=quad_backdrop_floor(s.x0-pad,g),y=quad_backdrop_floor(s.y0-pad,g);
    const struct QuadRect p={x,y,x+quad_backdrop_span(s.x1-s.x0,depth),y+quad_backdrop_span(s.y1-s.y0,depth)};
    return quad_rect_intersect(p,(struct QuadRect){0,0,(int32_t)width,(int32_t)height});
}

/* Mip-0 texels reserved for a tile of S: P's unclipped span, so the
 * reservation (and the atlas layout) does not change while S moves. */
static inline void quad_backdrop_reserve(struct QuadRect s,unsigned depth,unsigned base,uint32_t width,uint32_t height,
                                         uint32_t *w,uint32_t *h)
{
    const uint32_t b=1u<<base,a=QUAD_BACKDROP_ALIGN;
    const uint32_t span[2]={(uint32_t)quad_backdrop_span(s.x1-s.x0,depth),(uint32_t)quad_backdrop_span(s.y1-s.y0,depth)};
    const uint32_t screen[2]={width,height};
    uint32_t out[2];
    for(unsigned k=0;k<2;++k){
        uint32_t texels=(span[k]+b-1u)/b;
        const uint32_t whole=(screen[k]+b-1u)/b;
        if(texels>whole)texels=whole;
        out[k]=(texels+a-1u)/a*a;
    }
    *w=out[0];*h=out[1];
}

/* Shelf packing in tile order at capture scale 1/2^base. */
static inline int quad_backdrop_pack(struct QuadBackdrop *p,uint32_t width,uint32_t height,unsigned base)
{
    uint32_t x=0,y=0,band=0;
    for(unsigned i=0;i<p->count;++i){
        struct QuadBackdropTile *t=&p->tiles[i];
        t->depth=(uint8_t)quad_backdrop_depth(t->radius);
        if(t->depth<base+QUAD_BACKDROP_FINAL+1 || t->depth-base>=QUAD_BACKDROP_MIPS)return 0;
        uint32_t w,h;quad_backdrop_reserve(t->screen,t->depth,base,width,height,&w,&h);
        if(w>QUAD_BACKDROP_WIDTH)return 0;
        if(x+w>QUAD_BACKDROP_WIDTH){x=0;y+=band;band=0;}
        if(y+h>QUAD_BACKDROP_HEIGHT)return 0;
        t->base=(uint8_t)base;t->mips=(uint8_t)(t->depth-base);
        t->capture=quad_backdrop_capture(t->screen,t->depth,width,height);
        const int32_t b=1<<base;
        t->atlas=(struct QuadRect){(int32_t)x,(int32_t)y,
            (int32_t)x+(t->capture.x1-t->capture.x0+b-1)/b,(int32_t)y+(t->capture.y1-t->capture.y0+b-1)/b};
        x+=w;if(h>band)band=h;
    }
    return 1;
}

static inline int quad_backdrop_layout(struct QuadBackdrop *p,const uint32_t *scene,
                                      const struct QuadRect *holes,const uint64_t *keys)
{
    if(p->count>QUAD_BACKDROP_TILES || !scene[0] || !scene[1])goto fail;
    if(!p->count)return 1;
    for(unsigned i=0;i<p->count;++i){
        struct QuadBackdropTile *t=&p->tiles[i];
        if(t->layer>=scene[2] || !t->below || t->below>t->layer)goto fail;
        const uint32_t *own=quad_backdrop_layer(scene,t->layer);
        t->screen=quad_rect_intersect(t->screen,quad_layer_bounds(own,scene[0],scene[1]));
        int covered=0;
        for(unsigned top=t->layer+1;!covered && top<scene[2];++top){
            const uint32_t *higher=quad_backdrop_layer(scene,top);
            covered=quad_layer_replaces(higher) &&
                quad_rect_contains(quad_layer_bounds(higher,scene[0],scene[1]),t->screen) &&
                (!holes || quad_rect_empty(quad_rect_intersect(holes[top],t->screen)));
        }
        if(quad_rect_empty(t->screen) || covered){
            --p->count;
            memmove(t,t+1,(p->count-i)*sizeof(*t));
            --i;continue;
        }
        if(quad_layer_replaces(own))goto fail;
        for(unsigned j=0;j<i;++j)
            if(p->tiles[j].layer==t->layer && !quad_rect_empty(quad_rect_intersect(p->tiles[j].screen,t->screen)))
                goto fail;
    }
    if(!p->count)return 1;
    /* Half-resolution capture; a quarter if the tiles do not fit. */
    if(!quad_backdrop_pack(p,scene[0],scene[1],1) && !quad_backdrop_pack(p,scene[0],scene[1],2))goto fail;
    const uint64_t seed=quad_backdrop_mix(quad_backdrop_mix(UINT64_C(14695981039346656037),scene[0]),scene[1]);
    uint64_t key=seed;
    for(unsigned i=0;i<p->count;++i){
        struct QuadBackdropTile *t=&p->tiles[i];
        uint64_t k=quad_backdrop_mix_rect(quad_backdrop_mix_rect(seed,t->capture),t->atlas);
        k=quad_backdrop_mix(k,(uint64_t)t->depth<<8|t->base);
        for(unsigned src=quad_backdrop_bottom(scene,holes,t);src<t->below;++src){
            const uint32_t *l=quad_backdrop_layer(scene,src);
            const struct QuadRect r=quad_rect_intersect(t->capture,quad_layer_bounds(l,scene[0],scene[1]));
            if(quad_rect_empty(r) || (holes && quad_rect_contains(holes[src],r)))continue;
            p->used[src]=1;
            k=quad_backdrop_mix(k,src);
            k=quad_backdrop_mix(k,keys[src]);
            for(unsigned w=0;w<BROOK_VK_LAYER_WORDS;++w)k=quad_backdrop_mix(k,l[w]);
            if(holes)k=quad_backdrop_mix_rect(k,holes[src]);
        }
        t->key=k?k:1;
        key=quad_backdrop_mix(key,t->key);
    }
    p->key=key?key:1;
    /* Damage also follows where each glass rectangle is drawn. */
    for(unsigned i=0;i<p->count;++i)key=quad_backdrop_mix_rect(quad_backdrop_mix(key,p->tiles[i].layer),p->tiles[i].screen);
    p->damageKey=key?key:1;
    return 1;
fail:
    p->key=p->damageKey=p->count=0;memset(p->used,0,sizeof(p->used));
    return 0;
}
