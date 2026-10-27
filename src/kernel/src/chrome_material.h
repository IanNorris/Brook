#pragma once
#include <stdint.h>
namespace brook {
// Pixel-centre circle coverage, shared by the decoration mask and backdrop.
// Native scale is bounded to 100/150/200%, hence at most a ten-pixel radius.
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
inline uint32_t ChromeTitleColor(uint32_t base,unsigned row,unsigned height) {
    if(!height)return base;
    const unsigned middle=height/2;
    if(row<middle)return ChromeMix(base,0xffffff,64-54*row/(middle?middle:1));
    return row==middle?ChromeMix(base,0,10):ChromeMix(base,0xffffff,8);
}
// A cached 64px nine-slice shadow. The centre is a rounded, transparent hole;
// shading never darkens the window through its own translucent title bar.
inline uint32_t ChromeShadowPixel(unsigned x,unsigned y) {
    if(x>=64 || y>=64)return 0;
    if(x>=16 && x<48 && y>=16 && y<48) {
        const unsigned row=y<32?y-16:47-y;
        const unsigned inset=ChromeCornerInset(5,row);
        if(x>=16+inset && x<48-inset)return 0;
    }
    const unsigned dx=x<16?16-x:x>=48?x-47:0;
    const unsigned dy=y<16?16-y:y>=48?y-47:0;
    const unsigned distance=(dx>dy?dx:dy)+(dx<dy?dx:dy)/2;
    static constexpr uint8_t opacity[]={110,100,86,75,62,50,40,30,22,16,11,8,5,3,2,1,0};
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
