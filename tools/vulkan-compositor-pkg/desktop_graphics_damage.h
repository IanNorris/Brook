#pragma once
#include "desktop_damage.h"

/* Expand and merge damage so each source byte offset is BCS-aligned in a
 * full-size, tightly pitched output. Pixels added here must also be drawn
 * before any region is presented. */
static inline int desktop_graphics_damage(const struct DesktopDamageSet *input,uint32_t width,uint32_t height,
                                          struct DesktopDamageSet *output)
{
    if(!input || !output || !width || !height || (width&15u) ||
       !input->count || input->count>DESKTOP_DAMAGE_MAX_RECTS)return 0;
    struct DesktopDamageSet result={0};
    for(uint32_t i=0;i<input->count;++i){
        struct DesktopDamageRect r=input->rects[i];
        if(r.left<0 || r.top<0 || r.right<=r.left || r.bottom<=r.top ||
           r.right>width || r.bottom>height)return 0;
        r.left&=~INT64_C(15);r.right=(r.right+15)&~INT64_C(15);
        if(r.right>width)r.right=width;
        desktop_damage_add(&result,r,width,height);
    }
    if(!result.count)return 0;
    *output=result;return 1;
}
