// AF_UNIX peer death acceptance (Chromium GPU-process death hang).
//
// Run from INIT.RC as `run /boot/PEERDIE.ELF`; scripts/test_unix_peer_death_qemu.py
// builds the disposable image. Prints PEER_DEATH_OK or PEER_DEATH_FAIL <why>.
//
// Chromium's browser learns that a child died only when its Mojo channel (a
// SOCK_STREAM socketpair whose child end reached the child by fork/exec or
// SCM_RIGHTS) reports hang-up. Each case below has the parent already blocked
// in epoll_wait or poll when the peer process goes away, and requires
// EPOLLIN|EPOLLHUP|EPOLLRDHUP promptly followed by read() == 0. The child is
// never reaped before the hang-up is observed: Linux closes descriptors at
// exit, not at wait.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char* self;
static int failures;

static void fail(const char* format, ...)
{
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    printf("PEER_DEATH_FAIL %s\n", text);
    fflush(stdout);
    ++failures;
}

#define EXPECT(cond, ...) do { if (!(cond)) fail(__VA_ARGS__); } while (0)

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void sleep_ms(int ms)
{
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}

static int send_fd(int socket, int fd)
{
    char byte = 'f';
    struct iovec io = {&byte, 1};
    union { struct cmsghdr h; char raw[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    struct msghdr message = {0};
    message.msg_iov = &io; message.msg_iovlen = 1;
    message.msg_control = control.raw; message.msg_controllen = sizeof(control.raw);
    struct cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &fd, sizeof(int));
    return sendmsg(socket, &message, 0) == 1 ? 0 : -1;
}

static int recv_fd(int socket)
{
    char byte;
    struct iovec io = {&byte, 1};
    union { struct cmsghdr h; char raw[CMSG_SPACE(sizeof(int))]; } control;
    struct msghdr message = {0};
    message.msg_iov = &io; message.msg_iovlen = 1;
    message.msg_control = control.raw; message.msg_controllen = sizeof(control.raw);
    if (recvmsg(socket, &message, 0) != 1) return -1;
    struct cmsghdr* header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_type != SCM_RIGHTS) return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(header), sizeof(int));
    return fd;
}

// Block in epoll_wait (level-triggered, as Chromium's IO pump registers) until
// the socket hangs up; require the full Linux event set and EOF.
static void expect_hup_epoll(const char* name, int fd, int limit_ms)
{
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event want = {.events = EPOLLIN | EPOLLRDHUP, .data.fd = fd};
    EXPECT(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &want) == 0, "%s: epoll_ctl errno %d", name, errno);
    long start = now_ms();
    struct epoll_event got = {0};
    int n;
    do n = epoll_wait(ep, &got, 1, limit_ms + 2000); while (n < 0 && errno == EINTR);
    long took = now_ms() - start;
    close(ep);
    if (n != 1) { fail("%s: epoll_wait returned %d after %ld ms (no hang-up)", name, n, took); return; }
    uint32_t need = EPOLLIN | EPOLLHUP | EPOLLRDHUP;
    EXPECT((got.events & need) == need, "%s: events 0x%x lack IN|HUP|RDHUP", name, got.events);
    EXPECT(took <= limit_ms, "%s: hang-up took %ld ms", name, took);
    char byte;
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ssize_t r = read(fd, &byte, 1);
    EXPECT(r == 0, "%s: read after hang-up returned %zd errno %d", name, r, r < 0 ? errno : 0);
    printf("PEER_DEATH case %s events=0x%x ms=%ld\n", name, got.events, took);
    fflush(stdout);
}

