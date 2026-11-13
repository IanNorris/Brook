#include "desktop_content_layer.h"
#include <assert.h>

int main(void){
    struct BrookDesktopLayer l={.width=3,.height=2,.stride=4,.clientWidth=5,.clientHeight=3,
        .clientX=-2,.clientY=4,.upscale=2,.flags=BROOK_DESKTOP_LAYER_LEGACY_OPAQUE};
    struct BrookVkSceneLayer v={0};assert(desktop_content_layer(&l,20,20,&v));
    assert(v.opacity==(255u|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE));
    assert(v.sourceWidth==3 && v.sourceHeight==2 && v.width==6 && v.height==4);
    assert(v.x==-2 && v.y==4 && v.clipX==-2 && v.clipY==4 && v.clipWidth==5 && v.clipHeight==3);
    /* Pixel-centre nearest neighbour with an exact integer ratio agrees with
     * the CPU compositor's x/scale rule, including a clipped last column. */
    for(unsigned x=0;x<l.clientWidth;++x)assert(((2*x+1)*v.sourceWidth/(2*v.width))==x/l.upscale);
    l.flags=0;assert(desktop_content_layer(&l,20,20,&v));
    assert(v.width==3 && v.height==2 && v.clipWidth==20 && v.clipHeight==20);
    l.flags=BROOK_DESKTOP_LAYER_HAS_OPACITY|(128u<<BROOK_DESKTOP_LAYER_OPACITY_SHIFT);
    assert(desktop_content_layer(&l,20,20,&v) && v.opacity==(128u|BROOK_VK_STRAIGHT_ALPHA));
    l.flags=BROOK_DESKTOP_LAYER_HAS_OPACITY;assert(desktop_content_layer(&l,20,20,&v) && v.opacity==BROOK_VK_STRAIGHT_ALPHA);
    l.flags=BROOK_DESKTOP_LAYER_LEGACY_OPAQUE;l.upscale=0;assert(!desktop_content_layer(&l,20,20,&v));
    l.upscale=255;l.width=16384;assert(!desktop_content_layer(&l,20,20,&v));
    l.upscale=1;l.flags=4;assert(!desktop_content_layer(&l,20,20,&v));
    l.flags=1u<<BROOK_DESKTOP_LAYER_OPACITY_SHIFT;assert(!desktop_content_layer(&l,20,20,&v));
}
