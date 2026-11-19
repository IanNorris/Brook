// Surface records (compositor refactor stage 1.3): the rules reproduce what
// the virgl path drew before records existed (shadow and rounded glass
// corners only under glass, never when maximised or client-decorated), and
// the shared nine-slice pieces are exactly the old GpuComposeShadow quads.
#include "../../kernel/src/chrome_surface.h"
#include "../../shared/brook_chrome_shadow.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"CHROME_SURFACE line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
using namespace brook;

// chrome_material.h's ChromeShadowPixel before it moved to the shared header.
static unsigned OldCornerInset(unsigned radius,unsigned row) {
    if(!radius || row>=radius)return 0;
    const int dy=int(2*(radius-row)-1);
    for(unsigned x=0;x<radius;++x){const int dx=int(2*(radius-x)-1);
        if(dx*dx+dy*dy<=int(4*radius*radius))return x;}
    return radius;
}
static uint32_t OldShadowPixel(unsigned x,unsigned y) {
    if(x>=64 || y>=64)return 0;
    if(x>=16 && x<48 && y>=16 && y<48) {
        const unsigned row=y<32?y-16:47-y;
        const unsigned inset=OldCornerInset(y<32?7:2,row);
        if(x>=16+inset && x<48-inset)return 0;
    }
    const unsigned dx=x<16?16-x:x>=48?x-47:0;
    const unsigned dy=y<16?16-y:y>=48?y-47:0;
    const unsigned distance=(dx>dy?dx:dy)+(dx<dy?dx:dy)/2;
    static constexpr uint8_t opacity[]={136,113,92,73,56,42,30,21,14,9,5,3,2,1,0,0,0};
    unsigned alpha=distance<17?opacity[distance]:0;
    if(y<16)alpha=alpha*3/4;
    return alpha?(alpha<<24)|BROOK_CHROME_SHADE:0;
}

