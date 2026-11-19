/* Surface records in the renderer (stage 1.3/1.4), against the CPU oracle:
 *  - shadow quads from a record are valid scene layers whose composition
 *    (quad_reference.h) equals a direct nine-slice raster of the shared
 *    shadow source, and the window drawn over them hides them;
 *  - records are validated and matched by identity;
 *  - frame spans marked opaque from a record change no pixel but let the
 *    quad plan skip what lies beneath them;
 *  - a moved or changed shadow damages its old and new bounds. */
#include "desktop_surfaces.h"
#include "desktop_track.h"
#include "desktop_chrome_spans.h"
#include "quad_plan.h"
#include "quad_reference.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"SURFACES line %d: %s\n",__LINE__,#x);abort();}}while(0)
enum {W=200,H=150};
static uint32_t shadow[64*64],background[W*H],client[60*40],chrome[64*52];

static struct BrookDesktopSurface frame_record(int32_t x,int32_t y,uint32_t w,uint32_t h,uint32_t alpha,uint32_t spread,uint32_t corner){
    struct BrookDesktopSurface s;memset(&s,0,sizeof(s));
    s.kind=BROOK_DESKTOP_SURFACE_FRAME;s.window=7;s.incarnation=70;s.key=5;s.x=x;s.y=y;s.width=w;s.height=h;
    s.opacity=255;s.flags=BROOK_DESKTOP_SURFACE_OPAQUE;s.shadow=BROOK_DESKTOP_SHADOW_WINDOW;s.shadowAlpha=alpha;
    s.shadowSpread=spread;s.shadowCorner=corner;return s;
}
static void set_layer(uint32_t *scene,unsigned i,const struct BrookVkSceneLayer *l){
    memcpy(scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS,l,sizeof(*l));
}
/* Independent nine-slice raster of one shadow pixel: which slice the pixel
 * falls in, then nearest sampling of that slice's source span. */
