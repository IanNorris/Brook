#pragma once
#include <stdint.h>

namespace brook {
// Screen-space half-open bounds. Expand a dirty region to whole intersecting
// layers before repainting: every blended layer then sees a rebuilt backdrop.
// Repeating closure also catches a layer encountered before a later expansion.
struct SceneRect {
    int64_t left=0, top=0, right=0, bottom=0;
    bool empty() const { return left>=right || top>=bottom; }
    bool intersects(const SceneRect& other) const {
        return !empty() && !other.empty() && left<other.right && other.left<right &&
               top<other.bottom && other.top<bottom;
    }
    void include(const SceneRect& other) {
        if(other.empty())return;
        if(empty()) {*this=other;return;}
        if(other.left<left)left=other.left;
        if(other.top<top)top=other.top;
        if(other.right>right)right=other.right;
        if(other.bottom>bottom)bottom=other.bottom;
    }
    SceneRect clipped(int64_t width,int64_t height) const {
        return {left<0?0:left,top<0?0:top,right>width?width:right,bottom>height?height:bottom};
    }
};
inline SceneRect SceneClientBlitRect(int64_t x,int64_t y,uint32_t sourceW,uint32_t sourceH,
                                     uint32_t clientW,uint32_t clientH,uint32_t scale,
                                     uint32_t screenW,uint32_t screenH) {
    if(!scale)scale=1;
    const uint64_t scaledW=uint64_t(sourceW)*scale,scaledH=uint64_t(sourceH)*scale;
    const int64_t width=scaledW<clientW?scaledW:clientW;
    const int64_t height=scaledH<clientH?scaledH:clientH;
    return SceneRect{x,y,x+width,y+height}.clipped(screenW,screenH);
}
inline SceneRect SceneDamageClosure(SceneRect damage,const SceneRect* layers,uint32_t count) {
    bool expanded;
    do {
        expanded=false;
        for(uint32_t i=0;i<count;++i) {
            if(!damage.intersects(layers[i]))continue;
            SceneRect before=damage;
            damage.include(layers[i]);
            if(before.left!=damage.left || before.top!=damage.top ||
               before.right!=damage.right || before.bottom!=damage.bottom)expanded=true;
        }
    } while(expanded);
    return damage;
}
}
