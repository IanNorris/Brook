#pragma once
#include "quad_plan.h"

/* Each visible glass rectangle gets its own tile in one shared image. Tiles
 * isolate the stacks below overlapping windows and may use different radii.
 * Coordinates are in desktop pixels until the atlas is laid out. */
enum { QUAD_BACKDROP_WIDTH=480, QUAD_BACKDROP_HEIGHT=270, QUAD_BACKDROP_TILES=24 };
struct QuadBackdropTile {
    struct QuadRect screen, atlas;
    uint16_t layer, below;
    uint32_t radius;
};
struct QuadBackdrop {
    uint32_t count;
    uint64_t key;
    uint8_t used[BROOK_VK_MAX_LAYERS];
    struct QuadBackdropTile tiles[QUAD_BACKDROP_TILES];
};

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

/* Plan only layers intersecting a tile. Exact opaque coverage skips older
 * inputs, avoiding needless recapture when a hidden client commits. */
static inline unsigned quad_backdrop_bottom(const uint32_t *scene,const struct QuadRect *holes,
                                            const struct QuadBackdropTile *tile)
{
    for(unsigned i=tile->below;i>1;){
        --i;
        const uint32_t *l=quad_backdrop_layer(scene,i);
        if(quad_layer_replaces(l) && quad_rect_contains(quad_layer_bounds(l,scene[0],scene[1]),tile->screen) &&
           (!holes || quad_rect_empty(quad_rect_intersect(holes[i],tile->screen))))return i;
    }
    return 0;
}

static inline int quad_backdrop_layout(struct QuadBackdrop *p,const uint32_t *scene,
                                      const struct QuadRect *holes,const uint64_t *keys)
{
    if(p->count>QUAD_BACKDROP_TILES || !scene[0] || !scene[1])goto fail;
    if(!p->count)return 1;
    const uint32_t panelW=scene[0]<QUAD_BACKDROP_WIDTH?scene[0]:QUAD_BACKDROP_WIDTH;
    const uint32_t panelH=scene[1]<QUAD_BACKDROP_HEIGHT?scene[1]:QUAD_BACKDROP_HEIGHT;
    uint32_t x=0,y=0,band=0;
    uint64_t key=quad_backdrop_mix(quad_backdrop_mix(UINT64_C(14695981039346656037),scene[0]),scene[1]);
    for(unsigned i=0;i<p->count;++i){
        struct QuadBackdropTile *t=&p->tiles[i];
        if(t->layer>=scene[2] || !t->below || t->below>t->layer)goto fail;
        const uint32_t *own=quad_backdrop_layer(scene,t->layer);
        t->screen=quad_rect_intersect(t->screen,quad_layer_bounds(own,scene[0],scene[1]));
        if(quad_rect_empty(t->screen)){
            --p->count;
            memmove(t,t+1,(p->count-i)*sizeof(*t));
            --i;continue;
        }
        if(quad_layer_replaces(own))goto fail;
        for(unsigned j=0;j<i;++j)
            if(p->tiles[j].layer==t->layer && !quad_rect_empty(quad_rect_intersect(p->tiles[j].screen,t->screen)))
                goto fail;
        const uint32_t w=((t->screen.x1-t->screen.x0)*panelW+scene[0]-1)/scene[0];
        const uint32_t h=((t->screen.y1-t->screen.y0)*panelH+scene[1]-1)/scene[1];
        if(x+w>QUAD_BACKDROP_WIDTH){x=0;y+=band;band=0;}
        if(y+h>QUAD_BACKDROP_HEIGHT)goto fail;
        t->atlas=(struct QuadRect){(int32_t)x,(int32_t)y,(int32_t)(x+w),(int32_t)(y+h)};
        x+=w;if(h>band)band=h;
        key=quad_backdrop_mix(key,(uint32_t)t->screen.x0);
        key=quad_backdrop_mix(key,(uint32_t)t->screen.y0);
        key=quad_backdrop_mix(key,(uint32_t)t->screen.x1);
        key=quad_backdrop_mix(key,(uint32_t)t->screen.y1);
        key=quad_backdrop_mix(key,t->layer);
        key=quad_backdrop_mix(key,t->radius);
        for(unsigned src=quad_backdrop_bottom(scene,holes,t);src<t->below;++src){
            const uint32_t *l=quad_backdrop_layer(scene,src);
            const struct QuadRect r=quad_rect_intersect(t->screen,quad_layer_bounds(l,scene[0],scene[1]));
            if(quad_rect_empty(r) || (holes && quad_rect_contains(holes[src],r)))continue;
            p->used[src]=1;
            key=quad_backdrop_mix(key,keys[src]);
            for(unsigned k=0;k<BROOK_VK_LAYER_WORDS;++k)key=quad_backdrop_mix(key,l[k]);
            if(holes){
                key=quad_backdrop_mix(key,(uint32_t)holes[src].x0);
                key=quad_backdrop_mix(key,(uint32_t)holes[src].y0);
                key=quad_backdrop_mix(key,(uint32_t)holes[src].x1);
                key=quad_backdrop_mix(key,(uint32_t)holes[src].y1);
            }
        }
    }
    p->key=key?key:1;
    return 1;
fail:
    *p=(struct QuadBackdrop){0};
    return 0;
}
