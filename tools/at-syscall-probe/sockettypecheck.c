// Linux/Brook socket I/O error distinction and send-to-write fallback.
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

static unsigned checks;
#define CK(x) do { ++checks; if (!(x)) { fprintf(stderr,"SOCKETTYPE FAIL line=%d %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
static void errors(int fd, int error) {
    char buffer[8] = {0};
    errno=0; CK(send(fd, buffer, 1, MSG_NOSIGNAL)==-1 && errno==error);
    errno=0; CK(send(fd, buffer, 0, MSG_NOSIGNAL)==-1 && errno==error);
    errno=0; CK(recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT)==-1 && errno==error);
    errno=0; CK(recv(fd, buffer, 0, MSG_DONTWAIT)==-1 && errno==error);
    errno=0; CK(syscall(SYS_sendto, ((uint64_t)0x12345678<<32)|(uint32_t)fd,
                       buffer, 1, MSG_NOSIGNAL, NULL, 0)==-1 && errno==error);
    errno=0; CK(syscall(SYS_recvfrom, ((uint64_t)0x12345678<<32)|(uint32_t)fd,
                       buffer, 1, MSG_DONTWAIT, NULL, NULL)==-1 && errno==error);
}
static ssize_t fallback(int fd, const void* data, size_t length) {
    ssize_t result=send(fd,data,length,MSG_NOSIGNAL);
    return result<0 && errno==ENOTSOCK ? write(fd,data,length) : result;
}
int main(void) {
    errors(-1,EBADF);
    int pipefd[2]; CK(!pipe2(pipefd,O_NONBLOCK|O_CLOEXEC));
    errors(pipefd[0],ENOTSOCK); errors(pipefd[1],ENOTSOCK);
    char byte=0; CK(read(pipefd[0],&byte,1)==-1 && errno==EAGAIN);
    CK(fallback(pipefd[1],"x",1)==1);
    CK(read(pipefd[0],&byte,1)==1 && byte=='x');
    int closed=pipefd[0]; CK(!close(closed)); errors(closed,EBADF); CK(!close(pipefd[1]));
    int efd=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK); CK(efd>=0);
    errors(efd,ENOTSOCK); uint64_t value=0;
    CK(read(efd,&value,8)==-1 && errno==EAGAIN);
    value=7; CK(fallback(efd,&value,8)==8); errors(efd,ENOTSOCK);
    value=0; CK(read(efd,&value,8)==8 && value==7); CK(!close(efd));
    int mem=syscall(SYS_memfd_create,"sockettype-control",1); CK(mem>=0);
    errors(mem,ENOTSOCK); CK(fallback(mem,"m",1)==1); CK(lseek(mem,0,SEEK_SET)==0);
    errors(mem,ENOTSOCK); CK(read(mem,&byte,1)==1 && byte=='m'); CK(!close(mem));
    int dev=open("/dev/null",O_RDWR); CK(dev>=0); errors(dev,ENOTSOCK);
    CK(fallback(dev,"n",1)==1); CK(!close(dev));
    int path=open("/",O_PATH|O_CLOEXEC); CK(path>=0); errors(path,EBADF); CK(!close(path));
    int sockets[2]; CK(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    CK(send(sockets[0],"s",1,MSG_NOSIGNAL)==1);
    CK(recv(sockets[1],&byte,1,0)==1 && byte=='s');
    CK(!close(sockets[0]) && !close(sockets[1]));
    printf("SOCKETTYPE_PASS checks=%u\n",checks);
    return 0;
}
