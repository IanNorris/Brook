#pragma once
#include "desktop_layers_abi.h"
#include "scene.h"

static inline int desktop_layer_opacity(const struct BrookDesktopLayer *l,uint32_t *out){
    if(!l || !out || (l->flags&~(BROOK_DESKTOP_LAYER_LEGACY_OPAQUE|BROOK_DESKTOP_LAYER_HAS_OPACITY|BROOK_DESKTOP_LAYER_OPACITY_MASK)))return 0;
    if(!(l->flags&BROOK_DESKTOP_LAYER_HAS_OPACITY) && (l->flags&BROOK_DESKTOP_LAYER_OPACITY_MASK))return 0;
    *out=(l->flags&BROOK_DESKTOP_LAYER_HAS_OPACITY)?(l->flags&BROOK_DESKTOP_LAYER_OPACITY_MASK)>>BROOK_DESKTOP_LAYER_OPACITY_SHIFT:255u;
    return 1;
}

/* Match the software path: native VFBs are copied 1:1; legacy process VFBs
 * are integer-scaled, then clipped to the window's client rectangle. */
static inline int desktop_content_layer(const struct BrookDesktopLayer *l,uint32_t screenWidth,uint32_t screenHeight,
                                        struct BrookVkSceneLayer *out){
    uint32_t opacity=0;
    if(!l || !out || !l->width || !l->height || !l->clientWidth || !l->clientHeight || !l->upscale ||
       !desktop_layer_opacity(l,&opacity))return 0;
    if(l->flags&BROOK_DESKTOP_LAYER_LEGACY_OPAQUE){
        uint64_t dw=(uint64_t)l->width*l->upscale,dh=(uint64_t)l->height*l->upscale;
        if(dw>BROOK_VK_MAX_DIMENSION || dh>BROOK_VK_MAX_DIMENSION ||
           l->clientWidth>BROOK_VK_MAX_DIMENSION || l->clientHeight>BROOK_VK_MAX_DIMENSION)return 0;
        *out=(struct BrookVkSceneLayer){0,l->stride,l->width,l->height,0,0,l->width,l->height,
            l->clientX,l->clientY,(uint32_t)dw,(uint32_t)dh,l->clientX,l->clientY,l->clientWidth,l->clientHeight,
            opacity|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE};
    }else{
        uint32_t w=l->width<l->clientWidth?l->width:l->clientWidth,h=l->height<l->clientHeight?l->height:l->clientHeight;
        if(!w || !h)return 0;
        *out=(struct BrookVkSceneLayer){0,l->stride,l->width,l->height,0,0,w,h,
            l->clientX,l->clientY,w,h,0,0,screenWidth,screenHeight,opacity|BROOK_VK_STRAIGHT_ALPHA};
    }
    return 1;
}
