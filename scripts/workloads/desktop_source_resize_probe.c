#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

struct window_out {uint32_t id,stride;uint64_t pixels;};
static int show(unsigned width,unsigned height,uint32_t color,unsigned seconds){
    struct window_out w={0};
    if(syscall(506,(long)width,(long)height,(long)"Buffer size probe",(long)&w,4L,0L) || !w.pixels || w.stride<width)return 1;
    uint32_t *p=(uint32_t*)(uintptr_t)w.pixels;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)p[(uint64_t)y*w.stride+x]=color;
    if(syscall(508,(long)w.id,0L,0L,0L,0L,0L))return 1;
    printf("RESIZE_PROBE width=%u height=%u phase=ready\n",width,height);fflush(stdout);
    sleep(seconds);
    return syscall(507,(long)w.id,0L,0L,0L,0L,0L)!=0;
}
int main(void){
    if(show(1600,900,0xff204080,30) || show(320,200,0xff804020,40))return 1;
    puts("RESIZE_PROBE_PASS");return 0;
}
