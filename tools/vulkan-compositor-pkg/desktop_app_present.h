#pragma once
/* App-driven present cadence and direct scanout eligibility (pure, host-
 * tested by desktop_app_present_test.c). See docs/app-driven-present.md.
 *
 * Ian's decision (18 Nov 2026): a focused, visible window covering more than
 * 60% of the output's pixel area takes over the present cadence. The desktop
 * then presents when that window commits, instead of on its own schedule;
 * damage that does not come from that window (pointer, overlay, other
 * windows, popups) rides the next app frame, bounded by a short deadline so
 * nothing waits on a stalled app. When the window exactly covers the output
 * with an opaque, scanout-compatible buffer it may be scanned out directly.
 *
 * Nothing here touches the kernel or the GPU: the renderer feeds it the
 * layer list, the focus query (BROOK_DESKTOP_FOCUS_QUERY), its change
 * tracking and the selected GPU offer, and acts on the answer. */
#include "desktop_layers_abi.h"
#include "desktop_track.h"
#include "gpu_window_channel.h"
#include "wm_window_state.h"
#include <stdint.h>
#include <string.h>

enum { DESKTOP_APP_THRESHOLD_PERCENT=60 };
/* Non-app damage waits at most this long for an app commit: two 60 Hz
 * refreshes, so a ~30 fps game's next frame can carry it (Ian, 19 Nov: aim
 * for the next 33 ms slot rather than demanding 60 fps). It waits only when
 * the app's next commit is expected within it (see desktop_app_cadence).
 * Overridden by the kernel policy's deferUs (`desktop present defer <ms>`). */
#define DESKTOP_APP_DEFER_NS 33000000ull
/* After the expected commit, allow this much lateness before giving up. */
#define DESKTOP_APP_COMMIT_SLACK_NS 2000000ull

/* ---- Which window drives the cadence ------------------------------------ */
enum DesktopAppReason {
    DESKTOP_APP_DRIVEN=0,         /* a dominant window exists */
    DESKTOP_APP_NO_FOCUS_INFO,    /* older kernel: no BROOK_DESKTOP_FOCUS_QUERY */
    DESKTOP_APP_NO_FOCUS,         /* nothing focused (desktop, launcher) */
    DESKTOP_APP_FOCUS_HIDDEN,     /* focused window minimized/not composed */
    DESKTOP_APP_SMALL,            /* covers <= 60% of the output */
    DESKTOP_APP_TRANSLUCENT,      /* window opacity below 255: what is behind shows */
    DESKTOP_APP_REASONS
};
static const char *const desktop_app_reason_names[DESKTOP_APP_REASONS]={
    "driven","no_focus_info","no_focus","focus_hidden","small","translucent"};

struct DesktopFocusInfo { uint32_t valid,window,state; uint64_t incarnation;
    uint32_t shell,hasPointer; int32_t pointerX,pointerY; /* focus query version 2 */ };

/* The output rectangle a layer's client content covers (clipped). */
static inline uint64_t desktop_app_client_area(const struct BrookDesktopLayer *l,uint32_t width,uint32_t height,
                                               struct DesktopDamageRect *out){
    uint32_t w=l->clientWidth,h=l->clientHeight;
    if(!(l->flags&BROOK_DESKTOP_LAYER_LEGACY_OPAQUE)){if(l->width<w)w=l->width;if(l->height<h)h=l->height;}
    int64_t x0=l->clientX,y0=l->clientY,x1=x0+w,y1=y0+h;
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>(int64_t)width)x1=width;
    if(y1>(int64_t)height)y1=height;
    if(x1<x0)x1=x0;
    if(y1<y0)y1=y0;
    if(out)*out=(struct DesktopDamageRect){x0,y0,x1,y1};
    return (uint64_t)(x1-x0)*(uint64_t)(y1-y0);
}
/* Layer index of the window that drives the cadence, or -1 with the reason.
 * Only the focused window can drive, so two large windows never compete:
 * the focused one decides, and an unfocused large window never does. */
