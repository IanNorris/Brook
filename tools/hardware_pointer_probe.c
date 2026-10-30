/* Private QEMU probe: desktop physical coordinates versus raw movement. */
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
struct window_out {uint32_t id,stride;uint64_t pixels;};
struct event {uint8_t type,scan,ascii,mods;int16_t x,y;uint32_t token;};
int main(int argc,char **argv) {
    long percent=argc==2?strtol(argv[1],NULL,10):200;
    if(percent!=100&&percent!=150&&percent!=200)return 1;
    if(syscall(0xB04,1L,percent,0L,0L,0L,0L))return 2;
    unsigned i;
    for(i=0;i<500;i++) {if(syscall(0xB04,2L,0L,0L,0L,0L,0L)==percent)break;usleep(10000);}
    if(i==500)return 3;
    struct window_out out={0};
    if(syscall(506,640L,480L,(long)"DPI pointer probe",(long)&out,4L,0L)<0)return 4;
    uint32_t *pixels=(uint32_t *)(uintptr_t)out.pixels;
    for(unsigned y=0;y<480;y++)for(unsigned x=0;x<640;x++)pixels[y*out.stride+x]=0xff254f81;
    syscall(508,(long)out.id,0L,0L,0L,0L,0L);
    printf("POINTER_READY scale=%ld window=%u\n",percent,out.id);fflush(stdout);
    for(i=0;i<12000;i++) {
        struct event events[16];long n=syscall(510,(long)out.id,(long)events,16L,0L,0L,0L);
        if(n<0)return 5;
        for(long j=0;j<n;j++) {
            struct event *e=&events[j];
            if(e->type==0x80){syscall(507,(long)out.id,0L,0L,0L,0L,0L);return 0;}
            if(e->type==0 && e->scan==0x2e) {long t=syscall(0xB0A,(long)out.id,1L,0L,0L,0L,0L);printf("CAPTURE token=%ld\n",t);}
            if(e->type==2||e->type==6)printf("POINTER type=%u x=%d y=%d token=%u\n",e->type,e->x,e->y,e->token);
        }
        if(n)fflush(stdout);
        usleep(10000);
    }
    return 6;
}
