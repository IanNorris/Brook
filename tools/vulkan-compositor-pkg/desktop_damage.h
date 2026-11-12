#pragma once
#include <stdint.h>

/* Conservative estimate only: all windows and overlays may change every
 * frame, even without a publication. Old bounds cover moves and removals.
 * A real partial-present path must additionally track lease expiry, producer
 * synchronization, all desktop decorations and atomic output ownership. */
enum { DESKTOP_DAMAGE_MAX_RECTS=32 };
struct DesktopDamageRect { int64_t left,top,right,bottom; };
struct DesktopDamageSet { uint32_t count; struct DesktopDamageRect rects[DESKTOP_DAMAGE_MAX_RECTS]; };
struct DesktopDamageState { uint64_t display,backdrop; uint32_t width,height; struct DesktopDamageSet previous; };
/* Partial work may be based only on a displayed frame while no older frame
 * is pending. A later full frame may overlap; the kernel rejects a stale
 * partial ticket if that later frame presents first. */
static inline int desktop_async_damage_partial_allowed(int enabled,unsigned active){return enabled && !active;}
/* Give an in-flight partial frame a bounded opportunity to retire.
 * Missing/backwards clocks fail open; this never prevents a second frame. */
static inline int desktop_async_damage_grace(int enabled,int partial,uint64_t submitted,uint64_t now){
    return enabled && partial && submitted && now>=submitted && now-submitted<40000000ull;
}
static inline void desktop_async_damage_retire(struct DesktopDamageState *displayed,
                                               struct DesktopDamageState submitted,int partial,int presented){
    if(presented)*displayed=submitted;
    else if(partial)*displayed=(struct DesktopDamageState){0};
}
static inline struct DesktopDamageRect desktop_damage_clip(struct DesktopDamageRect r,uint32_t width,uint32_t height){
    if(r.left<0)r.left=0;
    if(r.top<0)r.top=0;
    if(r.right>(int64_t)width)r.right=width;
    if(r.bottom>(int64_t)height)r.bottom=height;
    if(r.left>=r.right || r.top>=r.bottom)return (struct DesktopDamageRect){0,0,0,0};
    return r;
}
static inline struct DesktopDamageRect desktop_damage_union(struct DesktopDamageRect a,struct DesktopDamageRect b){
    if(b.left<a.left)a.left=b.left;
    if(b.top<a.top)a.top=b.top;
    if(b.right>a.right)a.right=b.right;
    if(b.bottom>a.bottom)a.bottom=b.bottom;
    return a;
}
static inline void desktop_damage_add(struct DesktopDamageSet *s,struct DesktopDamageRect r,uint32_t width,uint32_t height){
    r=desktop_damage_clip(r,width,height);
    if(r.left>=r.right || r.top>=r.bottom)return;
    for(uint32_t i=0;i<s->count;){
        const struct DesktopDamageRect a=s->rects[i];
        if(r.left<a.right && a.left<r.right && r.top<a.bottom && a.top<r.bottom){
            r=desktop_damage_union(r,a);s->rects[i]=s->rects[--s->count];i=0;
        }else ++i;
    }
    if(s->count==DESKTOP_DAMAGE_MAX_RECTS){s->count=1;s->rects[0]=(struct DesktopDamageRect){0,0,width,height};return;}
    s->rects[s->count++]=r;
}
static inline struct DesktopDamageSet desktop_damage_current(struct DesktopDamageState s,uint64_t display,uint64_t backdrop,
                                                               uint32_t width,uint32_t height,struct DesktopDamageSet current){
    if(!display || !backdrop || !width || !height || s.display!=display || s.backdrop!=backdrop ||
       s.width!=width || s.height!=height){current.count=0;desktop_damage_add(&current,(struct DesktopDamageRect){0,0,width,height},width,height);return current;}
    for(uint32_t i=0;i<s.previous.count;++i)desktop_damage_add(&current,s.previous.rects[i],width,height);
    return current;
}
static inline void desktop_damage_commit(struct DesktopDamageState *s,uint64_t display,uint64_t backdrop,
                                         uint32_t width,uint32_t height,struct DesktopDamageSet current){
    *s=(struct DesktopDamageState){display,backdrop,width,height,current};
}
