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
    constexpr int border() const { return pixels(2); }
    constexpr int title() const { return pixels(36); }
    constexpr int captionButton() const { return pixels(46); }
    constexpr int taskbar() const { return pixels(44); }
    constexpr int icon() const { return pixels(24); }
    constexpr int fontPixels() const { return pixels(16); }
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
    int clientX(const DesktopMetrics& m) const { return x+(clientDecorated?0:m.border()); }
    int clientY(const DesktopMetrics& m) const { return y+(clientDecorated?0:m.border()+m.title()); }
};
inline DesktopRect DesktopCaptionRect(const DesktopWindowGeometry& g,const DesktopMetrics& m,int button) {
    if(g.clientDecorated || button<0 || button>=3) return {};
    int width=m.captionButton();
    if(width*3>g.clientW) width=g.clientW/3;
    return {g.x+g.clientW+m.border()-(button+1)*width,g.y+m.border(),width,m.title()};
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
