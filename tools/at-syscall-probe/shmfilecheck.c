// Small disposable-VM IPC diagnostic; prints fixed numeric results only.
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/vfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
int main(void){setbuf(stdout,NULL);struct statfs s;int z=statfs("/dev/shm",&s);printf("SHMPROBE statfs result=%d errno=%d type=%lx\n",z,z<0?errno:0,z<0?0:s.f_type);
char p[]="/dev/shm/brook-ipc-XXXXXX";int f=mkstemp(p);printf("SHMPROBE create result=%d errno=%d\n",f,f<0?errno:0);if(f<0)return 1;
z=ftruncate(f,4096);printf("SHMPROBE truncate result=%d errno=%d\n",z,z<0?errno:0);if(z<0){close(f);unlink(p);return 2;}
void* v=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,f,0);printf("SHMPROBE mmap result=%d errno=%d\n",v!=MAP_FAILED,v==MAP_FAILED?errno:0);if(v==MAP_FAILED){close(f);unlink(p);return 3;}
*(volatile int*)v=123;printf("SHMPROBE store result=%d\n",*(volatile int*)v==123);munmap(v,4096);close(f);unlink(p);return 0;}
