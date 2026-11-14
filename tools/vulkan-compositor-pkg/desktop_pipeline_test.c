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
int main(void){
    assert(desktop_pipeline_parse(NULL)==2 && desktop_pipeline_parse("")==2 && desktop_pipeline_parse("2")==2);
    assert(desktop_pipeline_parse("1")==1 && desktop_pipeline_parse("serial")==1);
    assert(!desktop_pipeline_parse("3") && !desktop_pipeline_parse("22") && !desktop_pipeline_parse("x") && !desktop_pipeline_parse("serialx"));

    struct DesktopPipelineInFlight f[3]={{0}};
    assert(desktop_pipeline_admit(2,f,3,0)==DESKTOP_PIPELINE_START);
    f[1]=(struct DesktopPipelineInFlight){7,1,2,1};
    assert(desktop_pipeline_admit(1,f,3,6)==DESKTOP_PIPELINE_WAIT_DEPTH);   /* serial */
    assert(desktop_pipeline_admit(2,f,3,6)==DESKTOP_PIPELINE_OVERLAP);
    assert(desktop_pipeline_admit(2,f,3,0)==DESKTOP_PIPELINE_WAIT_BASELINE);
    f[1].trackValid=0;assert(desktop_pipeline_admit(2,f,3,6)==DESKTOP_PIPELINE_WAIT_BASELINE);f[1].trackValid=1;
    f[1].partial=0;assert(desktop_pipeline_admit(2,f,3,6)==DESKTOP_PIPELINE_WAIT_FULL);f[1].partial=1;
    f[1].regions=0;assert(desktop_pipeline_admit(2,f,3,6)==DESKTOP_PIPELINE_WAIT_FULL);f[1].regions=2;
    f[0]=(struct DesktopPipelineInFlight){8,1,1,1};assert(desktop_pipeline_admit(2,f,3,6)==DESKTOP_PIPELINE_WAIT_DEPTH);

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
