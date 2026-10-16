#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

/* Run using the terminal shell's `exec`, so this replaces its owning process.
 * Resize twice when prompted. The second retires the first replacement source
 * while this process retains its /dev/fb0 alias.
 * Deliberately use pixel sizes with trailing page padding for the canary. */
static const char *result_path;
static void fail(int line) {
 int saved=errno;FILE *file=fopen(result_path,"w");
 if(file){fprintf(file,"TERMSTRESSCHECK FAIL line=%d errno=%d\n",line,saved);fclose(file);}
 fprintf(stderr,"TERMSTRESSCHECK FAIL line=%d errno=%d\n",line,saved);exit(1);
}
#define REQUIRE(x) do { if(!(x))fail(__LINE__); } while(0)
static uint64_t millis(void) {
 struct timespec t;REQUIRE(clock_gettime(CLOCK_MONOTONIC,&t)==0);
 return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static struct winsize size(void) {
 struct winsize value={0};REQUIRE(ioctl(1,TIOCGWINSZ,&value)==0);
 REQUIRE(value.ws_col && value.ws_row);return value;
}
static struct winsize await_resize(struct winsize previous) {
 uint64_t start=millis();
 while(millis()-start<60000){
  struct winsize current=size();
  if(current.ws_col!=previous.ws_col || current.ws_row!=previous.ws_row)return current;
  usleep(10000);
 }
 errno=ETIMEDOUT;fail(__LINE__);return previous;
}
static volatile uint32_t *map_framebuffer(int fd,size_t *length) {
 struct fb_fix_screeninfo info;memset(&info,0,sizeof(info));
 REQUIRE(ioctl(fd,FBIOGET_FSCREENINFO,&info)==0);
 *length=((size_t)info.smem_len+4095)&~(size_t)4095;
 REQUIRE(*length>=4096 && *length-info.smem_len>=sizeof(uint32_t));
 void *pixels=mmap(NULL,*length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 REQUIRE(pixels!=MAP_FAILED);return pixels;
}
static uint64_t epoch(void) {
 long value=syscall(0xB30,3,0,0,0,0,0);REQUIRE(value>=0);return (uint64_t)value;
}
int main(int argc,char **argv) {
 if(argc!=2)return 2;
 result_path=argv[1];REQUIRE(unlink(result_path)==0 || errno==ENOENT);
 struct sigevent event={0};event.sigev_notify=SIGEV_SIGNAL;event.sigev_signo=SIGALRM;
 timer_t watchdog;REQUIRE(timer_create(CLOCK_MONOTONIC,&event,&watchdog)==0);
 struct itimerspec deadline={0};deadline.it_value.tv_sec=150;
 REQUIRE(timer_settime(watchdog,0,&deadline,NULL)==0);
 (void)epoch(); /* Requires the test-only epoch observation for retirement proof. */
 struct winsize initial=size();
 REQUIRE(initial.ws_xpixel!=640 || initial.ws_ypixel!=480);
 char block[4096];
 for(size_t i=0;i<sizeof(block);++i)block[i]=(i%80==79)?'\n':(char)('a'+i%26);
 uint64_t start=millis();size_t total=0;
 while(total<262144){
  size_t remaining=262144-total;
  ssize_t written=write(1,block,remaining<sizeof(block)?remaining:sizeof(block));
  if(written<0 && errno==EINTR)continue;
  REQUIRE(written>0);total+=(size_t)written;
 }
 uint64_t output_ms=millis()-start;
 /* A terminal owner's exec must preserve its display without needing resize.
  * The old behavior auto-created a separate 640x480 window here instead. */
 int initial_fd=open("/dev/fb0",O_RDWR);REQUIRE(initial_fd>=0);
 struct fb_var_screeninfo initial_source={0};
 REQUIRE(ioctl(initial_fd,FBIOGET_VSCREENINFO,&initial_source)==0);
 REQUIRE(initial_source.xres==initial.ws_xpixel && initial_source.yres==initial.ws_ypixel);
 REQUIRE(close(initial_fd)==0);
 puts("TERMSTRESSCHECK SOURCE_AFTER_EXEC_OK");
 printf("\nTERMSTRESSCHECK OUTPUT_OK bytes=%zu ms=%llu; resize now\n",total,(unsigned long long)output_ms);fflush(stdout);
 struct winsize first=await_resize(initial);
 int fd=open("/dev/fb0",O_RDWR);REQUIRE(fd>=0);
 size_t old_length;volatile uint32_t *old=map_framebuffer(fd,&old_length);
 old[old_length/4-1]=0x42524f4f;
 puts("TERMSTRESSCHECK ALIAS_READY; resize again");fflush(stdout);
 struct winsize second=await_resize(first);
 /* Geometry is snapshotted under the resize lock, after retirement enqueue.
  * A third later frame entry proves the preceding epoch+2 drain completed. */
 uint64_t target=epoch()+3,retirement_start=millis();
 while(epoch()<target){REQUIRE(millis()-retirement_start<5000);usleep(1000);}
 REQUIRE(old[old_length/4-1]==0x42524f4f);
 size_t new_length;volatile uint32_t *fresh=map_framebuffer(fd,&new_length);
 REQUIRE(fresh[new_length/4-1]==0);
 fresh[new_length/4-1]=0x4b544553;
 REQUIRE(old[old_length/4-1]==0x42524f4f);
 REQUIRE(munmap((void *)old,old_length)==0 && munmap((void *)fresh,new_length)==0);
 REQUIRE(close(fd)==0);
 FILE *result=fopen(result_path,"w");REQUIRE(result!=NULL);
 REQUIRE(fprintf(result,"TERMSTRESSCHECK PASS bytes=%zu output_ms=%llu resize=%ux%u,%ux%u old_alias_canary=preserved new_padding=zero\n",total,(unsigned long long)output_ms,first.ws_col,first.ws_row,second.ws_col,second.ws_row)>0);
 REQUIRE(fclose(result)==0);
 puts("TERMSTRESSCHECK PASS output/resize/source-retirement/alias-canary");return 0;
}
