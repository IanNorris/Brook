#define _GNU_SOURCE
#include "desktop_layers_abi.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int call(unsigned op,struct BrookDesktopLayers *s){
    return (int)syscall(BROOK_SYS_DESKTOP_LAYERS,op,s,sizeof(*s),0,0,0);
}
static uint32_t *capture(uint32_t window,uint64_t incarnation,uint32_t *width,uint32_t *height,uint32_t *stride,uint64_t *generation){
    struct BrookDesktopLayers s={.version=BROOK_DESKTOP_LAYERS_VERSION};int selected=-1;
    if(call(BROOK_DESKTOP_LAYERS_QUERY,&s) || s.count>BROOK_DESKTOP_LAYER_LIMIT)return NULL;
    for(uint32_t i=0;i<s.count;++i){struct BrookDesktopLayer *l=&s.layers[i];
        if(l->window==window && l->incarnation==incarnation)selected=(int)i;
        if(!l->bytes || l->bytes>64ull*1024*1024)goto fail;
        void *p=malloc(l->bytes);if(!p)goto fail;l->pixels=(uintptr_t)p;l->capacity=l->bytes;
    }
    if(selected<0 || call(BROOK_DESKTOP_LAYERS_CAPTURE,&s))goto fail;
    struct BrookDesktopLayer *l=&s.layers[selected];
    if(!(l->flags&BROOK_DESKTOP_LAYER_LEGACY_OPAQUE))goto fail;
    *width=l->width;*height=l->height;*stride=l->stride;*generation=l->generation;
    uint32_t *result=(void *)(uintptr_t)l->pixels;l->pixels=0;
    for(uint32_t i=0;i<s.count;++i)free((void *)(uintptr_t)s.layers[i].pixels);
    return result;
fail:
    for(uint32_t i=0;i<s.count;++i)free((void *)(uintptr_t)s.layers[i].pixels);
    return NULL;
}
int main(int argc,char **argv){
    if(argc!=3)return 2;char *end=NULL;unsigned long id=strtoul(argv[1],&end,10);
    if(!end || *end || !id || id>UINT32_MAX)return 2;
    unsigned long long inc=strtoull(argv[2],&end,10);if(!end || *end || !inc)return 2;
    uint32_t *previous=NULL,pw=0,ph=0,ps=0;uint64_t pg=0;
    for(unsigned attempt=0;attempt<30;++attempt){
        uint32_t w=0,h=0,stride=0;uint64_t gen=0;
        uint32_t *current=capture((uint32_t)id,inc,&w,&h,&stride,&gen);
        if(current && previous && w==pw && h==ph && stride==ps && gen==pg){
            uint64_t count=0;uint32_t minX=w,minY=h,maxX=0,maxY=0;int invalid=0;
            for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
                uint32_t diff=current[(uint64_t)y*stride+x]^previous[(uint64_t)y*stride+x];
                if(!diff)continue;if(diff!=0x00ffffffu){invalid=1;break;}
                ++count;if(x<minX)minX=x;if(y<minY)minY=y;if(x>maxX)maxX=x;if(y>maxY)maxY=y;
            }
            if(!invalid && count && count<=4096 && count==(uint64_t)(maxX-minX+1)*(maxY-minY+1)){
                printf("DESKTOP_CARET_BLINK_PASS x=%u y=%u width=%u height=%u pixels=%lu\n",minX,minY,maxX-minX+1,maxY-minY+1,(unsigned long)count);
                free(previous);free(current);return 0;
            }
        }
        if(current){free(previous);previous=current;pw=w;ph=h;ps=stride;pg=gen;}
        usleep(125000);
    }
    free(previous);fprintf(stderr,"DESKTOP_CARET_BLINK_NOT_OBSERVED\n");return 1;
}
