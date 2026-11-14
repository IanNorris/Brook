#pragma once
/* Change-based desktop damage.
 *
 * desktop_damage.h's footprint estimate damages every window and its chrome
 * on every pass, so a frame whose only change is the clock or the pointer
 * still recomposites the whole game window. This header records what one
 * frame shows of every window (identity, stacking, geometry, the exact GPU
 * commit or a hash of the captured CPU pixels, and a hash of the chrome) and
 * compares it with the last *presented* frame. The result is the set of
 * output rectangles whose pixels can differ; overlay and pointer damage
 * still come from desktop_overlay_cache.h.
 *
 * The comparison is only meaningful against the frame that is on screen:
 * the caller commits a frame's state when its present succeeds and
 * invalidates the baseline when a present fails. Pixels are hashed in bands
 * of rows (64-bit, four interleaved lanes); a hash collision would leave a
 * stale band on screen until the next change, as with any hash-based check.
 * Geometry, stacking, opacity, source kind and size changes damage the old
 * and new footprints in full. */
#include "desktop_damage.h"
#include "desktop_layers_abi.h"
#include "scene.h"
#include <stdint.h>
#include <string.h>

enum { DESKTOP_TRACK_MAX_BANDS=256, DESKTOP_TRACK_MIN_BAND_ROWS=8 };
struct DesktopTrackBands { uint32_t rows,count; uint64_t hash[DESKTOP_TRACK_MAX_BANDS]; };
/* Interior rectangle (surface coordinates) excluded from a chrome hash
 * because the OS leaves it transparent; empty when width or height is 0. */
struct DesktopTrackHole { uint32_t x,y,width,height; };
struct DesktopTrackWindow {
    uint32_t window,direct,hasChrome,chromeOpacity;
    uint64_t incarnation,commit;
    /* Content layer words; the buffer offset and stride are not compared
     * for GPU sources, whose identity is the commit. */
    uint32_t layer[BROOK_VK_LAYER_WORDS];
    int32_t chromeX,chromeY;uint32_t chromeWidth,chromeHeight;
    struct DesktopTrackBands content,chrome;
};
struct DesktopTrackState {
    uint32_t valid,width,height,count;
    uint64_t display,backdrop;
    struct DesktopTrackWindow windows[BROOK_DESKTOP_LAYER_LIMIT];
};

static inline uint32_t desktop_track_band_rows(uint32_t height){
    uint32_t rows=(height+DESKTOP_TRACK_MAX_BANDS-1)/DESKTOP_TRACK_MAX_BANDS;
    return rows<DESKTOP_TRACK_MIN_BAND_ROWS?DESKTOP_TRACK_MIN_BAND_ROWS:rows;
}
/* Four independent multiply chains keep the hash near memory speed. */
struct DesktopTrackHash { uint64_t lane[4]; };
static inline void desktop_track_hash_words(struct DesktopTrackHash *h,const uint32_t *p,uint32_t n){
    const uint64_t k=0x9e3779b97f4a7c15ull;
    uint64_t a=h->lane[0],b=h->lane[1],c=h->lane[2],d=h->lane[3];uint32_t i=0;
    for(;i+4<=n;i+=4){a=(a^p[i])*k;b=(b^p[i+1])*k;c=(c^p[i+2])*k;d=(d^p[i+3])*k;}
    for(;i<n;++i)a=(a^p[i])*k;
    a^=n;h->lane[0]=a;h->lane[1]=b;h->lane[2]=c;h->lane[3]=d;
}
static inline uint64_t desktop_track_hash_final(const struct DesktopTrackHash *h){
    uint64_t x=h->lane[0]^(h->lane[1]<<1|h->lane[1]>>63)^(h->lane[2]<<2|h->lane[2]>>62)^(h->lane[3]<<3|h->lane[3]>>61);
    x^=x>>33;x*=0xff51afd7ed558ccdull;x^=x>>33;return x;
}
/* Hash rows [y,y+height) and columns [x,x+width) of a word-addressed
 * surface, excluding an optional hole, in bands of rows. */
