/* Drives the shared zero-copy ring the way the pipelined renderer does: two
 * render contexts alternate, frames stay in flight until they retire, the
 * kernel model keeps front/armed/queued images busy, and every frame draws
 * only its buffer-age damage. Every presented image must equal the reference
 * frame in full, because zero-copy scans the whole image out. */
#include "desktop_buffer_age.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { W=320, H=180 };
static uint32_t ref[W*H],img[DESKTOP_AGE_MAX_IMAGES][W*H];

/* Desktop model: wallpaper, a window, a taskbar overlay band with a clock,
 * a tray icon that blinks, and a cursor. */
struct Desk { int winX,winY,winW,winH,winFrame,clock,tray,curX,curY; };
static void render(const struct Desk *d){
    for(int y=0;y<H;++y)for(int x=0;x<W;++x)ref[y*W+x]=0x00102030u+(uint32_t)((x/8+y/8)&1);
    for(int y=d->winY;y<d->winY+d->winH;++y)for(int x=d->winX;x<d->winX+d->winW;++x)
        if(x>=0 && y>=0 && x<W && y<H)ref[y*W+x]=0x00800000u+(uint32_t)(d->winFrame*7+x+y);
    for(int y=H-16;y<H;++y)for(int x=0;x<W;++x)ref[y*W+x]=0x00303030u;      /* taskbar */
    for(int y=H-12;y<H-4;++y)for(int x=W-40;x<W-8;++x)ref[y*W+x]=0x00ffffffu-(uint32_t)d->clock; /* clock */
    if(d->tray)for(int y=H-12;y<H-4;++y)for(int x=W-60;x<W-52;++x)ref[y*W+x]=0x0000ff00u;     /* tray icon */
    for(int y=d->curY;y<d->curY+8;++y)for(int x=d->curX;x<d->curX+8;++x)
        if(x>=0 && y>=0 && x<W && y<H)ref[y*W+x]=0x00ffff00u;               /* cursor */
}
/* Own damage: the bounding box of every pixel that differs from the
 * previous reference frame, per 16-row band (like the renderer's tracked
 * damage, a superset of the true change). */
static uint32_t prev[W*H];
static struct DesktopDamageSet diff(void){
    struct DesktopDamageSet s={0};
    for(int band=0;band<H;band+=16){
        int x0=W,y0=H,x1=0,y1=0;
        for(int y=band;y<band+16 && y<H;++y)for(int x=0;x<W;++x)if(ref[y*W+x]!=prev[y*W+x]){
            if(x<x0)x0=x;
            if(y<y0)y0=y;
            if(x+1>x1)x1=x+1;
            if(y+1>y1)y1=y+1;}
        if(x1>x0)desktop_damage_add(&s,(struct DesktopDamageRect){x0,y0,x1,y1},W,H);
    }
    return s;
}
static uint64_t area(const struct DesktopDamageSet *s){
    uint64_t p=0;for(uint32_t i=0;i<s->count;++i)p+=(uint64_t)(s->rects[i].right-s->rects[i].left)*(uint64_t)(s->rects[i].bottom-s->rects[i].top);
    return p;
}
static void draw(unsigned image,const struct DesktopDamageSet *d){
    for(uint32_t i=0;i<d->count;++i){const struct DesktopDamageRect *r=&d->rects[i];
        for(int64_t y=r->top;y<r->bottom;++y)memcpy(&img[image][y*W+r->left],&ref[y*W+r->left],(size_t)(r->right-r->left)*4);}
}
/* Kernel model: front and an armed/queued image are not free while armed. */
static int front=-1,armed=-1;
static uint32_t kernel_free(unsigned n){
    uint32_t m=(1u<<n)-1u;if(front>=0)m&=~(1u<<front);if(armed>=0)m&=~(1u<<armed);return m;
}
struct Pending { int image; struct DesktopDamageSet own; uint32_t frame[W*H]; int valid; };
static struct Pending inflight[2];

