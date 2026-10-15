#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { printf("UNIXPENDINGCHECK FAIL line=%d errno=%d: %s\n", __LINE__, errno, #x); ++failures; } } while (0)
static struct sockaddr_un address;
static int listener(unsigned round) {
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "/tmp/pending-%ld-%u.sock", (long)getpid(), round);
    unlink(address.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    CHECK(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(listen(fd, 4) == 0);
    return fd;
}
static void send_capability(int fd) {
    int mem = memfd_create("pending-marker", MFD_CLOEXEC);
    CHECK(mem >= 0 && write(mem, "retained", 8) == 8);
    char payload = 'P'; struct iovec iov = {&payload, 1};
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    struct msghdr msg = {.msg_iov=&iov, .msg_iovlen=1, .msg_control=control.bytes, .msg_controllen=sizeof(control)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &mem, sizeof(mem));
    CHECK(sendmsg(fd, &msg, MSG_NOSIGNAL) == 1);
    close(mem);
}
static void receive_capability(int fd) {
    char payload = 0; struct iovec iov = {&payload, 1};
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    struct msghdr msg = {.msg_iov=&iov, .msg_iovlen=1, .msg_control=control.bytes, .msg_controllen=sizeof(control)};
    CHECK(recvmsg(fd, &msg, 0) == 1 && payload == 'P');
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
        CHECK(0); return;
    }
    int mem = -1; memcpy(&mem, CMSG_DATA(c), sizeof(mem));
    char bytes[8] = {};
    CHECK(pread(mem, bytes, sizeof(bytes), 0) == 8 && !memcmp(bytes, "retained", 8));
    close(mem);
    CHECK(read(fd, &payload, 1) == 0); // closed client leaves data then EOF
}
static void early_close(unsigned round) {
    int server = listener(round), barrier[2]; CHECK(pipe(barrier) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        close(server); close(barrier[0]);
        // Blocking connect must finish before accept is called. The barrier
        // makes the original missing pending-owner sequence deterministic.
        int client = socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(client >= 0 && connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
        send_capability(client); close(client);
        CHECK(write(barrier[1], "x", 1) == 1); close(barrier[1]);
        _exit(failures ? 1 : 0);
    }
    close(barrier[1]);
    struct pollfd ready = {barrier[0], POLLIN, 0};
    int polled;
    do { polled = poll(&ready, 1, 5000); } while (polled < 0 && errno == EINTR);
    if (polled <= 0) {
        CHECK(0); kill(child, SIGKILL); waitpid(child, NULL, 0);
        close(barrier[0]); close(server); unlink(address.sun_path); return;
    }
    char byte; CHECK(read(barrier[0], &byte, 1) == 1); close(barrier[0]);
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    // Encourage reuse of a prematurely freed ancillary queue by fresh sockets.
    int churn[12];
    for (unsigned i=0; i<12; ++i) { churn[i] = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(churn[i]>=0); }
    int accepted = accept(server, NULL, NULL); CHECK(accepted >= 0);
    if (accepted >= 0) { receive_capability(accepted); close(accepted); }
    for (unsigned i=0; i<12; ++i) {
        int type=-1; socklen_t n=sizeof(type);
        CHECK(getsockopt(churn[i], SOL_SOCKET, SO_TYPE, &type, &n) == 0 && type == SOCK_STREAM);
        close(churn[i]);
    }
    close(server); unlink(address.sun_path);
}
static void abandoned(unsigned round) {
    int server = listener(round), client = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(client >= 0 && connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
    send_capability(client);
    close(server); // pending endpoint and its references must be released
    char b;
    errno = 0; ssize_t n = read(client, &b, 1);
    CHECK(n == 0 || (n == -1 && errno == ECONNRESET));
    errno = 0; CHECK(send(client, "x", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE);
    close(client); unlink(address.sun_path);
}
static void retry_accept(void) {
    int server = listener(999), client = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(client >= 0 && connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(write(client, "R", 1) == 1); close(client);
    struct rlimit old, limited;
    CHECK(getrlimit(RLIMIT_NOFILE, &old) == 0);
    limited = old; if (limited.rlim_cur > 128) limited.rlim_cur = 128;
    CHECK(setrlimit(RLIMIT_NOFILE, &limited) == 0);
    int extra[256], count = 0;
    while (count < 256) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) break;
        extra[count++] = fd;
    }
    errno = 0; CHECK(accept(server, NULL, NULL) == -1 && errno == EMFILE);
    CHECK(count > 0); if (count) close(extra[--count]);
    int accepted = accept(server, NULL, NULL); CHECK(accepted >= 0);
    char value = 0; CHECK(read(accepted, &value, 1) == 1 && value == 'R');
    close(accepted);
    while (count) close(extra[--count]);
    CHECK(setrlimit(RLIMIT_NOFILE, &old) == 0);
    close(server); unlink(address.sun_path);
}
int main(void) {
    syscall(SYS_alarm, 45);
    signal(SIGPIPE, SIG_IGN);
    for (unsigned round=0; round<32 && !failures; ++round) {
        early_close(round*2); abandoned(round*2+1);
    }
    if (!failures) retry_accept();
    printf("UNIXPENDINGCHECK %s rounds=32 failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
