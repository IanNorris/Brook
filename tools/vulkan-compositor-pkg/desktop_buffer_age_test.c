#include "desktop_buffer_age.h"
#include <assert.h>
#include <stdio.h>

static struct DesktopDamageSet one(int64_t l,int64_t t,int64_t r,int64_t b){
    struct DesktopDamageSet s={1,{{l,t,r,b}}};return s;
}
static uint64_t pixels(const struct DesktopDamageSet *s){
    uint64_t p=0;for(uint32_t i=0;i<s->count;++i)p+=(uint64_t)(s->rects[i].right-s->rects[i].left)*(uint64_t)(s->rects[i].bottom-s->rects[i].top);
    return p;
}
int main(void){
    struct DesktopBufferAge a={0};struct DesktopDamageSet out;
    desktop_buffer_age_bind(&a,1,1,3200,1800);
    /* Unknown images draw in full. */
    struct DesktopDamageSet game=one(320,180,2880,1620); /* 2560x1440 game window */
    assert(!desktop_buffer_age_damage(&a,0,&game,&out) && out.count==1 && pixels(&out)==3200ull*1800);
    desktop_buffer_age_commit(&a,0,&game,1);
    assert(desktop_buffer_age_of(&a,0)==1 && !desktop_buffer_age_of(&a,1));
    desktop_buffer_age_commit(&a,1,&game,1);
    desktop_buffer_age_commit(&a,2,&game,1);
    /* Steady state: the game window changes every frame, so an age-3 image
     * repairs exactly the same rectangle - the ring costs no extra drawing. */
    assert(desktop_buffer_age_of(&a,0)==3);
    desktop_buffer_age_commit(&a,0,&game,0); /* frame 4 (own damage = game) */
    desktop_buffer_age_commit(&a,1,&game,0);
    desktop_buffer_age_commit(&a,2,&game,0);
    assert(desktop_buffer_age_damage(&a,0,&game,&out)==1 && pixels(&out)==2560ull*1440);
    /* A cursor move two frames ago is repaired too. */
    struct DesktopDamageSet cursor=one(100,100,148,148);
    desktop_buffer_age_commit(&a,0,&cursor,0);
    desktop_buffer_age_commit(&a,1,&game,0);
    struct DesktopDamageSet none={0};(void)none;
    assert(desktop_buffer_age_damage(&a,2,&game,&out)==1 && out.count==2 && pixels(&out)==2560ull*1440+48*48);
    /* The image holding the newest frame only needs this frame's damage. */
    assert(desktop_buffer_age_damage(&a,1,&cursor,&out)==1 && pixels(&out)==48*48);
    /* Too old for the history: full. */
    for(int i=0;i<DESKTOP_AGE_HISTORY;++i)desktop_buffer_age_commit(&a,1,&cursor,0);
    assert(desktop_buffer_age_of(&a,2)>DESKTOP_AGE_HISTORY && !desktop_buffer_age_damage(&a,2,&cursor,&out));
    /* An empty own damage is a full draw by contract (no partial baseline). */
    assert(!desktop_buffer_age_damage(&a,1,&none,&out));
    /* Union reaching the whole output is reported as full. */
    desktop_buffer_age_commit(&a,0,&cursor,1);
    assert(!desktop_buffer_age_damage(&a,1,&cursor,&out));
    /* Reset and identity changes forget image contents. */
    desktop_buffer_age_commit(&a,2,&cursor,1);
    assert(desktop_buffer_age_of(&a,2)==1);
    desktop_buffer_age_bind(&a,1,2,3200,1800);
    assert(!desktop_buffer_age_of(&a,2) && a.frame);
    desktop_buffer_age_commit(&a,2,&cursor,1);
    desktop_buffer_age_reset(&a);assert(!desktop_buffer_age_of(&a,2) && a.width==0);
    /* Pick: the youngest free image; unknown only when nothing else is free. */
    struct DesktopBufferAge p={0};desktop_buffer_age_bind(&p,1,1,3200,1800);
    desktop_buffer_age_commit(&p,0,&game,1);desktop_buffer_age_commit(&p,1,&game,1);desktop_buffer_age_commit(&p,2,&game,1);
    assert(desktop_buffer_age_pick(&p,0xf,4)==2);
    assert(desktop_buffer_age_pick(&p,0x9,4)==0);   /* 0 (age 3) beats unknown 3 */
    assert(desktop_buffer_age_pick(&p,0x8,4)==3);
    assert(desktop_buffer_age_pick(&p,0x0,4)==-1);
    assert(desktop_buffer_age_pick(&p,0x8,3)==-1);
    puts("desktop buffer age ok");
    return 0;
}
