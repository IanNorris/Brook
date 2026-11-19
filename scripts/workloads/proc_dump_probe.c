#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

static atomic_int waiting;

__attribute__((noinline)) static void* pipe_waiter(void* argument)
{
    const int fd=*(int*)argument;
    char byte=0;
    atomic_store(&waiting,1);
    (void)read(fd,&byte,1);
    return NULL;
}

__attribute__((noinline)) static void running_main(void)
{
    for(;;)__asm__ volatile("pause" ::: "memory");
}

static volatile uintptr_t fault_address;

__attribute__((noinline)) static void crash_frame(void)
{
    *(volatile int*)fault_address=5;
}

static int crash_probe(int disable)
{
    if(disable){
        struct rlimit limit={0,0};
        if(setrlimit(RLIMIT_CORE,&limit) || getrlimit(RLIMIT_CORE,&limit) ||
           limit.rlim_cur!=0 || limit.rlim_max!=0)return 4;
        puts("PROC_DUMP_LIMIT_ZERO");
    }
    printf("PROC_DUMP_CRASH %u\n",(unsigned)getpid());
    fflush(stdout);
    crash_frame();
    return 5;
}

static int export_crash(const char* pid_text,const char* artifact)
{
    static const char directory[]="/data/home/.local/share/brook/cores";
    const long pid=strtol(pid_text,NULL,10);
    if(pid<=0 || pid>65535 || strlen(artifact)!=32 ||
       strspn(artifact,"0123456789abcdef")!=32)return 6;
    DIR* listing=opendir(directory);
    if(!listing)return 7;
    char source[512]={};
    struct dirent* entry;
    while((entry=readdir(listing))){
        const char* dot=strchr(entry->d_name,'.');
        if(!dot || dot==entry->d_name)continue;
        char* end=NULL;
        if(strtol(dot+1,&end,10)!=pid || *end!='.')continue;
        if(!strstr(end,".core"))continue;
        if(snprintf(source,sizeof(source),"%s/%s",directory,entry->d_name)>=(int)sizeof(source))
            source[0]=0;
        break;
    }
    closedir(listing);
    if(!source[0])return 8;
    const int input=open(source,O_RDONLY|O_NOFOLLOW);
    if(input<0)return 9;
    char dest[128];
    snprintf(dest,sizeof(dest),"/data/.brook-transfer/%s.bin",artifact);
    const int output=open(dest,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(output<0){close(input);return 10;}
    char bytes[8192];
    ssize_t count;
    int status=0;
    while((count=read(input,bytes,sizeof(bytes)))>0){
        ssize_t offset=0;
        while(offset<count){
            const ssize_t written=write(output,bytes+offset,(size_t)(count-offset));
            if(written<=0){status=11;break;}
            offset+=written;
        }
        if(status)break;
    }
    if(count<0 || close(output)<0)status=12;
    close(input);
    return status;
}

int main(int argc,char** argv)
{
    if(argc==2 && !strcmp(argv[1],"--crash"))return crash_probe(0);
    if(argc==2 && !strcmp(argv[1],"--crash-limit-zero"))return crash_probe(1);
    if(argc==4 && !strcmp(argv[1],"--export-crash"))return export_crash(argv[2],argv[3]);
    if(argc!=1)return 13;
    int pipe_fds[2];
    pthread_t thread;
    if(pipe(pipe_fds) || pthread_create(&thread,NULL,pipe_waiter,&pipe_fds[0]))return 1;
    int file=open("/boot/PROCDUMP.ELF",O_RDONLY);
    if(file<0)return 2;
    void* named=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,file,0);
    close(file);
    if(named==MAP_FAILED || *(const volatile unsigned char*)named!=0x7f)return 3;
    while(!atomic_load(&waiting))sched_yield();
    printf("PROC_DUMP_READY %u\n",(unsigned)getpid());
    fflush(stdout);
    running_main();
}
