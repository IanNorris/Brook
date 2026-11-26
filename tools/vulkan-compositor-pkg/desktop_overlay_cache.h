#pragma once
#include "brook_debug_frame.h"
#include "desktop_damage.h"
#include <stdint.h>

/* What one frame shows of the desktop overlay: the captured bands of a
 * kernel content generation (0 = unknown, e.g. an older kernel, which is
 * treated as changed every frame) and the separately drawn pointer. */
struct DesktopOverlayCursorState { uint32_t visible,width,height; int32_t x,y; uint64_t serial; uint32_t stride; };
/* opaque/keys come from kernel op 9 (zero from older kernels): bit i means
 * band i is fully opaque, keys[i] identifies its pixels (0 = unknown). */
struct DesktopOverlayFrameState {
    uint32_t valid,width,height;
    uint64_t generation;
    struct BrookDesktopOverlayBands bands;
    struct DesktopOverlayCursorState cursor;
    uint32_t opaque;
    uint64_t keys[BROOK_DESKTOP_OVERLAY_MAX_BANDS];
};

/* Index of a band of s with exactly band b's geometry, key and opacity, or
 * -1. A zero key never matches: its content is unknown. */
static inline int desktop_overlay_find_band(const struct DesktopOverlayFrameState *s,
                                            const struct BrookDesktopOverlayBand *b,uint64_t key,int opaque){
    if(!key)return -1;
    for(uint32_t i=0;i<s->bands.count && i<BROOK_DESKTOP_OVERLAY_MAX_BANDS;++i){const struct BrookDesktopOverlayBand *x=&s->bands.bands[i];
        if(s->keys[i]==key && ((s->opaque>>i)&1u)==(uint32_t)(opaque!=0) &&
           x->x==b->x && x->y==b->y && x->width==b->width && x->height==b->height)return (int)i;}
    return -1;
}
/* Every band of s is keyed, so bands can be compared one by one. */
static inline int desktop_overlay_keyed(const struct DesktopOverlayFrameState *s){
    if(!s->valid)return 0;
    for(uint32_t i=0;i<s->bands.count && i<BROOK_DESKTOP_OVERLAY_MAX_BANDS;++i)if(!s->keys[i])return 0;
    return 1;
}

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
    if(sameOutput && frame->generation && desktop_overlay_keyed(displayed) && desktop_overlay_keyed(frame)){
        /* Keyed bands: damage only bands that appeared, vanished or changed,
         * so an unchanged launcher is not redrawn when the taskbar clock or
         * graphs change the overlay generation. */
        for(uint32_t i=0;i<displayed->bands.count && i<BROOK_DESKTOP_OVERLAY_MAX_BANDS;++i){const struct BrookDesktopOverlayBand *b=&displayed->bands.bands[i];
            if(desktop_overlay_find_band(frame,b,displayed->keys[i],(displayed->opaque>>i)&1u)<0)
                desktop_damage_add(damage,(struct DesktopDamageRect){b->x,b->y,(int64_t)b->x+b->width,(int64_t)b->y+b->height},width,height);}
        for(uint32_t i=0;i<frame->bands.count && i<BROOK_DESKTOP_OVERLAY_MAX_BANDS;++i){const struct BrookDesktopOverlayBand *b=&frame->bands.bands[i];
            if(desktop_overlay_find_band(displayed,b,frame->keys[i],(frame->opaque>>i)&1u)<0)
                desktop_damage_add(damage,(struct DesktopDamageRect){b->x,b->y,(int64_t)b->x+b->width,(int64_t)b->y+b->height},width,height);}
    }else if(!sameOutput || !frame->generation || displayed->generation!=frame->generation ||
       !desktop_overlay_bands_equal(&displayed->bands,&frame->bands)){
        if(sameOutput)desktop_overlay_add_bands(damage,displayed,width,height);
        desktop_overlay_add_bands(damage,frame,width,height);
    }
    if(!sameOutput || !desktop_overlay_cursor_equal(&displayed->cursor,&frame->cursor)){
        if(sameOutput)desktop_overlay_add_cursor(damage,&displayed->cursor,width,height);
        desktop_overlay_add_cursor(damage,&frame->cursor,width,height);
    }
}

/* Scene changes beneath an opaque overlay band that is unchanged since the
 * displayed frame change no visible pixel: remove them from the damage, so a
 * game under an open launcher neither shades nor reblends the launcher area.
 * Pieces stay disjoint; if they would not fit, the damage is left as it was
 * (overdraw, never a missed change). Call on scene damage only, before the
 * overlay's own damage is added. */
static inline void desktop_overlay_clip_opaque(const struct DesktopOverlayFrameState *displayed,
                                               const struct DesktopOverlayFrameState *frame,
                                               struct DesktopDamageSet *damage){
    if(!displayed->valid || !frame->valid || displayed->width!=frame->width || displayed->height!=frame->height)return;
    for(uint32_t i=0;i<frame->bands.count && i<BROOK_DESKTOP_OVERLAY_MAX_BANDS;++i){
        const struct BrookDesktopOverlayBand *b=&frame->bands.bands[i];
        if(!((frame->opaque>>i)&1u) || desktop_overlay_find_band(displayed,b,frame->keys[i],1)<0)continue;
        const struct DesktopDamageRect cut={b->x,b->y,(int64_t)b->x+b->width,(int64_t)b->y+b->height};
        struct DesktopDamageSet out={0};int fits=1;
        for(uint32_t r=0;r<damage->count && fits;++r){const struct DesktopDamageRect d=damage->rects[r];
            const struct DesktopDamageRect o={d.left>cut.left?d.left:cut.left,d.top>cut.top?d.top:cut.top,
                                              d.right<cut.right?d.right:cut.right,d.bottom<cut.bottom?d.bottom:cut.bottom};
            struct DesktopDamageRect parts[4];unsigned n=0;
            if(o.left>=o.right || o.top>=o.bottom)parts[n++]=d;
            else{
                parts[n++]=(struct DesktopDamageRect){d.left,d.top,d.right,o.top};
                parts[n++]=(struct DesktopDamageRect){d.left,o.bottom,d.right,d.bottom};
                parts[n++]=(struct DesktopDamageRect){d.left,o.top,o.left,o.bottom};
                parts[n++]=(struct DesktopDamageRect){o.right,o.top,d.right,o.bottom};
            }
            for(unsigned k=0;k<n;++k){if(parts[k].left>=parts[k].right || parts[k].top>=parts[k].bottom)continue;
                if(out.count==DESKTOP_DAMAGE_MAX_RECTS){fits=0;break;}
                out.rects[out.count++]=parts[k];}
        }
        if(fits)*damage=out;
    }
}

/* Record a successful presentation. Async frames retire in ticket order;
 * an older ticket must not overwrite the state of a newer displayed one. */
static inline void desktop_overlay_presented(struct DesktopOverlayFrameState *displayed,uint64_t *displayedTicket,
                                             const struct DesktopOverlayFrameState *frame,uint64_t ticket){
    if(ticket && *displayedTicket && ticket<*displayedTicket)return;
    *displayed=*frame;if(ticket)*displayedTicket=ticket;
}
