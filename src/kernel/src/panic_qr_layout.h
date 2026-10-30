#pragma once
#include <stdint.h>
namespace brook {
struct PanicQrArea { uint32_t x,y,width,height; };
struct PanicQrPlacement { uint32_t x=0,y=0,side=0,scale=0; };
// Fit full modules including quiet border; never rely on framebuffer clipping.
inline PanicQrPlacement FitPanicQr(PanicQrArea area,uint32_t modules,
                                  uint32_t preferred,uint32_t page,uint32_t pages) {
    if(!modules || !preferred || !pages || pages>4 || page>=pages) return {};
    const uint32_t columns=pages>1?2:1, rows=(pages+columns-1)/columns, gap=12;
    if(area.width<=(columns-1)*gap || area.height<=(rows-1)*gap) return {};
    const uint32_t cellW=(area.width-(columns-1)*gap)/columns;
    const uint32_t cellH=(area.height-(rows-1)*gap)/rows;
    uint32_t scale=cellW/modules;
    if(scale>cellH/modules) scale=cellH/modules;
    if(scale>preferred) scale=preferred;
    if(!scale) return {};
    const uint32_t side=modules*scale;
    return {area.x+(page%columns)*(cellW+gap)+(cellW-side)/2,
            area.y+(page/columns)*(cellH+gap)+(cellH-side)/2,side,scale};
}
}