static inline int desktop_app_dominant(const struct BrookDesktopLayer *layers,unsigned count,const struct DesktopFocusInfo *focus,
                                       uint32_t width,uint32_t height,unsigned percent,enum DesktopAppReason *reason){
    enum DesktopAppReason r=DESKTOP_APP_NO_FOCUS_INFO;int found=-1;
    if(focus && focus->valid){
        r=DESKTOP_APP_NO_FOCUS;
        if(focus->window){
            r=DESKTOP_APP_FOCUS_HIDDEN;
            if((focus->state&BROOK_WINDOW_STATE_VISIBLE) && !(focus->state&BROOK_WINDOW_STATE_MINIMIZED))
                for(unsigned i=0;i<count;++i)if(layers[i].window==focus->window && layers[i].incarnation==focus->incarnation){found=(int)i;break;}
            if(found>=0){
                const struct BrookDesktopLayer *l=&layers[found];
                const uint64_t area=desktop_app_client_area(l,width,height,NULL),total=(uint64_t)width*height;
                const uint32_t opacity=(l->flags&BROOK_DESKTOP_LAYER_HAS_OPACITY)?
                    (l->flags&BROOK_DESKTOP_LAYER_OPACITY_MASK)>>BROOK_DESKTOP_LAYER_OPACITY_SHIFT:255u;
                if(!total || area*100u<=total*percent){r=DESKTOP_APP_SMALL;found=-1;}
                else if(opacity!=255){r=DESKTOP_APP_TRANSLUCENT;found=-1;}
                else r=DESKTOP_APP_DRIVEN;
            }
        }
    }
    if(reason)*reason=r;
    return found;
}
/* Whether the window's content differs from the baseline frame (the one on
 * screen, or the latest in flight): a new GPU commit, or changed CPU pixel
 * bands. A window the baseline does not show is new, hence changed. */
static inline int desktop_app_window_changed(const struct DesktopTrackState *base,const struct DesktopTrackWindow *now){
    if(!base || !base->valid || !now)return 1;
    for(uint32_t i=0;i<base->count && i<BROOK_DESKTOP_LAYER_LIMIT;++i){const struct DesktopTrackWindow *was=&base->windows[i];
        if(was->window!=now->window || was->incarnation!=now->incarnation)continue;
        if(was->direct!=now->direct)return 1;
        if(now->direct)return was->commit!=now->commit;
        if(was->content.count!=now->content.count || was->content.rows!=now->content.rows)return 1;
        return memcmp(was->content.hash,now->content.hash,(size_t)now->content.count*sizeof(now->content.hash[0]))!=0;
    }
    return 1;
}

/* ---- When to present ---------------------------------------------------- */
enum DesktopAppAction {
    DESKTOP_APP_FREE=0,     /* no dominant window: the desktop's own cadence */
    DESKTOP_APP_PRESENT,    /* the dominant window committed: present now (with any waiting damage) */
    DESKTOP_APP_DEADLINE,   /* only non-app damage, and it has waited the deadline: present it */
    DESKTOP_APP_DEFER,      /* only non-app damage: wait for the app (until *wakeNs) */
    DESKTOP_APP_TRANSITION, /* a different window became dominant: present at once */
    /* Non-app damage presented at once, never deferred (gen 440: the pointer
     * and the launcher animated at the game's 34 fps over fullscreen Quake): */
    DESKTOP_APP_SHELL,      /* a shell surface (launcher, volume popup, tooltip, toast) is open or animating */
    DESKTOP_APP_ABOVE,      /* a window (popup, menu, dialog) lies over the dominant one */
    DESKTOP_APP_POINTER_OUTSIDE, /* the pointer is not over the dominant client area */
    DESKTOP_APP_STALLED,    /* no app commit for over one app interval (paused), or no history yet */
    DESKTOP_APP_NOT_DUE,    /* the next app commit is expected later than the deferral */
    DESKTOP_APP_ACTIONS
};
static const char *const desktop_app_action_names[DESKTOP_APP_ACTIONS]={"free","present","deadline","defer","transition",
    "shell","window_above","pointer_outside","app_stalled","app_not_due"};
