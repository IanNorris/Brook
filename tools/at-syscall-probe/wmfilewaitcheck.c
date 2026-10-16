#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
static uint64_t millis(void) {
 struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)){perror("clock_gettime");exit(2);}
 return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static long renderer_state(void) { return syscall(0xB30,1,0,0,0,0,0); }
static int await_renderer(int waiting) {
 uint64_t start=millis();
 while(millis()-start<5000){
  long state=renderer_state();
  if(state<0 || ((uint64_t)state>>32))return -1;
  if(state==(waiting?65537:1))return 0;
  usleep(1000);
 }
 return -1;
}
struct WindowResult { uint32_t id,stride;uint64_t pixels; };
int main(int argc,char** argv) {
 if(argc<2 || argc>3)return 2;
 int delayed=argc==3 && !strcmp(argv[2],"--delayed-renderer");
 if(argc==3 && !delayed)return 2;
 if(delayed && renderer_state()<0){perror("renderer test hooks required");return 2;}
 alarm(30);
 int fd=open(argv[1],O_CREAT|O_RDWR|O_TRUNC,0600);
 if(fd<0){perror("open");return 2;}
 for(unsigned round=0;round<32;++round){
  if(ftruncate(fd,4096)){perror("prepare truncate");return 2;}
  unsigned char expected=(unsigned char)(round+1);
  if(pwrite(fd,&expected,1,0)!=1)return 2;
  pid_t child=fork();if(child<0){perror("fork");return 2;}
  if(!child){
   volatile unsigned char* mapping=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,fd,0);
   if(mapping==MAP_FAILED||mapping[0]!=expected)_exit(3);
   struct WindowResult window={0};
   if(syscall(506,240,160,"Mapped file exit",&window,0)<0||!window.id||!window.pixels)_exit(4);
   ((volatile uint32_t*)(uintptr_t)window.pixels)[0]=0x0042756b;
   if(delayed){
    if(syscall(0xB30,0,window.pixels,4096,3,1,0))_exit(5);
    if(await_renderer(1))_exit(6);
   }
   /* Exit with both mappings alive. Consuming wait must release the file pin
      independently of the kernel framebuffer's compositor epoch retirement. */
   _exit(0);
  }
  int status=0;
  pid_t waited;
  if(delayed){
   uint64_t start=millis();
   while((waited=waitpid(child,&status,WNOHANG))==0 && millis()-start<5000)usleep(1000);
  }else waited=waitpid(child,&status,0);
  if(waited!=child||!WIFEXITED(status)||WEXITSTATUS(status)){
   if(delayed)syscall(0xB30,2,0,0,0,0,0);
   fprintf(stderr,"WMFILEWAITCHECK child round=%u status=%d errno=%d\n",round,status,errno);return 1;
  }
  if(ftruncate(fd,8192)||ftruncate(fd,1024)){
   fprintf(stderr,"WMFILEWAITCHECK truncate round=%u errno=%d\n",round,errno);return 1;
  }
  if(delayed){
   /* Prove wait and both truncates finished before the renderer was released. */
   if(renderer_state()!=65537)return 1;
   if(syscall(0xB30,2,0,0,0,0,0) || await_renderer(0))return 1;
  }
 }
 close(fd);unlink(argv[1]);
 printf("WMFILEWAITCHECK PASS rounds=32 exit/wait/immediate-truncate delayed_renderer=%d\n",delayed);return 0;
}
