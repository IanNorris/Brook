#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned failures, checks;
#define CHECK(test) do { ++checks; if (!(test)) { ++failures; printf("FAIL line=%d errno=%d\n", __LINE__, errno); } } while (0)

static void receive(int fd, pid_t expected, int enabled, size_t capacity, int rights)
{
    char byte = 0;
    union { struct cmsghdr align; char bytes[128]; } control;
    memset(&control, 0, sizeof(control));
    struct iovec iov = { &byte, 1 };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = capacity };
    CHECK(recvmsg(fd, &msg, MSG_CMSG_CLOEXEC) == 1 && byte == 'x');
    int credentials = 0, handles = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        CHECK(c->cmsg_len >= CMSG_LEN(0));
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS &&
            c->cmsg_len == CMSG_LEN(sizeof(struct ucred))) {
            struct ucred u;
            memcpy(&u, CMSG_DATA(c), sizeof(u));
            CHECK(u.pid == expected && u.uid == getuid() && u.gid == getgid());
            ++credentials;
        }
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int received;
            memcpy(&received, CMSG_DATA(c), sizeof(received));
            char marker = 0;
            CHECK((fcntl(received, F_GETFD) & FD_CLOEXEC) && read(received, &marker, 1) == 1 && marker == 'm');
            close(received);
            ++handles;
        }
    }
    if (capacity >= CMSG_SPACE(sizeof(struct ucred))) {
        CHECK(credentials == enabled && handles == rights && !(msg.msg_flags & MSG_CTRUNC));
    } else CHECK((msg.msg_flags & MSG_CTRUNC) != 0);
}

