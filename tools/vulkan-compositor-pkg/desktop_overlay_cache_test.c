#include "desktop_overlay_cache.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"OVERLAY_CACHE line %d: %s\n",__LINE__,#x);abort();}}while(0)

static struct DesktopOverlayFrameState frame(uint64_t generation){
    struct DesktopOverlayFrameState s={1,100,80,generation,{0},{0}};
    s.bands.count=1;s.bands.bands[0]=(struct BrookDesktopOverlayBand){0,70,100,10};
    return s;
}
static int covers(const struct DesktopDamageSet *d,int64_t x,int64_t y){
    for(uint32_t i=0;i<d->count;++i){const struct DesktopDamageRect *r=&d->rects[i];
        if(x>=r->left && x<r->right && y>=r->top && y<r->bottom)return 1;}
    return 0;
}

int main(void){
    /* Cache reuse needs the same nonzero generation and output size. */
    CHECK(desktop_overlay_cache_valid(5,100,80,5,100,80));
    CHECK(!desktop_overlay_cache_valid(5,100,80,6,100,80));
    CHECK(!desktop_overlay_cache_valid(0,100,80,0,100,80));
    CHECK(!desktop_overlay_cache_valid(5,100,80,5,120,80));
    CHECK(!desktop_overlay_cache_valid(5,100,80,0,100,80));

    struct DesktopOverlayFrameState none={0},a=frame(5),b=a;
    struct DesktopDamageSet d={0};
    /* Nothing displayed yet: the whole overlay is damaged. */
    desktop_overlay_damage(&none,&a,&d);CHECK(covers(&d,50,75) && !covers(&d,50,10));
    /* Unchanged generation, bands and hidden pointer: no damage at all. */
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&a,&b,&d);CHECK(d.count==0);
    /* Unknown generation (older kernel) is treated as changed every frame. */
    struct DesktopOverlayFrameState legacy=frame(0),legacyShown=frame(0);
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&legacyShown,&legacy,&d);CHECK(covers(&d,50,75));
    /* New generation with a launcher: old taskbar and new launcher bands. */
    b=frame(6);b.bands.count=2;b.bands.bands[0]=(struct BrookDesktopOverlayBand){0,20,40,30};
    b.bands.bands[1]=(struct BrookDesktopOverlayBand){0,70,100,10};
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&a,&b,&d);CHECK(covers(&d,10,30) && covers(&d,90,75) && !covers(&d,90,10));
    /* Launcher closes: its old band is damaged although no longer drawn. */
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&b,&a,&d);CHECK(covers(&d,10,30) && covers(&d,90,75));
    /* Same generation but changed geometry still damages. */
    struct DesktopOverlayFrameState moved=a;moved.bands.bands[0].y=60;moved.bands.bands[0].height=20;
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&a,&moved,&d);CHECK(covers(&d,50,65));
    /* Pointer motion damages old and new pointer rectangles only. */
    struct DesktopOverlayFrameState p0=a,p1=a;
    p0.cursor=(struct DesktopOverlayCursorState){1,12,16,10,10,1};
    p1.cursor=(struct DesktopOverlayCursorState){1,12,16,40,20,1};
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&p0,&p1,&d);
    CHECK(covers(&d,11,11) && covers(&d,45,25) && !covers(&d,50,75) && !covers(&d,30,5));
    /* An image change in place (busy animation, hover cursor) damages it. */
    p1=p0;p1.cursor.serial=2;d=(struct DesktopDamageSet){0};desktop_overlay_damage(&p0,&p1,&d);CHECK(covers(&d,11,11) && d.count==1);
    /* Hiding the pointer damages where it was; two hidden states are equal. */
    p1=p0;p1.cursor.visible=0;d=(struct DesktopDamageSet){0};desktop_overlay_damage(&p0,&p1,&d);CHECK(covers(&d,11,11));
    struct DesktopOverlayFrameState h0=a,h1=a;h0.cursor.visible=h1.cursor.visible=0;h1.cursor.x=99;
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&h0,&h1,&d);CHECK(d.count==0);
    /* Partly off-screen pointer is clipped, not rejected. */
    p1=p0;p1.cursor.x=-5;p1.cursor.y=-5;d=(struct DesktopDamageSet){0};desktop_overlay_damage(&p0,&p1,&d);CHECK(covers(&d,0,0));
    /* Output size change damages everything new, nothing stale out of bounds. */
    struct DesktopOverlayFrameState big=frame(5);big.width=200;big.height=160;big.bands.bands[0]=(struct BrookDesktopOverlayBand){0,150,200,10};
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&a,&big,&d);CHECK(covers(&d,150,155) && !covers(&d,50,75));

    /* A change stays damaged until a frame containing it is presented. */
    struct DesktopOverlayFrameState shown={0};uint64_t shownTicket=0;
    desktop_overlay_presented(&shown,&shownTicket,&a,0);
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&shown,&b,&d);CHECK(covers(&d,10,30)); /* frame fails */
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&shown,&b,&d);CHECK(covers(&d,10,30)); /* retry still damaged */
    desktop_overlay_presented(&shown,&shownTicket,&b,0);
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&shown,&b,&d);CHECK(d.count==0);
    /* Async: an older ticket retiring late does not regress the state. */
    shown=(struct DesktopOverlayFrameState){0};shownTicket=0;
    desktop_overlay_presented(&shown,&shownTicket,&b,8);desktop_overlay_presented(&shown,&shownTicket,&a,7);
    CHECK(shown.generation==6 && shownTicket==8);
    desktop_overlay_presented(&shown,&shownTicket,&a,9);CHECK(shown.generation==5 && shownTicket==9);
    puts("DESKTOP_OVERLAY_CACHE_PASS");
    return 0;
}
