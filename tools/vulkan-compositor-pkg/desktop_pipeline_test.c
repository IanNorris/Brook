/* Host test of desktop_pipeline.h: parsing, admission, retirement order,
 * forced-full classification, and a randomized screen model of two frames
 * in flight under every kernel outcome (the older frame accepted after or
 * before the younger one's reservation, or refused). */
#include "desktop_pipeline.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {W=64,H=32,STEPS=2000};
static uint32_t rng=0x9e3779b9u;
static uint32_t next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static uint32_t pick(uint32_t n){return n?next()%n:0;}

/* A frame's full content and the damage it presents. */
struct Frame {uint8_t px[H][W];struct DesktopDamageSet damage;struct BrookGpuPresentRegions regions;};
static void change(const uint8_t from[H][W],uint8_t to[H][W]){
    memcpy(to,from,(size_t)W*H);
    for(unsigned n=pick(4);n--;){const uint32_t x=pick(W),y=pick(H),w=1+pick(12),h=1+pick(8);const uint8_t v=(uint8_t)next();
        for(uint32_t j=y;j<y+h && j<H;++j)for(uint32_t i=x;i<x+w && i<W;++i)to[j][i]=v;}
}
/* Exact change damage, one rectangle per changed cell (merged by add). */
static struct DesktopDamageSet diff(const uint8_t a[H][W],const uint8_t b[H][W]){
    struct DesktopDamageSet d={0};
    for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x)if(a[y][x]!=b[y][x])
        desktop_damage_add(&d,(struct DesktopDamageRect){x,y,x+1,y+1},W,H);
    return d;
}
static struct BrookGpuPresentRegions regions(const struct DesktopDamageSet *d){
    struct BrookGpuPresentRegions r={0};r.count=d->count;
    for(uint32_t i=0;i<d->count;++i){const struct DesktopDamageRect *q=&d->rects[i];
        r.regions[i]=(struct BrookGpuPresentRegion){0,(uint32_t)q->left,(uint32_t)q->top,(uint32_t)(q->right-q->left),(uint32_t)(q->bottom-q->top),W,0};}
    return r;
}
static void present(uint8_t screen[H][W],const struct Frame *f,const struct BrookGpuPresentRegions *r){
    for(uint32_t i=0;i<r->count;++i){const struct BrookGpuPresentRegion *g=&r->regions[i];
        for(uint32_t y=g->y;y<g->y+g->height;++y)for(uint32_t x=g->x;x<g->x+g->width;++x)screen[y][x]=f->px[y][x];}
}
/* desktop_present_dependencies.h: every pixel of deps lies in writes. */
static int covered(const struct BrookGpuPresentRegions *deps,const struct BrookGpuPresentRegions *writes){
    static uint8_t mask[H][W];memset(mask,0,sizeof(mask));
    for(uint32_t i=0;i<writes->count;++i){const struct BrookGpuPresentRegion *g=&writes->regions[i];
        for(uint32_t y=g->y;y<g->y+g->height;++y)for(uint32_t x=g->x;x<g->x+g->width;++x)mask[y][x]=1;}
    for(uint32_t i=0;i<deps->count;++i){const struct BrookGpuPresentRegion *g=&deps->regions[i];
        for(uint32_t y=g->y;y<g->y+g->height;++y)for(uint32_t x=g->x;x<g->x+g->width;++x)if(!mask[y][x])return 0;}
    return 1;
}
static void model(void){
    static uint8_t screen[H][W];static struct Frame shown,a,b;
    for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x)screen[y][x]=shown.px[y][x]=(uint8_t)next();
    unsigned outcomes[3]={0};
    for(unsigned step=0;step<STEPS;++step){
        /* A against the screen, B against A carrying A's regions. */
        change(shown.px,a.px);a.damage=diff(shown.px,a.px);
        if(!a.damage.count)a.damage.rects[a.damage.count++]=(struct DesktopDamageRect){0,0,4,1};
        a.regions=regions(&a.damage);
        change(a.px,b.px);const struct DesktopDamageSet own=diff(a.px,b.px);
        if(!own.count){/* idle relative to the in-flight frame */
            present(screen,&a,&a.regions);assert(!memcmp(screen,a.px,sizeof(screen)));shown=a;continue;}
        b.damage=own;assert(desktop_pipeline_carry(&b.damage,&a.regions,W,H));b.regions=regions(&b.damage);
        const struct BrookGpuPresentRegions ownRegions=regions(&own);
        const unsigned outcome=pick(3);++outcomes[outcome];
        if(outcome==0){        /* A accepted after B's reservation: B must cover A's writes */
            present(screen,&a,&a.regions);assert(!memcmp(screen,a.px,sizeof(screen)));
            assert(covered(&a.regions,&b.regions));present(screen,&b,&b.regions);
        }else if(outcome==1){  /* A accepted first: B rebased, presents only its own changes */
            present(screen,&a,&a.regions);present(screen,&b,&ownRegions);
        }else present(screen,&b,&b.regions); /* A refused without touching the screen */
        assert(!memcmp(screen,b.px,sizeof(screen)));
        shown=b;
    }
    printf("DESKTOP_PIPELINE_MODEL_PASS steps=%u after=%u rebased=%u refused=%u\n",STEPS,outcomes[0],outcomes[1],outcomes[2]);
}
static uint64_t region_area(const struct BrookGpuPresentRegions *r){
    uint64_t a=0;for(uint32_t i=0;i<r->count;++i)a+=(uint64_t)r->regions[i].width*r->regions[i].height;return a;
}
static void fill(uint8_t px[H][W],struct DesktopDamageRect r,uint8_t v){
    for(int64_t y=r.top;y<r.bottom;++y)for(int64_t x=r.left;x<r.right;++x)px[y][x]=v;
}
/* A game rectangle G changes in every frame and each successor is reserved
 * before its predecessor is accepted (the kernel then requires it to cover
 * the predecessor's writes). A one-off change T (a window opening) at step
 * 5 is carried by every later frame unless a successor waits once for a
 * frame whose regions exceed its own damage (shed). The screen must equal
 * every presented frame either way. Returns the presented area at the end. */
