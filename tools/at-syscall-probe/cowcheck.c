#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    int *fds=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(fds==MAP_FAILED) return 1;
    fds[0]=fds[1]=-1;
    pid_t child=fork();
    if(child<0) return 1;
    if(child==0) _exit(0);
    int result=pipe(fds), error=errno;
    waitpid(child,NULL,0);
    if(result==0) { close(fds[0]); close(fds[1]); }
    if(mprotect(fds,4096,PROT_READ)!=0) return 1;
    errno=0;
    int protected_result=pipe(fds), protected_error=errno;
    munmap(fds,4096);
    int failed=result!=0||protected_result!=-1||protected_error!=EFAULT;
    printf("COWCHECK %s: post-fork pipe=%d errno=%d; read-only buffer errno=%d\n",failed?"FAIL":"PASS",result,error,protected_error);
    return failed;
}
