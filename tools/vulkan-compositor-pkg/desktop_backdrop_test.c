#define _GNU_SOURCE
#include "desktop_layers_abi.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"BACKDROP_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}} while(0)
int main(void){
    struct BrookDesktopLayers plane={.version=BROOK_DESKTOP_LAYERS_VERSION};
    CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_BACKDROP_QUERY,&plane,sizeof(plane),0,0,0));
    struct BrookDesktopLayer *l=&plane.layers[0];
    CHECK(plane.count==1 && l->width && l->height && l->stride==l->width &&
          l->bytes==(uint64_t)l->width*l->height*4 && l->incarnation && l->generation);
    uint32_t *pixels=malloc(l->bytes);CHECK(pixels);
    l->pixels=(uintptr_t)pixels;l->capacity=l->bytes;
    ++l->generation;errno=0;
    CHECK(syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_BACKDROP_CAPTURE,&plane,sizeof(plane),0,0,0)==-1 && errno==EAGAIN);
    --l->generation;
    CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_BACKDROP_CAPTURE,&plane,sizeof(plane),0,0,0));
    for(uint64_t i=0;i<l->bytes/4;++i)CHECK((pixels[i]>>24)==255);
    printf("DESKTOP_BACKDROP_PASS %ux%u\n",l->width,l->height);free(pixels);return 0;
}
