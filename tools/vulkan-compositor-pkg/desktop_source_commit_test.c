#define _GNU_SOURCE
#include "desktop_layers_abi.h"
#include "gpu_window_source_abi.h"
#include "wm_window_state.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"SOURCE_COMMIT_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
struct window_out {uint32_t id,stride;uint64_t pixels;};
static uint64_t generation(uint32_t window){
    struct BrookDesktopLayers layers={.version=BROOK_DESKTOP_LAYERS_VERSION};
    if(syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_QUERY,&layers,sizeof(layers),0,0,0))return 0;
    for(unsigned i=0;i<layers.count;++i)if(layers.layers[i].window==window)return layers.layers[i].generation;
    return 0;
}
int main(void){
    struct window_out w={0};CHECK(!syscall(506,320L,200L,(long)"Source commit fixture",(long)&w,4L,0L));
    BrookWindowState state={0};CHECK(!syscall(BROOK_SYS_WM_WINDOW_STATE,w.id,0L,&state,sizeof(state),0L,0L));
    CHECK(state.incarnation && w.pixels);uint64_t commit=0;
    uint64_t old=generation(w.id);CHECK(old);
    CHECK(!syscall(BROOK_SYS_WM_BEGIN_SOURCE_WRITE,w.id,state.incarnation,0L,0L,0L,0L));
    errno=0;CHECK(syscall(BROOK_SYS_WM_QUERY_SOURCE_COMMIT,w.id,state.incarnation,old,&commit,0L,0L)==-1 && errno==ESTALE);
    ((uint32_t*)(uintptr_t)w.pixels)[0]=0xff123456;
    CHECK(!syscall(BROOK_SYS_WM_PUBLISH_SOURCE_COMMIT,w.id,state.incarnation,123L,0L,0L,0L));
    uint64_t current=generation(w.id);CHECK(current>old);
    CHECK(!syscall(BROOK_SYS_WM_QUERY_SOURCE_COMMIT,w.id,state.incarnation,current,&commit,0L,0L) && commit==123);
    errno=0;CHECK(syscall(BROOK_SYS_WM_QUERY_SOURCE_COMMIT,w.id,state.incarnation+1,current,&commit,0L,0L)==-1 && errno==ESTALE);
    CHECK(!syscall(BROOK_SYS_WM_BEGIN_SOURCE_WRITE,w.id,state.incarnation,0L,0L,0L,0L));
    errno=0;CHECK(syscall(BROOK_SYS_WM_QUERY_SOURCE_COMMIT,w.id,state.incarnation,current,&commit,0L,0L)==-1 && errno==ESTALE);
    ((uint32_t*)(uintptr_t)w.pixels)[0]=0xff654321;
    CHECK(!syscall(BROOK_SYS_WM_PUBLISH_SOURCE_COMMIT,w.id,state.incarnation,124L,0L,0L,0L));
    uint64_t next=generation(w.id);CHECK(next>current);
    CHECK(!syscall(BROOK_SYS_WM_QUERY_SOURCE_COMMIT,w.id,state.incarnation,next,&commit,0L,0L) && commit==124);
    CHECK(!syscall(508,w.id,0L,0L,0L,0L,0L));
    uint64_t untagged=generation(w.id);CHECK(untagged>next);
    errno=0;CHECK(syscall(BROOK_SYS_WM_QUERY_SOURCE_COMMIT,w.id,state.incarnation,untagged,&commit,0L,0L)==-1 && errno==ESTALE);
    CHECK(!syscall(507,w.id,0L,0L,0L,0L,0L));
    puts("DESKTOP_SOURCE_COMMIT_PASS in_progress=invalid stale=invalid untagged=invalid");return 0;
}
