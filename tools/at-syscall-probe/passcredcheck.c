#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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
    close(sockets[0]); close(sockets[1]);
    printf("PASSCREDCHECK %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
