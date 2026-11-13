/* Hold a legacy opaque window over the upper-left desktop until terminated. */
#define _GNU_SOURCE
#include "desktop_layers_abi.h"
#include "wm_window_state.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
static volatile sig_atomic_t stop;
static void terminate(int signal_number){(void)signal_number;stop=1;}
int main(void){
    struct sigaction sa={0};sa.sa_handler=terminate;sigemptyset(&sa.sa_mask);
    if(sigaction(SIGTERM,&sa,NULL))return 1;
    int fd=open("/dev/fb0",O_RDWR);struct fb_var_screeninfo var={0};
    if(fd<0 || ioctl(fd,FBIOGET_VSCREENINFO,&var) || var.xres!=640 || var.yres!=480)return 1;
    var.xres=1000;var.yres=800;
    if(ioctl(fd,FBIOPUT_VSCREENINFO,&var) || ioctl(fd,FBIOGET_VSCREENINFO,&var) || var.xres!=1000 || var.yres!=800)return 1;
    size_t bytes=(size_t)var.xres*var.yres*4;
    uint32_t *pixels=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(pixels==MAP_FAILED)return 1;
    struct BrookDesktopLayers layers={.version=BROOK_DESKTOP_LAYERS_VERSION};BrookWindowState w={0};
    if(syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_QUERY,&layers,sizeof(layers),0L,0L,0L))return 1;
    for(unsigned i=0;i<layers.count;++i)if((layers.layers[i].flags&BROOK_DESKTOP_LAYER_LEGACY_OPAQUE) &&
       !syscall(BROOK_SYS_WM_WINDOW_STATE,(long)layers.layers[i].window,(long)layers.layers[i].incarnation,(long)&w,(long)sizeof(w),0L,0L))break;
    if(!w.window || syscall(BROOK_SYS_WM_PLACE_WINDOW,(long)w.window,(long)w.incarnation,0L,0L,0L,0L))return 1;
    for(size_t i=0;i<bytes/4;++i)pixels[i]=0xff00cc00u;
    char marker=0;if(write(fd,&marker,1)!=1)return 1;
    printf("OPAQUE_COVER_READY window=%u incarnation=%llu\n",w.window,(unsigned long long)w.incarnation);fflush(stdout);
    while(!stop)usleep(50000);
    munmap(pixels,bytes);close(fd);puts("OPAQUE_COVER_DONE");return 0;
}