int main(){
    for(unsigned r=0;r<20;++r)for(unsigned row=0;row<24;++row)CHECK(brook_chrome_corner_inset(r,row)==OldCornerInset(r,row));
    for(uint32_t scale:{1u,2u}){
        const ChromeSurfaceEffects opaque{false,0,0,0,16*scale,24*scale,0};
        const ChromeSurfaceEffects glass{true,6,8*scale,2*scale,16*scale,28*scale,12*scale,true};
        for(int maximised=0;maximised<2;++maximised)for(int focused=0;focused<2;++focused)for(int csd=0;csd<2;++csd){
            ChromeFrameSurfaceModel m{};
            m.window=3;m.incarnation=99;m.key=1234;m.clientDecorated=csd;m.maximised=maximised;m.focused=focused;
            m.outer={100,50,400,300};m.client={104,90,392,256};m.captionRadius=csd?8:0;m.opacity=200;
            const auto o=ChromeFrameSurface(m,opaque),g=ChromeFrameSurface(m,glass);
            CHECK(o.window==3 && o.incarnation==99 && o.key==1234 && o.opacity==200 && o.x==100 && o.width==400);
            CHECK(o.kind==(csd?BROOK_DESKTOP_SURFACE_CAPTION_BLOCK:BROOK_DESKTOP_SURFACE_FRAME));
            // Opaque: never a shadow, never rounded (except a matched caption corner).
            CHECK(o.material==BROOK_DESKTOP_MATERIAL_OPAQUE && o.shadow==BROOK_DESKTOP_SHADOW_NONE && !o.level);
            CHECK(!o.radii[0] && !o.radii[2] && !o.radii[3]);
            CHECK(o.radii[1]==(csd && !maximised?8:0));
            CHECK(((o.flags&BROOK_DESKTOP_SURFACE_OPAQUE)!=0)==(!csd || maximised));
            if(!csd)CHECK(o.clientX==104 && o.clientY==90 && o.clientWidth==392 && o.clientHeight==256);
            else CHECK(!o.clientWidth && !o.clientHeight);
            // A CSD overlay remains opaque over the client's own title strip.
            CHECK(g.material==(csd?BROOK_DESKTOP_MATERIAL_OPAQUE:BROOK_DESKTOP_MATERIAL_GLASS));
            CHECK(g.level==(csd?0u:6u));
            CHECK((g.flags&BROOK_DESKTOP_SURFACE_OPAQUE)!=0 ? csd && maximised : !csd || !maximised);
            const bool shadow=!csd && !maximised;
            CHECK(g.shadow==(shadow?(focused?BROOK_DESKTOP_SHADOW_WINDOW_ACTIVE:BROOK_DESKTOP_SHADOW_WINDOW_INACTIVE):
                                     BROOK_DESKTOP_SHADOW_NONE));
            if(shadow)CHECK(g.shadowAlpha==255 && g.shadowSpread==16*scale && g.shadowCorner==28*scale &&
                            g.radii[0]==8*scale && g.radii[1]==8*scale && g.radii[2]==2*scale && g.radii[3]==2*scale &&
                            (g.flags&BROOK_DESKTOP_SURFACE_LIGHT_SHADOW));
            if(!csd && maximised)CHECK(!g.radii[0] && !g.radii[1] && !g.radii[2] && !g.radii[3]);
        }
        // Shell surfaces: opaque outside glass; material ones translucent under it.
        for(uint32_t kind=BROOK_DESKTOP_SURFACE_TASKBAR;kind<=BROOK_DESKTOP_SURFACE_PERFORMANCE_HUD;++kind){
            const bool material = true;
            const auto o=ChromeShellSurfaceRecord(kind,{0,1000,1920,80},material,77,180,opaque);
            const auto g=ChromeShellSurfaceRecord(kind,{0,1000,1920,80},material,77,180,glass);
            CHECK(o.kind==kind && o.key==77 && o.opacity==255 && (o.flags&BROOK_DESKTOP_SURFACE_OPAQUE) && !o.radii[0] && !o.shadow);
            CHECK(g.material==(material?BROOK_DESKTOP_MATERIAL_GLASS:BROOK_DESKTOP_MATERIAL_OPAQUE));
            CHECK(g.opacity==180 && !(g.flags&BROOK_DESKTOP_SURFACE_OPAQUE));
            const bool panel = kind != BROOK_DESKTOP_SURFACE_TASKBAR;
            const bool shadow = panel && kind != BROOK_DESKTOP_SURFACE_TOOLTIP;
            for (auto radius : g.radii) CHECK(radius == (panel ? 12 * scale : 0));
            CHECK(g.shadow == (shadow ? BROOK_DESKTOP_SHADOW_PANEL : BROOK_DESKTOP_SHADOW_NONE));
            if (shadow) CHECK(g.shadowAlpha == 255 && g.shadowSpread == 16 * scale && g.shadowCorner == 28 * scale);
        }
    }
    // Nine-slice pieces against the old GpuComposeShadow arithmetic.
    for(int x=-50;x<60;x+=7)for(uint32_t w:{1u,10u,17u,40u,333u})for(uint32_t h:{1u,16u,48u,200u})for(uint32_t scale:{1u,2u}){
        const int pad=16*scale,corner=24*scale;
        const int left=x-pad,top=x/2-pad,width=int(w)+2*pad,height=int(h)+2*pad;
        const bool any=!(width<corner*2 || height<corner*2);
        const int dx[]={left,left+corner,left+width-corner},dy[]={top,top+corner,top+height-corner};
        const int dw[]={corner,width-2*corner,corner},dh[]={corner,height-2*corner,corner};
        const unsigned source[]={0,24,40},span[]={24,16,24};
        unsigned piece=0;
        for(unsigned yy=0;yy<3;++yy)for(unsigned xx=0;xx<3;++xx){if(xx==1&&yy==1)continue;
            BrookChromeShadowPiece p{};const int got=brook_chrome_shadow_piece(x,x/2,w,h,pad,corner,piece++,&p);
            const bool expected=any && dw[xx]>0 && dh[yy]>0; // zero-size quads draw nothing either way
            CHECK(got==expected);
            if(got)CHECK(p.sx==source[xx] && p.sy==source[yy] && p.sw==span[xx] && p.sh==span[yy] &&
                         p.dx==dx[xx] && p.dy==dy[yy] && int(p.dw)==dw[xx] && int(p.dh)==dh[yy]);
        }
    }
    // The shared source keeps the old pixels (spot values and the hole).
    CHECK(brook_chrome_shadow_pixel(32,32)==0 && brook_chrome_shadow_pixel(0,0)==0);
    CHECK((brook_chrome_shadow_pixel(10,32)&0xffffff)==BROOK_CHROME_SHADE);
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)CHECK(brook_chrome_shadow_pixel(x,y)==OldShadowPixel(x,y));
    for(unsigned kind=BROOK_DESKTOP_SHADOW_WINDOW_ACTIVE;kind<=BROOK_DESKTOP_SHADOW_PANEL;++kind){
        const unsigned y=brook_chrome_shadow_tile_y(kind,BROOK_DESKTOP_SURFACE_LIGHT_SHADOW);
        CHECK(brook_chrome_shadow_atlas_pixel(32,y+32)==0);
        CHECK(brook_chrome_shadow_atlas_pixel(32,y)==0 && brook_chrome_shadow_atlas_pixel(32,y+63)==0);
        CHECK((brook_chrome_shadow_atlas_pixel(32,y+49)&0xffffff)==0x142238);
        BrookChromeShadowPiece piece{};
        CHECK(brook_chrome_shadow_variant_piece(10,20,400,300,16,28,4,kind,BROOK_DESKTOP_SURFACE_LIGHT_SHADOW,&piece));
        CHECK(piece.sy==y+28 && piece.sw==28 && piece.sh==8);
    }
    const unsigned active=brook_chrome_shadow_atlas_pixel(32,brook_chrome_shadow_tile_y(BROOK_DESKTOP_SHADOW_WINDOW_ACTIVE,0)+54)>>24;
    const unsigned panel=brook_chrome_shadow_atlas_pixel(32,brook_chrome_shadow_tile_y(BROOK_DESKTOP_SHADOW_PANEL,0)+54)>>24;
    const unsigned inactive=brook_chrome_shadow_atlas_pixel(32,brook_chrome_shadow_tile_y(BROOK_DESKTOP_SHADOW_WINDOW_INACTIVE,0)+54)>>24;
    CHECK(active>panel && panel>inactive);
    std::puts("CHROME_SURFACE PASS");
}
