/* Private guest regression probe: native self-origin versus relay default. */
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
struct window_out {uint32_t id,stride;uint64_t pixels;};
struct event {uint8_t type,scan,ascii,mods;int16_t x,y;uint32_t token;};
int main(int argc,char **argv) {
    const int native=argc==2&&!strcmp(argv[1],"--native");
    struct window_out out={0};
    if(syscall(506,320L,200L,(long)(native?"Native close probe":"Relay unknown probe"),(long)&out,native?4L:0L,0L)<0){perror("create");return 1;}
    uint32_t *pixels=(uint32_t *)(uintptr_t)out.pixels;
    for(unsigned y=0;y<200;y++)for(unsigned x=0;x<320;x++)pixels[y*out.stride+x]=0xff254f81;
    syscall(508,(long)out.id,0L,0L,0L,0L,0L);
    printf("NATIVE_WINDOW_READY native=%d id=%u pid=%d\n",native,out.id,getpid());fflush(stdout);
    for(unsigned ticks=0;ticks<12000;ticks++) {
        struct event events[16];long n=syscall(510,(long)out.id,(long)events,16L,0L,0L,0L);
        if(n<0){perror("input");return 2;}
        for(long i=0;i<n;i++)if(events[i].type==0x80){
            if(syscall(507,(long)out.id,0L,0L,0L,0L,0L)<0)return 3;
            puts("NATIVE_WINDOW_CLOSED");return 0;
        }
        usleep(10000);
    }
    return 4;
}