static inline int desktop_track_bands(struct DesktopTrackBands *out,const uint32_t *pixels,uint64_t words,uint32_t stride,
                                      uint32_t x,uint32_t y,uint32_t width,uint32_t height,const struct DesktopTrackHole *hole){
    if(!out || !pixels || !width || !height || stride<width || x>stride-width ||
       ((uint64_t)y+height-1)*stride+x+width>words)return 0;
    const uint32_t rows=desktop_track_band_rows(height),count=(height+rows-1)/rows;
    if(count>DESKTOP_TRACK_MAX_BANDS)return 0;
    const int holed=hole && hole->width && hole->height;
    out->rows=rows;out->count=count;
    for(uint32_t band=0;band<count;++band){
        struct DesktopTrackHash h={{band+1u,0x5bd1e995u,0x1b873593u,0xcc9e2d51u}};
        const uint32_t first=band*rows,last=first+rows<height?first+rows:height;
        for(uint32_t r=first;r<last;++r){
            const uint32_t *row=pixels+(uint64_t)(y+r)*stride+x;
            if(holed && r>=hole->y && r-hole->y<hole->height){
                const uint32_t left=hole->x<width?hole->x:width;
                const uint32_t right=hole->width<width-left?left+hole->width:width;
                desktop_track_hash_words(&h,row,left);desktop_track_hash_words(&h,row+right,width-right);
            }else desktop_track_hash_words(&h,row,width);
        }
        out->hash[band]=desktop_track_hash_final(&h);
    }
    return 1;
}
static inline struct DesktopDamageRect desktop_track_layer_bounds(const uint32_t *l){
    int64_t left=(int32_t)l[8],top=(int32_t)l[9],right=left+l[10],bottom=top+l[11];
    const int64_t cl=(int32_t)l[12],ct=(int32_t)l[13],cr=cl+l[14],cb=ct+l[15];
    if(left<cl)left=cl;
    if(top<ct)top=ct;
    if(right>cr)right=cr;
    if(bottom>cb)bottom=cb;
    return (struct DesktopDamageRect){left,top,right,bottom};
}
static inline struct DesktopDamageRect desktop_track_intersect(struct DesktopDamageRect a,struct DesktopDamageRect b){
    if(b.left>a.left)a.left=b.left;
    if(b.top>a.top)a.top=b.top;
    if(b.right<a.right)a.right=b.right;
    if(b.bottom<a.bottom)a.bottom=b.bottom;
    return a;
}
static inline void desktop_track_footprint(const struct DesktopTrackWindow *w,struct DesktopDamageSet *d,uint32_t width,uint32_t height){
    desktop_damage_add(d,desktop_track_layer_bounds(w->layer),width,height);
    if(w->hasChrome)desktop_damage_add(d,(struct DesktopDamageRect){w->chromeX,w->chromeY,
        (int64_t)w->chromeX+w->chromeWidth,(int64_t)w->chromeY+w->chromeHeight},width,height);
}
/* Output rows that may sample source rows [r0,r1) of the layer (relative
 * to its crop), widened by a row for the nearest-sampling rounding. */
static inline struct DesktopDamageRect desktop_track_content_rows(const uint32_t *l,uint32_t r0,uint32_t r1){
    const int64_t dy=(int32_t)l[9],sh=l[7],dh=l[11];
    int64_t top,bottom;
    if(sh==dh){top=dy+r0;bottom=dy+r1;}
    else {top=dy+(int64_t)r0*dh/sh-1;bottom=dy+((int64_t)r1*dh+sh-1)/sh+1;}
    const struct DesktopDamageRect rows={(int32_t)l[8],top,(int64_t)(int32_t)l[8]+l[10],bottom};
    return desktop_track_intersect(rows,desktop_track_layer_bounds(l));
}
static inline void desktop_track_band_damage(const struct DesktopTrackBands *now,const struct DesktopTrackBands *was,
        int content,const struct DesktopTrackWindow *w,uint32_t surfaceHeight,struct DesktopDamageSet *d,uint32_t width,uint32_t height){
    for(uint32_t band=0;band<now->count;){
        if(now->hash[band]==was->hash[band]){++band;continue;}
        uint32_t end=band+1;while(end<now->count && now->hash[end]!=was->hash[end])++end;
        const uint32_t r0=band*now->rows,r1=end*now->rows<surfaceHeight?end*now->rows:surfaceHeight;
        if(content)desktop_damage_add(d,desktop_track_content_rows(w->layer,r0,r1),width,height);
        else desktop_damage_add(d,(struct DesktopDamageRect){w->chromeX,(int64_t)w->chromeY+r0,
            (int64_t)w->chromeX+w->chromeWidth,(int64_t)w->chromeY+r1},width,height);
        band=end;
    }
}
static inline int desktop_track_same_geometry(const struct DesktopTrackWindow *a,const struct DesktopTrackWindow *b){
    if(a->direct!=b->direct || a->hasChrome!=b->hasChrome)return 0;
    /* GPU sources change buffer offset/stride with the commit; size and
     * crop/destination/clip/flags must match. */
    const unsigned first=a->direct?2:0;
    if(memcmp(a->layer+first,b->layer+first,(BROOK_VK_LAYER_WORDS-first)*sizeof(uint32_t)))return 0;
    if(a->hasChrome && (a->chromeX!=b->chromeX || a->chromeY!=b->chromeY || a->chromeWidth!=b->chromeWidth ||
       a->chromeHeight!=b->chromeHeight || a->chromeOpacity!=b->chromeOpacity))return 0;
    return 1;
}
/* Adds to damage every output rectangle that can differ between the
 * displayed frame and this one. Returns 0 when no valid baseline applies;
 * the caller must then redraw everything (or fall back to the footprint). */
