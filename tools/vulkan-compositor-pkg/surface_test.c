#include "surface.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t over(uint32_t src,uint32_t dst) {
    uint32_t out=0,inv=255-(src>>24);
    for(unsigned k=0;k<4;++k){
        uint32_t v=((src>>(k*8))&255)+(((dst>>(k*8))&255)*inv+127)/255;
        out|=(v>255?255:v)<<(k*8);
    }
    return out;
}
int main(int argc,char **argv) {
    if(argc>2 || (argc==2 && strcmp(argv[1],"--software")))return 2;
    BrookVkSurface *renderer=brook_vk_surface_create(argc==2);if(!renderer)return 1;
    uint32_t background[56],foreground[9],out[56];
    for(unsigned i=0;i<56;++i)background[i]=0xff000000u|((i*17u&255u)<<8)|(i*3u&255u);
    for(unsigned i=0;i<9;++i)foreground[i]=0x80000000u|((16u+i)<<16)|((32u+i)<<8)|(8u+i);
    const struct BrookVkSurfaceLayer layers[]={
        {background,8,7,0,0,8,7,0,0,8,7},
        {foreground,3,3,0,0,3,3,-1,2,5,5}
    };
    if(brook_vk_surface_compose(renderer,8,7,layers,2,out,56)!=BROOK_VK_COMPLETE)return 1;
    fprintf(stderr,"SURFACE_DEVICE %s\n",brook_vk_surface_device(renderer));
    for(unsigned y=0;y<7;++y)for(unsigned x=0;x<8;++x){
        uint32_t want=background[y*8+x];
        if(x<4 && y>=2){
            unsigned sx=(2*(x+1)+1)*3/10,sy=(2*(y-2)+1)*3/10;
            want=over(foreground[sy*3+sx],want);
        }
        if(out[y*8+x]!=want){fprintf(stderr,"surface mismatch %u,%u got=%08x expected=%08x\n",x,y,out[y*8+x],want);return 1;}
    }
    out[0]=0x12345678;
    if(brook_vk_surface_compose(renderer,8,7,layers,2,out,10)!=BROOK_VK_REJECTED || out[0]!=0x12345678)return 1;
    struct BrookVkSurfaceExternalLayer invalid={.fd=-1,.syncFd=-1,.words=56,.stride=8,.imageWidth=8,.imageHeight=7,
        .sw=8,.sh=7,.width=8,.height=7,.clipWidth=8,.clipHeight=7,.opacity=255};
    if(brook_vk_surface_compose_external(renderer,8,7,&invalid,1,out,56)!=BROOK_VK_REJECTED || out[0]!=0x12345678)return 1;
    if(!brook_vk_surface_destroy(renderer))return 1;
    puts("VULKAN_SURFACE_COMPOSE_PASS");return 0;
}