static uint64_t run(unsigned images,int frames,int (*scenario)(int,struct Desk*),uint64_t *ownPixels){
    struct DesktopScanoutRing ring={0};ring.count=images;
    desktop_buffer_age_bind(&ring.age,1,1,W,H);
    memset(img,0,sizeof(img));memset(prev,0xff,sizeof(prev));front=armed=-1;memset(inflight,0,sizeof(inflight));
    struct Desk d={40,30,120,80,0,0,0,10,10};
    uint64_t repair=0;*ownPixels=0;
    for(int f=0;f<frames;++f){
        const int nonImage=scenario(f,&d);
        render(&d);
        struct DesktopDamageSet own=diff();
        if(!own.count){memcpy(prev,ref,sizeof(prev));continue;}
        const int ctx=f&1; /* two pipelined contexts alternate */
        /* Retire this context's previous frame first (present it). */
        if(inflight[ctx].valid){
            struct Pending *p=&inflight[ctx];
            assert(!memcmp(img[p->image],p->frame,sizeof(p->frame))); /* image complete when scanned out */
            front=armed>=0?armed:front;armed=p->image;
            desktop_scanout_ring_end(&ring,(unsigned)p->image);p->valid=0;
        }
        if(nonImage){ /* fallback path drew elsewhere: forget image contents */
            desktop_buffer_age_reset(&ring.age);desktop_buffer_age_bind(&ring.age,1,1,W,H);
            memcpy(prev,ref,sizeof(prev));continue;
        }
        int image=desktop_scanout_ring_pick(&ring,kernel_free(images));
        if(image<0){ /* a latch frees the front: retry once */
            if(armed>=0){front=armed;armed=-1;}
            image=desktop_scanout_ring_pick(&ring,kernel_free(images));
        }
        assert(image>=0);
        struct DesktopDamageSet drawDamage;
        const int partial=desktop_buffer_age_damage(&ring.age,(unsigned)image,&own,&drawDamage);
        draw((unsigned)image,&drawDamage);
        if(partial)repair+=area(&drawDamage);
        *ownPixels+=area(&own);
        desktop_buffer_age_commit(&ring.age,(unsigned)image,&own,0);
        desktop_scanout_ring_begin(&ring,(unsigned)image);
        inflight[ctx]=(struct Pending){image,own,{0},1};memcpy(inflight[ctx].frame,ref,sizeof(ref));
        /* Two frames in flight never share an image. */
        assert(!inflight[ctx^1].valid || inflight[ctx^1].image!=image);
        memcpy(prev,ref,sizeof(prev));
    }
    for(int c=0;c<2;++c)if(inflight[c].valid)assert(!memcmp(img[inflight[c].image],inflight[c].frame,sizeof(inflight[c].frame)));
    return repair;
}
/* Scenarios: return 1 for a frame drawn by a non-image fallback path. */
static int mouse_and_taskbar(int f,struct Desk *d){
    d->curX=10+(f*5)%280;d->curY=10+(f*3)%150;     /* cursor every frame */
    if(f%7==0)++d->clock;                           /* clock overlay band */
    if(f%11==0)d->tray^=1;                          /* tray icon blinks */
    return 0;
}
static int game(int f,struct Desk *d){++d->winFrame;if(f%30==0)++d->clock;return 0;}
static int mixed(int f,struct Desk *d){
    mouse_and_taskbar(f,d);
    if(f%5==0)++d->winFrame;
    if(f==40){d->winX=100;d->winY=60;}              /* window move */
    return f==55;                                   /* one fallback frame */
}
/* The gen 372 bug: each pipelined context kept its own age history, so an
 * image missed the damage of frames the other context prepared. Same model,
 * split histories: presented images must then differ from the reference. */
static unsigned run_split_histories(unsigned images,int frames){
    struct DesktopBufferAge age[2]={{0},{0}};desktop_buffer_age_bind(&age[0],1,1,W,H);desktop_buffer_age_bind(&age[1],1,1,W,H);
    unsigned inflightCount[DESKTOP_AGE_MAX_IMAGES]={0};
    memset(img,0,sizeof(img));memset(prev,0xff,sizeof(prev));front=armed=-1;memset(inflight,0,sizeof(inflight));
    struct Desk d={40,30,120,80,0,0,0,10,10};unsigned wrong=0;
    for(int f=0;f<frames;++f){
        mouse_and_taskbar(f,&d);render(&d);
        struct DesktopDamageSet own=diff();
        const int ctx=f&1;
        if(inflight[ctx].valid){struct Pending *p=&inflight[ctx];
            if(memcmp(img[p->image],p->frame,sizeof(p->frame)))++wrong;
            front=armed>=0?armed:front;armed=p->image;--inflightCount[p->image];p->valid=0;}
        uint32_t freeMask=kernel_free(images);
        for(unsigned i=0;i<images;++i)if(inflightCount[i])freeMask&=~(1u<<i);
        int image=desktop_buffer_age_pick(&age[ctx],freeMask,images);
        if(image<0){if(armed>=0){front=armed;armed=-1;}freeMask=kernel_free(images);
            for(unsigned i=0;i<images;++i)if(inflightCount[i])freeMask&=~(1u<<i);
            image=desktop_buffer_age_pick(&age[ctx],freeMask,images);}
        assert(image>=0);
        struct DesktopDamageSet drawDamage;(void)desktop_buffer_age_damage(&age[ctx],(unsigned)image,&own,&drawDamage);
        draw((unsigned)image,&drawDamage);
        desktop_buffer_age_commit(&age[ctx],(unsigned)image,&own,0);++inflightCount[image];
        inflight[ctx]=(struct Pending){image,own,{0},1};memcpy(inflight[ctx].frame,ref,sizeof(ref));
        memcpy(prev,ref,sizeof(prev));
    }
    return wrong;
}
int main(void){
    for(unsigned images=3;images<=4;++images){
        uint64_t own=0,repair;
        repair=run(images,200,mouse_and_taskbar,&own);
        printf("images=%u mouse+taskbar: own=%llu repair=%llu\n",images,(unsigned long long)own,(unsigned long long)repair);
        repair=run(images,200,game,&own);
        /* Steady game: the window changes every frame, so the repair is the
         * frame's own damage; the ring adds no drawing. */
        assert(repair<=own);
        printf("images=%u game: own=%llu repair=%llu\n",images,(unsigned long long)own,(unsigned long long)repair);
        repair=run(images,200,mixed,&own);
        printf("images=%u mixed: own=%llu repair=%llu\n",images,(unsigned long long)own,(unsigned long long)repair);
    }
    const unsigned wrong=run_split_histories(4,200);
    printf("per-context histories (gen 372 bug): %u of 198 presented frames wrong\n",wrong);
    assert(wrong>0);
    puts("desktop scanout ring ok");
    return 0;
}
