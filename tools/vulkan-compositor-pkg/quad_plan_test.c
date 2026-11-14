/* Host checks for quad_plan.h: occlusion, damage clipping, holes, and an
 * oracle comparison of the planned draws (with exact integer emulation of
 * fixed-function blending) against composite.comp's per-pixel loop. */
#include "quad_plan.h"
#include "quad_reference.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"QUAD_PLAN_FAIL line=%d\n",__LINE__);return 1;}}while(0)

static uint32_t rng=0x12345678u;
static uint32_t next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static uint32_t pick(uint32_t n){return n?next()%n:0;}

static void set_layer(uint32_t *scene,unsigned i,const struct BrookVkSceneLayer *l){
    memcpy(scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS,l,sizeof(*l));
}
static int draws_inside(const struct QuadPlan *p,const struct DesktopDamageSet *d){
    for(uint32_t i=0;i<p->drawCount;++i){int inside=0;
        for(uint32_t r=0;r<d->count;++r){const struct DesktopDamageRect *x=&d->rects[r];
            const struct QuadRect q={(int32_t)x->left,(int32_t)x->top,(int32_t)x->right,(int32_t)x->bottom};
            if(quad_rect_contains(q,p->draws[i].rect))inside=1;}
        if(!inside)return 0;}
    return 1;
}
static int layer_draws_intersect(const struct QuadPlan *p,unsigned layer,struct QuadRect r){
    for(uint32_t i=0;i<p->drawCount;++i)if(p->draws[i].layer==layer && !quad_rect_empty(quad_rect_intersect(p->draws[i].rect,r)))return 1;
    return 0;
}

/* The desktop: wallpaper, opaque 2x game, transparent-interior chrome,
 * a taskbar band and a cursor. */
