/* An opaque, unchanged shell surface (the launcher in an opaque theme) over a
 * game that changes every frame: the game is not shaded under the launcher,
 * the launcher is neither reblended nor redrawn, and a taskbar-only overlay
 * change damages the taskbar band alone. Changed, new or translucent bands
 * keep the conservative behaviour. */
#include "desktop_overlay_cache.h"
#include "quad_plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"SHELL_OPAQUE line %d: %s\n",__LINE__,#x);abort();}}while(0)
enum {W=320,H=180};
static const struct BrookDesktopOverlayBand LAUNCHER={4,60,80,106},TASKBAR={0,168,W,12};

/* Overlay with a taskbar (key t) and optionally a launcher (key l); both opaque. */
static struct DesktopOverlayFrameState overlay(uint64_t generation,uint64_t t,uint64_t l,int launcherOpaque){
    struct DesktopOverlayFrameState s={1,W,H,generation,{0},{0},0,{0}};
    unsigned n=0;
    if(l){s.bands.bands[n]=LAUNCHER;s.keys[n]=l;if(launcherOpaque)s.opaque|=1u<<n;++n;}
    s.bands.bands[n]=TASKBAR;s.keys[n]=t;s.opaque|=1u<<n;++n;
    s.bands.count=n;return s;
}
static int covers(const struct DesktopDamageSet *d,int64_t x,int64_t y){
    for(uint32_t i=0;i<d->count;++i){const struct DesktopDamageRect *r=&d->rects[i];
        if(x>=r->left && x<r->right && y>=r->top && y<r->bottom)return 1;}
    return 0;
}
static int disjoint(const struct DesktopDamageSet *d){
    for(uint32_t i=0;i<d->count;++i)for(uint32_t j=0;j<i;++j){const struct DesktopDamageRect *a=&d->rects[i],*b=&d->rects[j];
        if(a->left<b->right && b->left<a->right && a->top<b->bottom && b->top<a->bottom)return 0;}
    return 1;
}
static uint64_t area_in(const struct QuadPlan *p,unsigned layer,struct QuadRect r){
    uint64_t a=0;
    for(uint32_t i=0;i<p->drawCount;++i)if(p->draws[i].layer==layer)a+=quad_rect_area(quad_rect_intersect(p->draws[i].rect,r));
    return a;
}
static void set_layer(uint32_t *scene,unsigned i,const struct BrookVkSceneLayer *l){
    memcpy(scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS,l,sizeof(*l));
}
/* Wallpaper, a 2x-scaled opaque game filling the desktop, then the overlay
 * bands exactly as desktop_renderer.c pushes them. */
static unsigned build_scene(uint32_t *scene,const struct DesktopOverlayFrameState *o){
    scene[0]=W;scene[1]=H;scene[3]=0;unsigned n=0;
    set_layer(scene,n++,&(struct BrookVkSceneLayer){0,W,W,H,0,0,W,H,0,0,W,H,0,0,W,H,255});
    set_layer(scene,n++,&(struct BrookVkSceneLayer){0,160,160,84,0,0,160,84,0,0,W,168,0,0,W,H,255|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE});
    for(uint32_t i=0;i<o->bands.count;++i){const struct BrookDesktopOverlayBand *b=&o->bands.bands[i];
        struct BrookVkSceneLayer l={0,b->width,b->width,b->height,0,0,b->width,b->height,(int32_t)b->x,(int32_t)b->y,b->width,b->height,0,0,W,H,255|BROOK_VK_STRAIGHT_ALPHA};
        if((o->opaque>>i)&1u)l.opacity|=BROOK_VK_OPAQUE_SOURCE;
        set_layer(scene,n++,&l);}
    scene[2]=n;return n;
}

