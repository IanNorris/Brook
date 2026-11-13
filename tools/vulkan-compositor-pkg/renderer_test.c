#include "renderer.h"
#include "compositor_probe.h"
#include "desktop_content_layer.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc,char **argv) {
    if(argc>2 || (argc==2 && strcmp(argv[1],"--software")))return 2;
    BrookVkRenderer *r=brook_vk_renderer_create(16384,argc==2);
    if(!r){fprintf(stderr,"renderer initialization failed\n");return 1;}
    fprintf(stderr,"COMPOSITOR_DEVICE %s\n",brook_vk_renderer_device(r));
    uint32_t scene[16384],out[4096];
    for(uint32_t round=0;round<3;++round){
        compositor_probe_prepare(scene,16384,round);
        memset(out,0,sizeof(out));
        if(brook_vk_renderer_compose(r,scene,16384,out,4096,10000000000ULL)!=BROOK_VK_COMPLETE)return 1;
        for(uint32_t i=0;i<4096;++i){
            uint32_t want=compositor_probe_pixel(scene,i%64,i/64);
            if(out[i]!=want){fprintf(stderr,"mismatch round=%u pixel=%u got=%08x expected=%08x\n",round,i,out[i],want);return 1;}
        }
    }
    compositor_probe_prepare(scene,16384,2);
    for(uint32_t i=0;i<4096;++i)scene[4160+i]=((i&255u)<<24)|0x00f08c32u;
    for(uint32_t opacity=128;opacity<=255;opacity+=127){
    scene[21+16]=BROOK_VK_STRAIGHT_ALPHA|opacity;
    if(brook_vk_renderer_compose(r,scene,16384,out,4096,10000000000ULL)!=BROOK_VK_COMPLETE)return 1;
    for(uint32_t y=0;y<64;++y)for(uint32_t x=0;x<64;++x){
        uint32_t want=scene[64+y*64+x];
        if(x>=2 && x<40 && y>=10 && y<34 && x<42 && y<45){
            uint32_t sx=3+(2*(x+4)+1)*23/(2*46),sy=4+(2*(y-7)+1)*19/(2*38);
            uint32_t src=scene[4160+sy*64+sx],a=compositor_probe_scale(src>>24,opacity),inv=255-a,composed=0xff000000u;
            for(unsigned c=0;c<3;++c){uint32_t s=(src>>(c*8))&255,d=(want>>(c*8))&255;
                composed|=((s*a+d*inv+127)/255)<<(c*8);}
            want=composed;
        }
        if(out[y*64+x]!=want){fprintf(stderr,"straight mismatch %u,%u got=%08x expected=%08x\n",x,y,out[y*64+x],want);return 1;}
    }
    }
    /* Exercise the same scaled legacy mapping as the desktop renderer, with
     * a negative origin and a client rectangle clipping the scaled source. */
    memset(scene,0,sizeof(scene));scene[0]=64;scene[1]=64;scene[2]=1;scene[3]=8192;
    struct BrookDesktopLayer legacy={.width=3,.height=2,.stride=4,.clientWidth=5,.clientHeight=3,
        .clientX=-2,.clientY=4,.upscale=2,.flags=BROOK_DESKTOP_LAYER_LEGACY_OPAQUE};
    struct BrookVkSceneLayer scaled; if(!desktop_content_layer(&legacy,64,64,&scaled))return 1;
    scaled.sourceOffsetWords=64;memcpy(scene+4,&scaled,sizeof(scaled));
    for(unsigned y=0;y<2;++y)for(unsigned x=0;x<3;++x)scene[64+y*4+x]=((y*3+x)*41u<<24)|(y*3+x+1);
    if(brook_vk_renderer_compose(r,scene,16384,out,4096,10000000000ULL)!=BROOK_VK_COMPLETE)return 1;
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
        uint32_t want=0;
        if(x<3 && y>=4 && y<7)want=scene[64+((y-4)/2)*4+(x+2)/2]|0xff000000u;
        if(out[y*64+x]!=want){fprintf(stderr,"scaled mismatch %u,%u got=%08x expected=%08x\n",x,y,out[y*64+x],want);return 1;}
    }
    scene[4+16]=(scene[4+16]&~255u)|128u;
    if(brook_vk_renderer_compose(r,scene,16384,out,4096,10000000000ULL)!=BROOK_VK_COMPLETE)return 1;
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
        uint32_t want=0;
        if(x<3 && y>=4 && y<7){uint32_t src=scene[64+((y-4)/2)*4+(x+2)/2];want=0x80000000u;
            for(unsigned c=0;c<3;++c)want|=((((src>>(c*8))&255u)*128u+127u)/255u)<<(c*8);}
        if(out[y*64+x]!=want){fprintf(stderr,"scaled alpha mismatch %u,%u got=%08x expected=%08x\n",x,y,out[y*64+x],want);return 1;}
    }
    scene[4+16]=BROOK_VK_OPAQUE_SOURCE|255u;
    if(brook_vk_renderer_compose(r,scene,16384,out,4096,10000000000ULL)!=BROOK_VK_REJECTED)return 1;
    compositor_probe_prepare(scene,16384,1);scene[21]=12288;
    out[0]=0x12345678;
    if(brook_vk_renderer_compose(r,scene,16384,out,4096,10000000000ULL)!=BROOK_VK_REJECTED || out[0]!=0x12345678)return 1;
    if(!brook_vk_renderer_destroy(r))return 1;
    puts("VULKAN_COMPOSITOR_RENDERER_PASS");return 0;
}
