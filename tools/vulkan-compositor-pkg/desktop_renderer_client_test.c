#define _GNU_SOURCE
#include "wm_window_state.h"
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"RENDERER_CLIENT_FAIL line=%d\n",__LINE__);return 1;}} while(0)
struct window_out {uint32_t id,stride;uint64_t pixels;};
int main(void){
    struct window_out window={0};
    CHECK(!syscall(506,320L,200L,(long)"Live compositor client",(long)&window,4L,0L));
    BrookWindowState state={0};
    CHECK(!syscall(BROOK_SYS_WM_WINDOW_STATE,(long)window.id,0L,(long)&state,(long)sizeof(state),0L,0L));
    uint32_t *pixels=(uint32_t*)(uintptr_t)window.pixels;CHECK(pixels && window.stride>=320);
    for(unsigned frame=0;frame<60;++frame){
        for(unsigned y=0;y<200;++y)for(unsigned x=0;x<320;++x)
            pixels[y*window.stride+x]=0xff000000u|(((x+frame*3)&255)<<16)|(((y+frame*5)&255)<<8)|((x+y+frame)&255);
        CHECK(!syscall(508,(long)window.id,0L,0L,0L,0L,0L));
        CHECK(!syscall(BROOK_SYS_WM_PLACE_WINDOW,(long)window.id,(long)state.incarnation,100L+(long)frame*3,100L+(long)frame,0L,0L));
        usleep(100000);
    }
    CHECK(!syscall(507,(long)window.id,0L,0L,0L,0L,0L));
    puts("RENDERER_CLIENT_PASS");return 0;
}