static int direct_shadow(const struct BrookDesktopSurface *s,int32_t x,int32_t y,uint32_t *out){
    const int64_t left=(int64_t)s->x-s->shadowSpread,top=(int64_t)s->y-s->shadowSpread;
    const int64_t w=(int64_t)s->width+2*s->shadowSpread,h=(int64_t)s->height+2*s->shadowSpread,c=s->shadowCorner;
    if(x<left || y<top || x>=left+w || y>=top+h || w<2*c || h<2*c)return 0;
    const int64_t u=x-left,v=y-top;
    const int col=u<c?0:u>=w-c?2:1,row=v<c?0:v>=h-c?2:1;
    if(col==1 && row==1)return 0;
    const int64_t start[3]={0,c,w-c},startY[3]={0,c,h-c},len[3]={c,w-2*c,c},lenY[3]={c,h-2*c,c};
    static const uint32_t src[3]={0,24,40},span[3]={24,16,24};
    const int64_t du=u-start[col],dv=v-startY[row];
    const uint32_t sx=src[col]+(uint32_t)(span[col]==(uint32_t)len[col]?du:(2*du+1)*span[col]/(2*len[col]));
    const uint32_t sy=src[row]+(uint32_t)(span[row]==(uint32_t)lenY[row]?dv:(2*dv+1)*span[row]/(2*lenY[row]));
    *out=shadow[sy*64+sx];return 1;
}
int main(void){
    desktop_shadow_source(shadow);
    for(unsigned i=0;i<W*H;++i)background[i]=0xff000000u|(i*2654435761u>>8);
    for(unsigned i=0;i<60*40;++i)client[i]=0xff000000u|(i*40503u);
    /* Validation and lookup. */
    {
        struct BrookDesktopSurfaces reply;memset(&reply,0,sizeof(reply));reply.version=BROOK_DESKTOP_SURFACES_VERSION;
        reply.count=3;reply.surfaces[0]=frame_record(10,10,50,40,255,6,10);
        reply.surfaces[1]=reply.surfaces[0];reply.surfaces[1].window=8;reply.surfaces[1].shadowCorner=0; /* invalid */
        reply.surfaces[2]=reply.surfaces[0];reply.surfaces[2].window=9;reply.surfaces[2].kind=99;        /* invalid */
        struct DesktopSurfaceSet set;desktop_surface_set_from(&set,&reply);
        CHECK(set.count==1 && desktop_surface_find(&set,7,70) && !desktop_surface_find(&set,7,71) && !desktop_surface_find(&set,8,70));
        reply.version=1;desktop_surface_set_from(&set,&reply);CHECK(set.count==0);
        struct BrookDesktopSurface none=frame_record(10,10,50,40,255,6,10);none.shadow=BROOK_DESKTOP_SHADOW_NONE;
        struct BrookVkSceneLayer pieces[8];CHECK(!desktop_surface_shadow_layers(&none,W,H,pieces));
        none.shadow=BROOK_DESKTOP_SHADOW_WINDOW;none.shadowAlpha=0;CHECK(!desktop_surface_shadow_layers(&none,W,H,pieces));
        struct BrookDesktopSurface tiny=frame_record(10,10,2,2,255,1,10);CHECK(!desktop_surface_shadow_layers(&tiny,W,H,pieces));
        struct BrookDesktopSurface off=frame_record(-500,-500,50,40,255,6,10);CHECK(!desktop_surface_shadow_layers(&off,W,H,pieces));
    }
    /* Shadow quads against the direct raster, at several sizes, positions
     * (partly off screen), spreads, corners and alphas. */
    const int32_t xs[]={-20,0,37,150},ys[]={-15,5,90};const uint32_t ws[]={6,48,64,120},hs[]={30,40,77};
    const uint32_t corners[]={10,24,48},spreads[]={0,6,16,32},alphas[]={255,180,1};
    unsigned checked=0;
    for(unsigned a=0;a<4;++a)for(unsigned b=0;b<3;++b)for(unsigned c=0;c<4;++c)for(unsigned d=0;d<3;++d)
    for(unsigned e=0;e<3;++e)for(unsigned f=0;f<4;++f)for(unsigned g=0;g<3;++g){
        const struct BrookDesktopSurface s=frame_record(xs[a],ys[b],ws[c],hs[d],alphas[g],spreads[f],corners[e]);
        struct BrookVkSceneLayer pieces[8];const unsigned n=desktop_surface_shadow_layers(&s,W,H,pieces);
        uint32_t scene[BROOK_VK_SCENE_HEADER+9*BROOK_VK_LAYER_WORDS]={W,H,0,0};const uint32_t *src[9];
        const struct BrookVkSceneLayer base={0,W,W,H,0,0,W,H,0,0,W,H,0,0,W,H,255};
        set_layer(scene,0,&base);src[0]=background;
        for(unsigned k=0;k<n;++k){set_layer(scene,1+k,&pieces[k]);src[1+k]=shadow;}
        scene[2]=1+n;
        size_t counts[9];counts[0]=W*H;for(unsigned k=0;k<n;++k)counts[1+k]=64*64;
        CHECK(brook_vk_scene_valid_multi_output(scene,BROOK_VK_SCENE_HEADER+(1+n)*BROOK_VK_LAYER_WORDS,counts,1+n));
        for(int32_t y=0;y<H;++y)for(int32_t x=0;x<W;++x){
            uint32_t expected=background[y*W+x],sp;
            if(direct_shadow(&s,x,y,&sp))expected=quad_ref_blend(s.shadowAlpha|BROOK_VK_STRAIGHT_ALPHA,sp,expected);
            CHECK(quad_reference_pixel(scene,src,(uint32_t)x,(uint32_t)y)==expected);
            ++checked;
        }
        /* The bounds cover every shadowed pixel. */
        struct DesktopDamageRect bounds;
        if(n){CHECK(desktop_surface_shadow_bounds(&s,&bounds));
            for(unsigned k=0;k<n;++k)CHECK(pieces[k].x>=bounds.left && pieces[k].y>=bounds.top &&
                pieces[k].x+(int64_t)pieces[k].width<=bounds.right && pieces[k].y+(int64_t)pieces[k].height<=bounds.bottom);}
    }
    /* A window over its shadow: content opaque, frame spans opaque from the
     * record. Pixels equal those of a whole straight chrome layer; the plan
     * then shades nothing under the opaque strips. */
    {
        const int32_t wx=40,wy=30;const uint32_t cw=64,ch=52;           /* chrome: 2 px borders, 10 px title */
        for(uint32_t y=0;y<ch;++y)for(uint32_t x=0;x<cw;++x){const int interior=x>=2 && x<62 && y>=10 && y<50;
            chrome[y*cw+x]=interior?0xdeadbeefu:0xff000000u|((x*7+y*13)&0xffffffu);} /* interior never sampled */
        const struct BrookDesktopSurface s=frame_record(wx-2,wy-10,cw,ch,255,6,10);
        struct BrookDesktopLayer cl;memset(&cl,0,sizeof(cl));cl.window=7;cl.incarnation=70;cl.clientX=wx;cl.clientY=wy;cl.clientWidth=60;cl.clientHeight=40;
        struct BrookDesktopLayer cr=cl;cr.clientX=wx-2;cr.clientY=wy-10;cr.width=cr.stride=cw;cr.height=ch;
        struct DesktopChromeSpan spans[4];const unsigned ns=desktop_chrome_spans(&cl,&cr,spans);CHECK(ns==4);
        CHECK(desktop_surface_frame_opaque(&s,255) && !desktop_surface_frame_opaque(&s,200));
        struct BrookDesktopSurface glass=s;glass.material=BROOK_DESKTOP_MATERIAL_GLASS;CHECK(!desktop_surface_frame_opaque(&glass,255));
        for(int opaque=0;opaque<2;++opaque){
            uint32_t scene[BROOK_VK_SCENE_HEADER+16*BROOK_VK_LAYER_WORDS]={W,H,0,0};const uint32_t *src[16];unsigned n=0;
            const struct BrookVkSceneLayer base={0,W,W,H,0,0,W,H,0,0,W,H,0,0,W,H,255};set_layer(scene,n,&base);src[n++]=background;
            struct BrookVkSceneLayer pieces[8];const unsigned np=desktop_surface_shadow_layers(&s,W,H,pieces);CHECK(np==8);
            for(unsigned k=0;k<np;++k){set_layer(scene,n,&pieces[k]);src[n++]=shadow;}
            const struct BrookVkSceneLayer content={0,60,60,40,0,0,60,40,wx,wy,60,40,0,0,W,H,255|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE};
            set_layer(scene,n,&content);src[n++]=client;
            for(unsigned j=0;j<ns;++j){const struct DesktopChromeSpan *p=&spans[j];
                const struct BrookVkSceneLayer span={0,cw,cw,ch,p->x,p->y,p->width,p->height,cr.clientX+(int32_t)p->x,cr.clientY+(int32_t)p->y,
                    p->width,p->height,0,0,W,H,255|BROOK_VK_STRAIGHT_ALPHA|(opaque?BROOK_VK_OPAQUE_SOURCE:0u)};
                set_layer(scene,n,&span);src[n++]=chrome;}
            scene[2]=n;
            struct DesktopDamageSet full={1,{{0,0,W,H}}};static struct QuadPlan plan;
            CHECK(quad_plan_build(scene,BROOK_VK_SCENE_HEADER+n*BROOK_VK_LAYER_WORDS,NULL,&full,1,&plan));
            static uint32_t out[W*H];memset(out,0x5a,sizeof(out));quad_reference_execute(scene,src,&plan,out,W);
            for(int32_t y=0;y<H;++y)for(int32_t x=0;x<W;++x){
                uint32_t expected=background[y*W+x],sp;
                if(direct_shadow(&s,x,y,&sp))expected=quad_ref_blend(255|BROOK_VK_STRAIGHT_ALPHA,sp,expected);
                if(x>=wx-2 && x<wx-2+(int32_t)cw && y>=wy-10 && y<wy-10+(int32_t)ch){
                    const uint32_t c=chrome[(y-wy+10)*cw+(x-wx+2)];
                    expected=(x>=wx && x<wx+60 && y>=wy && y<wy+40)?client[(y-wy)*60+(x-wx)]:c;}
                CHECK(out[y*W+x]==expected && quad_reference_pixel(scene,src,(uint32_t)x,(uint32_t)y)==expected);
            }
            /* Under opaque strips no shadow or background is shaded. */
            const struct QuadRect top={wx-2,wy-10,wx-2+(int32_t)cw,wy};
            uint64_t under=0;
            for(uint32_t i=0;i<plan.drawCount;++i)if(plan.draws[i].layer<1+np)under+=quad_rect_area(quad_rect_intersect(plan.draws[i].rect,top));
            CHECK(opaque?under==0:under>0);
        }
    }
    /* Damage: a shadow that appears, moves or changes alpha damages the old
     * and new bounds even where the window itself is unchanged. */
    {
        static struct DesktopTrackState a,b;memset(&a,0,sizeof(a));
        a.valid=1;a.width=W;a.height=H;a.count=1;a.display=1;a.backdrop=1;
        struct DesktopTrackWindow *w=&a.windows[0];w->window=7;w->incarnation=70;w->direct=1;w->commit=3;
        const struct BrookVkSceneLayer content={0,60,60,40,0,0,60,40,40,30,60,40,0,0,W,H,255};memcpy(w->layer,&content,sizeof(content));
        b=a;b.windows[0].shadowAlpha=255;b.windows[0].shadowX=32;b.windows[0].shadowY=14;b.windows[0].shadowWidth=76;b.windows[0].shadowHeight=64;
        struct DesktopDamageSet d={0};CHECK(desktop_track_diff(&a,&b,&d));
        int coversCorner=0;for(uint32_t i=0;i<d.count;++i)if(d.rects[i].left<=32 && d.rects[i].top<=14 && d.rects[i].right>32 && d.rects[i].bottom>14)coversCorner=1;
        CHECK(coversCorner);
        struct DesktopDamageSet same={0};CHECK(desktop_track_diff(&b,&b,&same) && same.count==0);
        a=b;a.windows[0].shadowAlpha=180;struct DesktopDamageSet alpha={0};CHECK(desktop_track_diff(&a,&b,&alpha) && alpha.count);
    }
    printf("SURFACES PASS pixels=%u\n",checked);
    return 0;
}
