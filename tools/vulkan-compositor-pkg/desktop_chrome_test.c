#define _GNU_SOURCE
#include "desktop_layers_abi.h"
#include "wm_window_state.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"CHROME_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}} while(0)
struct window_out {uint32_t id,stride;uint64_t pixels;};
int main(void){
    struct window_out window={0};
    CHECK(!syscall(506,320L,200L,(long)"Chrome capture test",(long)&window,4L,0L));
    BrookWindowState state={0};
    CHECK(!syscall(BROOK_SYS_WM_WINDOW_STATE,(long)window.id,0L,(long)&state,(long)sizeof(state),0L,0L));
    struct BrookDesktopLayers wire={.version=BROOK_DESKTOP_LAYERS_VERSION};wire.layers[0].window=window.id;
    CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_QUERY,&wire,sizeof(wire),0,0,0));
    struct BrookDesktopLayer *l=&wire.layers[0];CHECK(wire.count==1 && l->bytes && l->width && l->height);
    uint32_t *pixels=malloc(l->bytes);CHECK(pixels);l->pixels=(uintptr_t)pixels;l->capacity=l->bytes;
    ++l->generation;errno=0;CHECK(syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_CAPTURE,&wire,sizeof(wire),0,0,0)==-1 && errno==EAGAIN);--l->generation;
    CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_CAPTURE,&wire,sizeof(wire),0,0,0));
    int32_t cx=state.client_x-l->clientX,cy=state.client_y-l->clientY;
    CHECK(cx>=0 && cy>=0 && (uint32_t)cx+state.width<=l->width && (uint32_t)cy+state.height<=l->height);
    for(uint32_t y=0;y<l->height;++y)for(uint32_t x=0;x<l->width;++x){
        int inside=x>=(uint32_t)cx && x<(uint32_t)cx+state.width && y>=(uint32_t)cy && y<(uint32_t)cy+state.height;
        CHECK((pixels[(uint64_t)y*l->stride+x]>>24)==(inside?0u:255u));
    }
    CHECK(!syscall(507,(long)window.id,0L,0L,0L,0L,0L));free(pixels);puts("DESKTOP_CHROME_PASS");return 0;
}
