#include "graphics_layers.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct BrookVkGraphicsLayer a={173,0,-3,7,62,50,4,5,55,56,2,3,28,26};
    assert(brook_vk_graphics_layer_valid(&a,64,64,32,32));
    struct BrookVkGraphicsLayer bad=a;bad.sourceWidth=31;
    assert(!brook_vk_graphics_layer_valid(&bad,64,64,32,32));
    bad=a;bad.x=INT32_MIN;assert(!brook_vk_graphics_layer_valid(&bad,64,64,32,32));
    bad=a;bad.width=0;assert(!brook_vk_graphics_layer_valid(&bad,64,64,32,32));
    bad=a;bad.opacity=256;assert(!brook_vk_graphics_layer_valid(&bad,64,64,32,32));
    bad=a;bad.premultiplied=2;assert(!brook_vk_graphics_layer_valid(&bad,64,64,32,32));
    bad=a;bad.sourceX=UINT32_MAX;assert(!brook_vk_graphics_layer_valid(&bad,64,64,32,32));
    bad=a;bad.clipY=INT32_MAX;assert(!brook_vk_graphics_layer_valid(&bad,64,64,32,32));
    assert(!brook_vk_graphics_layer_valid(NULL,64,64,32,32));
    /* Exhaust every visible pixel in a clipped, noninteger-scaled case and
     * check that its selected sample lies within the declared source crop. */
    for(uint32_t y=0;y<64;++y)for(uint32_t x=0;x<64;++x){
        int64_t px=x,py=y;
        if(px<a.x || py<a.y || px>=(int64_t)a.x+a.width || py>=(int64_t)a.y+a.height ||
           px<a.clipX || py<a.clipY || px>=(int64_t)a.clipX+a.clipWidth || py>=(int64_t)a.clipY+a.clipHeight)continue;
        uint64_t sx=a.sourceX+((2u*(px-a.x)+1u)*a.sourceWidth)/(2u*a.width);
        uint64_t sy=a.sourceY+((2u*(py-a.y)+1u)*a.sourceHeight)/(2u*a.height);
        assert(sx>=a.sourceX && sx<(uint64_t)a.sourceX+a.sourceWidth);
        assert(sy>=a.sourceY && sy<(uint64_t)a.sourceY+a.sourceHeight);
    }
    assert(brook_vk_graphics_linear_layout_valid(1280,720,0,1280,4194304,0,5120,3686400));
    assert(!brook_vk_graphics_linear_layout_valid(1280,720,0,1280,4194304,0,8192,3686400));
    assert(!brook_vk_graphics_linear_layout_valid(1280,720,1,1280,4194304,0,5120,3686400));
    assert(!brook_vk_graphics_linear_layout_valid(1280,720,0,1280,3000000,0,5120,3686400));
    assert(!brook_vk_graphics_linear_layout_valid(1280,720,0,1279,4194304,0,5116,3686400));
    assert(!brook_vk_graphics_linear_layout_valid(1280,720,0,1280,4194304,0,5120,3686399));
    assert(!brook_vk_graphics_linear_layout_valid(0,720,0,1280,4194304,0,5120,3686400));
    assert(!brook_vk_graphics_linear_layout_valid(1280,720,UINT32_MAX,1280,4194304,17179869180ull,5120,3686400));
    puts("GRAPHICS_LAYER_ADMISSION_PASS");
    return 0;
}