/* lastAppNs/intervalNs: the dominant window's last commit as seen here and
 * its smoothed commit interval (0 = unknown). */
struct DesktopAppCadence { uint32_t window; uint64_t incarnation,foreignSinceNs,lastAppNs,intervalNs; };
/* What the frame shows besides the damage itself. */
struct DesktopAppFrame {
    int dominant;uint32_t window;uint64_t incarnation;
    int appChanged;      /* the dominant window committed */
    int shellOpen;       /* a shell surface is open, animating, or over the window */
    int windowAbove;     /* another window shows over the dominant one */
    int pointerOutside;  /* the pointer is outside the dominant client area */
    uint64_t now,deferNs;
};
/* Called for a frame that has damage. Non-app damage waits only when nothing
 * else needs the screen to move (no shell surface, no window above, the
 * pointer over the app) and the app is running steadily with its next
 * commit due within the deferral; it then waits for that commit, bounded
 * by the deferral. Losing the dominant window, or a new one taking over,
 * never delays a frame. */
static inline enum DesktopAppAction desktop_app_cadence(struct DesktopAppCadence *s,const struct DesktopAppFrame *f,uint64_t *wakeNs){
    if(wakeNs)*wakeNs=0;
    if(!f->dominant){*s=(struct DesktopAppCadence){0};return DESKTOP_APP_FREE;}
    const uint64_t now=f->now?f->now:1;
    if(s->window!=f->window || s->incarnation!=f->incarnation){
        *s=(struct DesktopAppCadence){f->window,f->incarnation,0,f->appChanged?now:0,0};return DESKTOP_APP_TRANSITION;}
    if(f->appChanged){
        if(s->lastAppNs && now>s->lastAppNs){const uint64_t d=now-s->lastAppNs;
            /* A pause (gap over 4 intervals) is not a frame interval. */
            if(d>=1000000000ull)s->intervalNs=0;
            else if(!s->intervalNs)s->intervalNs=d;
            else if(d<=4*s->intervalNs)s->intervalNs=(s->intervalNs*7+d)/8;}
        s->lastAppNs=now;s->foreignSinceNs=0;return DESKTOP_APP_PRESENT;}
    enum DesktopAppAction now_action=DESKTOP_APP_DEFER;
    const uint64_t silent=s->lastAppNs && now>=s->lastAppNs?now-s->lastAppNs:UINT64_MAX;
    const uint64_t interval=s->intervalNs,due=s->lastAppNs+interval;
    if(f->shellOpen)now_action=DESKTOP_APP_SHELL;
    else if(f->windowAbove)now_action=DESKTOP_APP_ABOVE;
    else if(f->pointerOutside)now_action=DESKTOP_APP_POINTER_OUTSIDE;
    else if(!interval || silent>interval+interval/4+DESKTOP_APP_COMMIT_SLACK_NS)now_action=DESKTOP_APP_STALLED;
    else if(due>now && due-now>f->deferNs)now_action=DESKTOP_APP_NOT_DUE;
    if(now_action!=DESKTOP_APP_DEFER){s->foreignSinceNs=0;return now_action;}
    if(!s->foreignSinceNs)s->foreignSinceNs=now;
    if(now-s->foreignSinceNs>=f->deferNs){s->foreignSinceNs=0;return DESKTOP_APP_DEADLINE;}
    uint64_t wake=s->foreignSinceNs+f->deferNs;
    if(due+DESKTOP_APP_COMMIT_SLACK_NS<wake)wake=due+DESKTOP_APP_COMMIT_SLACK_NS;
    if(wake<=now){s->foreignSinceNs=0;return DESKTOP_APP_DEADLINE;}
    if(wakeNs)*wakeNs=wake;
    return DESKTOP_APP_DEFER;
}
/* A frame with no damage at all: nothing waits any more. */
static inline void desktop_app_cadence_idle(struct DesktopAppCadence *s){s->foreignSinceNs=0;}
/* Whether point (x,y) lies in the layer's on-screen client rectangle. */
static inline int desktop_app_point_inside(const struct BrookDesktopLayer *l,uint32_t width,uint32_t height,int32_t x,int32_t y){
    struct DesktopDamageRect r;if(!desktop_app_client_area(l,width,height,&r))return 0;
    return x>=r.left && x<r.right && y>=r.top && y<r.bottom;
}
/* Any later (higher) layer with on-screen area that overlaps the dominant
 * window's client rectangle. */
