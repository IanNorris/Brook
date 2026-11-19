#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
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

static const char directory[]="/data/home/.local/share/brook/cores";

static int crash_mount_namespace(void)
{
    const long child=syscall(SYS_clone,CLONE_NEWNS|SIGCHLD,0L,0L,0L,0L);
    if(child<0){perror("clone mount namespace");return 14;}
    if(child){
        int status=0;
        return waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV?0:15;
    }
    if(mount("tmpfs","/data/home/.local","tmpfs",MS_NODEV|MS_NOSUID,"mode=0755")){
        perror("mount private home");_exit(16);
    }
    if(access(directory,F_OK)==0 || errno!=ENOENT)_exit(17);
    if(setgid(1000) || setuid(1000) || getuid()!=1000 || geteuid()!=1000)_exit(18);
    puts("PROC_DUMP_PRIVATE_HOME");
    return crash_probe(0);
}

static int install_legacy_directory(void)
{
    const char* paths[]={"/data/home/.local","/data/home/.local/share",
                         "/data/home/.local/share/brook",directory};
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);++i)
        if(mkdir(paths[i],paths[i]==directory?0700:0755) && errno!=EEXIST)return 19;
    struct stat st;
    return stat(directory,&st) || st.st_uid || (st.st_mode&0777)!=0700?20:0;
}

static int open_crash_core(const char* pid_text)
{
    const long pid=strtol(pid_text,NULL,10);
    if(pid<=0 || pid>65535)return -1;
    DIR* listing=opendir(directory);
    if(!listing)return -1;
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
    return source[0]?open(source,O_RDONLY|O_NOFOLLOW):-1;
}

static int read_owned_crash(const char* pid_text)
{
    struct stat dir;
    if(getuid()!=1000 || stat(directory,&dir) || dir.st_uid!=1000 || (dir.st_mode&0777)!=0700)return 21;
    const int fd=open_crash_core(pid_text);
    if(fd<0)return 22;
    struct stat file;
    char magic[4];
    const int ok=!fstat(fd,&file) && file.st_uid==1000 && (file.st_mode&0777)==0600 &&
        read(fd,magic,sizeof(magic))==(ssize_t)sizeof(magic) && !memcmp(magic,"\177ELF",4);
    close(fd);
    if(!ok)return 23;
    puts("PROC_DUMP_OWNER_READ_PASS");
    return 0;
}

static int export_crash(const char* pid_text,const char* artifact)
{
    if(strlen(artifact)!=32 || strspn(artifact,"0123456789abcdef")!=32)return 6;
    const int input=open_crash_core(pid_text);
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
    if(argc==2 && !strcmp(argv[1],"--crash-mount-ns"))return crash_mount_namespace();
    if(argc==2 && !strcmp(argv[1],"--legacy-core-directory"))return install_legacy_directory();
    if(argc==3 && !strcmp(argv[1],"--read-owned-crash"))return read_owned_crash(argv[2]);
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
