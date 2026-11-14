#pragma once
#include <stdint.h>
#include "desktop_damage.h"

/* Buffer-age repair for zero-copy scanout. The renderer draws into a ring of
 * N scanout images instead of one retained output, so an image it writes may
 * be several frames old. Before drawing frame n into image i, it must also
 * redraw every pixel changed by the frames since i last held a frame.
 *
 * Frames are committed in submission order (the kernel presents dependent
 * tickets in that order); the history keeps each committed frame's own
 * damage. Any failed or unknown frame must reset the whole state, which turns
 * the next use of every image into a full draw. */
enum { DESKTOP_AGE_HISTORY=8, DESKTOP_AGE_MAX_IMAGES=4 };
struct DesktopBufferAge {
    uint64_t frame;                                   /* frames committed so far */
    uint64_t imageFrame[DESKTOP_AGE_MAX_IMAGES];      /* frame the image holds, 0 = unknown */
    struct DesktopDamageSet history[DESKTOP_AGE_HISTORY]; /* own damage of frame f at f % HISTORY */
    uint64_t display,backdrop; uint32_t width,height; /* the output the history describes */
};
static inline void desktop_buffer_age_reset(struct DesktopBufferAge *a){
    const uint64_t frame=a->frame;*a=(struct DesktopBufferAge){0};a->frame=frame;
}
/* Same output identity or a reset: a different display/backdrop/size cannot
 * reuse old image contents. */
static inline void desktop_buffer_age_bind(struct DesktopBufferAge *a,uint64_t display,uint64_t backdrop,uint32_t width,uint32_t height){
    if(a->display!=display || a->backdrop!=backdrop || a->width!=width || a->height!=height){
        desktop_buffer_age_reset(a);a->display=display;a->backdrop=backdrop;a->width=width;a->height=height;
    }
}
/* Frames since the image last held one (1 = it holds the newest frame);
 * 0 when unknown. */
static inline uint64_t desktop_buffer_age_of(const struct DesktopBufferAge *a,unsigned image){
    if(image>=DESKTOP_AGE_MAX_IMAGES || !a->imageFrame[image] || a->imageFrame[image]>a->frame)return 0;
    return a->frame-a->imageFrame[image]+1;
}
/* Damage to draw into image for the next frame, whose own damage is own.
 * Returns 1 for a partial draw, 0 when out is the full output. */
static inline int desktop_buffer_age_damage(const struct DesktopBufferAge *a,unsigned image,
                                            const struct DesktopDamageSet *own,struct DesktopDamageSet *out){
    const uint32_t width=a->width,height=a->height;
    const uint64_t age=desktop_buffer_age_of(a,image);
    *out=(struct DesktopDamageSet){1,{{0,0,width,height}}};
    if(!width || !height || !own || !own->count || !age || age>DESKTOP_AGE_HISTORY)return 0;
    struct DesktopDamageSet u={0};
    for(uint32_t i=0;i<own->count;++i)desktop_damage_add(&u,own->rects[i],width,height);
    for(uint64_t f=a->imageFrame[image]+1;f<=a->frame;++f){
        const struct DesktopDamageSet *h=&a->history[f%DESKTOP_AGE_HISTORY];
        for(uint32_t i=0;i<h->count;++i)desktop_damage_add(&u,h->rects[i],width,height);
    }
    uint64_t pixels=0;
    for(uint32_t i=0;i<u.count;++i)pixels+=(uint64_t)(u.rects[i].right-u.rects[i].left)*(uint64_t)(u.rects[i].bottom-u.rects[i].top);
    if(!u.count || pixels>=(uint64_t)width*height)return 0;
    *out=u;return 1;
}
/* The frame was submitted into image with its own damage (full = whole
 * output). Call in submission order. */
static inline void desktop_buffer_age_commit(struct DesktopBufferAge *a,unsigned image,const struct DesktopDamageSet *own,int full){
    if(image>=DESKTOP_AGE_MAX_IMAGES)return;
    ++a->frame;
    struct DesktopDamageSet *h=&a->history[a->frame%DESKTOP_AGE_HISTORY];
    if(full || !own)*h=(struct DesktopDamageSet){1,{{0,0,a->width,a->height}}};
    else *h=*own;
    a->imageFrame[image]=a->frame;
}
/* Pick the image to write: the free one (bit i of freeMask) that is
 * youngest, so the repair is smallest. Returns -1 when none is free. */
static inline int desktop_buffer_age_pick(const struct DesktopBufferAge *a,uint32_t freeMask,unsigned images){
    int best=-1;uint64_t bestAge=0;
    for(unsigned i=0;i<images && i<DESKTOP_AGE_MAX_IMAGES;++i){
        if(!(freeMask&(1u<<i)))continue;
        const uint64_t age=desktop_buffer_age_of(a,i);
        if(best<0 || (age && (!bestAge || age<bestAge))){best=(int)i;bestAge=age;}
    }
    return best;
}

/* The process-wide ring. Every render context (one per pipelined frame)
 * draws into the same images and shares one age history: frames are
 * committed in submission order whichever context prepared them. A frame
 * keeps its image in flight from submit until it retires (presented or
 * refused), so a second pipelined frame never writes the same image even
 * while the kernel still reports it free (zero-copy not armed). */
struct DesktopScanoutRing {
    unsigned count;
    struct DesktopBufferAge age;
    unsigned inflight[DESKTOP_AGE_MAX_IMAGES];
};
static inline uint32_t desktop_scanout_ring_busy(const struct DesktopScanoutRing *r){
    uint32_t mask=0;for(unsigned i=0;i<r->count && i<DESKTOP_AGE_MAX_IMAGES;++i)if(r->inflight[i])mask|=1u<<i;
    return mask;
}
static inline int desktop_scanout_ring_pick(const struct DesktopScanoutRing *r,uint32_t kernelFree){
    return desktop_buffer_age_pick(&r->age,kernelFree&~desktop_scanout_ring_busy(r),r->count);
}
static inline void desktop_scanout_ring_begin(struct DesktopScanoutRing *r,unsigned image){if(image<r->count)++r->inflight[image];}
static inline void desktop_scanout_ring_end(struct DesktopScanoutRing *r,unsigned image){if(image<r->count && r->inflight[image])--r->inflight[image];}
