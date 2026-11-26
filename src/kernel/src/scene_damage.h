#pragma once
#include <stdint.h>

namespace brook {
inline bool SceneRowOpaque(const uint32_t* pixels,uint32_t count) {
    for(uint32_t i=0;i<count;++i)if((pixels[i]>>24)!=255)return false;
    return true;
}
// Restore the backdrop only if this row needs it. Subsequent window layers
// supply a no-op because the lower layers are already in the destination.
template <typename RestoreBackdrop>
inline void SceneBlitWindowRow(uint32_t* dstRow,const uint32_t* srcRow,uint32_t rowLen,
                               RestoreBackdrop restoreBackdrop) {
    if (SceneRowOpaque(srcRow,rowLen)) {
        __builtin_memcpy(dstRow,srcRow,uint64_t(rowLen)*4);
        return;
    }
    restoreBackdrop();
    for (uint32_t dx = 0; dx < rowLen; ++dx)
    {
        uint32_t sp = srcRow[dx];
        uint32_t a = sp >> 24;
        if (a == 0)
            continue;
        if (a == 255)
        {
            dstRow[dx] = sp;
            continue;
        }

        uint32_t dp = dstRow[dx];
        uint32_t inv = 255 - a;
        uint32_t sr = (sp >> 16) & 0xff;
        uint32_t sg = (sp >> 8) & 0xff;
        uint32_t sb = sp & 0xff;
        uint32_t dr = (dp >> 16) & 0xff;
        uint32_t dg = (dp >> 8) & 0xff;
        uint32_t db = dp & 0xff;
        uint32_t r = (sr * a + dr * inv + 127) / 255;
        uint32_t g = (sg * a + dg * inv + 127) / 255;
        uint32_t b = (sb * a + db * inv + 127) / 255;
        dstRow[dx] = 0xff000000u | (r << 16) | (g << 8) | b;
    }
}
// Screen-space half-open bounds. Expand a dirty region to whole intersecting
// layers before repainting: every blended layer then sees a rebuilt backdrop.
// Repeating closure also catches a layer encountered before a later expansion.
struct SceneRect {
    int64_t left=0, top=0, right=0, bottom=0;
    bool empty() const { return left>=right || top>=bottom; }
    bool covers(const SceneRect& other) const {
        return !other.empty() && left<=other.left && top<=other.top &&
               right>=other.right && bottom>=other.bottom;
    }
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
// D3: a layer that can be redrawn clipped to the damage (an opaque-or-blended client
// surface with no server-side chrome) does not pull its whole rectangle in; only its
// `extra` rect (the WM's caption overlay drawn over a CSD client) does, once the damage
// reaches it. Layers with `expand` set (server-side chrome, legacy surfaces) join whole.
inline SceneRect SceneDamageClosureLayered(SceneRect damage,const SceneRect* layers,const bool* expand,
                                           const SceneRect* extra,uint32_t count) {
    bool expanded;
    do {
        expanded=false;
        for(uint32_t i=0;i<count;++i) {
            if(!damage.intersects(layers[i]))continue;
            const SceneRect add=expand[i]?layers[i]:extra[i];
            if(add.empty() || (!expand[i] && !damage.intersects(add)))continue;
            SceneRect before=damage;
            damage.include(add);
            if(before.left!=damage.left || before.top!=damage.top ||
               before.right!=damage.right || before.bottom!=damage.bottom)expanded=true;
        }
    } while(expanded);
    return damage;
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
