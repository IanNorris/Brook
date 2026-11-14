#pragma once
/* Two-frame pipelining policy (BROOK_RENDER_PIPELINE=2).
 *
 * Serial admission prepares a frame only once the previous one has been
 * presented. Pipelined admission prepares and submits frame B while frame A
 * is still composing or being blitted, with at most two frames in flight:
 *
 *  - B's change damage is computed against A (the latest submitted frame),
 *    not against the frame on screen, so B never redraws what A already
 *    covers unless the kernel requires it.
 *  - B carries A's present regions. Whichever of "A accepted before B was
 *    reserved", "A accepted after", or "A failed" happens, B then covers
 *    diff(screen, B): diff(S, B) is within diff(S, A) | diff(A, B). The
 *    kernel's dependent reservation (RESERVE_DEPENDENT on the last accepted
 *    ticket) checks that B covers A's accepted writes.
 *  - B is never turned into a full-screen frame because A is in flight. If
 *    A is itself full (no partial baseline) B waits for A instead; carrying
 *    a full frame would only produce a second full-screen frame.
 *  - Frames retire strictly in ticket order, so the older frame is never
 *    discarded as stale behind its successor.
 *
 * Enabling: BROOK_RENDER_PIPELINE=2 (or the supervisor's --pipeline, which
 * sets it unless already set) with the buffered partial renderer
 * (--async-partial). It turns on queued presents unless
 * BROOK_RENDER_ASYNC_PRESENT=0 keeps them inline. Serial admission stays
 * the default; the boot supervisor passes the variable through, so a live
 * A/B only restarts it:
 *   /boot/VKSUP.ELF --async-partial --pipeline --gpu-source /tmp/brook-desktop-gpu.sock /boot/VKRENDER.ELF
 * The stats JSON reports "pipeline" (depth, in flight, submissions by
 * frames in flight, carried/rebased) and "full_forced".
 *
 * Everything here is pure policy so it can be tested without a GPU. */
#include "desktop_damage.h"
#include "gpu_output_abi.h"
#include <stdint.h>

enum { DESKTOP_PIPELINE_SERIAL=1, DESKTOP_PIPELINE_MAX=2 };

/* BROOK_RENDER_PIPELINE: unset or "1" serial, "2" two frames in flight.
 * Returns 0 for an invalid value. */
static inline unsigned desktop_pipeline_parse(const char *value){
    if(!value || !*value || (value[0]=='1' && !value[1]))return DESKTOP_PIPELINE_SERIAL;
    if(value[0]=='2' && !value[1])return DESKTOP_PIPELINE_MAX;
    return 0;
}

/* What admission may do with one free slot. */
enum DesktopPipelineAdmit {
    DESKTOP_PIPELINE_START,        /* nothing in flight: compare with the screen */
    DESKTOP_PIPELINE_OVERLAP,      /* one frame in flight: compare with it and carry its regions */
    DESKTOP_PIPELINE_WAIT_FULL,    /* the in-flight frame is full-screen (or has no regions) */
    DESKTOP_PIPELINE_WAIT_BASELINE,/* no accepted ticket yet, or the in-flight frame's state is unknown */
    DESKTOP_PIPELINE_WAIT_DEPTH,   /* both slots in flight */
};
struct DesktopPipelineInFlight {
    long ticket;              /* 0: slot free */
    int partial;              /* presents regions (not a full frame) */
    uint32_t regions;         /* region count */
    int trackValid;           /* its change-tracking state is usable as a baseline */
};
static inline enum DesktopPipelineAdmit desktop_pipeline_admit(unsigned depth,const struct DesktopPipelineInFlight *frames,unsigned count,
                                                               uint64_t lastAccepted){
    unsigned active=0;const struct DesktopPipelineInFlight *inflight=0;
    for(unsigned i=0;i<count;++i)if(frames[i].ticket){++active;inflight=&frames[i];}
    if(!active)return DESKTOP_PIPELINE_START;
    if(depth<DESKTOP_PIPELINE_MAX || active>=DESKTOP_PIPELINE_MAX)return DESKTOP_PIPELINE_WAIT_DEPTH;
    if(!inflight->partial || !inflight->regions || inflight->regions>BROOK_GPU_PRESENT_MAX_REGIONS)return DESKTOP_PIPELINE_WAIT_FULL;
    if(!lastAccepted || !inflight->trackValid)return DESKTOP_PIPELINE_WAIT_BASELINE;
    return DESKTOP_PIPELINE_OVERLAP;
}

/* Add the in-flight frame's present regions to the successor's damage.
 * Returns 0 (damage unchanged) when the regions are malformed. */
static inline int desktop_pipeline_carry(struct DesktopDamageSet *damage,const struct BrookGpuPresentRegions *carry,
                                         uint32_t width,uint32_t height){
    if(!damage || !carry || !carry->count || carry->count>BROOK_GPU_PRESENT_MAX_REGIONS)return 0;
    for(uint32_t i=0;i<carry->count;++i){const struct BrookGpuPresentRegion *r=&carry->regions[i];
        if(!r->width || !r->height)return 0;}
    for(uint32_t i=0;i<carry->count;++i){const struct BrookGpuPresentRegion *r=&carry->regions[i];
        desktop_damage_add(damage,(struct DesktopDamageRect){r->x,r->y,(int64_t)r->x+r->width,(int64_t)r->y+r->height},width,height);}
    return 1;
}

/* Index of the oldest in-flight ticket (the only one allowed to present
 * next), or count when none is in flight. */
static inline unsigned desktop_pipeline_oldest(const long *tickets,unsigned count){
    unsigned best=count;
    for(unsigned i=0;i<count;++i)if(tickets[i]>0 && (best==count || tickets[i]<tickets[best]))best=i;
    return best;
}

/* Why a submitted asynchronous frame is full-screen. A frame whose real
 * damage covers the whole output is not forced. */
enum DesktopPipelineFull {
    DESKTOP_FULL_NOT_FORCED=0,
    DESKTOP_FULL_NO_BASELINE,  /* first frame, display/backdrop change, or reset after a failure */
    DESKTOP_FULL_IN_FLIGHT,    /* partial refused only because another frame was in flight */
    DESKTOP_FULL_OTHER,        /* damage list overflow, alignment, or a rejected partial plan */
};
static inline enum DesktopPipelineFull desktop_pipeline_full_reason(int full,int baseline,int partialAllowed,
                                                                    uint64_t damagePixels,uint64_t screenPixels){
    if(!full)return DESKTOP_FULL_NOT_FORCED;
    if(!baseline)return DESKTOP_FULL_NO_BASELINE;
    if(!partialAllowed)return DESKTOP_FULL_IN_FLIGHT;
    if(damagePixels>=screenPixels)return DESKTOP_FULL_NOT_FORCED;
    return DESKTOP_FULL_OTHER;
}
