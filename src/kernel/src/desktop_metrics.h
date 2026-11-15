#pragma once
#include <stdint.h>

namespace brook {
// Immutable values shared by physical-pixel layout and input hit testing.
// Client buffers/coordinates are never implicitly scaled by these metrics.
struct DesktopMetrics {
    uint32_t percent;
    static constexpr bool Supported(uint32_t value) { return value==100 || value==150 || value==200; }
    constexpr int pixels(int base) const {
        return base>=0 ? static_cast<int>((static_cast<int64_t>(base)*percent+50)/100)
                       : -static_cast<int>((-static_cast<int64_t>(base)*percent+50)/100);
    }
    // The one source of the frame and taskbar metrics, in logical pixels at
    // 100%. window.h's WM_* constants are defined from these.
    static constexpr int BorderBase = 2;
    static constexpr int TitleBase = 36;
    static constexpr int CaptionButtonBase = 46;
    static constexpr int TaskbarBase = 44;
    static constexpr int IconBase = 24;
    static constexpr int FontBase = 16;
    constexpr int border() const { return pixels(BorderBase); }
    constexpr int title() const { return pixels(TitleBase); }
    constexpr int captionButton() const { return pixels(CaptionButtonBase); }
    constexpr int taskbar() const { return pixels(TaskbarBase); }
    constexpr int icon() const { return pixels(IconBase); }
    constexpr int fontPixels() const { return pixels(FontBase); }
    // Offset of a natively decorated window's client area from its outer
    // origin, for code outside the WM that positions client buffers.
    constexpr int clientOffsetX() const { return border(); }
    constexpr int clientOffsetY() const { return border() + title(); }
};
inline constexpr DesktopMetrics DesktopMetric100{100}, DesktopMetric150{150}, DesktopMetric200{200};
inline constexpr const DesktopMetrics* DesktopMetricsFor(uint32_t percent) {
    return percent==100 ? &DesktopMetric100 : percent==150 ? &DesktopMetric150 : percent==200 ? &DesktopMetric200 : nullptr;
}
struct DesktopRect {
    int x,y,w,h;
    bool contains(int px,int py) const {
        return w>0 && h>0 && px>=x && py>=y && px-x<w && py-y<h;
    }
};
struct DesktopWindowGeometry {
    int x,y,clientW,clientH;
    bool clientDecorated;
    // A client-decorated window may still show the native caption buttons over
    // the top-right of its own surface (a browser tab strip owns the rest of
    // the top edge). Insets exclude the client's transparent shadow margin.
    bool captionOverlay=false;
    int captionInsetRight=0,captionInsetTop=0;
    int captionHeight=0; // client's title-strip height in physical pixels; 0: native title()
    int clientX(const DesktopMetrics& m) const { return x+(clientDecorated?0:m.border()); }
    int clientY(const DesktopMetrics& m) const { return y+(clientDecorated?0:m.border()+m.title()); }
};
inline DesktopRect DesktopCaptionRect(const DesktopWindowGeometry& g,const DesktopMetrics& m,int button) {
    if(button<0 || button>=3) return {};
    if(g.clientDecorated) {
        if(!g.captionOverlay) return {};
        const int right=g.captionInsetRight>0?g.captionInsetRight:0,top=g.captionInsetTop>0?g.captionInsetTop:0;
        const int span=g.clientW-right;
        int width=m.captionButton();
        if(width*3>span) width=span/3;
        if(width<=0 || top>=g.clientH) return {};
        const int want=g.captionHeight>0?g.captionHeight:m.title();
        const int height=want<g.clientH-top?want:g.clientH-top;
        return {g.x+span-(button+1)*width,g.y+top,width,height};
    }
    int width=m.captionButton();
    if(width*3>g.clientW) width=g.clientW/3;
    return {g.x+g.clientW+m.border()-(button+1)*width,g.y+m.border(),width,m.title()};
}
// Overlaid captions matched to the client's own title strip. The Wayland
// server samples the strip colour (and the frame's top-right corner radius)
// from the client's buffer and packs them: bits 0-23 RGB, bit 24 valid,
// bits 25-31 radius in physical pixels. Zero keeps the native title style.
inline constexpr uint32_t DesktopCaptionStyleValid=1u<<24;
inline constexpr uint32_t DesktopCaptionStyleRgb(uint32_t style){return style&0xffffffu;}
inline constexpr unsigned DesktopCaptionStyleRadius(uint32_t style){return style>>25;}
inline constexpr bool DesktopCaptionStripDark(uint32_t rgb){
    return ((rgb>>16&255)*299+(rgb>>8&255)*587+(rgb&255)*114)<140*1000;
}
// Glyph colour with contrast against the strip (Chromium's own caption ink).
inline constexpr uint32_t DesktopCaptionInk(uint32_t rgb){
    return DesktopCaptionStripDark(rgb)?0xe8eaedu:0x1f1f1fu;
}
// The union of the three overlaid caption buttons (empty unless overlaid).
inline DesktopRect DesktopCaptionOverlayRect(const DesktopWindowGeometry& g,const DesktopMetrics& m) {
    if(!g.clientDecorated || !g.captionOverlay) return {};
    const DesktopRect close=DesktopCaptionRect(g,m,0),minimize=DesktopCaptionRect(g,m,2);
    if(close.w<=0 || close.h<=0) return {};
    return {minimize.x,close.y,close.x+close.w-minimize.x,close.h};
}
// Preserve client pixels and origin while changing native decorations. Keep the
// caption reachable when a larger title would otherwise extend above the screen.
// Saved restore geometry can use clamp=false, then clamp when it is restored.
inline DesktopWindowGeometry ReframeDesktopWindow(DesktopWindowGeometry g,
        const DesktopMetrics& before,const DesktopMetrics& after,int screenW,int desktopH,bool clamp=true) {
    if(!g.clientDecorated) {
        g.x+=before.border()-after.border();
        g.y+=before.border()+before.title()-after.border()-after.title();
    }
    if(clamp) {
        int visible=after.captionButton();
        if(visible>screenW) visible=screenW;
        if(visible<1) visible=1;
        const int outerW=g.clientW+(g.clientDecorated?0:2*after.border());
        if(outerW>screenW) g.x=screenW-outerW; // Keep right-side caption controls reachable.
        else {
            if(g.x<0)g.x=0;
            if(g.x>screenW-outerW)g.x=screenW-outerW;
        }
        const int maxY=desktopH-(g.clientDecorated?visible:after.title()+2*after.border());
        if(g.y>maxY)g.y=maxY;
        if(g.y<0)g.y=0;
    }
    return g;
}
}
