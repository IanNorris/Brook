/* Host test for desktop_app_present.h: which window drives the present
 * cadence, when a frame presents, direct scanout eligibility and the direct
 * scanout state machine (buffer release ordering). */
#include "desktop_app_present.h"
#include <assert.h>
#include <stdio.h>

enum { W=3200, H=1800 };
static struct BrookDesktopLayer layer(uint32_t window,uint64_t incarnation,int32_t x,int32_t y,uint32_t w,uint32_t h){
    struct BrookDesktopLayer l;memset(&l,0,sizeof(l));
    l.window=window;l.incarnation=incarnation;l.generation=1;l.width=l.stride=l.clientWidth=w;l.height=l.clientHeight=h;
    l.clientX=x;l.clientY=y;l.upscale=1;l.bytes=(uint64_t)w*h*4;
    return l;
}
static struct DesktopFocusInfo visible(uint32_t window,uint64_t incarnation){
    return (struct DesktopFocusInfo){.valid=1,.window=window,.state=BROOK_WINDOW_STATE_VISIBLE|BROOK_WINDOW_STATE_FOCUSED,.incarnation=incarnation};
}
static void test_dominance(void){
    enum DesktopAppReason r;
    struct BrookDesktopLayer l[3]={layer(1,10,0,0,W,H),layer(2,20,100,100,800,600),layer(3,30,0,0,W,H)};
    struct DesktopFocusInfo f=visible(1,10);
    /* No focus query (older kernel), or nothing focused. */
    assert(desktop_app_dominant(l,1,NULL,W,H,60,&r)==-1 && r==DESKTOP_APP_NO_FOCUS_INFO);
    struct DesktopFocusInfo none={.valid=1};
    assert(desktop_app_dominant(l,1,&none,W,H,60,&r)==-1 && r==DESKTOP_APP_NO_FOCUS);
    /* Fullscreen focused window drives. */
    assert(desktop_app_dominant(l,1,&f,W,H,60,&r)==0 && r==DESKTOP_APP_DRIVEN);
    /* Minimized or not composed (absent from the layers, stale incarnation). */
    struct DesktopFocusInfo m=f;m.state|=BROOK_WINDOW_STATE_MINIMIZED;
    assert(desktop_app_dominant(l,1,&m,W,H,60,&r)==-1 && r==DESKTOP_APP_FOCUS_HIDDEN);
    struct DesktopFocusInfo stale=visible(1,11);
    assert(desktop_app_dominant(l,1,&stale,W,H,60,&r)==-1 && r==DESKTOP_APP_FOCUS_HIDDEN);
    /* Threshold: exactly 60% does not drive, just over does. 3200x1080 = 60%. */
    l[0]=layer(1,10,0,0,W,1080);
    assert(desktop_app_dominant(l,1,&f,W,H,60,&r)==-1 && r==DESKTOP_APP_SMALL);
    l[0]=layer(1,10,0,0,W,1081);
    assert(desktop_app_dominant(l,1,&f,W,H,60,&r)==0);
    /* Off-screen parts do not count. A maximized window (taskbar below) drives. */
    l[0]=layer(1,10,-1600,0,W,H); /* half on screen */
    assert(desktop_app_dominant(l,1,&f,W,H,60,&r)==-1 && r==DESKTOP_APP_SMALL);
    l[0]=layer(1,10,0,0,W,H-96);
    assert(desktop_app_dominant(l,1,&f,W,H,60,&r)==0);
    /* A VFB smaller than its client rectangle covers only the VFB. */
    l[0]=layer(1,10,0,0,W,H);l[0].width=1600;
    assert(desktop_app_dominant(l,1,&f,W,H,60,&r)==-1 && r==DESKTOP_APP_SMALL);
    l[0]=layer(1,10,0,0,W,H);
    /* Translucent windows never drive: what is behind them shows. */
    l[0].flags=BROOK_DESKTOP_LAYER_HAS_OPACITY|(200u<<BROOK_DESKTOP_LAYER_OPACITY_SHIFT);
    assert(desktop_app_dominant(l,1,&f,W,H,60,&r)==-1 && r==DESKTOP_APP_TRANSLUCENT);
    l[0].flags=0;
    /* Two large windows: only the focused one can drive. */
    struct DesktopFocusInfo f3=visible(3,30);
    assert(desktop_app_dominant(l,3,&f3,W,H,60,&r)==2);
    struct DesktopFocusInfo f2=visible(2,20); /* focused small window over a big one */
    assert(desktop_app_dominant(l,3,&f2,W,H,60,&r)==-1 && r==DESKTOP_APP_SMALL);
}
static void test_changed(void){
    struct DesktopTrackState base;memset(&base,0,sizeof(base));base.valid=1;base.count=2;
    base.windows[0]=(struct DesktopTrackWindow){.window=1,.incarnation=10,.direct=1,.commit=5};
    base.windows[1]=(struct DesktopTrackWindow){.window=2,.incarnation=20};
    base.windows[1].content.rows=8;base.windows[1].content.count=2;base.windows[1].content.hash[0]=1;base.windows[1].content.hash[1]=2;
    struct DesktopTrackWindow now=base.windows[0];
    assert(!desktop_app_window_changed(&base,&now));
    now.commit=6;assert(desktop_app_window_changed(&base,&now));
    now=base.windows[1];assert(!desktop_app_window_changed(&base,&now));
    now.content.hash[1]=3;assert(desktop_app_window_changed(&base,&now));
    now=base.windows[1];now.direct=1;assert(desktop_app_window_changed(&base,&now));
    now=(struct DesktopTrackWindow){.window=3,.incarnation=1};assert(desktop_app_window_changed(&base,&now)); /* new window */
    base.valid=0;now=base.windows[0];assert(desktop_app_window_changed(&base,&now)); /* no baseline */
}
static enum DesktopAppAction step(struct DesktopAppCadence *c,struct DesktopAppFrame f,uint64_t *wake){
    return desktop_app_cadence(c,&f,wake);
}
#define MS 1000000ull
static void test_cadence(void){
    struct DesktopAppCadence c={0};uint64_t wake=0;const uint64_t d=DESKTOP_APP_DEFER_NS;
    const struct DesktopAppFrame app={.dominant=1,.window=1,.incarnation=10,.deferNs=d};
    struct DesktopAppFrame f=app;
    /* No dominant window: the desktop's own cadence. */
    f.dominant=0;f.now=1*MS;assert(step(&c,f,&wake)==DESKTOP_APP_FREE && !wake);
    /* A window becomes dominant: present at once. */
    f=app;f.now=2*MS;assert(step(&c,f,&wake)==DESKTOP_APP_TRANSITION);
    /* Pointer damage before any commit history: the app may be paused, so
     * never wait on it. */
    f.now=3*MS;assert(step(&c,f,&wake)==DESKTOP_APP_STALLED && !wake);
    /* A steady 60 fps app (commits every 16 ms). */
    for(uint64_t t=10;t<=106;t+=16){f.appChanged=1;f.now=t*MS;assert(step(&c,f,&wake)==DESKTOP_APP_PRESENT);}
    assert(c.intervalNs==16*MS && c.lastAppNs==106*MS);
    f.appChanged=0;
    /* Pointer move 4 ms after a commit: the next is due in 12 ms, so it
     * waits for it (plus slack), well under one refresh... */
    f.now=110*MS;assert(step(&c,f,&wake)==DESKTOP_APP_DEFER && wake==(106+16)*MS+DESKTOP_APP_COMMIT_SLACK_NS);
    /* ...and rides it. */
    f.appChanged=1;f.now=122*MS;assert(step(&c,f,&wake)==DESKTOP_APP_PRESENT && !c.foreignSinceNs);f.appChanged=0;
    /* The commit is late: the wake time passes and the damage presents. */
    f.now=124*MS;assert(step(&c,f,&wake)==DESKTOP_APP_DEFER && wake==140*MS);
    f.now=140*MS;assert(step(&c,f,&wake)==DESKTOP_APP_DEADLINE);
    /* Paused game: no commit for more than an interval -> at once. */
    f.now=150*MS;assert(step(&c,f,&wake)==DESKTOP_APP_STALLED && !wake);
    f.now=400*MS;assert(step(&c,f,&wake)==DESKTOP_APP_STALLED);
    /* A frame without damage clears any wait. */
    f.appChanged=1;f.now=401*MS;assert(step(&c,f,&wake)==DESKTOP_APP_PRESENT);
    assert(c.intervalNs==16*MS); /* the 295 ms pause is not a frame interval */
    f.appChanged=0;f.now=405*MS;assert(step(&c,f,&wake)==DESKTOP_APP_DEFER);
    desktop_app_cadence_idle(&c);assert(!c.foreignSinceNs);
    /* Gen 440 (Quake at 34 fps, commits every 29 ms): a pointer move early
     * in the frame is not deferred (the commit is more than a refresh
     * away); late in the frame it rides the commit. */
    /* Default limit (33 ms, two refreshes): even an early move rides the next
     * 29 ms frame. */
    {struct DesktopAppCadence q={0};struct DesktopAppFrame g=app;g.now=1*MS;assert(step(&q,g,&wake)==DESKTOP_APP_TRANSITION);
     g.appChanged=1;for(uint64_t t=10;t<=126;t+=29){g.now=t*MS;assert(step(&q,g,&wake)==DESKTOP_APP_PRESENT);}
     g.appChanged=0;g.now=130*MS;assert(d==33*MS && step(&q,g,&wake)==DESKTOP_APP_DEFER && wake==(126+29)*MS+DESKTOP_APP_COMMIT_SLACK_NS);}
    /* With `desktop present defer 16`: */
    {struct DesktopAppCadence q={0};struct DesktopAppFrame g=app;g.deferNs=16*MS;g.now=1*MS;assert(step(&q,g,&wake)==DESKTOP_APP_TRANSITION);
     g.appChanged=1;for(uint64_t t=10;t<=126;t+=29){g.now=t*MS;assert(step(&q,g,&wake)==DESKTOP_APP_PRESENT);}
     assert(q.intervalNs==29*MS);g.appChanged=0;
     g.now=130*MS;assert(step(&q,g,&wake)==DESKTOP_APP_NOT_DUE && !wake);
     g.now=145*MS;assert(step(&q,g,&wake)==DESKTOP_APP_DEFER && wake<=145*MS+16*MS && wake==(126+29)*MS+DESKTOP_APP_COMMIT_SLACK_NS);
     /* Launcher open over the game (the gen 440 stutter): never deferred. */
     g.shellOpen=1;g.now=146*MS;assert(step(&q,g,&wake)==DESKTOP_APP_SHELL && !wake && !q.foreignSinceNs);g.shellOpen=0;
     /* A menu or popup over the game. */
     g.windowAbove=1;assert(step(&q,g,&wake)==DESKTOP_APP_ABOVE);g.windowAbove=0;
     /* Pointer outside the game's client area (over chrome or another window). */
     g.pointerOutside=1;assert(step(&q,g,&wake)==DESKTOP_APP_POINTER_OUTSIDE);g.pointerOutside=0;
     /* Shell wins over every other reason; an app commit still presents as app. */
     g.shellOpen=g.windowAbove=g.pointerOutside=1;assert(step(&q,g,&wake)==DESKTOP_APP_SHELL);
     g.appChanged=1;g.now=155*MS;assert(step(&q,g,&wake)==DESKTOP_APP_PRESENT);}
    /* Focus loss with damage waiting: presented at once, state cleared. */
    f.now=406*MS;assert(step(&c,f,&wake)==DESKTOP_APP_DEFER);
    f.dominant=0;f.now=407*MS;assert(step(&c,f,&wake)==DESKTOP_APP_FREE && !c.window && !c.intervalNs);
    /* Another window takes over (alt-tab), or the slot is reused. */
    f=app;f.now=500*MS;assert(step(&c,f,&wake)==DESKTOP_APP_TRANSITION);
    f.window=3;f.incarnation=30;assert(step(&c,f,&wake)==DESKTOP_APP_TRANSITION);
    f.window=1;f.incarnation=11;assert(step(&c,f,&wake)==DESKTOP_APP_TRANSITION && !c.intervalNs);
    for(unsigned a=0;a<DESKTOP_APP_ACTIONS;++a)assert(desktop_app_action_names[a] && *desktop_app_action_names[a]);
}
static void test_geometry(void){
    struct BrookDesktopLayer l[3]={layer(1,10,0,0,W,H-96),layer(2,20,1000,800,400,300),layer(3,30,W+5,0,200,200)};
    assert(desktop_app_point_inside(&l[0],W,H,0,0) && desktop_app_point_inside(&l[0],W,H,W-1,H-97));
    assert(!desktop_app_point_inside(&l[0],W,H,10,H-50));  /* over the taskbar */
    assert(!desktop_app_point_inside(&l[0],W,H,-1,10) && !desktop_app_point_inside(&l[0],W,H,W,10));
    assert(desktop_app_window_above(l,2,0,W,H));            /* popup over it */
    assert(!desktop_app_window_above(l,1,0,W,H));
    {struct BrookDesktopLayer m[2]={l[0],l[2]};assert(!desktop_app_window_above(m,2,0,W,H));} /* off screen */
    {struct BrookDesktopLayer m[2]={l[1],l[0]};assert(!desktop_app_window_above(m,2,1,W,H));} /* below it */
    {struct BrookDesktopLayer m[2]={l[0],layer(2,20,0,H-96,400,96)};assert(!desktop_app_window_above(m,2,0,W,H));} /* beside it */
    assert(!desktop_app_window_above(l,2,-1,W,H));
}
static struct DesktopDirectBuffer xrgb(uint32_t strideBytes,uint32_t flags){
    const uint32_t rows=(flags&BROOK_GPU_WINDOW_LAYER_TILED_X)?(H+7u)&~7u:H;
    return (struct DesktopDirectBuffer){W,H,strideBytes/4,0,0x34325258u,flags,(uint64_t)strideBytes/4*rows,0,0,W,H,W,H};
}
static void test_buffer(void){
    struct DesktopDirectBuffer b=xrgb(W*4,0);
    assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_OK);             /* linear 12800 */
    b=xrgb(W*4,BROOK_GPU_WINDOW_LAYER_TILED_X);
    assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_OK);             /* X-tiled 12800 = 25 tiles */
    b=xrgb(13312,BROOK_GPU_WINDOW_LAYER_TILED_X);
    assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_OK);             /* padded pitch */
    b=xrgb(W*4,BROOK_GPU_WINDOW_LAYER_TILED_Y);
    assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_TILING_Y);
    b=xrgb(12864,BROOK_GPU_WINDOW_LAYER_TILED_X);                            /* 64- but not 512-aligned */
    assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_STRIDE);
    b=xrgb(12864,0);assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_OK);
    b=xrgb(12832,0);assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_STRIDE); /* not 64-aligned */
    b=xrgb(32768+512,0);b.words=(uint64_t)b.stride*H;assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_STRIDE);
    b=xrgb(W*4,0);b.offset=1024;assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_STRIDE);
    b=xrgb(W*4,0);b.words-=1;assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_STRIDE);
    b=xrgb(W*4,BROOK_GPU_WINDOW_LAYER_TILED_X);b.height=b.srcHeight=b.dstHeight=1801;b.words=(uint64_t)b.stride*1801;
    assert(desktop_direct_buffer_ok(&b,W,1801)==DESKTOP_DIRECT_STRIDE);      /* X-tiled rows round to 8 */
    b=xrgb(W*4,0);b.fourcc=0x34325241u;assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_FORMAT);
    b.flags=BROOK_GPU_WINDOW_LAYER_OPAQUE;assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_OK);
    b=xrgb(W*4,0);b.fourcc=0x30334241u;assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_FORMAT);
    b=xrgb(W*4,0);b.dstWidth=1600;assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_SCALED);
    b=xrgb(W*4,0);b.srcY=8;assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_SCALED);
    b=(struct DesktopDirectBuffer){1600,900,1600,0,0x34325258u,0,1600ull*900,0,0,1600,900,W,H};
    assert(desktop_direct_buffer_ok(&b,W,H)==DESKTOP_DIRECT_SCALED);         /* 640x480-style upscale */
}
static void test_scene(void){
    struct BrookDesktopLayer l[3]={layer(1,10,0,0,W,H),layer(2,20,1000,800,400,300),layer(3,30,0,0,W,H)};
    struct DesktopDirectScene s={.dominant=0,.layers=l,.count=1,.gpuSource=1,.offerLayers=1,.buffer=xrgb(W*4,BROOK_GPU_WINDOW_LAYER_TILED_X),
        .ringActive=1,.kernelCapable=1};
    assert(desktop_direct_eligible(&s,W,H)==DESKTOP_DIRECT_OK);
    assert(desktop_direct_eligible(NULL,W,H)==DESKTOP_DIRECT_NO_DOMINANT);
    {struct DesktopDirectScene t=s;t.dominant=-1;assert(desktop_direct_eligible(&t,W,H)==DESKTOP_DIRECT_NO_DOMINANT);}
    /* Maximized (stops at the taskbar) is app-driven but never scanned out. */
    l[0]=layer(1,10,0,0,W,H-96);assert(desktop_direct_eligible(&s,W,H)==DESKTOP_DIRECT_NOT_COVERING);
    l[0]=layer(1,10,0,0,W,H);
    {struct DesktopDirectScene t=s;t.chromeVisible=1;assert(desktop_direct_eligible(&t,W,H)==DESKTOP_DIRECT_CHROME);}
    {struct DesktopDirectScene t=s;t.gpuSource=0;assert(desktop_direct_eligible(&t,W,H)==DESKTOP_DIRECT_CPU_SOURCE);}
    {struct DesktopDirectScene t=s;t.offerLayers=2;assert(desktop_direct_eligible(&t,W,H)==DESKTOP_DIRECT_LAYERED);}
    {struct DesktopDirectScene t=s;t.buffer.flags=BROOK_GPU_WINDOW_LAYER_TILED_Y;assert(desktop_direct_eligible(&t,W,H)==DESKTOP_DIRECT_TILING_Y);}
    /* A popup or menu over it falls back; one entirely off screen does not. */
    {struct DesktopDirectScene t=s;t.count=2;assert(desktop_direct_eligible(&t,W,H)==DESKTOP_DIRECT_ABOVE);}
    l[1]=layer(2,20,W+10,0,400,300);
    {struct DesktopDirectScene t=s;t.count=2;assert(desktop_direct_eligible(&t,W,H)==DESKTOP_DIRECT_OK);}
    /* A window below the dominant one is hidden by it. */
    {struct BrookDesktopLayer below[2]={layer(2,20,100,100,400,300),layer(1,10,0,0,W,H)};
     struct DesktopDirectScene t=s;t.layers=below;t.count=2;t.dominant=1;assert(desktop_direct_eligible(&t,W,H)==DESKTOP_DIRECT_OK);}
    {struct DesktopDirectScene t=s;t.overlayBands=1;assert(desktop_direct_eligible(&t,W,H)==DESKTOP_DIRECT_OVERLAY);}
    {struct DesktopDirectScene t=s;t.cursorVisible=1;assert(desktop_direct_eligible(&t,W,H)==DESKTOP_DIRECT_CURSOR);}
    {struct DesktopDirectScene t=s;t.ringActive=0;assert(desktop_direct_eligible(&t,W,H)==DESKTOP_DIRECT_NO_RING);}
    {struct DesktopDirectScene t=s;t.kernelCapable=0;assert(desktop_direct_eligible(&t,W,H)==DESKTOP_DIRECT_KERNEL);}
    /* The first blocker is reported: a windowed, Y-tiled game with the
     * pointer over it counts as not covering. */
    {struct DesktopDirectScene t=s;l[0]=layer(1,10,320,180,2560,1440);t.cursorVisible=1;t.buffer.flags=BROOK_GPU_WINDOW_LAYER_TILED_Y;
     assert(desktop_direct_eligible(&t,W,H)==DESKTOP_DIRECT_NOT_COVERING);}
    for(unsigned i=0;i<DESKTOP_DIRECT_REASONS;++i)assert(desktop_direct_reason_names[i] && *desktop_direct_reason_names[i]);
    for(unsigned i=0;i<DESKTOP_APP_REASONS;++i)assert(desktop_app_reason_names[i] && *desktop_app_reason_names[i]);
}
static void test_machine(void){
    struct DesktopDirectMachine m;memset(&m,0,sizeof(m));struct DesktopDirectHold out[DESKTOP_DIRECT_HOLDS];
    const uint64_t A=0xa,B=0xb,C=0xc;
    /* Enter: A armed while a ring image is still front. Nothing to release. */
    assert(desktop_direct_present(&m,A,1,1) && m.state==DESKTOP_DIRECT_SCANNING && m.enters==1);
    struct DesktopDirectKernel k={0,A,0,0};
    assert(!desktop_direct_release(&m,&k,out,6) && m.held==1);
    /* A latched; B presented and armed: A is still front, so still held. */
    k=(struct DesktopDirectKernel){A,0,0,0};assert(!desktop_direct_release(&m,&k,out,6));
    assert(desktop_direct_present(&m,B,1,2) && m.enters==1 && m.held==2);
    k=(struct DesktopDirectKernel){A,B,0,0};assert(!desktop_direct_release(&m,&k,out,6));
    /* B latched: only now may the client reuse A. */
    k=(struct DesktopDirectKernel){B,0,0,0};
    assert(desktop_direct_release(&m,&k,out,6)==1 && out[0].buffer==A && out[0].commit==1 && m.held==1);
    /* Mailbox: B front, A armed, C queued behind it. */
    assert(desktop_direct_present(&m,A,1,3) && desktop_direct_present(&m,C,1,4));
    k=(struct DesktopDirectKernel){B,A,C,0};assert(!desktop_direct_release(&m,&k,out,6) && m.held==3);
    /* A newer commit replaced C in the mailbox before it was ever armed: the
     * kernel no longer reports C, which is released while A and B stay. */
    k=(struct DesktopDirectKernel){B,A,0,0};
    assert(desktop_direct_release(&m,&k,out,6)==1 && out[0].buffer==C && m.held==2);
    /* Uncertain scanout state: nothing is released, whatever the slots say. */
    k=(struct DesktopDirectKernel){0,0,0,1};assert(!desktop_direct_release(&m,&k,out,6) && m.held==2);
    /* Exit (a popup opened): composite, owing a full draw. The client
     * buffers stay held until the composited image is front. */
    desktop_direct_composite(&m);
    assert(m.state==DESKTOP_DIRECT_LEAVING && m.fullDrawOwed && m.exits==1);
    desktop_direct_composite(&m);assert(m.exits==1); /* idempotent */
    k=(struct DesktopDirectKernel){B,A,0,0};assert(!desktop_direct_release(&m,&k,out,6));
    k=(struct DesktopDirectKernel){A,0,0,0};assert(desktop_direct_release(&m,&k,out,6)==1 && out[0].buffer==B);
    /* The ring image latched (front 0): A is released, but the machine only
     * returns to compositing once the owed full draw has happened. */
    k=(struct DesktopDirectKernel){0,0,0,0};assert(desktop_direct_release(&m,&k,out,6)==1 && out[0].buffer==A && !m.held);
    assert(m.state==DESKTOP_DIRECT_LEAVING);
    desktop_direct_full_drawn(&m);(void)desktop_direct_release(&m,&k,out,6);
    assert(m.state==DESKTOP_DIRECT_COMPOSITING && m.released==4);
    /* Compositing without ever scanning out changes nothing. */
    desktop_direct_composite(&m);assert(m.state==DESKTOP_DIRECT_COMPOSITING && !m.fullDrawOwed);
    /* Re-entering and presenting the same buffer twice holds it once. */
    assert(desktop_direct_present(&m,A,1,9) && desktop_direct_present(&m,A,1,10) && m.held==1 && m.holds[0].commit==10 && m.enters==2);
    /* A bounded release batch keeps the rest for the next call. */
    for(uint64_t b=1;b<DESKTOP_DIRECT_HOLDS;++b)assert(desktop_direct_present(&m,0x100+b,1,10+b));
    assert(m.held==DESKTOP_DIRECT_HOLDS && !desktop_direct_present(&m,0x200,1,99) && m.refusedHold==1);
    assert(!desktop_direct_present(&m,0,1,1)); /* no identity: refused */
    k=(struct DesktopDirectKernel){0x105,0,0,0};
    assert(desktop_direct_release(&m,&k,out,2)==2 && m.held==DESKTOP_DIRECT_HOLDS-2);
    assert(desktop_direct_release(&m,&k,out,6)==3 && m.held==1 && m.holds[0].buffer==0x105);
}
static void test_overlay_capture(void){
    const uint64_t n=DESKTOP_APP_OVERLAY_CAPTURE_NS;
    /* No app driving: every changed overlay is captured, as before. */
    assert(desktop_app_overlay_capture_due(0,1,100*MS,101*MS,n));
    /* App driving: at most one capture per interval while one is held. */
    assert(!desktop_app_overlay_capture_due(1,1,100*MS,101*MS,n));
    assert(!desktop_app_overlay_capture_due(1,1,100*MS,100*MS+n-1,n));
    assert(desktop_app_overlay_capture_due(1,1,100*MS,100*MS+n,n));
    /* No usable capture (first frame, size change) or no history: capture. */
    assert(desktop_app_overlay_capture_due(1,0,100*MS,101*MS,n));
    assert(desktop_app_overlay_capture_due(1,1,0,101*MS,n));
    assert(desktop_app_overlay_capture_due(1,1,200*MS,101*MS,n)); /* clock went back */
    /* Gen 441 shape: launcher open over a 34 fps game whose overlay changes
     * every frame. One second of app frames now captures at most 10 times
     * (the cadence-off run measured 8.3/s; cadence on had 17.5/s). */
    uint64_t last=0;unsigned captures=0;
    for(uint64_t t=1*MS;t<1000*MS;t+=29*MS)if(desktop_app_overlay_capture_due(1,captures>0,last,t,n)){++captures;last=t;}
    assert(captures>=9 && captures<=10);
}
int main(void){
    test_dominance();test_changed();test_cadence();test_geometry();test_overlay_capture();test_buffer();test_scene();test_machine();
    printf("desktop_app_present_test: ok\n");
    return 0;
}
