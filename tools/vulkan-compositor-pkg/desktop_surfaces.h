#pragma once
/* Surface records (layers ABI v2, compositor refactor stage 1.3) in the
 * renderer: the kernel resolves each chrome and shell surface's material,
 * opacity, radii and shadow class against this backend, and the renderer
 * draws what a record asks for and nothing else.
 *
 *  - Shadow: the 64 px nine-slice of brook_chrome_shadow.h, eight straight-
 *    alpha quads beneath the window at the record's shadow alpha, sampled
 *    nearest like every scaled layer (virgl filters linearly; the quads and
 *    the source pixels are the same).
 *  - Opaque frames: a record with BROOK_DESKTOP_SURFACE_OPAQUE has alpha 255
 *    in every frame pixel, so at opacity 255 its spans may be marked
 *    BROOK_VK_OPAQUE_SOURCE and occlude what lies beneath in the quad plan.
 *
 * Records come from layers op 16 (frames) and overlay op 10 (shell
 * surfaces). Older kernels refuse both with EINVAL; then there are no
 * records, no shadows and nothing is marked opaque, exactly as before. A
 * record that fails validation is ignored the same way. */
#include "desktop_layers_abi.h"
#include "brook_chrome_shadow.h"
#include "desktop_damage.h"
#include "scene.h"
#include <stdint.h>
#include <string.h>

/* Shadows on the top four windows, as virgl (ChromeShadowWindowLimit). */
enum { DESKTOP_SHADOW_PIECES = 8, DESKTOP_SHADOW_WINDOWS = 4, DESKTOP_SHADOW_MAX_EXTENT = 1024 };

struct DesktopSurfaceSet {
    uint32_t count;
    struct BrookDesktopSurface surfaces[BROOK_DESKTOP_LAYER_LIMIT];
};

static inline int desktop_surface_valid(const struct BrookDesktopSurface *s){
    if(!s || s->kind<BROOK_DESKTOP_SURFACE_FRAME || s->kind>BROOK_DESKTOP_SURFACE_BOOT_SUMMARY)return 0;
    if(!s->width || !s->height || s->width>BROOK_VK_MAX_DIMENSION || s->height>BROOK_VK_MAX_DIMENSION)return 0;
    if(s->x<-BROOK_VK_MAX_DIMENSION || s->x>BROOK_VK_MAX_DIMENSION || s->y<-BROOK_VK_MAX_DIMENSION || s->y>BROOK_VK_MAX_DIMENSION)return 0;
    if(s->opacity>255 || s->material>BROOK_DESKTOP_MATERIAL_GLASS || s->shadow>BROOK_DESKTOP_SHADOW_WINDOW)return 0;
    if(s->shadow && (s->shadowAlpha>255 || s->shadowSpread>DESKTOP_SHADOW_MAX_EXTENT ||
                     !s->shadowCorner || s->shadowCorner>DESKTOP_SHADOW_MAX_EXTENT))return 0;
    return 1;
}

/* Keep the valid records of a reply (count clamped to the array). */
static inline void desktop_surface_set_from(struct DesktopSurfaceSet *set,const struct BrookDesktopSurfaces *reply){
    set->count=0;
    if(!reply || reply->version!=BROOK_DESKTOP_SURFACES_VERSION)return;
    const uint32_t n=reply->count<BROOK_DESKTOP_LAYER_LIMIT?reply->count:BROOK_DESKTOP_LAYER_LIMIT;
    for(uint32_t i=0;i<n;++i)if(desktop_surface_valid(&reply->surfaces[i]))set->surfaces[set->count++]=reply->surfaces[i];
}

/* The record of a window layer, matched by identity (never by order). */
static inline const struct BrookDesktopSurface *desktop_surface_find(const struct DesktopSurfaceSet *set,
                                                                     uint32_t window,uint64_t incarnation){
    if(!set || !window)return NULL;
    for(uint32_t i=0;i<set->count;++i){const struct BrookDesktopSurface *s=&set->surfaces[i];
        if(s->window==window && s->incarnation==incarnation)return s;}
    return NULL;
}

/* The shared 64x64 shadow source, BGRA8 straight alpha (as kernel ARGB). */
static inline void desktop_shadow_source(uint32_t *pixels){
    for(unsigned y=0;y<BROOK_CHROME_SHADOW_SIZE;++y)for(unsigned x=0;x<BROOK_CHROME_SHADOW_SIZE;++x)
        pixels[y*BROOK_CHROME_SHADOW_SIZE+x]=brook_chrome_shadow_pixel(x,y);
}

static inline int desktop_surface_has_shadow(const struct BrookDesktopSurface *s){
    return s && s->shadow==BROOK_DESKTOP_SHADOW_WINDOW && s->shadowAlpha && desktop_surface_valid(s);
}

/* Output rectangle the shadow can touch (the outer rectangle grown by the
 * spread); 0 without a shadow. */
static inline int desktop_surface_shadow_bounds(const struct BrookDesktopSurface *s,struct DesktopDamageRect *out){
    if(!desktop_surface_has_shadow(s))return 0;
    *out=(struct DesktopDamageRect){(int64_t)s->x-s->shadowSpread,(int64_t)s->y-s->shadowSpread,
        (int64_t)s->x+s->width+s->shadowSpread,(int64_t)s->y+s->height+s->shadowSpread};
    return 1;
}

/* Up to eight scene layers drawing the record's shadow from a 64x64 source
 * (stride 64 words, offset 0), clipped to the output. Returns the count. */
static inline unsigned desktop_surface_shadow_layers(const struct BrookDesktopSurface *s,uint32_t outputWidth,uint32_t outputHeight,
                                                     struct BrookVkSceneLayer out[DESKTOP_SHADOW_PIECES]){
    if(!desktop_surface_has_shadow(s) || !outputWidth || !outputHeight)return 0;
    unsigned n=0;
    for(unsigned piece=0;piece<DESKTOP_SHADOW_PIECES;++piece){
        struct BrookChromeShadowPiece p;
        if(!brook_chrome_shadow_piece(s->x,s->y,s->width,s->height,s->shadowSpread,s->shadowCorner,piece,&p))continue;
        if(p.dx>=(int64_t)outputWidth || p.dy>=(int64_t)outputHeight || (int64_t)p.dx+p.dw<=0 || (int64_t)p.dy+p.dh<=0)continue;
        if(p.dw>BROOK_VK_MAX_DIMENSION || p.dh>BROOK_VK_MAX_DIMENSION)continue;
        out[n++]=(struct BrookVkSceneLayer){0,BROOK_CHROME_SHADOW_SIZE,BROOK_CHROME_SHADOW_SIZE,BROOK_CHROME_SHADOW_SIZE,
            p.sx,p.sy,p.sw,p.sh,p.dx,p.dy,p.dw,p.dh,0,0,outputWidth,outputHeight,s->shadowAlpha|BROOK_VK_STRAIGHT_ALPHA};
    }
    return n;
}

/* Frame spans of this record may replace what lies beneath them. */
static inline int desktop_surface_frame_opaque(const struct BrookDesktopSurface *s,uint32_t layerOpacity){
    return s && desktop_surface_valid(s) && (s->flags&BROOK_DESKTOP_SURFACE_OPAQUE) && s->opacity==255 && layerOpacity==255 &&
           s->material==BROOK_DESKTOP_MATERIAL_OPAQUE;
}
