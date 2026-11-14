#pragma once
/* Per-context record of the client-window pixels a renderer buffer holds.
 *
 * A CPU (shm/vfb) window's pixels change only when its producer publishes,
 * which bumps the layer's content generation. Once a capture has filled a
 * context buffer, later frames ask the kernel to KEEP it
 * (BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_KEEP): if the window still has the
 * same incarnation, generation and geometry the kernel copies nothing and
 * flags the layer KEPT, so the renderer also reuses the change-tracking band
 * hashes it computed for those pixels instead of re-reading the buffer.
 *
 * The record is trusted only while the same buffer allocation (serial)
 * holds the pixels. Any failed capture call forgets every record of the
 * context, because a failed copy may have partly overwritten a buffer. */
#include <stdint.h>
#include <string.h>
#include "desktop_layers_abi.h"
#include "desktop_track.h"

struct DesktopClientHeld {
    uint64_t serial;       /* 0 = nothing held */
    uint64_t incarnation,generation,bytes;
    uint32_t window,width,height,stride;
    /* Band hashes of the held pixels for the region the frame tracked. */
    int bandsValid;
    uint32_t bandStride,bandX,bandY,bandWidth,bandHeight;
    struct DesktopTrackBands bands;
};

static inline int desktop_client_can_keep(const struct DesktopClientHeld *h,const struct BrookDesktopLayer *query,uint64_t serial){
    return h->serial && serial && h->serial==serial && h->window==query->window &&
           h->incarnation==query->incarnation && h->bytes && h->bytes==query->bytes;
}
/* Keep request: describe the held pixels (the kernel compares them with the
 * current publication); the buffer itself is still supplied by the caller. */
static inline void desktop_client_keep_request(const struct DesktopClientHeld *h,struct BrookDesktopLayer *wire){
    wire->generation=h->generation;wire->bytes=h->bytes;
    wire->width=h->width;wire->height=h->height;wire->stride=h->stride;
}
static inline void desktop_client_hold(struct DesktopClientHeld *h,const struct BrookDesktopLayer *captured,uint64_t serial){
    memset(h,0,sizeof(*h));
    if(!serial || !captured->bytes || captured->generation==~0ull)return;
    h->serial=serial;h->incarnation=captured->incarnation;h->generation=captured->generation;h->bytes=captured->bytes;
    h->window=captured->window;h->width=captured->width;h->height=captured->height;h->stride=captured->stride;
}
static inline void desktop_client_forget(struct DesktopClientHeld *h){memset(h,0,sizeof(*h));}
static inline int desktop_client_bands_match(const struct DesktopClientHeld *h,uint32_t stride,uint32_t x,uint32_t y,uint32_t w,uint32_t hgt){
    return h->bandsValid && h->bandStride==stride && h->bandX==x && h->bandY==y && h->bandWidth==w && h->bandHeight==hgt;
}
static inline void desktop_client_store_bands(struct DesktopClientHeld *h,const struct DesktopTrackBands *b,
                                              uint32_t stride,uint32_t x,uint32_t y,uint32_t w,uint32_t hgt){
    if(!h->serial)return;
    h->bands=*b;h->bandStride=stride;h->bandX=x;h->bandY=y;h->bandWidth=w;h->bandHeight=hgt;h->bandsValid=1;
}
