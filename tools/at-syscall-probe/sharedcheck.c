#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static volatile int ready;
static volatile uint32_t *shared;
static void *worker(void *unused){
 (void)unused;while(!__atomic_load_n(&ready,__ATOMIC_ACQUIRE))usleep(1000);
 pid_t child=fork();if(child==0){*shared=77;_exit(0);}
 if(child>0)waitpid(child,NULL,0);return NULL;
}
int main(void){
 pthread_t thread;if(pthread_create(&thread,NULL,worker,NULL))return 1;
 int fd=memfd_create("shared-fork",MFD_CLOEXEC);
 if(fd<0||ftruncate(fd,4096))return 1;
 shared=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 if(shared==MAP_FAILED)return 1;
 *shared=1;pid_t child=fork();if(child<0)return 1;
 if(child==0){*shared=42;_exit(0);}
 waitpid(child,NULL,0);uint32_t child_value=*shared;
 *shared=43;uint32_t stored=0;lseek(fd,0,SEEK_SET);read(fd,&stored,sizeof(stored));
 __atomic_store_n(&ready,1,__ATOMIC_RELEASE);pthread_join(thread,NULL);
 uint32_t thread_value=*shared;
 int good=child_value==42&&stored==43&&thread_value==77;
 printf("SHAREDCHECK %s: child=%u expected42; backing=%u expected43; thread-fork=%u expected77\n",good?"PASS":"FAIL",child_value,stored,thread_value);
 munmap((void*)shared,4096);close(fd);return !good;
}
