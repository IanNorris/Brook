#pragma once
#include <stdint.h>
namespace brook {
// Same pixels and layout in UEFI and the kernel. No services, allocation or TTY.
inline void DrawBootSplash(uint32_t* fb,uint32_t width,uint32_t height,uint32_t stride,
                           const uint32_t* logo,uint32_t logoW,uint32_t logoH,
                           unsigned percent,bool full=true,bool rgb=false) {
    if(!fb||!width||!height||stride<width)return;
    auto pixel=[&](uint32_t x,uint32_t y,uint32_t color){
        if(x>=width||y>=height)return;
        if(rgb)color=((color&255)<<16)|(color&0xff00)|((color>>16)&255);
        fb[static_cast<uint64_t>(y)*stride+x]=color;
    };
    if(percent>100)percent=100;
    const uint32_t scale=(logoW+40>width||logoH+100>height)?2:1;
    const uint32_t lw=logoW/scale,lh=logoH/scale;
    const uint32_t ox=width>lw?(width-lw)/2:0,oy=height>lh?(height-lh)/3:0;
    if(full){
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)pixel(x,y,0);
        for(uint32_t y=0;y<lh;++y)for(uint32_t x=0;x<lw;++x)pixel(ox+x,oy+y,logo[(y*scale)*logoW+x*scale]);
    }
    uint32_t bw=lw*3/2;if(bw>width)bw=width;
    uint32_t bx=(width-bw)/2,by=oy+lh+20;
    for(uint32_t y=0;y<8;++y)for(uint32_t x=0;x<bw;++x)pixel(bx+x,by+y,x<bw*percent/100?0x4c80c0:0x293342);
    // Compact 3x5 percentage glyphs; identical across the handoff.
    static constexpr uint16_t digits[]={0x7b6f,0x2492,0x73e7,0x73cf,0x5bc9,0x79cf,0x79ef,0x7249,0x7bef,0x7bcf};
    unsigned values[3]={percent/100,(percent/10)%10,percent%10},first=percent>=100?0:percent>=10?1:2;
    const unsigned textWidth=(3-first)*8+10;uint32_t tx=width>textWidth?(width-textWidth)/2:0;
    for(unsigned y=0;y<14;++y)for(uint32_t x=bx;x<bx+bw;++x)pixel(x,by+15+y,0);
    for(unsigned n=first;n<3;++n,tx+=8)for(unsigned y=0;y<5;++y)for(unsigned x=0;x<3;++x)
        if(digits[values[n]]&(1u<<(14-y*3-x)))for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx)pixel(tx+x*2+dx,by+16+y*2+dy,0xb8c9df);
    for(unsigned y=0;y<10;++y)for(unsigned x=0;x<8;++x)
        if((x==6-y/2)||(x<2&&y<2)||(x>=6&&y>=8))pixel(tx+x,by+16+y,0xb8c9df);
}
}
