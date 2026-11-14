#pragma once
/* CPU planning for the per-layer quad compositor (quad_pass.c).
 *
 * The scene is the packed scene.h ABI; its pixel result is defined by
 * composite.comp: start from zero and blend every layer in order. The plan
 * draws each layer as a few pixel-aligned rectangles limited to the damage:
 *
 *  - Damage rectangles must be disjoint, so draws can be issued layer by
 *    layer without blending any pixel twice.
 *  - A layer whose source is opaque (BROOK_VK_OPAQUE_SOURCE) at opacity 255
 *    replaces what lies beneath it. Every lower layer's rectangles exclude
 *    it, so, for example, the wallpaper under the game is never read.
 *  - A layer may name a hole: a rectangle it leaves unchanged (the
 *    transparent chrome interior), which is excluded from its own draws.
 *  - Where layer 0 covers a damage rectangle it is drawn without blending,
 *    computing its exact over-zero value; elsewhere the rectangle is first
 *    cleared to zero. Each pixel is therefore written before it is blended.
 *
 * Rectangle subtraction is capped per layer and damage rectangle; when a
 * cap is reached the remaining occluders are not subtracted, which costs
 * overdraw but not correctness (the replacing layer is drawn later). */
#include "scene.h"
#include "desktop_damage.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { QUAD_KIND_STRAIGHT=0, QUAD_KIND_PREMULTIPLIED=1, QUAD_KIND_REPLACE=2, QUAD_KIND_OVER_ZERO=3 };
enum { QUAD_PLAN_MAX_DRAWS=1024, QUAD_PLAN_MAX_PIECES=16, QUAD_PLAN_MAX_CLEARS=DESKTOP_DAMAGE_MAX_RECTS };
struct QuadRect { int32_t x0,y0,x1,y1; };
struct QuadDraw { uint16_t layer,kind; struct QuadRect rect; };
struct QuadPlan {
    uint32_t width,height,drawCount,clearCount,regionCount,layersDrawn,occlusion;
    uint64_t damagePixels,pixelsShaded,clearPixels;
    struct QuadRect bounds;
    uint8_t used[BROOK_VK_MAX_LAYERS];
    struct QuadRect clears[QUAD_PLAN_MAX_CLEARS];
    struct QuadDraw draws[QUAD_PLAN_MAX_DRAWS];
};

static inline int quad_rect_empty(struct QuadRect r){return r.x0>=r.x1 || r.y0>=r.y1;}
static inline uint64_t quad_rect_area(struct QuadRect r){return quad_rect_empty(r)?0:(uint64_t)(r.x1-r.x0)*(uint64_t)(r.y1-r.y0);}
static inline struct QuadRect quad_rect_intersect(struct QuadRect a,struct QuadRect b){
    struct QuadRect r={a.x0>b.x0?a.x0:b.x0,a.y0>b.y0?a.y0:b.y0,a.x1<b.x1?a.x1:b.x1,a.y1<b.y1?a.y1:b.y1};
    return r;
}
static inline int quad_rect_contains(struct QuadRect outer,struct QuadRect inner){
    return outer.x0<=inner.x0 && outer.y0<=inner.y0 && outer.x1>=inner.x1 && outer.y1>=inner.y1;
}
/* Destination bounds clipped to the layer's clip rectangle and the output. */
static inline struct QuadRect quad_layer_bounds(const uint32_t *l,uint32_t width,uint32_t height){
    int64_t x0=(int32_t)l[8],y0=(int32_t)l[9],x1=x0+l[10],y1=y0+l[11];
    const int64_t cx0=(int32_t)l[12],cy0=(int32_t)l[13],cx1=cx0+l[14],cy1=cy0+l[15];
    if(x0<cx0)x0=cx0;
    if(y0<cy0)y0=cy0;
    if(x1>cx1)x1=cx1;
    if(y1>cy1)y1=cy1;
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>width)x1=width;
    if(y1>height)y1=height;
    if(x0>=x1 || y0>=y1)return (struct QuadRect){0,0,0,0};
    return (struct QuadRect){(int32_t)x0,(int32_t)y0,(int32_t)x1,(int32_t)y1};
}
static inline int quad_layer_replaces(const uint32_t *l){
    return (l[16]&(BROOK_VK_OPAQUE_SOURCE|255u))==(BROOK_VK_OPAQUE_SOURCE|255u);
}
/* Subtract cut from every piece. Returns 0 (pieces unchanged) when the
 * result would exceed the cap. */
static inline int quad_subtract(struct QuadRect *pieces,unsigned *count,struct QuadRect cut){
    struct QuadRect out[QUAD_PLAN_MAX_PIECES];unsigned n=0;
    for(unsigned i=0;i<*count;++i){const struct QuadRect p=pieces[i];
        const struct QuadRect o=quad_rect_intersect(p,cut);
        if(quad_rect_empty(o)){if(n==QUAD_PLAN_MAX_PIECES)return 0;out[n++]=p;continue;}
        const struct QuadRect parts[4]={{p.x0,p.y0,p.x1,o.y0},{p.x0,o.y1,p.x1,p.y1},{p.x0,o.y0,o.x0,o.y1},{o.x1,o.y0,p.x1,o.y1}};
        for(unsigned k=0;k<4;++k)if(!quad_rect_empty(parts[k])){if(n==QUAD_PLAN_MAX_PIECES)return 0;out[n++]=parts[k];}
    }
    memcpy(pieces,out,n*sizeof(*out));*count=n;return 1;
}
/* scene: header and layer words (validated by the caller or quad_pass).
 * holes: optional per-layer output rectangles to skip (empty = none).
 * damage: disjoint rectangles inside the output. Returns 0 if the input is
 * malformed or the draw list does not fit; the caller then uses another
 * renderer. */
