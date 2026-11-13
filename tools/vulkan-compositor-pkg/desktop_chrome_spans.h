#pragma once
#include "desktop_layers_abi.h"
#include <stdint.h>
#include <string.h>

/* Partition chrome around its transparent client interior. Coordinates are
 * relative to the captured chrome surface. The caller must only use matching
 * tracked client/chrome captures and preserve the original chrome layer order. */
struct DesktopChromeSpan { uint32_t x,y,width,height; };
static inline uint32_t desktop_chrome_clip(int64_t n,uint32_t limit){
    return n<=0?0:n>=(int64_t)limit?limit:(uint32_t)n;
}
static inline unsigned desktop_chrome_spans(const struct BrookDesktopLayer *client,
                                             const struct BrookDesktopLayer *chrome,
                                             struct DesktopChromeSpan out[4]){
    if(!client || !chrome || !out || !chrome->width || !chrome->height ||
       !client->clientWidth || !client->clientHeight || client->window!=chrome->window ||
       client->incarnation!=chrome->incarnation)return 0;
    const int64_t x=(int64_t)client->clientX-chrome->clientX,y=(int64_t)client->clientY-chrome->clientY;
    const uint32_t l=desktop_chrome_clip(x,chrome->width),r=desktop_chrome_clip(x+client->clientWidth,chrome->width);
    const uint32_t t=desktop_chrome_clip(y,chrome->height),b=desktop_chrome_clip(y+client->clientHeight,chrome->height);
    unsigned n=0;
    if(t)out[n++]=(struct DesktopChromeSpan){0,0,chrome->width,t};
    if(b<chrome->height)out[n++]=(struct DesktopChromeSpan){0,b,chrome->width,chrome->height-b};
    if(b>t && l)out[n++]=(struct DesktopChromeSpan){0,t,l,b-t};
    if(b>t && r<chrome->width)out[n++]=(struct DesktopChromeSpan){r,t,chrome->width-r,b-t};
    return n;
}
static inline int desktop_chrome_copy_span(const uint32_t *pixels,uint32_t stride,uint32_t width,uint32_t height,
                                            const struct DesktopChromeSpan *s,uint32_t *out,uint64_t outWords){
    if(!pixels || !out || !s || stride<width || !s->width || !s->height || s->x>=width || s->y>=height ||
       s->width>width-s->x || s->height>height-s->y || (uint64_t)s->width*s->height>outWords)return 0;
    for(uint32_t row=0;row<s->height;++row)
        memcpy(out+(uint64_t)row*s->width,pixels+(uint64_t)(s->y+row)*stride+s->x,(size_t)s->width*4);
    return 1;
}