static int explicit_send(int fd,struct ucred credentials,int rights,int malformed)
{
    union {struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int))+CMSG_SPACE(sizeof(struct ucred))];} control;
    memset(&control,0,sizeof(control));size_t offset=0;
    if(rights>=0) {
        struct cmsghdr *c=(struct cmsghdr *)control.bytes;
        c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c),&rights,sizeof(rights));offset=CMSG_SPACE(sizeof(int));
    }
    struct cmsghdr *c=(struct cmsghdr *)(control.bytes+offset);
    c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_CREDENTIALS;
    c->cmsg_len=CMSG_LEN(sizeof(credentials))-(malformed?1:0);
    memcpy(CMSG_DATA(c),&credentials,sizeof(credentials));
    struct iovec iov={(void *)"x",1};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,
        .msg_controllen=offset+CMSG_SPACE(sizeof(credentials))};
    return sendmsg(fd,&msg,MSG_NOSIGNAL);
}
static void nothing_delivered(int fd) {
    struct pollfd p={fd,POLLIN,0};CHECK(poll(&p,1,0)==0);
}
static void explicit_rollback(struct ucred own)
{
    int sockets[2],p[2];
    CHECK(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0,sockets)==0);
    CHECK(pipe2(p,O_CLOEXEC|O_NONBLOCK)==0);
    char bytes[4096];memset(bytes,'f',sizeof(bytes));size_t filled=0;
    for(;;) {
        ssize_t n=write(sockets[1],bytes,sizeof(bytes));
        if(n<0) {CHECK(errno==EAGAIN);break;}
        CHECK(n>0);if(n<=0)break;filled+=(size_t)n;
    }
    errno=0;CHECK(explicit_send(sockets[1],own,p[1],0)==-1 && errno==EAGAIN);
    CHECK(close(p[1])==0);CHECK(read(p[0],bytes,1)==0);close(p[0]);
    size_t drained=0;
    while(drained<filled) {
        ssize_t n=read(sockets[0],bytes,sizeof(bytes));CHECK(n>0);if(n<=0)break;
        drained+=(size_t)n;
    }
    CHECK(drained==filled);nothing_delivered(sockets[0]);
    // Rights precede a credentials record crossing into an inaccessible page.
    // The entire rejected message must leave no bytes or descriptor references.
    CHECK(pipe2(p,O_CLOEXEC|O_NONBLOCK)==0);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    char *mapping=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(mapping!=MAP_FAILED);
    if(mapping!=MAP_FAILED) {
        char *control=mapping+page-CMSG_SPACE(sizeof(int))-CMSG_LEN(0);
        struct cmsghdr *rights=(struct cmsghdr *)control;
        rights->cmsg_level=SOL_SOCKET;rights->cmsg_type=SCM_RIGHTS;rights->cmsg_len=CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(rights),&p[1],sizeof(p[1]));
        struct cmsghdr *credentials=(struct cmsghdr *)(control+CMSG_SPACE(sizeof(int)));
        credentials->cmsg_level=SOL_SOCKET;credentials->cmsg_type=SCM_CREDENTIALS;
        credentials->cmsg_len=CMSG_LEN(sizeof(own));memcpy(CMSG_DATA(credentials),&own,sizeof(own));
        CHECK(mprotect(mapping+page,page,PROT_NONE)==0);
        struct iovec iov={(void *)"x",1};
        struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,
            .msg_controllen=CMSG_SPACE(sizeof(int))+CMSG_SPACE(sizeof(own))};
        errno=0;CHECK(sendmsg(sockets[1],&msg,MSG_NOSIGNAL)==-1 && errno==EFAULT);
        CHECK(munmap(mapping,2*page)==0);
    }
    CHECK(close(p[1])==0);CHECK(read(p[0],bytes,1)==0);close(p[0]);
    nothing_delivered(sockets[0]);close(sockets[0]);close(sockets[1]);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    syscall(SYS_alarm, 60);
    int sockets[2], enabled = 1, value = -1;
    socklen_t size = sizeof(value);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) return 1;
    CHECK(getsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &value, &size) == 0 && value == 0);
    CHECK(setsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0);
    size = sizeof(value);
    CHECK(getsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &value, &size) == 0 && value == 1);
    errno = 0;
    CHECK(setsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &enabled, 1) == -1 && errno == EINVAL);
    struct ucred peer;
    size = sizeof(peer);
    CHECK(getsockopt(sockets[0], SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 && peer.pid == getpid());
    /* The endpoint was created by the parent. Each actual writer's identity
     * must survive its exit and reach the receiver with the matching bytes. */
    for (unsigned round = 0; round < 96; ++round) {
        pid_t child = fork();
        if (child == 0) { close(sockets[0]); _exit(write(sockets[1], "xx", 2) == 2 ? 0 : 1); }
        if (child < 0) return 1;
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        receive(sockets[0], child, 1, 128, 0);
        receive(sockets[0], child, 1, 128, 0);
    }
    int backing = memfd_create("passcred", MFD_CLOEXEC);
    if (backing < 0) return 1;
    CHECK(write(backing, "m", 1) == 1 && lseek(backing, 0, SEEK_SET) == 0);
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    struct iovec iov = { "x", 1 };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &backing, sizeof(int));
    if (sendmsg(sockets[1], &msg, 0) != 1) { perror("sendmsg rights"); return 1; }
    close(backing);
    receive(sockets[0], getpid(), 1, 128, 1);
    CHECK(write(sockets[1], "x", 1) == 1);
    receive(sockets[0], getpid(), 1, 0, 0);
    CHECK(write(sockets[1], "x", 1) == 1);
    receive(sockets[0], getpid(), 1, CMSG_LEN(sizeof(struct ucred)) - 1, 0);
    enabled = 0;
    CHECK(setsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0);
    CHECK(write(sockets[1], "x", 1) == 1);
    receive(sockets[0], getpid(), 0, 128, 0);
    // Explicit unprivileged credentials use the actual identity, never an
    // unchecked assertion. SO_PASSCRED affects delivery, not validation.
    struct ucred own={getpid(),getuid(),getgid()};
    CHECK(explicit_send(sockets[1],own,-1,0)==1);
    receive(sockets[0],getpid(),0,128,0);
    enabled=1;CHECK(setsockopt(sockets[0],SOL_SOCKET,SO_PASSCRED,&enabled,sizeof(enabled))==0);
    CHECK(explicit_send(sockets[1],own,-1,0)==1);
    receive(sockets[0],getpid(),1,128,0);
    pid_t explicit_child=fork();
    if(!explicit_child) {
        struct ucred child_own={getpid(),getuid(),getgid()};
        _exit(explicit_send(sockets[1],child_own,-1,0)==1?0:2);
    }
    CHECK(explicit_child>0);int explicit_status=0;
    CHECK(waitpid(explicit_child,&explicit_status,0)==explicit_child && WIFEXITED(explicit_status) && WEXITSTATUS(explicit_status)==0);
    receive(sockets[0],explicit_child,1,128,0);
    int extra=memfd_create("explicit-passcred",MFD_CLOEXEC);CHECK(extra>=0);
    CHECK(write(extra,"m",1)==1 && lseek(extra,0,SEEK_SET)==0);
    CHECK(explicit_send(sockets[1],own,extra,0)==1);close(extra);
    receive(sockets[0],getpid(),1,128,1);
    struct ucred forged=own;forged.pid=0;errno=0;
    CHECK(explicit_send(sockets[1],forged,-1,0)==-1 && (errno==EPERM || errno==ESRCH));nothing_delivered(sockets[0]);
    forged=own;forged.uid=getuid()+1;errno=0;
    CHECK(explicit_send(sockets[1],forged,-1,0)==-1 && (errno==EPERM || errno==EINVAL));nothing_delivered(sockets[0]);
    forged=own;forged.gid=getgid()+1;errno=0;
    CHECK(explicit_send(sockets[1],forged,-1,0)==-1 && (errno==EPERM || errno==EINVAL));nothing_delivered(sockets[0]);
    forged=own;forged.uid=(uid_t)-1;errno=0;
    CHECK(explicit_send(sockets[1],forged,-1,0)==-1 && errno==EINVAL);nothing_delivered(sockets[0]);
    errno=0;CHECK(explicit_send(sockets[1],own,-1,1)==-1 && errno==EINVAL);nothing_delivered(sockets[0]);
    // Rejected credentials after SCM_RIGHTS must release the staged endpoint.
    int rollback[2];CHECK(pipe2(rollback,O_CLOEXEC|O_NONBLOCK)==0);
    forged=own;forged.pid=0;errno=0;
    CHECK(explicit_send(sockets[1],forged,rollback[1],0)==-1 && (errno==EPERM || errno==ESRCH));
    CHECK(close(rollback[1])==0);char eof=0;CHECK(read(rollback[0],&eof,1)==0);close(rollback[0]);nothing_delivered(sockets[0]);
    close(sockets[0]); close(sockets[1]);
    explicit_rollback(own);
    printf("PASSCREDCHECK %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
