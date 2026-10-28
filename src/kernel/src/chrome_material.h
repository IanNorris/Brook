#pragma once
#include <stdint.h>
namespace brook {
// Pixel-centre circle coverage, shared by the decoration mask and backdrop.
// Native scale is bounded to 100/150/200%.
inline unsigned ChromeCornerInset(unsigned radius,unsigned row) {
    if(!radius || row>=radius)return 0;
    const int dy=int(2*(radius-row)-1);
    for(unsigned x=0;x<radius;++x){const int dx=int(2*(radius-x)-1);
        if(dx*dx+dy*dy<=int(4*radius*radius))return x;}
    return radius;
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
inline uint32_t ChromeTitleColor(uint32_t base,unsigned row,unsigned height) {
    if(!height)return base;
    if(row>=height)row=height-1;
    const unsigned t=height>1?255*(height-1-row)/(height-1):255;
    return ChromeMix(ChromeMix(base,0x071526,18*(255-t)/255),0xffffff,72*t*t/(255*255));
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
    bool Ensure(uint32_t active,uint32_t inactive,unsigned alpha,unsigned titleHeight,unsigned topRadius,unsigned bottomRadius) {
        active&=0xffffff;inactive&=0xffffff;
        if(titleHeight>MaxRows)titleHeight=MaxRows;
        if(topRadius>ChromeCornerMask::MaxRadius)topRadius=ChromeCornerMask::MaxRadius;
        if(bottomRadius>ChromeCornerMask::MaxRadius)bottomRadius=ChromeCornerMask::MaxRadius;
        if(valid&&bases[1]==active&&bases[0]==inactive&&opacity==alpha&&height==titleHeight&&
           top.radius==topRadius&&bottom.radius==bottomRadius)return false;
        valid=true;bases[1]=active;bases[0]=inactive;opacity=alpha;height=titleHeight;
        top.Build(topRadius);bottom.Build(bottomRadius);
        for(unsigned focused=0;focused<2;++focused){
            const uint32_t base=bases[focused],rimAlpha=focused?(alpha+255)/2:alpha;
            rims[focused][0]=(rimAlpha<<24)|ChromeMix(base,0xffffff,focused?184:0);
            rims[focused][1]=(rimAlpha<<24)|ChromeMix(base,0xffffff,focused?88:0);
            rims[focused][2]=(alpha<<24)|ChromeMix(base,0x071526,focused?64:20);
            for(unsigned y=0;y<height;++y)
                rows[focused][y]=(alpha<<24)|ChromeMix(base,ChromeTitleColor(base,y,height),focused?255:104);
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
// A cached 64px nine-slice shadow. The centre is a rounded, transparent hole;
// shading never darkens the window through its own translucent title bar.
inline uint32_t ChromeShadowPixel(unsigned x,unsigned y) {
    if(x>=64 || y>=64)return 0;
    if(x>=16 && x<48 && y>=16 && y<48) {
        const unsigned row=y<32?y-16:47-y;
        const unsigned inset=ChromeCornerInset(y<32?7:2,row);
        if(x>=16+inset && x<48-inset)return 0;
    }
    const unsigned dx=x<16?16-x:x>=48?x-47:0;
    const unsigned dy=y<16?16-y:y>=48?y-47:0;
    const unsigned distance=(dx>dy?dx:dy)+(dx<dy?dx:dy)/2;
    static constexpr uint8_t opacity[]={136,113,92,73,56,42,30,21,14,9,5,3,2,1,0,0,0};
    unsigned alpha=distance<17?opacity[distance]:0;
    if(y<16)alpha=alpha*3/4;
    return alpha?(alpha<<24)|0x071526:0;
}
static constexpr unsigned ChromeGlassWindowLimit=8,ChromeShadowWindowLimit=4;
// 32 clients+four decoration strips; up to six rounded backdrop bands/window;
// eight shadow patches per eligible window; desktop/shell/cursor/boot popup.
static_assert(32*5+ChromeGlassWindowLimit*6+ChromeShadowWindowLimit*8+9<=256,
              "Material effects must fit the driver's frame quad budget");
}