static inline int desktop_app_window_above(const struct BrookDesktopLayer *layers,unsigned count,int dominant,uint32_t width,uint32_t height){
    if(dominant<0)return 0;
    struct DesktopDamageRect d;if(!desktop_app_client_area(&layers[dominant],width,height,&d))return 0;
    for(unsigned i=(unsigned)dominant+1;i<count;++i){struct DesktopDamageRect r;
        if(!desktop_app_client_area(&layers[i],width,height,&r))continue;
        if(r.left<d.right && d.left<r.right && r.top<d.bottom && d.top<r.bottom)return 1;}
    return 0;
}

/* ---- Direct scanout eligibility ----------------------------------------- */
/* Broadwell primary plane (Linux i915 i9xx_plane.c for gen8: ivb_primary
 * formats, i9xx_format_modifiers {X_TILED, LINEAR}, hsw_primary_max_stride
 * min(8192*cpp, 32 KiB)): XRGB8888 linear or X-tiled, never Y-tiled; linear
 * stride a multiple of 64 bytes, X-tiled a multiple of the 512-byte tile
 * row; the surface starts at the buffer (offset 0). The plane has no alpha:
 * an ARGB buffer is eligible only when the client declared it opaque, and
 * is then scanned out as XRGB (identical bytes). */
enum { DESKTOP_DIRECT_MAX_STRIDE_BYTES=32768 };
enum DesktopDirectReason {
    DESKTOP_DIRECT_OK=0,
    DESKTOP_DIRECT_NO_DOMINANT,  /* no window drives the cadence */
    DESKTOP_DIRECT_NOT_COVERING, /* not exactly the output rectangle (maximized, windowed) */
    DESKTOP_DIRECT_CHROME,       /* window chrome visible */
    DESKTOP_DIRECT_CPU_SOURCE,   /* drawn from the CPU snapshot, not a GPU buffer */
    DESKTOP_DIRECT_LAYERED,      /* several client layers (subsurfaces) */
    DESKTOP_DIRECT_SCALED,       /* buffer is not the output size 1:1 */
    DESKTOP_DIRECT_FORMAT,       /* not XRGB8888 / opaque ARGB8888 */
    DESKTOP_DIRECT_TILING_Y,     /* Y-tiled: the gen8 primary plane cannot scan it out */
    DESKTOP_DIRECT_STRIDE,       /* stride/offset/size outside the plane rules */
    DESKTOP_DIRECT_ABOVE,        /* another window (popup, menu, dialog) over the output */
    DESKTOP_DIRECT_OVERLAY,      /* shell overlay band visible (taskbar, notification) */
    DESKTOP_DIRECT_CURSOR,       /* pointer visible: Brook has no hardware cursor plane */
    DESKTOP_DIRECT_NO_RING,      /* zero-copy scanout not active */
    DESKTOP_DIRECT_KERNEL,       /* kernel cannot scan out client buffers (no CLIENT_SCANOUT cap) */
    DESKTOP_DIRECT_REASONS
};
static const char *const desktop_direct_reason_names[DESKTOP_DIRECT_REASONS]={
    "ok","no_dominant","not_covering","chrome","cpu_source","layered","scaled","format","tiling_y","stride",
    "window_above","overlay","cursor","no_ring","kernel"};

/* The client buffer as offered (one layer). stride/offset are in pixels,
 * words is the buffer size in 32-bit words, as on the GPU window wire. */
struct DesktopDirectBuffer { uint32_t width,height,stride,offset,fourcc,flags; uint64_t words;
    uint32_t srcX,srcY,srcWidth,srcHeight,dstWidth,dstHeight; };
