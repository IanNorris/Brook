#pragma once
/* Per-context record of the window chrome pixels a renderer buffer holds.
 * A keyed capture returns a content key for exactly the pixels it wrote; on
 * later frames the renderer asks the kernel to KEEP them (no render, no
 * copy) as long as the same window incarnation still maps to the same
 * buffer allocation. Any change, refusal or reallocation means recapture. */
#include <stdint.h>
#include <string.h>
#include "desktop_layers_abi.h"

struct DesktopChromeHeld {
    uint64_t key;          /* 0 = nothing held */
    uint64_t serial;       /* allocation serial of the buffer holding the pixels */
    uint64_t incarnation;
    uint32_t window;
    struct BrookDesktopLayer meta; /* metadata the keyed capture returned */
};

static inline int desktop_chrome_can_keep(const struct DesktopChromeHeld *held,uint32_t window,
                                          uint64_t incarnation,uint64_t serial){
    return held->key && serial && held->serial==serial && held->window==window &&
           held->incarnation==incarnation && held->meta.bytes;
}
/* Request for TRACKED_KEEP: the held metadata, no pixels, key in reserved. */
static inline void desktop_chrome_keep_request(const struct DesktopChromeHeld *held,struct BrookDesktopLayers *wire){
    memset(wire,0,sizeof(*wire));wire->version=BROOK_DESKTOP_LAYERS_VERSION;wire->count=1;
    wire->reserved=held->key;wire->layers[0]=held->meta;
    wire->layers[0].pixels=0;wire->layers[0].capacity=0;
}
static inline void desktop_chrome_hold(struct DesktopChromeHeld *held,const struct BrookDesktopLayers *wire,uint64_t serial){
    memset(held,0,sizeof(*held));
    if(!wire->reserved || !wire->layers[0].bytes || !serial)return;
    held->key=wire->reserved;held->serial=serial;held->incarnation=wire->layers[0].incarnation;
    held->window=wire->layers[0].window;held->meta=wire->layers[0];
}
static inline void desktop_chrome_forget(struct DesktopChromeHeld *held){memset(held,0,sizeof(*held));}
