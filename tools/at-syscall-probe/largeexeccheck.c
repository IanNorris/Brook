#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static unsigned ticks,stop;
static void *heartbeat(void *unused) {
    (void)unused;
    while (!__atomic_load_n(&stop,__ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&ticks,1,__ATOMIC_RELEASE);
        usleep(1000);
    }
    return NULL;
}
static int tests(const char *self,const char *large) {
    int fd=open(self,O_RDONLY);struct stat st;
    if(fd<0||fstat(fd,&st)||st.st_size<4096||st.st_size>1024*1024) return 2;
    size_t size=(size_t)st.st_size;
    unsigned char *original=malloc(size+4),*copy=malloc(size+4);
    if(!original||!copy) return 2;
    size_t done=0;while(done<size) {ssize_t n=read(fd,original+done,size-done);if(n<=0)return 2;done+=(size_t)n;}close(fd);
    pthread_t thread;if(pthread_create(&thread,NULL,heartbeat,NULL))return 2;
    while(!__atomic_load_n(&ticks,__ATOMIC_ACQUIRE))usleep(1000);
    for(int test=0;test<7;++test) {
        memcpy(copy,original,size);Elf64_Ehdr *h=(void*)copy;
        Elf64_Phdr *ph=(void*)(copy+h->e_phoff),*load=NULL,*spare=NULL;
        for(unsigned i=0;i<h->e_phnum;++i) {if(ph[i].p_type==PT_LOAD&&!load)load=&ph[i];if(ph[i].p_type==PT_GNU_STACK)spare=&ph[i];}
        if(!load||!spare)return 2;
        size_t written=size;
        switch(test) {
        case 0:h->e_phoff=UINT64_MAX-4;break;
        case 1:h->e_phentsize=1;break;
        case 2:load->p_offset=size+1;break;
        case 3:load->p_filesz=load->p_memsz+1;break;
        case 4:load->p_vaddr=UINT64_MAX-1;load->p_memsz=4096;break;
        case 5:load->p_memsz=513ULL*1024*1024;break;
        case 6:spare->p_type=PT_INTERP;spare->p_offset=size;spare->p_filesz=4;spare->p_memsz=4;memcpy(copy+size,"/bad",4);written+=4;break;
        }
        char path[96];snprintf(path,sizeof(path),"/data/elf-malformed-%d-%d",(int)getpid(),test);
        fd=open(path,O_CREAT|O_WRONLY|O_TRUNC,0755);if(fd<0)return 2;
        done=0;while(done<written){ssize_t n=write(fd,copy+done,written-done);if(n<=0)return 2;done+=(size_t)n;}close(fd);
        char *args[]={path,NULL};errno=0;
        int r=execve(path,args,environ),error=errno;unlink(path);
        unsigned before=__atomic_load_n(&ticks,__ATOMIC_ACQUIRE);usleep(5000);
        if(r!=-1||error!=ENOEXEC||__atomic_load_n(&ticks,__ATOMIC_ACQUIRE)<=before) {
            printf("LARGEEXECCHECK FAIL malformed=%d result=%d errno=%d heartbeat=%u/%u\n",test,r,error,before,ticks);return 1;
        }
    }
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);if(pthread_join(thread,NULL))return 2;
    free(copy);free(original);
    pid_t p=fork();if(p<0)return 2;
    if(!p){execl(large,large,"exec",(char*)NULL);_exit(127);}
    int status;if(waitpid(p,&status,0)!=p||!WIFEXITED(status)||WEXITSTATUS(status)!=37)return 1;
    puts("LARGEEXECCHECK PASS malformed=7 live_sibling=preserved exec_payload=40MiB");return 0;
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);if(argc!=2)return 2;
    pid_t child=fork();if(child<0)return 2;if(!child)_exit(tests(argv[0],argv[1]));
    for(unsigned i=0;i<6000;++i){int status;pid_t p=waitpid(child,&status,WNOHANG);if(p==child)return WIFEXITED(status)?WEXITSTATUS(status):1;if(p<0&&errno!=EINTR)return 2;usleep(5000);}
    puts("LARGEEXECCHECK FAIL watchdog");kill(child,SIGKILL);return 1;
}