static inline int quad_plan_build(const uint32_t *scene,size_t words,const struct QuadRect *holes,
                                  const struct DesktopDamageSet *damage,int occlusion,struct QuadPlan *plan){
    if(!scene || !damage || !plan || words<BROOK_VK_SCENE_HEADER)return 0;
    const uint32_t width=scene[0],height=scene[1],count=scene[2];
    if(!width || !height || width>BROOK_VK_MAX_DIMENSION || height>BROOK_VK_MAX_DIMENSION ||
       !count || count>BROOK_VK_MAX_LAYERS || words<BROOK_VK_SCENE_HEADER+(size_t)count*BROOK_VK_LAYER_WORDS ||
       !damage->count || damage->count>QUAD_PLAN_MAX_CLEARS)return 0;
    struct QuadRect rects[QUAD_PLAN_MAX_CLEARS];
    for(uint32_t i=0;i<damage->count;++i){const struct DesktopDamageRect *d=&damage->rects[i];
        if(d->left<0 || d->top<0 || d->right<=d->left || d->bottom<=d->top || d->right>width || d->bottom>height)return 0;
        rects[i]=(struct QuadRect){(int32_t)d->left,(int32_t)d->top,(int32_t)d->right,(int32_t)d->bottom};
        for(uint32_t j=0;j<i;++j)if(!quad_rect_empty(quad_rect_intersect(rects[i],rects[j])))return 0;
    }
    memset(plan,0,offsetof(struct QuadPlan,clears));
    plan->width=width;plan->height=height;plan->regionCount=damage->count;plan->occlusion=occlusion!=0;
    plan->bounds=rects[0];
    struct QuadRect bounds[BROOK_VK_MAX_LAYERS];
    for(uint32_t i=0;i<count;++i)bounds[i]=quad_layer_bounds(scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS,width,height);
    uint8_t base[QUAD_PLAN_MAX_CLEARS];
    for(uint32_t r=0;r<damage->count;++r){
        const struct QuadRect d=rects[r];plan->damagePixels+=quad_rect_area(d);
        if(d.x0<plan->bounds.x0)plan->bounds.x0=d.x0;
        if(d.y0<plan->bounds.y0)plan->bounds.y0=d.y0;
        if(d.x1>plan->bounds.x1)plan->bounds.x1=d.x1;
        if(d.y1>plan->bounds.y1)plan->bounds.y1=d.y1;
        base[r]=quad_rect_contains(bounds[0],d) && (!holes || quad_rect_empty(quad_rect_intersect(holes[0],d)));
        if(!base[r]){plan->clears[plan->clearCount++]=d;plan->clearPixels+=quad_rect_area(d);}
    }
    for(uint32_t i=0;i<count;++i){
        const uint32_t *l=scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS;
        if(quad_rect_empty(bounds[i]))continue;
        const int replace=quad_layer_replaces(l);
        const uint16_t kind=replace?QUAD_KIND_REPLACE:(l[16]&BROOK_VK_STRAIGHT_ALPHA)?QUAD_KIND_STRAIGHT:QUAD_KIND_PREMULTIPLIED;
        for(uint32_t r=0;r<damage->count;++r){
            const int overZero=i==0 && base[r];
            /* A zero-opacity layer changes nothing unless it defines the base. */
            if(!(l[16]&255u) && !overZero)continue;
            struct QuadRect pieces[QUAD_PLAN_MAX_PIECES];unsigned n=0;
            pieces[0]=quad_rect_intersect(bounds[i],rects[r]);
            if(quad_rect_empty(pieces[0]))continue;
            n=1;
            if(holes && !quad_rect_empty(holes[i]))(void)quad_subtract(pieces,&n,holes[i]);
            for(uint32_t j=i+1;occlusion && n && j<count;++j){
                const uint32_t *upper=scene+BROOK_VK_SCENE_HEADER+j*BROOK_VK_LAYER_WORDS;
                if(!quad_layer_replaces(upper) || quad_rect_empty(bounds[j]))continue;
                (void)quad_subtract(pieces,&n,bounds[j]);
            }
            for(unsigned k=0;k<n;++k){
                if(plan->drawCount==QUAD_PLAN_MAX_DRAWS)return 0;
                plan->draws[plan->drawCount++]=(struct QuadDraw){(uint16_t)i,overZero?(uint16_t)QUAD_KIND_OVER_ZERO:kind,pieces[k]};
                plan->pixelsShaded+=quad_rect_area(pieces[k]);
                if(!plan->used[i]){plan->used[i]=1;++plan->layersDrawn;}
            }
        }
    }
    return 1;
}
/* Retry without occlusion when the occluded plan does not fit. */
static inline int quad_plan_build_auto(const uint32_t *scene,size_t words,const struct QuadRect *holes,
                                       const struct DesktopDamageSet *damage,int occlusion,struct QuadPlan *plan){
    return quad_plan_build(scene,words,holes,damage,occlusion,plan) ||
           (occlusion && quad_plan_build(scene,words,holes,damage,0,plan));
}
