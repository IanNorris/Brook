#pragma once
#include <stdint.h>

namespace brook {
// Render a horizontal span of the desktop's opaque wallpaper plane. The
// source has tightly packed rows and is centred only when smaller than the
// display, matching the software compositor. This plane deliberately excludes
// the build badge, windows, decorations, overlays and cursor.
inline void DesktopBackdropSpan(uint32_t* out,uint32_t left,uint32_t right,uint32_t y,
                                uint32_t screenWidth,uint32_t screenHeight,
                                const uint32_t* wallpaper,uint32_t wallpaperWidth,uint32_t wallpaperHeight) {
    if(!out || left>=right || right>screenWidth || y>=screenHeight)return;
    const uint32_t width=wallpaperWidth<screenWidth?wallpaperWidth:screenWidth;
    const uint32_t height=wallpaperHeight<screenHeight?wallpaperHeight:screenHeight;
    const uint32_t x0=(screenWidth-width)/2,y0=(screenHeight-height)/2;
    if(!wallpaper || !width || !height || y<y0 || y-y0>=height || right<=x0 || left>=x0+width){
        for(uint32_t x=left;x<right;++x)out[x-left]=0x00001a3a;
        return;
    }
    const uint32_t start=left>x0?left:x0,end=right<x0+width?right:x0+width;
    for(uint32_t x=left;x<start;++x)out[x-left]=0x00001a3a;
    __builtin_memcpy(out+start-left,wallpaper+uint64_t(y-y0)*wallpaperWidth+start-x0,uint64_t(end-start)*4);
    for(uint32_t x=end;x<right;++x)out[x-left]=0x00001a3a;
}
}
