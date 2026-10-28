#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
int main(int argc,char** argv) {
    assert(argc==2);int fd=open(argv[1],O_RDONLY);assert(fd>=0);
    char buf[64];assert(read(fd,buf,sizeof(buf))==-1 && errno==EIO);
    pid_t child=fork();assert(child>=0);
    if(!child) {
        volatile unsigned char* p=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,fd,0);
        if((void*)p==MAP_FAILED){assert(errno==EIO);_exit(0);}
        volatile unsigned char value=p[0];(void)value;_exit(99);
    }
    int status;assert(waitpid(child,&status,0)==child);
    assert((WIFSIGNALED(status) && WTERMSIG(status)==SIGBUS) || (WIFEXITED(status) && WEXITSTATUS(status)==0));
    char* args[]={argv[1],NULL};execve(argv[1],args,environ);
    assert(errno==EIO || errno==ENOEXEC);assert(close(fd)==0);
    puts("COMPRESSED_IMAGE_CORRUPT_PASS read_EIO mapped_SIGBUS_or_EIO exec_rejected kernel_alive");
}
