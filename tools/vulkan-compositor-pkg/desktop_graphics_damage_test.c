#include "desktop_graphics_damage.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct DesktopDamageSet in={0},out={0};
    desktop_damage_add(&in,(struct DesktopDamageRect){3,5,23,19},64,48);
    desktop_damage_add(&in,(struct DesktopDamageRect){26,8,35,15},64,48);
    assert(desktop_graphics_damage(&in,64,48,&out));
    for(unsigned y=0;y<48;++y)for(unsigned x=0;x<64;++x){
        int before=0,after=0;
        for(unsigned i=0;i<in.count;++i){struct DesktopDamageRect r=in.rects[i];before|=x>=r.left && x<r.right && y>=r.top && y<r.bottom;}
        for(unsigned i=0;i<out.count;++i){struct DesktopDamageRect r=out.rects[i];after|=x>=r.left && x<r.right && y>=r.top && y<r.bottom;}
        assert(!before || after);
    }
    for(unsigned i=0;i<out.count;++i){struct DesktopDamageRect r=out.rects[i];
        assert(!(r.left&15) && !(((uint64_t)r.top*64u+r.left)*4u&63u));
        assert(r.right<=64 && r.bottom<=48);
    }
    assert(!desktop_graphics_damage(&in,63,48,&out));
    in.rects[0].left=-1;assert(!desktop_graphics_damage(&in,64,48,&out));
    puts("DESKTOP_GRAPHICS_DAMAGE_PASS");return 0;
}