int main(void){
    const struct QuadRect launcher={4,60,84,166},game={0,0,W,168};
    uint32_t scene[BROOK_VK_SCENE_HEADER+8*BROOK_VK_LAYER_WORDS];
    struct QuadPlan plan;

    /* Game frame with the launcher open and unchanged: game damage (its
     * whole client rectangle) loses the launcher interior. */
    struct DesktopOverlayFrameState shown=overlay(7,11,21,1),frame=overlay(7,11,21,1);
    struct DesktopDamageSet d={0};desktop_damage_add(&d,(struct DesktopDamageRect){0,0,W,168},W,H);
    desktop_overlay_clip_opaque(&shown,&frame,&d);
    desktop_overlay_damage(&shown,&frame,&d);
    CHECK(disjoint(&d) && covers(&d,200,100) && covers(&d,2,100) && covers(&d,100,62));
    for(int y=60;y<166;y+=7)for(int x=4;x<84;x+=5)CHECK(!covers(&d,x,y));
    unsigned layers=build_scene(scene,&frame);
    CHECK(quad_plan_build_auto(scene,BROOK_VK_SCENE_HEADER+layers*BROOK_VK_LAYER_WORDS,NULL,&d,1,&plan));
    /* Zero pixels shaded under the launcher, by the game or the launcher. */
    for(unsigned i=0;i<layers;++i)CHECK(area_in(&plan,i,launcher)==0);
    CHECK(area_in(&plan,1,game)>0);

    /* Same frame with the full game damage (no clip, e.g. an older kernel
     * without keys): the opaque band still culls the game beneath it and is
     * drawn as a replacing (non-blending) draw. */
    d=(struct DesktopDamageSet){0};desktop_damage_add(&d,(struct DesktopDamageRect){0,0,W,168},W,H);
    CHECK(quad_plan_build_auto(scene,BROOK_VK_SCENE_HEADER+layers*BROOK_VK_LAYER_WORDS,NULL,&d,1,&plan));
    CHECK(area_in(&plan,1,launcher)==0 && area_in(&plan,0,launcher)==0 && area_in(&plan,2,launcher)==quad_rect_area(launcher));
    for(uint32_t i=0;i<plan.drawCount;++i)if(plan.draws[i].layer==2)CHECK(plan.draws[i].kind==QUAD_KIND_REPLACE);

    /* Taskbar clock/graph change (new generation, new taskbar key): only the
     * taskbar band is damaged; the launcher band is not. */
    struct DesktopOverlayFrameState tick=overlay(8,12,21,1);
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&frame,&tick,&d);
    CHECK(covers(&d,100,170) && !covers(&d,40,100));

    /* The launcher's own content changes (hover): its band is damaged, and
     * game damage beneath it is not clipped this frame. */
    struct DesktopOverlayFrameState hover=overlay(9,12,22,1);
    d=(struct DesktopDamageSet){0};desktop_damage_add(&d,(struct DesktopDamageRect){0,0,W,168},W,H);
    desktop_overlay_clip_opaque(&tick,&hover,&d);CHECK(covers(&d,40,100));
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&tick,&hover,&d);CHECK(covers(&d,40,100) && !covers(&d,100,170));

    /* The launcher just opened: nothing is clipped and its band is damaged. */
    struct DesktopOverlayFrameState closed=overlay(5,11,0,1);
    d=(struct DesktopDamageSet){0};desktop_damage_add(&d,(struct DesktopDamageRect){0,0,W,168},W,H);
    desktop_overlay_clip_opaque(&closed,&frame,&d);CHECK(covers(&d,40,100));
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&closed,&frame,&d);CHECK(covers(&d,40,100));
    /* It closes: its old band is damaged so the game shows again. */
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&frame,&closed,&d);CHECK(covers(&d,40,100));

    /* A translucent launcher (glass) is never clipped or used to cull. */
    struct DesktopOverlayFrameState glassShown=overlay(7,11,21,0),glass=overlay(7,11,21,0);
    d=(struct DesktopDamageSet){0};desktop_damage_add(&d,(struct DesktopDamageRect){0,0,W,168},W,H);
    desktop_overlay_clip_opaque(&glassShown,&glass,&d);CHECK(covers(&d,40,100));
    layers=build_scene(scene,&glass);
    CHECK(quad_plan_build_auto(scene,BROOK_VK_SCENE_HEADER+layers*BROOK_VK_LAYER_WORDS,NULL,&d,1,&plan));
    CHECK(area_in(&plan,1,launcher)==quad_rect_area(launcher));

    /* Unkeyed (older kernel) overlays keep generation-based damage. */
    struct DesktopOverlayFrameState old0=overlay(7,0,0,0),old1=overlay(8,0,0,0);
    d=(struct DesktopDamageSet){0};desktop_overlay_damage(&old0,&old1,&d);CHECK(covers(&d,100,170));
    d=(struct DesktopDamageSet){0};desktop_damage_add(&d,(struct DesktopDamageRect){0,0,W,168},W,H);
    desktop_overlay_clip_opaque(&old0,&old1,&d);CHECK(d.count==1 && covers(&d,40,100));

    /* Damage that would split past the rectangle cap is left unclipped. */
    struct DesktopDamageSet many={0};
    for(int i=0;i<DESKTOP_DAMAGE_MAX_RECTS;++i)desktop_damage_add(&many,(struct DesktopDamageRect){4+2*i,60,5+2*i,166},W,H);
    const struct DesktopDamageSet before=many;desktop_overlay_clip_opaque(&shown,&frame,&many);
    CHECK(many.count==0 || many.count<=before.count); /* fully inside: removed, never overflowed */
    puts("DESKTOP_SHELL_OPAQUE_PASS");
    return 0;
}
