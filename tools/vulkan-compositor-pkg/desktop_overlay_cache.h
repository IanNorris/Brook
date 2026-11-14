#pragma once
#include "brook_debug_frame.h"
#include "desktop_damage.h"
#include <stdint.h>

/* What one frame shows of the desktop overlay: the captured bands of a
 * kernel content generation (0 = unknown, e.g. an older kernel, which is
 * treated as changed every frame) and the separately drawn pointer. */
struct DesktopOverlayCursorState { uint32_t visible,width,height; int32_t x,y; uint64_t serial; };
struct DesktopOverlayFrameState {
    uint32_t valid,width,height;
    uint64_t generation;
    struct BrookDesktopOverlayBands bands;
    struct DesktopOverlayCursorState cursor;
};

/* A context's retained overlay bands may be reused only for the same
 * nonzero generation and output size; anything else needs a new capture. */
static inline int desktop_overlay_cache_valid(uint64_t cached,uint32_t cachedWidth,uint32_t cachedHeight,
                                              uint64_t current,uint32_t width,uint32_t height){
    return cached && current && cached==current && cachedWidth==width && cachedHeight==height;
}

static inline int desktop_overlay_bands_equal(const struct BrookDesktopOverlayBands *a,const struct BrookDesktopOverlayBands *b){
    if(a->count!=b->count)return 0;
    for(uint32_t i=0;i<a->count && i<BROOK_DESKTOP_OVERLAY_MAX_BANDS;++i){
        const struct BrookDesktopOverlayBand *x=&a->bands[i],*y=&b->bands[i];
        if(x->x!=y->x || x->y!=y->y || x->width!=y->width || x->height!=y->height)return 0;
    }
    return 1;
}
static inline int desktop_overlay_cursor_equal(const struct DesktopOverlayCursorState *a,const struct DesktopOverlayCursorState *b){
    if(a->visible!=b->visible)return 0;
    if(!a->visible)return 1;
    return a->width==b->width && a->height==b->height && a->x==b->x && a->y==b->y && a->serial==b->serial;
}
static inline void desktop_overlay_add_bands(struct DesktopDamageSet *damage,const struct DesktopOverlayFrameState *s,uint32_t width,uint32_t height){
    for(uint32_t i=0;i<s->bands.count && i<BROOK_DESKTOP_OVERLAY_MAX_BANDS;++i){const struct BrookDesktopOverlayBand *b=&s->bands.bands[i];
        desktop_damage_add(damage,(struct DesktopDamageRect){b->x,b->y,(int64_t)b->x+b->width,(int64_t)b->y+b->height},width,height);}
}
static inline void desktop_overlay_add_cursor(struct DesktopDamageSet *damage,const struct DesktopOverlayCursorState *c,uint32_t width,uint32_t height){
    if(c->visible)desktop_damage_add(damage,(struct DesktopDamageRect){c->x,c->y,(int64_t)c->x+c->width,(int64_t)c->y+c->height},width,height);
}

/* Damage the overlay only where this frame differs from the last presented
 * frame: old and new bands when the content generation or geometry changed,
 * old and new pointer rectangles when the pointer moved or changed. Comparing
 * against the presented (not the last prepared) frame keeps a change damaged
 * until a frame containing it is actually displayed. */
static inline void desktop_overlay_damage(const struct DesktopOverlayFrameState *displayed,
                                          const struct DesktopOverlayFrameState *frame,
                                          struct DesktopDamageSet *damage){
    const uint32_t width=frame->width,height=frame->height;
    const int sameOutput=displayed->valid && displayed->width==width && displayed->height==height;
    if(!sameOutput || !frame->generation || displayed->generation!=frame->generation ||
       !desktop_overlay_bands_equal(&displayed->bands,&frame->bands)){
        if(sameOutput)desktop_overlay_add_bands(damage,displayed,width,height);
        desktop_overlay_add_bands(damage,frame,width,height);
    }
    if(!sameOutput || !desktop_overlay_cursor_equal(&displayed->cursor,&frame->cursor)){
        if(sameOutput)desktop_overlay_add_cursor(damage,&displayed->cursor,width,height);
        desktop_overlay_add_cursor(damage,&frame->cursor,width,height);
    }
}

/* Record a successful presentation. Async frames retire in ticket order;
 * an older ticket must not overwrite the state of a newer displayed one. */
static inline void desktop_overlay_presented(struct DesktopOverlayFrameState *displayed,uint64_t *displayedTicket,
                                             const struct DesktopOverlayFrameState *frame,uint64_t ticket){
    if(ticket && *displayedTicket && ticket<*displayedTicket)return;
    *displayed=*frame;if(ticket)*displayedTicket=ticket;
}
