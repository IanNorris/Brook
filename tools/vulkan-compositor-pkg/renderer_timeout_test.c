#define _POSIX_C_SOURCE 200809L
#include "renderer.h"
#include "compositor_probe.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc,char **argv) {
    const int hardware=argc==2 && !strcmp(argv[1],"--hardware");
    if(argc!=1 && !hardware)return 2;
    uint32_t scene[16384],out[4096];
    for(unsigned attempt=0;attempt<64;++attempt){
        BrookVkRenderer *r=brook_vk_renderer_create(16384,!hardware);
        if(!r)return 1;
        if(!attempt)fprintf(stderr,"COMPOSITOR_DEVICE %s\n",brook_vk_renderer_device(r));
        if(brook_vk_renderer_retire_timeout(&r) || !r || brook_vk_renderer_retire_timeout(NULL))return 1;
        compositor_probe_prepare(scene,16384,0);
        BrookVkResult result=brook_vk_renderer_compose(r,scene,16384,out,4096,0);
        if(result==BROOK_VK_COMPLETE){if(!brook_vk_renderer_destroy(r))return 1;continue;}
        if(result!=BROOK_VK_UNCERTAIN || brook_vk_renderer_destroy(r))return 1;
        struct BrookVkFailure failure=brook_vk_renderer_failure(r);
        if(failure.stage!=BROOK_VK_FAILURE_WAIT || failure.result!=2 || failure.fence_status_valid)return 1; /* VK_TIMEOUT */
        for(unsigned n=0;n<2000;++n){
            if(brook_vk_renderer_retire_timeout(&r)){
                if(r)return 1;
                r=brook_vk_renderer_create(16384,!hardware);
                if(!r || brook_vk_renderer_compose(r,scene,16384,out,4096,10000000000ULL)!=BROOK_VK_COMPLETE)return 1;
                for(unsigned i=0;i<4096;++i)if(out[i]!=compositor_probe_pixel(scene,i%64,i/64))return 1;
                if(!brook_vk_renderer_destroy(r))return 1;
                puts("VULKAN_LATE_FENCE_RECOVERY_PASS");return 0;
            }
            struct timespec pause={0,1000000};nanosleep(&pause,NULL);
        }
        fprintf(stderr,"Timed-out fence did not safely retire; retaining resources\n");
        for(;;)pause();
    }
    fprintf(stderr,"No zero-timeout wait occurred; test inconclusive\n");return 77;
}
