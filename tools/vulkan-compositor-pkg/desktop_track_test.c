/* Host checks for desktop_track.h change-based damage. */
#include "desktop_track.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"DESKTOP_TRACK_FAIL line=%d\n",__LINE__);return 1;}}while(0)
enum {W=640,H=400};
static struct DesktopTrackState a,b;
static uint32_t content[2][200*120],chrome[2][210*140];

static int covered(const struct DesktopDamageSet *d,int64_t x,int64_t y){
    for(uint32_t i=0;i<d->count;++i)if(x>=d->rects[i].left && x<d->rects[i].right && y>=d->rects[i].top && y<d->rects[i].bottom)return 1;
    return 0;
}
static uint64_t area(const struct DesktopDamageSet *d){uint64_t n=0;
    for(uint32_t i=0;i<d->count;++i)n+=(uint64_t)(d->rects[i].right-d->rects[i].left)*(uint64_t)(d->rects[i].bottom-d->rects[i].top);
    return n;}
/* One CPU window (200x120 at 2x, at 100,50) with chrome around it, and one
 * GPU window. */
static int build(struct DesktopTrackState *s,unsigned buffer,int32_t x,uint64_t commit,int gpuFirst){
    memset(s,0,sizeof(*s));s->width=W;s->height=H;s->display=7;s->backdrop=3;s->count=2;
    struct DesktopTrackWindow *cpu=&s->windows[gpuFirst?1:0],*gpu=&s->windows[gpuFirst?0:1];
    *cpu=(struct DesktopTrackWindow){.window=1,.incarnation=11,.direct=0,.hasChrome=1,.chromeOpacity=255};
    const struct BrookVkSceneLayer l={0,200,200,120,0,0,200,120,x,50,400,240,x,50,400,240,255|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE};
    memcpy(cpu->layer,&l,sizeof(l));
    cpu->chromeX=x-4;cpu->chromeY=20;cpu->chromeWidth=408;cpu->chromeHeight=274;
    CHECK(desktop_track_bands(&cpu->content,content[buffer],200*120,200,0,0,200,120,NULL));
    const struct DesktopTrackHole hole={4,30,200,100};
    CHECK(desktop_track_bands(&cpu->chrome,chrome[buffer],210*140,210,0,0,210,140,&hole));
    *gpu=(struct DesktopTrackWindow){.window=2,.incarnation=5,.direct=1,.commit=commit};
    const struct BrookVkSceneLayer g={1234,64,64,64,0,0,64,64,500,300,64,64,0,0,W,H,255};
    memcpy(gpu->layer,&g,sizeof(g));
    s->valid=1;return 0;
}
int main(void){
    for(unsigned i=0;i<200*120;++i)content[0][i]=content[1][i]=(uint32_t)rand();
    for(unsigned i=0;i<210*140;++i)chrome[0][i]=chrome[1][i]=(uint32_t)rand();
    struct DesktopDamageSet d={0};
    CHECK(!build(&a,0,100,9,0) && !build(&b,1,100,9,0));
    CHECK(desktop_track_diff(&a,&b,&d) && d.count==0);
    /* Invalid or mismatched baselines are not usable. */
    a.valid=0;CHECK(!desktop_track_diff(&a,&b,&d));a.valid=1;
    a.backdrop=4;CHECK(!desktop_track_diff(&a,&b,&d));a.backdrop=3;
    /* A new GPU commit damages exactly its layer. */
    CHECK(!build(&b,1,100,10,0));d.count=0;CHECK(desktop_track_diff(&a,&b,&d));
    CHECK(d.count==1 && d.rects[0].left==500 && d.rects[0].top==300 && d.rects[0].right==564 && d.rects[0].bottom==364);
    /* A buffer offset change alone (same commit) is not damage. */
    CHECK(!build(&b,1,100,9,0));b.windows[1].layer[0]=99;d.count=0;CHECK(desktop_track_diff(&a,&b,&d) && !d.count);
    /* One changed CPU source row: damage covers its two output rows and is
     * one band (8 source rows -> 16 output rows, plus rounding) high. */
    content[1][57*200+33]^=1;CHECK(!build(&b,1,100,9,0));d.count=0;CHECK(desktop_track_diff(&a,&b,&d));
    CHECK(d.count==1 && covered(&d,100+66,50+114) && covered(&d,100+67,50+115) && area(&d)<=400*20);
    CHECK(d.rects[0].left==100 && d.rects[0].right==500);
    content[1][57*200+33]^=1;
    /* Brute force: every output row sampling a changed source row is
     * covered, for every source row of the scaled layer. */
    for(unsigned row=0;row<120;row+=7){content[1][row*200]^=0x100;CHECK(!build(&b,1,100,9,0));d.count=0;CHECK(desktop_track_diff(&a,&b,&d));
        for(uint32_t v=0;v<240;++v)if((2u*v+1u)*120u/(2u*240u)==row)CHECK(covered(&d,100,50+v) && covered(&d,499,50+v));
        content[1][row*200]^=0x100;}
    /* Chrome: a title change damages its band; a change in the transparent
     * interior does not. */
    chrome[1][10*210+50]^=1;CHECK(!build(&b,1,100,9,0));d.count=0;CHECK(desktop_track_diff(&a,&b,&d));
    CHECK(d.count==1 && d.rects[0].top==28 && d.rects[0].bottom==36 && d.rects[0].left==96 && d.rects[0].right==504);
    chrome[1][10*210+50]^=1;
    chrome[1][60*210+100]^=1;CHECK(!build(&b,1,100,9,0));d.count=0;CHECK(desktop_track_diff(&a,&b,&d) && !d.count);
    chrome[1][60*210+100]^=1;
    chrome[1][60*210+2]^=1;CHECK(!build(&b,1,100,9,0));d.count=0;CHECK(desktop_track_diff(&a,&b,&d) && d.count==1);
    chrome[1][60*210+2]^=1;
    /* A move damages old and new footprints. */
    CHECK(!build(&b,1,140,9,0));d.count=0;CHECK(desktop_track_diff(&a,&b,&d));
    CHECK(covered(&d,96,20) && covered(&d,543,293) && !covered(&d,600,10));
    /* Restacking damages everything the windows cover, before and after. */
    CHECK(!build(&b,1,100,9,1));d.count=0;CHECK(desktop_track_diff(&a,&b,&d));
    CHECK(covered(&d,96,20) && covered(&d,563,363) && !covered(&d,10,390));
    /* A window closing (count change) damages its footprint. */
    CHECK(!build(&b,1,100,9,0));b.count=1;d.count=0;CHECK(desktop_track_diff(&a,&b,&d));
    CHECK(covered(&d,530,330) && covered(&d,96,20));
    /* CPU <-> GPU source switch counts as a geometry change. */
    CHECK(!build(&b,1,100,9,0));b.windows[0].direct=1;d.count=0;CHECK(desktop_track_diff(&a,&b,&d) && covered(&d,300,200));
    /* Opacity change. */
    CHECK(!build(&b,1,100,9,0));b.windows[0].layer[16]=128|BROOK_VK_STRAIGHT_ALPHA;d.count=0;CHECK(desktop_track_diff(&a,&b,&d) && covered(&d,300,200));
    /* Hole helper matches desktop_chrome_spans' clipping. */
    const struct BrookDesktopLayer client={.window=1,.incarnation=2,.clientX=104,.clientY=50,.clientWidth=400,.clientHeight=240};
    const struct BrookDesktopLayer frame={.window=1,.incarnation=2,.clientX=100,.clientY=20,.width=408,.height=274};
    const struct DesktopTrackHole h=desktop_track_chrome_hole(&client,&frame);
    CHECK(h.x==4 && h.y==30 && h.width==400 && h.height==240);
    printf("DESKTOP_TRACK_PASS\n");
    return 0;
}