static inline enum DesktopDirectReason desktop_direct_buffer_ok(const struct DesktopDirectBuffer *b,uint32_t width,uint32_t height){
    if(b->fourcc!=0x34325258u && !(b->fourcc==0x34325241u && (b->flags&BROOK_GPU_WINDOW_LAYER_OPAQUE)))return DESKTOP_DIRECT_FORMAT;
    if(b->flags&BROOK_GPU_WINDOW_LAYER_TILED_Y)return DESKTOP_DIRECT_TILING_Y;
    if(b->width!=width || b->height!=height || b->srcX || b->srcY || b->srcWidth!=width || b->srcHeight!=height ||
       b->dstWidth!=width || b->dstHeight!=height)return DESKTOP_DIRECT_SCALED;
    const int x=(b->flags&BROOK_GPU_WINDOW_LAYER_TILED_X)!=0;
    const uint64_t pitch=(uint64_t)b->stride*4u,rows=x?((uint64_t)height+7u)&~(uint64_t)7u:height;
    if(b->offset || pitch<(uint64_t)width*4u || pitch>DESKTOP_DIRECT_MAX_STRIDE_BYTES || (pitch&(x?511u:63u)) ||
       (uint64_t)b->words*4u<pitch*rows)return DESKTOP_DIRECT_STRIDE;
    return DESKTOP_DIRECT_OK;
}
/* What the frame shows above and around the dominant window. */
struct DesktopDirectScene {
    int dominant;                   /* layer index, -1 none */
    const struct BrookDesktopLayer *layers;unsigned count;
    int chromeVisible;              /* the window's chrome has pixels on screen */
    int gpuSource;                  /* drawn from a GPU offer this frame */
    unsigned offerLayers;           /* layers of that offer (1 for a plain offer) */
    struct DesktopDirectBuffer buffer;
    unsigned overlayBands;          /* visible overlay bands (non-empty) */
    int cursorVisible;
    int ringActive,kernelCapable;   /* zero-copy active; CLIENT_SCANOUT present */
};
/* Checks run in a fixed order, so the reason is the first blocker (the one
 * the counters report). */
static inline enum DesktopDirectReason desktop_direct_eligible(const struct DesktopDirectScene *s,uint32_t width,uint32_t height){
    if(!s || s->dominant<0 || (unsigned)s->dominant>=s->count)return DESKTOP_DIRECT_NO_DOMINANT;
    const struct BrookDesktopLayer *l=&s->layers[s->dominant];
    if(l->clientX || l->clientY || l->clientWidth!=width || l->clientHeight!=height ||
       ((l->flags&BROOK_DESKTOP_LAYER_LEGACY_OPAQUE) && l->upscale!=1) || l->width!=width || l->height!=height)
        return DESKTOP_DIRECT_NOT_COVERING;
    if(s->chromeVisible)return DESKTOP_DIRECT_CHROME;
    if(!s->gpuSource)return DESKTOP_DIRECT_CPU_SOURCE;
    if(s->offerLayers!=1)return DESKTOP_DIRECT_LAYERED;
    const enum DesktopDirectReason buffer=desktop_direct_buffer_ok(&s->buffer,width,height);
    if(buffer!=DESKTOP_DIRECT_OK)return buffer;
    /* Layers are bottom to top: anything after the dominant one that shows
     * on the output covers it. */
    for(unsigned i=(unsigned)s->dominant+1;i<s->count;++i)if(desktop_app_client_area(&s->layers[i],width,height,NULL))return DESKTOP_DIRECT_ABOVE;
    if(s->overlayBands)return DESKTOP_DIRECT_OVERLAY;
    if(s->cursorVisible)return DESKTOP_DIRECT_CURSOR;
    if(!s->ringActive)return DESKTOP_DIRECT_NO_RING;
    if(!s->kernelCapable)return DESKTOP_DIRECT_KERNEL;
    return DESKTOP_DIRECT_OK;
}