static void expect_hup_poll(const char* name, int fd, int limit_ms)
{
    struct pollfd p = {fd, POLLIN | POLLRDHUP, 0};
    long start = now_ms();
    int n;
    do n = poll(&p, 1, limit_ms + 2000); while (n < 0 && errno == EINTR);
    long took = now_ms() - start;
    if (n != 1) { fail("%s: poll returned %d after %ld ms (no hang-up)", name, n, took); return; }
    short need = POLLIN | POLLHUP | POLLRDHUP;
    EXPECT((p.revents & need) == need, "%s: revents 0x%x lack IN|HUP|RDHUP", name, p.revents);
    EXPECT(took <= limit_ms, "%s: hang-up took %ld ms", name, took);
    printf("PEER_DEATH case %s revents=0x%x ms=%ld\n", name, p.revents, took);
    fflush(stdout);
}

static void reap(const char* name, pid_t pid, int expect_signal)
{
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) { fail("%s: waitpid errno %d", name, errno); return; }
    // Linux reports the group's SIGKILL even after the leader's raw exit(0);
    // Brook keeps the leader's provisional status (wait status only, tracked
    // separately), so that case passes -1 and checks hang-up alone.
    if (expect_signal < 0) return;
    if (expect_signal)
        EXPECT(WIFSIGNALED(status) && WTERMSIG(status) == expect_signal,
               "%s: status 0x%x, expected signal %d", name, status, expect_signal);
    else
        EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "%s: status 0x%x", name, status);
}

struct killer { pid_t pid; int sig; int delay_ms; };

static void* killer_main(void* arg)
{
    struct killer* k = arg;
    sleep_ms(k->delay_ms);
    kill(k->pid, k->sig);
    return NULL;
}

// Child-side modes. Each child already owns only its end of the pair.
enum mode { EXIT_NORMAL, WAIT_FOR_KILL, CRASH, THREADS_WAIT_FOR_KILL, THREADS_CRASH,
            LEADER_EXITS_THREAD_EXITS, LEADER_EXITS_THREAD_WAITS };

static int idle_pipe[2];
static pthread_mutex_t idle_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle_cond = PTHREAD_COND_INITIALIZER;
static int child_socket = -1;

static void* idle_read(void* arg) { (void)arg; char b; for (;;) if (read(idle_pipe[0], &b, 1) < 0) sleep_ms(1); return NULL; }
static void* idle_cond_wait(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&idle_lock);
    for (;;) pthread_cond_wait(&idle_cond, &idle_lock);
    return NULL;
}
static void* idle_epoll(void* arg)
{
    (void)arg;
    int ep = epoll_create1(0);
    struct epoll_event e = {.events = EPOLLIN, .data.fd = child_socket};
    epoll_ctl(ep, EPOLL_CTL_ADD, child_socket, &e);
    for (;;) epoll_wait(ep, &e, 1, -1);
    return NULL;
}
static void* idle_poll(void* arg)
{
    (void)arg;
    struct pollfd p = {child_socket, POLLIN, 0};
    for (;;) { poll(&p, 1, 50); }
    return NULL;
}
static void* crash_thread(void* arg)
{
    (void)arg;
    sleep_ms(300);
    *(volatile int*)0 = 1;
    return NULL;
}

// The leader leaves with raw SYS_exit; the group lives on in one thread.
static void* last_thread_exit(void* arg)
{
    (void)arg;
    sleep_ms(300);
    syscall(SYS_exit, 0);
    return NULL;
}
static void* last_thread_wait(void* arg) { (void)arg; for (;;) pause(); return NULL; }

static void child_body(int fd, enum mode mode)
{
    child_socket = fd;
    switch (mode) {
    case EXIT_NORMAL: sleep_ms(300); _exit(0);
    case WAIT_FOR_KILL: for (;;) pause();
    case CRASH: sleep_ms(300); *(volatile int*)0 = 1; _exit(1);
    case LEADER_EXITS_THREAD_EXITS:
    case LEADER_EXITS_THREAD_WAITS: {
        pthread_t t;
        pthread_create(&t, NULL, mode == LEADER_EXITS_THREAD_EXITS ? last_thread_exit : last_thread_wait, NULL);
        syscall(SYS_exit, 0);
        _exit(5);
    }
    case THREADS_WAIT_FOR_KILL:
    case THREADS_CRASH: {
        pthread_t t;
        pipe(idle_pipe);
        pthread_create(&t, NULL, idle_read, NULL);
        pthread_create(&t, NULL, idle_cond_wait, NULL);
        pthread_create(&t, NULL, idle_epoll, NULL);
        pthread_create(&t, NULL, idle_poll, NULL);
        if (mode == THREADS_CRASH) pthread_create(&t, NULL, crash_thread, NULL);
        char b;
        for (;;) read(fd, &b, 1); // main thread blocked on the channel itself
    }
    }
    _exit(2);
}