static int desktop_scene(void){
    enum {W=320,H=180};
    uint32_t scene[BROOK_VK_SCENE_HEADER+5*BROOK_VK_LAYER_WORDS]={W,H,5,0};
    set_layer(scene,0,&(struct BrookVkSceneLayer){0,W,W,H,0,0,W,H,0,0,W,H,0,0,W,H,255});
    set_layer(scene,1,&(struct BrookVkSceneLayer){0,64,64,36,0,0,64,36,40,30,128,72,0,0,W,H,255|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE});
    set_layer(scene,2,&(struct BrookVkSceneLayer){0,132,132,80,0,0,132,80,38,22,132,80,0,0,W,H,255|BROOK_VK_STRAIGHT_ALPHA});
    set_layer(scene,3,&(struct BrookVkSceneLayer){0,W,W,10,0,0,W,10,0,170,W,10,0,0,W,H,255|BROOK_VK_STRAIGHT_ALPHA});
    set_layer(scene,4,&(struct BrookVkSceneLayer){0,16,12,12,0,0,12,12,100,60,12,12,0,0,W,H,255|BROOK_VK_STRAIGHT_ALPHA});
    struct QuadRect holes[5]={{0}};holes[2]=(struct QuadRect){40,30,168,102};
    const struct QuadRect game={40,30,168,102};
    struct DesktopDamageSet full={1,{{0,0,W,H}}};
    static struct QuadPlan plan;
    CHECK(quad_plan_build(scene,sizeof(scene)/4,holes,&full,1,&plan));
    /* The wallpaper and the chrome are never drawn under the game, so the
     * whole game rectangle is shaded once, by the game, plus the cursor. */
    CHECK(!layer_draws_intersect(&plan,0,game) && !layer_draws_intersect(&plan,2,game));
    CHECK(plan.layersDrawn==5 && !plan.clearCount && plan.damagePixels==(uint64_t)W*H);
    CHECK(plan.pixelsShaded==(uint64_t)W*H+(132*80-128*72)+(uint64_t)W*10+12*12);
    for(uint32_t i=0;i<plan.drawCount;++i)if(plan.draws[i].layer==0)CHECK(plan.draws[i].kind==QUAD_KIND_OVER_ZERO);
    for(uint32_t i=0;i<plan.drawCount;++i)if(plan.draws[i].layer==1)CHECK(plan.draws[i].kind==QUAD_KIND_REPLACE);
    /* Without occlusion and holes everything under the game is drawn too. */
    static struct QuadPlan plain;
    CHECK(quad_plan_build(scene,sizeof(scene)/4,NULL,&full,0,&plain));
    CHECK(plain.pixelsShaded==(uint64_t)W*H+128*72+132*80+W*10+12*12);
    /* A cursor-sized damage over the game draws only the game and cursor
     * there; one over the taskbar draws wallpaper, taskbar and nothing else. */
    struct DesktopDamageSet cursor={2,{{96,56,116,76},{0,168,32,180}}};
    CHECK(quad_plan_build(scene,sizeof(scene)/4,holes,&cursor,1,&plan));
    CHECK(draws_inside(&plan,&cursor) && plan.damagePixels==20*20+32*12);
    CHECK(plan.used[0] && plan.used[1] && !plan.used[2] && plan.used[3] && plan.used[4] && plan.layersDrawn==4);
    CHECK(!layer_draws_intersect(&plan,0,game));
    CHECK(plan.pixelsShaded==20*20+12*12+32*12+32*10);
    /* Overlapping damage is rejected; so is damage outside the output. */
    struct DesktopDamageSet overlap={2,{{0,0,10,10},{5,5,20,20}}};
    CHECK(!quad_plan_build(scene,sizeof(scene)/4,holes,&overlap,1,&plan));
    struct DesktopDamageSet outside={1,{{0,0,W+1,10}}};
    CHECK(!quad_plan_build(scene,sizeof(scene)/4,holes,&outside,1,&plan));
    /* No base layer coverage: the region is cleared before blending. */
    uint32_t partial[BROOK_VK_SCENE_HEADER+BROOK_VK_LAYER_WORDS]={W,H,1,0};
    set_layer(partial,0,&(struct BrookVkSceneLayer){0,10,10,10,0,0,10,10,5,5,10,10,0,0,W,H,128|BROOK_VK_STRAIGHT_ALPHA});
    struct DesktopDamageSet small={1,{{0,0,20,20}}};
    CHECK(quad_plan_build(partial,sizeof(partial)/4,NULL,&small,1,&plan));
    CHECK(plan.clearCount==1 && plan.drawCount==1 && plan.draws[0].kind==QUAD_KIND_STRAIGHT && plan.pixelsShaded==100);
    return 0;
}

/* Random scenes against the composite.comp oracle. Output pixels outside
 * the damage must be untouched; inside it every pixel must be exact. */