/* ---- Direct scanout state machine --------------------------------------- *
 * What the renderer believes the display shows, and which client buffers it
 * must keep from the client. A client buffer handed to the display is held
 * (no SAFE to the provider) until the kernel reports that the display has
 * latched away from it: it is neither front, armed nor queued. A buffer the
 * mailbox replaced before it was ever armed was never fetched and is freed
 * at once (the kernel reports it in none of the three). Leaving direct
 * scanout always draws the next composited image in full (the ring images
 * missed every direct frame), and the client buffer stays held until that
 * image has latched, so no torn or stale frame is shown and the client never
 * rewrites a buffer the display may still fetch. While the kernel reports
 * the scanout state uncertain, nothing is released. */
enum DesktopDirectState { DESKTOP_DIRECT_COMPOSITING=0, DESKTOP_DIRECT_SCANNING=1, DESKTOP_DIRECT_LEAVING=2 };
enum { DESKTOP_DIRECT_HOLDS=6 };
struct DesktopDirectHold { uint64_t buffer,commit; uint32_t window; };
struct DesktopDirectMachine {
    enum DesktopDirectState state;
    struct DesktopDirectHold holds[DESKTOP_DIRECT_HOLDS];unsigned held;
    int fullDrawOwed;     /* next composited frame must redraw the whole output */
    uint64_t enters,exits,frames,released,refusedHold;
};
/* Kernel view of the plane: buffer identities (0 = a ring image or GOP). */
struct DesktopDirectKernel { uint64_t front,armed,queued; int uncertain; };
static inline int desktop_direct_holding(const struct DesktopDirectMachine *m,uint64_t buffer){
    for(unsigned i=0;i<m->held;++i)if(m->holds[i].buffer==buffer)return 1;
    return 0;
}
/* Present a client buffer directly. Returns 0 (stay compositing) when it
 * cannot be held; the caller composites this frame instead. */
static inline int desktop_direct_present(struct DesktopDirectMachine *m,uint64_t buffer,uint32_t window,uint64_t commit){
    if(!buffer)return 0;
    if(!desktop_direct_holding(m,buffer)){
        if(m->held==DESKTOP_DIRECT_HOLDS){++m->refusedHold;return 0;}
        m->holds[m->held++]=(struct DesktopDirectHold){buffer,commit,window};
    }else for(unsigned i=0;i<m->held;++i)if(m->holds[i].buffer==buffer){m->holds[i].commit=commit;m->holds[i].window=window;}
    if(m->state!=DESKTOP_DIRECT_SCANNING)++m->enters;
    m->state=DESKTOP_DIRECT_SCANNING;++m->frames;return 1;
}
/* The frame is composited (ineligible, refused, or not attempted). Leaving
 * direct scanout owes a full draw. */
static inline void desktop_direct_composite(struct DesktopDirectMachine *m){
    if(m->state==DESKTOP_DIRECT_SCANNING){m->state=DESKTOP_DIRECT_LEAVING;m->fullDrawOwed=1;++m->exits;}
}
/* The full composited frame owed after leaving was drawn and submitted. */
static inline void desktop_direct_full_drawn(struct DesktopDirectMachine *m){m->fullDrawOwed=0;}
/* After each kernel status read: release (copy out) holds the display can no
 * longer fetch. Returns how many were written to out. */
static inline unsigned desktop_direct_release(struct DesktopDirectMachine *m,const struct DesktopDirectKernel *k,
                                              struct DesktopDirectHold *out,unsigned max){
    if(!k || k->uncertain)return 0;
    unsigned n=0,kept=0;
    for(unsigned i=0;i<m->held;++i){const struct DesktopDirectHold h=m->holds[i];
        const int live=h.buffer==k->front || h.buffer==k->armed || h.buffer==k->queued;
        if(live || n>=max){m->holds[kept++]=h;continue;}
        out[n++]=h;++m->released;
    }
    m->held=kept;
    if(m->state==DESKTOP_DIRECT_LEAVING && !m->held && !m->fullDrawOwed)m->state=DESKTOP_DIRECT_COMPOSITING;
    return n;
}
