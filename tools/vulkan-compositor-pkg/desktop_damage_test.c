#include "desktop_damage.h"
#include "desktop_retry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int covers(struct DesktopDamageSet s,int x,int y){
    for(uint32_t i=0;i<s.count;++i){struct DesktopDamageRect r=s.rects[i];
        if(x>=r.left && x<r.right && y>=r.top && y<r.bottom)return 1;}
    return 0;
}
enum { SCREEN_W=64, SCREEN_H=48, MAX_LAYERS=7 };
struct TestLayer {int x,y,w,h;uint32_t color,alpha;};
static uint32_t randomState=0x8341257u;
static uint32_t next_random(void){randomState^=randomState<<13;randomState^=randomState>>17;randomState^=randomState<<5;return randomState;}
static uint32_t compose_pixel(int x,int y,uint32_t backdrop,const struct TestLayer *layers,unsigned count){
    uint32_t result=backdrop;
    for(unsigned i=0;i<count;++i){const struct TestLayer *l=&layers[i];
        if(x<l->x || y<l->y || x>=l->x+l->w || y>=l->y+l->h)continue;
        uint32_t blended=0;for(unsigned c=0;c<3;++c){
            uint32_t src=(l->color>>(8*c))&255,dst=(result>>(8*c))&255;
            blended|=((src*l->alpha+dst*(255-l->alpha)+127)/255)<<(8*c);
        }
        result=0xff000000u|blended;
    }
    return result;
}
static void successive_frames(void){
    struct DesktopDamageState state={0};uint32_t presented[SCREEN_W*SCREEN_H]={0};
    uint64_t backdropGeneration=1;uint32_t backdrop=0xff142638;
    for(unsigned frame=0;frame<2500;++frame){
        struct TestLayer layers[MAX_LAYERS];const unsigned count=next_random()%(MAX_LAYERS+1);
        struct DesktopDamageSet footprint={0};
        for(unsigned i=0;i<count;++i){struct TestLayer *l=&layers[i];
            l->x=(int)(next_random()%100)-20;l->y=(int)(next_random()%80)-16;
            l->w=1+(int)(next_random()%30);l->h=1+(int)(next_random()%25);
            l->color=next_random()&0xffffffu;l->alpha=next_random()%256;
            desktop_damage_add(&footprint,(struct DesktopDamageRect){l->x,l->y,l->x+l->w,l->y+l->h},SCREEN_W,SCREEN_H);
        }
        if(frame && frame%113==0){++backdropGeneration;backdrop=0xff000000u|(next_random()&0xffffffu);}
        struct DesktopDamageSet damage=desktop_damage_current(state,1,backdropGeneration,SCREEN_W,SCREEN_H,footprint);
        for(int y=0;y<SCREEN_H;++y)for(int x=0;x<SCREEN_W;++x){
            const uint32_t full=compose_pixel(x,y,backdrop,layers,count);
            if(covers(damage,x,y))presented[y*SCREEN_W+x]=full;
            assert(presented[y*SCREEN_W+x]==full);
        }
        desktop_damage_commit(&state,1,backdropGeneration,SCREEN_W,SCREEN_H,footprint);
        // A failed partial frame discards the renderer's base and forces the
        // next successful frame to reconstruct the full screen.
        if(frame%97==0)memset(&state,0,sizeof(state));
    }
    puts("DESKTOP_DAMAGE_SUCCESSIVE_FRAMES_PASS");
}
static void overlapping_frames(void){
    const uint32_t backdrop=0xff142638;
    for(unsigned trial=0;trial<1000;++trial){
        struct TestLayer layers[3][MAX_LAYERS];unsigned count[3];struct DesktopDamageSet footprint[3]={{0}};
        for(unsigned f=0;f<3;++f){
            count[f]=next_random()%(MAX_LAYERS+1);
            for(unsigned i=0;i<count[f];++i){struct TestLayer *l=&layers[f][i];
                l->x=(int)(next_random()%100)-20;l->y=(int)(next_random()%80)-16;
                l->w=1+(int)(next_random()%30);l->h=1+(int)(next_random()%25);
                l->color=next_random()&0xffffffu;l->alpha=next_random()%256;
                desktop_damage_add(&footprint[f],(struct DesktopDamageRect){l->x,l->y,l->x+l->w,l->y+l->h},SCREEN_W,SCREEN_H);
            }
        }
        struct DesktopDamageState base={0},older={0},combined;
        desktop_damage_commit(&base,1,1,SCREEN_W,SCREEN_H,footprint[0]);
        desktop_damage_commit(&older,1,1,SCREEN_W,SCREEN_H,footprint[1]);
        struct DesktopDamageSet first=desktop_damage_current(base,1,1,SCREEN_W,SCREEN_H,footprint[1]);
        combined=base;assert(desktop_damage_include_pending(&combined,older));
        struct DesktopDamageSet second=desktop_damage_current(combined,1,1,SCREEN_W,SCREEN_H,footprint[2]);
        for(int y=0;y<SCREEN_H;++y)for(int x=0;x<SCREEN_W;++x){
            const uint32_t initial=compose_pixel(x,y,backdrop,layers[0],count[0]);
            const uint32_t middle=compose_pixel(x,y,backdrop,layers[1],count[1]);
            const uint32_t expected=compose_pixel(x,y,backdrop,layers[2],count[2]);
            uint32_t ordered=covers(first,x,y)?middle:initial;
            if(covers(second,x,y))ordered=expected;
            assert(ordered==expected);
            /* If the newer ticket presents first, the kernel rejects the old
             * ticket; it must also reconstruct correctly from the baseline. */
            assert((covers(second,x,y)?expected:initial)==expected);
        }
    }
    puts("DESKTOP_DAMAGE_OVERLAPPING_FRAMES_PASS");
}
int main(void){
    struct DesktopRetry retry={0};
    assert(!desktop_retry_waiting(&retry,100));
    desktop_retry_failure(&retry,ENOTSUP,1,100);
    assert(desktop_retry_waiting(&retry,100) && desktop_retry_waiting(&retry,50000099));
    assert(!desktop_retry_waiting(&retry,50000100));
    desktop_retry_failure(&retry,ENOTSUP,0,100);assert(!desktop_retry_waiting(&retry,99));
    desktop_retry_failure(&retry,ENOTSUP,0,100);assert(!desktop_retry_waiting(&retry,0));
    desktop_retry_failure(&retry,ENOTSUP,0,100);desktop_retry_success(&retry);assert(!desktop_retry_waiting(&retry,101));
    desktop_retry_failure(&retry,EAGAIN,0,100);assert(!desktop_retry_waiting(&retry,101));
    desktop_retry_failure(&retry,EBUSY,1,100);assert(!desktop_retry_waiting(&retry,101));
    desktop_retry_failure(&retry,EBUSY,0,100);assert(desktop_retry_waiting(&retry,101));
    assert(desktop_sync_retry_delay_us(ENOTSUP)==50000);
    assert(desktop_sync_retry_delay_us(EBUSY)==50000);
    assert(desktop_sync_retry_delay_us(EAGAIN)==10000);
    assert(desktop_sync_retry_delay_us(ESTALE)==10000);
    struct DesktopDamageState state={0};struct DesktopDamageSet first={0};
    desktop_damage_add(&first,(struct DesktopDamageRect){-5,10,20,30},100,80);
    desktop_damage_add(&first,(struct DesktopDamageRect){0,70,100,80},100,80);
    assert(first.count==2);
    struct DesktopDamageSet full=desktop_damage_current(state,1,2,100,80,first);
    assert(full.count==1 && covers(full,99,0));
    desktop_damage_commit(&state,1,2,100,80,first);
    struct DesktopDamageSet moved={0};desktop_damage_add(&moved,(struct DesktopDamageRect){30,20,50,40},100,80);
    struct DesktopDamageSet damage=desktop_damage_current(state,1,2,100,80,moved);
    assert(damage.count==3 && covers(damage,0,10) && covers(damage,49,39) && covers(damage,99,79));
    assert(!covers(damage,80,10));
    desktop_damage_add(&moved,(struct DesktopDamageRect){19,15,35,25},100,80);
    damage=desktop_damage_current(state,1,2,100,80,moved);
    assert(damage.count==2 && covers(damage,0,10) && covers(damage,49,39));
    desktop_damage_commit(&state,1,2,100,80,moved);
    struct DesktopDamageSet empty={0};damage=desktop_damage_current(state,1,2,100,80,empty);
    assert(covers(damage,49,39));
    full=desktop_damage_current(state,1,3,100,80,empty);assert(full.count==1 && covers(full,99,79));
    full=desktop_damage_current(state,2,2,100,80,empty);assert(full.count==1 && covers(full,99,79));
    /* A successor may retire after an older in-flight move; its damage must
     * erase both the displayed position and that intermediate position. */
    struct DesktopDamageState overlap={0},older={0};
    struct DesktopDamageSet middle={0},newest={0};
    desktop_damage_add(&middle,(struct DesktopDamageRect){55,10,65,20},100,80);
    desktop_damage_add(&newest,(struct DesktopDamageRect){75,10,85,20},100,80);
    desktop_damage_commit(&overlap,1,2,100,80,first);
    desktop_damage_commit(&older,1,2,100,80,middle);
    assert(desktop_damage_include_pending(&overlap,older));
    damage=desktop_damage_current(overlap,1,2,100,80,newest);
    assert(covers(damage,0,10) && covers(damage,60,15) && covers(damage,80,15));
    older.backdrop=3;assert(!desktop_damage_include_pending(&overlap,older));
    older.backdrop=2;older.width=99;assert(!desktop_damage_include_pending(&overlap,older));
    struct DesktopDamageState noBaseline={0};assert(!desktop_damage_include_pending(&noBaseline,older));
    assert(desktop_async_damage_partial_allowed(1,0));
    assert(!desktop_async_damage_partial_allowed(1,1) && !desktop_async_damage_partial_allowed(0,0));
    assert(desktop_async_damage_grace(1,1,100,200));
    assert(!desktop_async_damage_grace(1,1,100,40000100));
    assert(!desktop_async_damage_grace(1,1,100,99));
    assert(!desktop_async_damage_grace(1,1,0,100));
    assert(!desktop_async_damage_grace(0,1,100,200) && !desktop_async_damage_grace(1,0,100,200));
    struct DesktopDamageState displayed=state,submitted={0};
    desktop_damage_commit(&submitted,1,2,100,80,first);
    desktop_async_damage_retire(&displayed,submitted,1,1);
    damage=desktop_damage_current(displayed,1,2,100,80,moved);
    assert(covers(damage,0,10) && covers(damage,49,39));
    desktop_async_damage_retire(&displayed,submitted,1,0);
    full=desktop_damage_current(displayed,1,2,100,80,moved);assert(full.count==1 && covers(full,99,79));
    desktop_async_damage_retire(&displayed,submitted,0,1);
    desktop_async_damage_retire(&displayed,state,0,0);assert(displayed.display==submitted.display && displayed.previous.count==submitted.previous.count);
    struct DesktopDamageSet many={0};for(int i=0;i<33;++i)
        desktop_damage_add(&many,(struct DesktopDamageRect){i*3,0,i*3+1,1},100,80);
    assert(many.count==1 && covers(many,99,79));
    successive_frames();overlapping_frames();puts("DESKTOP_DAMAGE_PASS");return 0;
}