static uint64_t chain(int shed){
    static uint8_t screen[H][W];static struct Frame shown,inflight,next;
    memset(screen,0,sizeof(screen));memset(&shown,0,sizeof(shown));
    const struct DesktopDamageRect G={4,4,20,20},T={30,2,62,30};
    const uint64_t slack=desktop_pipeline_carry_slack("16",W,H);
    int busy=0;uint64_t ownArea=0,last=0;unsigned waits=0;
    for(unsigned step=0;step<40;++step){
        const int overlap=busy && !(shed && desktop_pipeline_carry_excess(ownArea,region_area(&inflight.regions),slack));
        if(busy && !overlap){present(screen,&inflight,&inflight.regions);shown=inflight;busy=0;++waits;}
        memcpy(next.px,busy?inflight.px:shown.px,sizeof(next.px));
        struct DesktopDamageSet own={0};
        fill(next.px,G,(uint8_t)(step+1));desktop_damage_add(&own,G,W,H);
        if(step==5){fill(next.px,T,0xee);desktop_damage_add(&own,T,W,H);}
        next.damage=own;
        if(busy)assert(desktop_pipeline_carry(&next.damage,&inflight.regions,W,H));
        next.regions=regions(&next.damage);
        if(busy){ /* the predecessor is accepted after this reservation */
            present(screen,&inflight,&inflight.regions);assert(!memcmp(screen,inflight.px,sizeof(screen)));
            assert(covered(&inflight.regions,&next.regions));
        }
        const struct BrookGpuPresentRegions ownRegions=regions(&own);ownArea=region_area(&ownRegions);
        inflight=next;busy=1;last=region_area(&inflight.regions);
    }
    present(screen,&inflight,&inflight.regions);assert(!memcmp(screen,inflight.px,sizeof(screen)));
    if(shed)assert(waits==1);else assert(!waits);
    return last;
}
int main(void){
    /* Bounded stop drain: never before a stop, expires at 2.5 s. */
    assert(!desktop_stop_drain_expired(0,UINT64_MAX));
    assert(!desktop_stop_drain_expired(1000,1000+DESKTOP_STOP_DRAIN_NS-1));
    assert(desktop_stop_drain_expired(1000,1000+DESKTOP_STOP_DRAIN_NS));
    assert(!desktop_stop_drain_expired(5000,4000));
    assert(desktop_pipeline_parse(NULL)==2 && desktop_pipeline_parse("")==2 && desktop_pipeline_parse("2")==2);
    assert(desktop_pipeline_parse("1")==1 && desktop_pipeline_parse("serial")==1);
    assert(!desktop_pipeline_parse("3") && !desktop_pipeline_parse("22") && !desktop_pipeline_parse("x") && !desktop_pipeline_parse("serialx"));

    struct DesktopPipelineInFlight f[3]={{0}};
    assert(desktop_pipeline_admit(2,f,3,0)==DESKTOP_PIPELINE_START);
    f[1]=(struct DesktopPipelineInFlight){7,1,2,1,0};
    assert(desktop_pipeline_admit(1,f,3,6)==DESKTOP_PIPELINE_WAIT_DEPTH);   /* serial */
    assert(desktop_pipeline_admit(2,f,3,6)==DESKTOP_PIPELINE_OVERLAP);
    assert(desktop_pipeline_admit(2,f,3,0)==DESKTOP_PIPELINE_WAIT_BASELINE);
    f[1].trackValid=0;assert(desktop_pipeline_admit(2,f,3,6)==DESKTOP_PIPELINE_WAIT_BASELINE);f[1].trackValid=1;
    f[1].partial=0;assert(desktop_pipeline_admit(2,f,3,6)==DESKTOP_PIPELINE_WAIT_FULL);f[1].partial=1;
    f[1].regions=0;assert(desktop_pipeline_admit(2,f,3,6)==DESKTOP_PIPELINE_WAIT_FULL);f[1].regions=2;
    f[1].carryExcess=1;assert(desktop_pipeline_admit(2,f,3,6)==DESKTOP_PIPELINE_WAIT_CARRY);
    f[1].partial=0;assert(desktop_pipeline_admit(2,f,3,6)==DESKTOP_PIPELINE_WAIT_FULL);f[1].partial=1;
    assert(desktop_pipeline_admit(1,f,3,6)==DESKTOP_PIPELINE_WAIT_DEPTH);f[1].carryExcess=0;
    f[0]=(struct DesktopPipelineInFlight){8,1,1,1,0};assert(desktop_pipeline_admit(2,f,3,6)==DESKTOP_PIPELINE_WAIT_DEPTH);

    /* Carry slack: default 1/64 of the output (at least 4096), a pixel
     * count, or "off". */
    assert(desktop_pipeline_carry_slack(NULL,3200,1800)==90000 && desktop_pipeline_carry_slack("",64,32)==4096);
    assert(desktop_pipeline_carry_slack("123",3200,1800)==123 && desktop_pipeline_carry_slack("0",3200,1800)==0);
    assert(desktop_pipeline_carry_slack("off",3200,1800)==UINT64_MAX && desktop_pipeline_carry_slack("-5",3200,1800)==90000);
    assert(desktop_pipeline_carry_slack("12x",3200,1800)==90000);
    assert(!desktop_pipeline_carry_excess(100,100,0) && !desktop_pipeline_carry_excess(100,90,0));
    assert(!desktop_pipeline_carry_excess(100,116,16) && desktop_pipeline_carry_excess(100,117,16));
    assert(!desktop_pipeline_carry_excess(0,UINT64_MAX,UINT64_MAX));
    /* A one-off change stays in every overlapped frame unless shed. */
    {const uint64_t kept=chain(0),shed=chain(1);
     assert(kept==16*16+32*28 && shed==16*16);
     printf("DESKTOP_PIPELINE_CARRY_PASS kept_px=%llu shed_px=%llu\n",(unsigned long long)kept,(unsigned long long)shed);}

    const long tickets[3]={9,0,4};assert(desktop_pipeline_oldest(tickets,3)==2);
    const long none[3]={0,0,0};assert(desktop_pipeline_oldest(none,3)==3);

    assert(desktop_pipeline_full_reason(0,0,0,1,100)==DESKTOP_FULL_NOT_FORCED);
    assert(desktop_pipeline_full_reason(1,0,1,1,100)==DESKTOP_FULL_NO_BASELINE);
    assert(desktop_pipeline_full_reason(1,1,0,1,100)==DESKTOP_FULL_IN_FLIGHT);
    assert(desktop_pipeline_full_reason(1,1,1,100,100)==DESKTOP_FULL_NOT_FORCED);
    assert(desktop_pipeline_full_reason(1,1,1,99,100)==DESKTOP_FULL_OTHER);

    struct DesktopDamageSet d={0};struct BrookGpuPresentRegions r={0};
    assert(!desktop_pipeline_carry(&d,&r,W,H));
    r.count=1;r.regions[0]=(struct BrookGpuPresentRegion){0,4,5,0,3,W,0};assert(!desktop_pipeline_carry(&d,&r,W,H) && !d.count);
    r.regions[0].width=6;assert(desktop_pipeline_carry(&d,&r,W,H) && d.count==1 && d.rects[0].left==4 && d.rects[0].right==10 && d.rects[0].bottom==8);

    model();
    puts("DESKTOP_PIPELINE_PASS");
    return 0;
}
