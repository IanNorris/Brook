#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char** argv){
    if(argc!=2 || getuid()!=1000)return 99;
    if(!strcmp(argv[1],"window")){
        struct {uint32_t id,stride;uint64_t pixels;} out={0};
        if(syscall(506L,160L,120L,"Hardware bridge window",&out,0L)<0)return 98;
        uint32_t* pixels=(uint32_t*)(uintptr_t)out.pixels;
        for(unsigned y=0;y<120;++y)for(unsigned x=0;x<160;++x)pixels[y*out.stride+x]=0x285b98;
        syscall(508L,(long)out.id);puts("window ready");fflush(stdout);
        sleep(30);syscall(507L,(long)out.id);return 0;
    }
    if(strcmp(argv[1],"hello world"))return 99;
    puts("hardware bridge: \"hello world\" ✓");
    fputs("separate stderr\n",stderr);return 17;
}