enum {RW=72,RH=48,MAXL=8};
static uint32_t pixels[MAXL][64*64];
static int random_scene(unsigned iteration){
    uint32_t scene[BROOK_VK_SCENE_HEADER+MAXL*BROOK_VK_LAYER_WORDS]={RW,RH,0,0};
    struct QuadRect holes[MAXL];memset(holes,0,sizeof(holes));
    const uint32_t count=1+pick(MAXL);scene[2]=count;
    const uint32_t *sources[MAXL];
    for(uint32_t i=0;i<count;++i){
        uint32_t sw=1+pick(40),sh=1+pick(40),stride=sw+pick(3);
        uint32_t scale=pick(4),dw=sw,dh=sh;
        if(scale==1){dw=2*sw;dh=2*sh;}
        else if(scale==2){dw=3*sw;dh=sh;}
        else if(scale==3){dw=1+pick(60);dh=1+pick(60);}
        uint32_t flags=pick(3)==0?0:BROOK_VK_STRAIGHT_ALPHA;
        if(flags && pick(3)==0)flags|=BROOK_VK_OPAQUE_SOURCE;
        uint32_t opacity=pick(4)==0?255:pick(4)==0?0:pick(256);
        int32_t x=(int32_t)pick(RW+20)-10,y=(int32_t)pick(RH+20)-10;
        struct BrookVkSceneLayer l={0,stride,sw,sh,0,0,sw,sh,x,y,dw,dh,0,0,RW,RH,opacity|flags};
        if(pick(3)==0){l.clipX=(int32_t)pick(RW);l.clipY=(int32_t)pick(RH);l.clipWidth=pick(RW);l.clipHeight=pick(RH);}
        if(i==0 && pick(2)){l=(struct BrookVkSceneLayer){0,RW,RW,RH,0,0,RW,RH,0,0,RW,RH,0,0,RW,RH,255};stride=RW;sw=RW;sh=RH;}
        for(uint32_t k=0;k<64*64;++k)pixels[i][k]=next();
        if(!(l.opacity&BROOK_VK_STRAIGHT_ALPHA))for(uint32_t k=0;k<64*64;++k){
            /* Mostly valid premultiplied pixels, occasionally invalid ones. */
            uint32_t p=pixels[i][k],a=p>>24;if(pick(8)){uint32_t c[3];
                for(unsigned j=0;j<3;++j){c[j]=((p>>(8*j))&255u);if(c[j]>a)c[j]=a;}
                pixels[i][k]=c[0]|(c[1]<<8)|(c[2]<<16)|(a<<24);}
        }
        /* A transparent interior for some unscaled straight layers. */
        if((l.opacity&BROOK_VK_STRAIGHT_ALPHA) && !(l.opacity&BROOK_VK_OPAQUE_SOURCE) && scale==0 && sw>4 && sh>4 && pick(2)){
            const uint32_t hx=1+pick(sw/2),hy=1+pick(sh/2),hw=1+pick(sw-hx-1),hh=1+pick(sh-hy-1);
            for(uint32_t r=hy;r<hy+hh;++r)for(uint32_t c=hx;c<hx+hw;++c)pixels[i][r*stride+c]&=0x00ffffffu;
            holes[i]=(struct QuadRect){x+(int32_t)hx,y+(int32_t)hy,x+(int32_t)(hx+hw),y+(int32_t)(hy+hh)};
        }
        set_layer(scene,i,&l);sources[i]=pixels[i];
    }
    struct DesktopDamageSet damage={0};
    const unsigned rects=1+pick(5);
    for(unsigned r=0;r<rects;++r){int64_t x=pick(RW),y=pick(RH);
        desktop_damage_add(&damage,(struct DesktopDamageRect){x,y,x+1+pick(RW),y+1+pick(RH)},RW,RH);}
    if(pick(6)==0){damage.count=1;damage.rects[0]=(struct DesktopDamageRect){0,0,RW,RH};}
    static struct QuadPlan plan;
    const int occlusion=iteration%4!=3;
    CHECK(quad_plan_build_auto(scene,sizeof(scene)/4,holes,&damage,occlusion,&plan));
    CHECK(draws_inside(&plan,&damage));
    uint32_t expected[RW*RH],actual[RW*RH],garbage[RW*RH];
    for(uint32_t k=0;k<RW*RH;++k)actual[k]=garbage[k]=next();
    for(uint32_t y=0;y<RH;++y)for(uint32_t x=0;x<RW;++x)expected[y*RW+x]=quad_reference_pixel(scene,sources,x,y);
    quad_reference_execute(scene,sources,&plan,actual,RW);
    for(uint32_t y=0;y<RH;++y)for(uint32_t x=0;x<RW;++x){
        int damaged=0;for(uint32_t r=0;r<damage.count;++r){const struct DesktopDamageRect *d=&damage.rects[r];
            if(x>=d->left && x<d->right && y>=d->top && y<d->bottom)damaged=1;}
        const uint32_t want=damaged?expected[y*RW+x]:garbage[y*RW+x];
        if(actual[y*RW+x]!=want){
            fprintf(stderr,"QUAD_PLAN_ORACLE_MISMATCH iteration=%u x=%u y=%u damaged=%d got=%08x want=%08x layers=%u\n",
                iteration,x,y,damaged,actual[y*RW+x],want,count);
            return 1;
        }
    }
    return 0;
}

int main(void){
    if(desktop_scene())return 1;
    for(unsigned i=0;i<4000;++i)if(random_scene(i))return 1;
    printf("QUAD_PLAN_PASS iterations=4000\n");
    return 0;
}