// Socketpair across fork: parent keeps sv[0], child keeps sv[1].
static void test_fork_case(const char* name, enum mode mode, int sig, int use_poll)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { fail("%s: socketpair", name); return; }
    pid_t pid = fork();
    if (pid == 0) { close(sv[0]); child_body(sv[1], mode); }
    close(sv[1]);
    pthread_t t;
    struct killer k = {pid, SIGKILL, 300};
    int killer = mode == WAIT_FOR_KILL || mode == THREADS_WAIT_FOR_KILL || mode == LEADER_EXITS_THREAD_WAITS;
    if (killer) pthread_create(&t, NULL, killer_main, &k);
    if (use_poll) expect_hup_poll(name, sv[0], 1500);
    else expect_hup_epoll(name, sv[0], 1500);
    if (killer) pthread_join(t, NULL);
    close(sv[0]);
    reap(name, pid, sig);
}

// Mojo-style handoff: the child's end reaches it via SCM_RIGHTS after fork,
// and the parent closes its copy before the child dies.
static void test_scm_rights_child_end(const char* name, int crash)
{
    int control[2], sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, control)) { fail("%s: control", name); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(control[0]);
        int fd = recv_fd(control[1]);
        close(control[1]);
        if (fd < 0) _exit(3);
        child_body(fd, crash ? THREADS_CRASH : EXIT_NORMAL);
    }
    close(control[1]);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { fail("%s: socketpair", name); return; }
    EXPECT(send_fd(control[0], sv[1]) == 0, "%s: send_fd errno %d", name, errno);
    close(sv[1]);
    close(control[0]);
    expect_hup_epoll(name, sv[0], 1500);
    close(sv[0]);
    reap(name, pid, crash ? SIGSEGV : 0);
}

// Rights queued in a socket that is closed unread must be released.
static void test_inflight_closed_unread(void)
{
    int carrier[2], sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, carrier);
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    EXPECT(send_fd(carrier[0], sv[1]) == 0, "inflight: send_fd errno %d", errno);
    close(sv[1]);
    close(carrier[0]);
    close(carrier[1]);
    expect_hup_epoll("inflight-closed-unread", sv[0], 500);
    close(sv[0]);
}

// The receiving end of the carrier belongs to a child that exits unread.
static void test_inflight_receiver_exits(int sig)
{
    const char* name = sig ? "inflight-receiver-killed" : "inflight-receiver-exits";
    int carrier[2], sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, carrier);
    pid_t pid = fork();
    if (pid == 0) { close(carrier[0]); child_body(carrier[1], sig ? WAIT_FOR_KILL : EXIT_NORMAL); }
    close(carrier[1]);
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    EXPECT(send_fd(carrier[0], sv[1]) == 0, "%s: send_fd errno %d", name, errno);
    close(sv[1]);
    close(carrier[0]);
    pthread_t t;
    struct killer k = {pid, SIGKILL, 300};
    if (sig) pthread_create(&t, NULL, killer_main, &k);
    expect_hup_epoll(name, sv[0], 1500);
    if (sig) pthread_join(t, NULL);
    close(sv[0]);
    reap(name, pid, sig);
}

