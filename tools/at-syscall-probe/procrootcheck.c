#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"PROCROOT FAIL line=%d %s errno=%d (%s)\n",__LINE__,#x,errno,strerror(errno));return 1;}}while(0)
static int same(const struct stat* a,const struct stat* b) {return a->st_dev==b->st_dev && a->st_ino==b->st_ino;}
int main(int argc,char** argv) {
    int brook=argc>1 && !strcmp(argv[1],"--brook");
    char saved[512];CHECK(getcwd(saved,sizeof(saved)));
    struct stat root,link,target;
    CHECK(!stat("/",&root) && S_ISDIR(root.st_mode));
    CHECK(!lstat("/proc/self/root",&link) && S_ISLNK(link.st_mode));
    char text[512]={0};ssize_t length=readlink("/proc/self/root",text,sizeof(text)-1);
    CHECK(length==1 && text[0]=='/');
    char guard[3]={'a','b','c'};CHECK(readlink("/proc/self/root",guard+1,1)==1 && guard[0]=='a' && guard[1]=='/' && guard[2]=='c');
    CHECK(!stat("/proc/self/root",&target) && same(&root,&target));
    int fd=open("/proc/self/root",O_RDONLY|O_DIRECTORY);CHECK(fd>=0);
    CHECK(!fstat(fd,&target) && same(&root,&target));
    CHECK(!fstatat(fd,".",&target,0) && same(&root,&target));
    CHECK(!fstatat(fd,"..",&target,0) && same(&root,&target));
    int proc=open("/proc/self",O_RDONLY|O_DIRECTORY);CHECK(proc>=0);
    CHECK(!fstatat(proc,"root",&target,0) && same(&root,&target));
    CHECK(!fstatat(proc,"root",&target,AT_SYMLINK_NOFOLLOW) && S_ISLNK(target.st_mode));
    int other=openat(proc,"root/proc/self/status",O_RDONLY);CHECK(other>=0);
    CHECK(read(other,text,sizeof(text))>0);CHECK(!close(other));
    CHECK(!fchdir(fd));CHECK(getcwd(text,sizeof(text)) && !strcmp(text,"/"));
    CHECK(!chdir(saved));
    CHECK(!close(proc));CHECK(!close(fd));
    // The supported permission policy is intentionally narrower than Linux:
    // another process's magic root is denied until inspection policy exists.
    int ready[2],finish[2];CHECK(!pipe(ready));CHECK(!pipe(finish));
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        close(ready[0]);close(finish[1]);char token='R';
        if(write(ready[1],&token,1)!=1 || read(finish[0],&token,1)!=1)_exit(21);
        _exit(0);
    }
    CHECK(!close(ready[1]));CHECK(!close(finish[0]));char token=0;
    CHECK(read(ready[0],&token,1)==1 && token=='R');
    snprintf(text,sizeof(text),"/proc/%d/root",child);
    errno=0;other=open(text,O_RDONLY|O_DIRECTORY);
    if(brook) {CHECK(other<0 && errno==EACCES);}
    else {CHECK(other>=0);CHECK(!close(other));}
    CHECK(write(finish[1],"X",1)==1);int status=0;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(!close(ready[0]));CHECK(!close(finish[1]));
    if(brook) {
        // These isolation flags must fail before creating a global-namespace
        // child. A successful ordinary fork would be false isolation support.
        errno=0;long isolated=syscall(SYS_clone,0x10020000UL|SIGCHLD,0,0,0,0);
        if(!isolated)_exit(79);
        if(isolated>0)waitpid(isolated,NULL,0);
        CHECK(isolated<0 && errno==EINVAL);
        unsigned long args[11]={0};args[0]=0x80;args[4]=SIGCHLD;
        errno=0;isolated=syscall(SYS_clone3,args,sizeof(args));
        if(!isolated)_exit(80);
        if(isolated>0)waitpid(isolated,NULL,0);
        CHECK(isolated<0 && errno==EINVAL);
    }
    printf("PROCROOT PASS checks=%u readlink/lstat/stat/open/dirfd/root-dotdot/cross-process-policy\n",checks);
    return 0;
}
