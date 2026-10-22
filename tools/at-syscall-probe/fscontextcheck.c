#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// musl's public clone wrapper deliberately rejects CLONE_THREAD. Its raw
// trampoline exposes the kernel ABI needed by this compatibility fixture.
extern int __clone(int (*)(void*),void*,int,void*,int*,void*,int*);
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) {fprintf(stderr,"FSCONTEXT FAIL line=%d %s errno=%d (%s)\n",__LINE__,#x,errno,strerror(errno));return 1;} } while(0)
struct child { const char* path;const char* executable;int mode,pipe; };
static int child_main(void* arg) {
    struct child* c=arg;
    if(c->mode==2) {
        char* const args[]={(char*)c->executable,"--exec-child",(char*)c->path,NULL};
        execv(c->executable,args);return 91;
    }
    if(c->mode==3) {
        char* const args[]={"missing",NULL};
        execv("no-such-executable-fscontext",args);
        if(errno!=ENOENT)return 92;
    }
    if(syscall(SYS_chdir,c->path))return 93;
    syscall(SYS_umask,077);
    if(c->mode==4) {
        char token='T';syscall(SYS_write,c->pipe,&token,1);
        syscall(SYS_exit,0);__builtin_unreachable();
    }
    return 0;
}
static int run_child(struct child* c,int flags) {
    void* stack=malloc(128*1024);if(!stack)return -1;
    pid_t pid=clone(child_main,(char*)stack+128*1024,flags|SIGCHLD,c);
    if(pid<0) {free(stack);return -1;}
    int status=0;pid_t waited=waitpid(pid,&status,0);free(stack);
    return waited==pid && WIFEXITED(status)?WEXITSTATUS(status):-1;
}
static mode_t current_mask(void) {mode_t mask=umask(0);umask(mask);return mask;}
int main(int argc,char** argv) {
    if(argc==3 && !strcmp(argv[1],"--exec-child")) {
        if(chdir(argv[2]))return 94;
        umask(077);return 0;
    }
    const char* parent=argc>1?argv[1]:"/data/home";
    const int exec_shared=1; // Linux retains CLONE_FS across ordinary exec.
    char original[512],root[512],a[512],b[512],moved[512],exe[512];
    CHECK(getcwd(original,sizeof(original)));
    ssize_t exe_length=readlink("/proc/self/exe",exe,sizeof(exe)-1);
    CHECK(exe_length>0);exe[exe_length]=0;
    struct timespec now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));
    CHECK(snprintf(root,sizeof(root),"%s/fscontext-%d-%ld",parent,getpid(),now.tv_nsec)<(int)sizeof(root));
    CHECK(!mkdir(root,0755));CHECK(!chdir(root));
    CHECK(!mkdir("a",0755));CHECK(!mkdir("b",0755));
    CHECK(snprintf(a,sizeof(a),"%s/a",root)<(int)sizeof(a));
    CHECK(snprintf(b,sizeof(b),"%s/b",root)<(int)sizeof(b));
    CHECK(snprintf(moved,sizeof(moved),"%s/moved",root)<(int)sizeof(moved));
    int directory=open("a",O_RDONLY|O_DIRECTORY);CHECK(directory>=0);
    CHECK(!chdir("a"));CHECK(!rename(a,moved));CHECK(!mkdir(a,0755));
    int file=open("marker",O_CREAT|O_EXCL|O_WRONLY,0640);CHECK(file>=0);CHECK(write(file,"held",4)==4);CHECK(!close(file));
    struct stat st;CHECK(!fstatat(directory,"marker",&st,0));CHECK(S_ISREG(st.st_mode));
    CHECK(fstatat(AT_FDCWD,a,&st,0)==0 && S_ISDIR(st.st_mode));
    char cwd[512];CHECK(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,moved));
    CHECK(!chdir(a));CHECK(access("marker",F_OK)<0 && errno==ENOENT);
    CHECK(!fchdir(directory));CHECK(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,moved));
    file=open("/proc/self/cwd/marker",O_RDONLY);CHECK(file>=0);CHECK(!close(file));
    CHECK(!symlink("marker","link"));char target[32]={0};CHECK(readlinkat(directory,"link",target,sizeof(target))==6 && !strcmp(target,"marker"));
    CHECK(!fstatat(-999,b,&st,0)); // absolute ignores invalid dirfd
    errno=0;CHECK(fstatat(-999,"marker",&st,0)<0 && errno==EBADF);
    CHECK(!chdir(root));umask(022);
    struct child c={b,exe,0,-1};
    CHECK(run_child(&c,0)==0);CHECK(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,root));CHECK(current_mask()==022);
    CHECK(run_child(&c,CLONE_FS)==0);CHECK(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,b));CHECK(current_mask()==077);
    CHECK(!chdir(root));umask(022);c.mode=2;
    CHECK(run_child(&c,CLONE_FS)==0);
    CHECK(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,exec_shared?b:root));
    CHECK(current_mask()==(exec_shared?077:022));
    CHECK(!chdir(root));umask(022);
    c.mode=3;CHECK(run_child(&c,CLONE_FS)==0);CHECK(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,b));CHECK(current_mask()==077);
    CHECK(!chdir(root));umask(022);
    // A real thread without CLONE_FS must own an independent context. Use only
    // raw successful syscalls in that thread, avoiding shared libc/TLS state.
    int pipefd[2];CHECK(!pipe(pipefd));void* stack=malloc(128*1024);CHECK(stack);
    c.mode=4;c.pipe=pipefd[1];
    int child_tid=0;
    int tid=__clone(child_main,(char*)stack+128*1024,
                    CLONE_VM|CLONE_THREAD|CLONE_SIGHAND|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID,
                    &c,&child_tid,NULL,&child_tid);CHECK(tid>0);
    char token=0;CHECK(read(pipefd[0],&token,1)==1 && token=='T');
    CHECK(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,root));CHECK(current_mask()==022);
    unsigned waited=0;
    while(__atomic_load_n(&child_tid,__ATOMIC_ACQUIRE) && waited++<5000)usleep(1000);
    CHECK(!__atomic_load_n(&child_tid,__ATOMIC_ACQUIRE));free(stack);
    CHECK(!close(pipefd[0]));CHECK(!close(pipefd[1]));
    int proc=open("/proc/self",O_RDONLY|O_DIRECTORY);CHECK(proc>=0);
    file=openat(proc,"status",O_RDONLY);CHECK(file>=0);char contents[256];CHECK(read(file,contents,sizeof(contents))>0);CHECK(!close(file));
    CHECK(!fstatat(proc,"status",&st,0) && S_ISREG(st.st_mode));
    CHECK(!fchdir(proc));CHECK(getcwd(cwd,sizeof(cwd)) && !strncmp(cwd,"/proc/",6));CHECK(!chdir(root));CHECK(!close(proc));
    // Mask is applied once by retained parent creation.
    umask(027);file=open("masked",O_CREAT|O_EXCL|O_WRONLY,0666);CHECK(file>=0);CHECK(!fstat(file,&st) && (st.st_mode&0777)==0640);CHECK(!close(file));
    CHECK(!unlink("masked"));CHECK(!unlinkat(directory,"link",0));CHECK(!unlinkat(directory,"marker",0));CHECK(!close(directory));
    CHECK(!rmdir(moved));CHECK(!rmdir(a));CHECK(!rmdir(b));CHECK(!chdir(original));CHECK(!rmdir(root));
    printf("FSCONTEXT PASS checks=%u rename/dirfd/fork/CLONE_FS/thread-copy/exec-preserve/failed-exec/proc/mask\n",checks);
    return 0;
}