// fork+exec: an FD_CLOEXEC end must not survive exec; a remapped end (as
// Chromium's LaunchProcess dup2s the channel to fd 3) must close at exit.
static void test_exec(int cloexec)
{
    const char* name = cloexec ? "exec-cloexec" : "exec-remapped";
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM | (cloexec ? SOCK_CLOEXEC : 0), 0, sv);
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        if (!cloexec && sv[1] != 3) { dup2(sv[1], 3); close(sv[1]); }
        char* argv[] = {(char*)self, cloexec ? "sleeper-long" : "sleeper", NULL};
        execv(self, argv);
        _exit(4);
    }
    close(sv[1]);
    // The cloexec child lives 3 s; hang-up must come from exec, not exit.
    expect_hup_epoll(name, sv[0], cloexec ? 1000 : 1500);
    close(sv[0]);
    reap(name, pid, 0);
}

// Chromium's CloseSuperfluousFds walks /proc/self/fd in the forked child and
// closes everything not remapped. A missing entry leaks a channel end.
static void test_proc_self_fd(void)
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int high[] = {37, 100, 255, 300, 1000};
    for (unsigned i = 0; i < sizeof(high) / sizeof(high[0]); ++i)
        EXPECT(dup2(sv[1], high[i]) == high[i], "procfd: dup2 %d errno %d", high[i], errno);
    int seen[1024 + 1] = {0};
    DIR* d = opendir("/proc/self/fd");
    if (!d) { fail("procfd: opendir errno %d", errno); return; }
    struct dirent* e;
    while ((e = readdir(d))) {
        int n = atoi(e->d_name);
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9' && n >= 0 && n <= 1024) seen[n] = 1;
    }
    closedir(d);
    EXPECT(seen[sv[0]] && seen[sv[1]], "procfd: socketpair fds %d/%d missing", sv[0], sv[1]);
    for (unsigned i = 0; i < sizeof(high) / sizeof(high[0]); ++i)
        EXPECT(seen[high[i]], "procfd: fd %d missing from /proc/self/fd", high[i]);
    for (unsigned i = 0; i < sizeof(high) / sizeof(high[0]); ++i) close(high[i]);
    close(sv[1]);
    expect_hup_epoll("procfd-close-dups", sv[0], 500);
    close(sv[0]);
}

int main(int argc, char** argv)
{
    self = argv[0];
    if (argc > 1 && !strcmp(argv[1], "sleeper")) { sleep_ms(300); return 0; }
    if (argc > 1 && !strcmp(argv[1], "sleeper-long")) { sleep_ms(3000); return 0; }
    signal(SIGPIPE, SIG_IGN);
    printf("PEER_DEATH start\n");
    fflush(stdout);
    test_fork_case("fork-exit", EXIT_NORMAL, 0, 0);
    test_fork_case("fork-exit-poll", EXIT_NORMAL, 0, 1);
    test_fork_case("fork-sigkill", WAIT_FOR_KILL, SIGKILL, 0);
    test_fork_case("fork-segv", CRASH, SIGSEGV, 0);
    test_fork_case("threads-sigkill", THREADS_WAIT_FOR_KILL, SIGKILL, 0);
    test_fork_case("threads-segv", THREADS_CRASH, SIGSEGV, 0);
    test_fork_case("threads-segv-poll", THREADS_CRASH, SIGSEGV, 1);
    test_fork_case("leader-exits-thread-exits", LEADER_EXITS_THREAD_EXITS, 0, 0);
    test_fork_case("leader-exits-thread-sigkill", LEADER_EXITS_THREAD_WAITS, -1, 0);
    test_scm_rights_child_end("scm-child-exit", 0);
    test_scm_rights_child_end("scm-child-threads-segv", 1);
    test_inflight_closed_unread();
    test_inflight_receiver_exits(0);
    test_inflight_receiver_exits(SIGKILL);
    test_exec(1);
    test_exec(0);
    test_proc_self_fd();
    if (failures) printf("PEER_DEATH_FAIL count=%d\n", failures);
    else printf("PEER_DEATH_OK\n");
    fflush(stdout);
    return failures ? 1 : 0;
}
