#pragma once
#include <stdint.h>
#include "../../shared/brook_theme.h"
#include "../../shared/brook_chrome_shadow.h"
namespace brook {
// Pixel-centre circle coverage, shared by the decoration mask and backdrop.
// Native scale is bounded to 100/150/200%.
inline unsigned ChromeCornerInset(unsigned radius,unsigned row) {
    return brook_chrome_corner_inset(radius,row);
}
inline uint32_t ChromeMix(uint32_t base,uint32_t tint,unsigned alpha) {
    uint32_t out=0;
    for(unsigned shift=0;shift<24;shift+=8)
        out|=(((((base>>shift)&255)*(255-alpha)+((tint>>shift)&255)*alpha)+127)/255)<<shift;
    return out;
}
// Straight-alpha source-over for compositor-owned text/icon rasterization.
// Zero destination alpha denotes the historical opaque RGB raster convention.
inline uint32_t ChromePixelOver(uint32_t dst,uint32_t src){
    const uint32_t sa=src>>24,da=(dst>>24)?dst>>24:255;
    if(!sa)return dst;
    const uint32_t weight=sa*255+da*(255-sa);
    uint32_t out=((weight+127)/255)<<24;
    for(unsigned shift=0;shift<24;shift+=8){
        const uint32_t n=((src>>shift)&255)*sa*255+((dst>>shift)&255)*da*(255-sa);
        out|=((n+weight/2)/weight)<<shift;
    }return out;
}
// Small theme/scale-dependent tables, rebuilt only when their key changes.
// Corner coverage uses 4x4 subpixels; no floating point in the renderer.
struct ChromeCornerMask {
    static constexpr unsigned MaxRadius=16;
    unsigned radius=0;
    uint8_t coverage[MaxRadius][MaxRadius]{},inner[MaxRadius][MaxRadius]{},inset[MaxRadius]{};
    static uint8_t Sample(unsigned r,int x,int y) {
        if(!r)return 255;
        if(x<0||y<0)return 0;
        unsigned inside=0;
        for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){
            const int dx=int(r*8)-(x*8+sx*2+1),dy=int(r*8)-(y*8+sy*2+1);
            if(dx*dx+dy*dy<=int(r*r*64))++inside;
        }
        return (inside*255+8)/16;
    }
    void Build(unsigned r) {
        radius=r>MaxRadius?MaxRadius:r;
        for(unsigned y=0;y<radius;++y){
            inset[y]=radius;
            for(unsigned x=0;x<radius;++x){
                coverage[y][x]=Sample(radius,x,y);
                inner[y][x]=(x&&y)?Sample(radius-1,x-1,y-1):0;
                if(inner[y][x]==255&&inset[y]==radius)inset[y]=x;
            }
        }
    }
};
struct ChromeMaterialCache {
    static constexpr unsigned MaxRows=96;
    bool valid=false;
    unsigned height=0,opacity=0;
    uint32_t bases[2]{},rows[2][MaxRows]{},rims[2][3]{};
    ChromeCornerMask top,bottom;
    uint32_t taskbarRows[MaxRows]{};
    unsigned taskbarHeight=0;
    uint32_t paints[4]{};
    bool Ensure(uint32_t active,uint32_t inactive,unsigned alpha,unsigned titleHeight,unsigned topRadius,
                unsigned bottomRadius,const BrookThemePalette* palette=nullptr,unsigned barHeight=0) {
        active&=0xffffff;inactive&=0xffffff;
        if(titleHeight>MaxRows)titleHeight=MaxRows;
        if(barHeight>MaxRows)barHeight=MaxRows;
        if(topRadius>ChromeCornerMask::MaxRadius)topRadius=ChromeCornerMask::MaxRadius;
        if(bottomRadius>ChromeCornerMask::MaxRadius)bottomRadius=ChromeCornerMask::MaxRadius;
        const uint32_t colors[]={palette?palette->borderActive:active,palette?palette->borderInactive:inactive,
                                 palette?palette->sheen:23,
                                 palette?palette->innerHighlight:51};
        bool same=valid&&bases[1]==active&&bases[0]==inactive&&opacity==alpha&&height==titleHeight&&
                  taskbarHeight==barHeight&&top.radius==topRadius&&bottom.radius==bottomRadius;
        for(unsigned i=0;i<4;++i)same=same&&paints[i]==colors[i];
        if(same)return false;
        valid=true;bases[1]=active;bases[0]=inactive;opacity=alpha;height=titleHeight;taskbarHeight=barHeight;
        for(unsigned i=0;i<4;++i)paints[i]=colors[i];
        top.Build(topRadius);bottom.Build(bottomRadius);
        auto sheen=[&](uint32_t base,unsigned y,unsigned h){
            const unsigned length=h*3/5;
            const unsigned amount=y<length?colors[2]*(length-y)/length:0;
            return ChromeMix(base,0xffffff,amount);
        };
        for(unsigned focused=0;focused<2;++focused){
            const unsigned boosted=focused&&palette?alpha+20:alpha;
            const unsigned titleAlpha=boosted>255?255:boosted;
            const uint32_t rim=colors[focused?0:1];
            rims[focused][0]=(titleAlpha<<24)|rim;
            rims[focused][1]=0xff000000u|rim;
            rims[focused][2]=0xff000000u|rim;
            for(unsigned y=0;y<height;++y){
                const unsigned t=focused&&height>1?255*(height-1-y)/(height-1):0;
                const uint32_t base=ChromeMix(inactive,active,t);
                rows[focused][y]=(titleAlpha<<24)|sheen(base,y,height);
            }
        }
        for(unsigned y=0;y<taskbarHeight;++y){
            const uint32_t rgb=y?sheen(inactive,y,taskbarHeight):ChromeMix(inactive,0xffffff,colors[3]);
            taskbarRows[y]=(alpha<<24)|rgb;
        }
        return true;
    }
};
// Replace the outer one-pixel ring and preserve straight-alpha RGB. Applying
// this to freshly painted corners avoids progressive erosion on idle frames.
inline uint32_t ChromeMaskedRim(uint32_t fill,unsigned coverage,unsigned inner,uint32_t rim) {
    if(!coverage)return 0;
    if(inner>coverage)inner=coverage;
    const unsigned fillAlpha=(fill>>24)?fill>>24:255;
    const unsigned a=fillAlpha*inner,b=(rim>>24)*(coverage-inner),sum=a+b;
    if(!sum)return 0;
    uint32_t out=((sum+127)/255)<<24;
    for(unsigned shift=0;shift<24;shift+=8)
        out|=(((((fill>>shift)&255)*a+((rim>>shift)&255)*b)+sum/2)/sum)<<shift;
    return out;
}
// The cached 64px nine-slice shadow (brook_chrome_shadow.h, shared with the
// user-mode renderer).
inline uint32_t ChromeShadowPixel(unsigned x,unsigned y) {
    return brook_chrome_shadow_pixel(x,y);
}
static constexpr unsigned ChromeGlassWindowLimit=7,ChromeShadowWindowLimit=3,ChromeShadowShellLimit=2;
// 32 clients+four decoration strips; up to six rounded backdrop bands/window;
// eight shadow patches per eligible window; desktop/shell/cursor/boot popup.
static_assert(32*5+ChromeGlassWindowLimit*6+(ChromeShadowWindowLimit+ChromeShadowShellLimit)*8+9<=256,
              "Material effects must fit the driver's frame quad budget");
}