static inline int desktop_track_diff(const struct DesktopTrackState *displayed,const struct DesktopTrackState *frame,
                                     struct DesktopDamageSet *damage){
    if(!displayed || !frame || !damage || !displayed->valid || !frame->valid || !frame->display || !frame->backdrop ||
       displayed->display!=frame->display || displayed->backdrop!=frame->backdrop ||
       displayed->width!=frame->width || displayed->height!=frame->height ||
       displayed->count>BROOK_DESKTOP_LAYER_LIMIT || frame->count>BROOK_DESKTOP_LAYER_LIMIT)return 0;
    const uint32_t width=frame->width,height=frame->height;
    int restack=displayed->count!=frame->count;
    for(uint32_t i=0;!restack && i<frame->count;++i)
        restack=displayed->windows[i].window!=frame->windows[i].window ||
                displayed->windows[i].incarnation!=frame->windows[i].incarnation;
    if(restack){
        for(uint32_t i=0;i<displayed->count;++i)desktop_track_footprint(&displayed->windows[i],damage,width,height);
        for(uint32_t i=0;i<frame->count;++i)desktop_track_footprint(&frame->windows[i],damage,width,height);
        return 1;
    }
    for(uint32_t i=0;i<frame->count;++i){
        const struct DesktopTrackWindow *now=&frame->windows[i],*was=&displayed->windows[i];
        if(!desktop_track_same_geometry(now,was)){
            desktop_track_footprint(was,damage,width,height);desktop_track_footprint(now,damage,width,height);continue;
        }
        if(now->direct){
            if(now->commit!=was->commit)desktop_damage_add(damage,desktop_track_layer_bounds(now->layer),width,height);
        }else if(now->content.count!=was->content.count || now->content.rows!=was->content.rows)
            desktop_damage_add(damage,desktop_track_layer_bounds(now->layer),width,height);
        else desktop_track_band_damage(&now->content,&was->content,1,now,now->layer[7],damage,width,height);
        if(now->hasChrome){
            if(now->chrome.count!=was->chrome.count || now->chrome.rows!=was->chrome.rows)
                desktop_damage_add(damage,(struct DesktopDamageRect){now->chromeX,now->chromeY,
                    (int64_t)now->chromeX+now->chromeWidth,(int64_t)now->chromeY+now->chromeHeight},width,height);
            else desktop_track_band_damage(&now->chrome,&was->chrome,0,now,now->chromeHeight,damage,width,height);
        }
    }
    return 1;
}
/* Chrome interior that the OS leaves transparent (see
 * WmDesktopChromeCapture): the client rectangle relative to the chrome
 * surface, clipped like desktop_chrome_spans(). Empty when unknown. */
static inline struct DesktopTrackHole desktop_track_chrome_hole(const struct BrookDesktopLayer *client,
                                                                const struct BrookDesktopLayer *chrome){
    struct DesktopTrackHole hole={0,0,0,0};
    if(!client || !chrome || !chrome->width || !chrome->height || !client->clientWidth || !client->clientHeight ||
       client->window!=chrome->window || client->incarnation!=chrome->incarnation)return hole;
    const int64_t x=(int64_t)client->clientX-chrome->clientX,y=(int64_t)client->clientY-chrome->clientY;
#define DESKTOP_TRACK_CLIP(v,limit) ((v)<=0?0u:(v)>=(int64_t)(limit)?(uint32_t)(limit):(uint32_t)(v))
    const uint32_t l=DESKTOP_TRACK_CLIP(x,chrome->width),r=DESKTOP_TRACK_CLIP(x+client->clientWidth,chrome->width);
    const uint32_t t=DESKTOP_TRACK_CLIP(y,chrome->height),b=DESKTOP_TRACK_CLIP(y+client->clientHeight,chrome->height);
#undef DESKTOP_TRACK_CLIP
    if(r>l && b>t)hole=(struct DesktopTrackHole){l,t,r-l,b-t};
    return hole;
}
