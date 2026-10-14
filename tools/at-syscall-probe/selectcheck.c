#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static void fatal(const char *s) { perror(s); exit(1); }
static void check(int ok, const char *s)
{
    printf("SELECTCHECK %s %s\n", ok ? "PASS" : "FAIL", s);
    failures += !ok;
}
static uint64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) fatal("clock_gettime");
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
struct writer { int fd, event; };
static void *delayed_write(void *arg)
{
    struct writer *w = arg;
    usleep(50000);
    uint64_t one = 1;
    if (write(w->fd, &one, w->event ? 8 : 1) != (w->event ? 8 : 1)) fatal("worker write");
    return NULL;
}
static int wait_read(int fd, const struct timespec *timeout)
{
    fd_set r;
    FD_ZERO(&r); FD_SET(fd, &r);
    int result = pselect(fd+1, &r, NULL, NULL, timeout, NULL);
    return result == 1 && FD_ISSET(fd, &r);
}
struct connector { struct sockaddr_in address; int fd; };
static void *delayed_connect(void *arg)
{
    struct connector *c = arg;
    usleep(50000);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0 || connect(c->fd, (struct sockaddr *)&c->address, sizeof(c->address))) fatal("worker connect");
    usleep(50000);
    if (write(c->fd, "T", 1) != 1) fatal("TCP write");
    return NULL;
}
static int run_checks(const char *local_address)
{
    int e = eventfd(0, EFD_NONBLOCK);
    if (e < 0) fatal("eventfd");
    struct timespec zero = {0, 0};
    check(!wait_read(e, &zero), "empty eventfd is not immediately readable");
    struct writer writer = {e, 1};
    pthread_t worker;
    if (pthread_create(&worker, NULL, delayed_write, &writer)) fatal("pthread_create");
    check(wait_read(e, NULL), "infinite pselect wakes for delayed worker eventfd write");
    pthread_join(worker, NULL);
    fd_set r, w, x;
    FD_ZERO(&r); FD_ZERO(&w); FD_ZERO(&x);
    FD_SET(e, &r); FD_SET(e, &w); FD_SET(e, &x);
    check(pselect(e+1, &r, &w, &x, &zero, NULL) == 2 &&
          FD_ISSET(e, &r) && FD_ISSET(e, &w) && !FD_ISSET(e, &x),
          "return count counts read/write bits and excludes normal exception readiness");
    int high = fcntl(e, F_DUPFD, 200);
    if (high < 0) fatal("dup high fd");
    check(wait_read(high, &zero), "fd above old 128 limit is supported");
    close(high);

    long page = sysconf(_SC_PAGESIZE);
    char *memory = mmap(NULL, page*2, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    // Brook does not yet enforce PROT_NONE in all user-buffer validation.
    // Remove the mapping so this tests an actually missing page.
    if (memory == MAP_FAILED || munmap(memory+page, page)) fatal("guard pages");
    uint64_t *lastword = (uint64_t *)(memory+page-sizeof(uint64_t));
    if (e >= 64) fatal("unexpected initial descriptor");
    *lastword = 1ULL << e;
    struct timeval tv = {0, 0};
    check(syscall(SYS_select, e+1, lastword, NULL, NULL, &tv) == 1 &&
          *lastword == (1ULL << e), "select writes only one fdset word before unmapped page");
    errno = 0;
    check(syscall(SYS_select, 65, lastword, NULL, NULL, &tv) == -1 && errno == EFAULT,
          "cross-page fdset rejected with EFAULT");
    struct pollfd *lastpoll = (struct pollfd *)(memory+page-sizeof(struct pollfd));
    *lastpoll = (struct pollfd){e, POLLIN, 0};
    check(poll(lastpoll, 1, 0) == 1 && (lastpoll->revents & POLLIN), "poll kernel-copy wrapper preserves readiness");
    errno = 0;
    check(syscall(SYS_poll, lastpoll, 2, 0) == -1 && errno == EFAULT, "cross-page poll array rejected");
    uint64_t value;
    if (read(e, &value, 8) != 8) fatal("eventfd drain");
    close(e);
    FD_ZERO(&x); FD_SET(e, &x);
    errno = 0;
    check(syscall(SYS_select, e+1, NULL, NULL, &x, &tv) == -1 && errno == EBADF,
          "invalid descriptor in exception set returns EBADF");
    errno = 0;
    check(syscall(SYS_select, -1, NULL, NULL, NULL, &tv) == -1 && errno == EINVAL,
          "negative descriptor count rejected");
    errno = 0;
    check(syscall(SYS_poll, NULL, UINT64_MAX, 0) == -1 && errno == EINVAL,
          "oversized poll count rejected before pointer access");
    errno = 0;
    check(syscall(SYS_pselect6, 0, NULL, NULL, NULL, memory+page-8, NULL) == -1 && errno == EFAULT,
          "cross-page timespec rejected");
    struct timespec bad = {0, 1000000000};
    errno = 0;
    check(pselect(0, NULL, NULL, NULL, &bad, NULL) == -1 && errno == EINVAL,
          "invalid nanoseconds rejected");
    tv = (struct timeval){0, -1};
    errno = 0;
    check(syscall(SYS_select, 0, NULL, NULL, NULL, &tv) == -1 && errno == EINVAL,
          "negative microseconds rejected");
    check(pselect(0, NULL, NULL, NULL, &zero, NULL) == 0, "zero timeout returns immediately");
    tv = (struct timeval){0, 20000};
    uint64_t start = now_ms();
    check(select(0, NULL, NULL, NULL, &tv) == 0 && now_ms()-start >= 15,
          "select empty sets sleeps for timeval microseconds");
    struct timespec short_wait = {0, 200000};
    start = now_ms();
    check(pselect(0, NULL, NULL, NULL, &short_wait, NULL) == 0 && now_ms()-start < 100,
          "pselect small nanoseconds are not interpreted as microseconds");
    start = now_ms();
    check(poll(NULL, 0, 20) == 0 && now_ms()-start >= 15, "poll empty set honors timeout");
    munmap(memory, page);

    int pipes[2];
    if (pipe(pipes)) fatal("pipe");
    check(!wait_read(pipes[0], &zero), "empty pipe not readable");
    writer = (struct writer){pipes[1], 0};
    if (pthread_create(&worker, NULL, delayed_write, &writer)) fatal("pipe worker");
    check(wait_read(pipes[0], NULL), "delayed pipe write wakes pselect");
    pthread_join(worker, NULL);
    char byte;
    if (read(pipes[0], &byte, 1) != 1) fatal("pipe read");
    close(pipes[1]);
    check(wait_read(pipes[0], &zero), "pipe EOF is select-readable");
    close(pipes[0]);

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct connector connector = {.address = {.sin_family=AF_INET, .sin_addr.s_addr=htonl(INADDR_LOOPBACK)}};
    if (inet_pton(AF_INET, local_address, &connector.address.sin_addr) != 1) {
        fprintf(stderr, "invalid local TCP address: %s\n", local_address);
        return 2;
    }
    socklen_t length = sizeof(connector.address);
    if (listener < 0 || bind(listener, (struct sockaddr *)&connector.address, length) ||
        listen(listener, 1) || getsockname(listener, (struct sockaddr *)&connector.address, &length)) fatal("TCP listener");
    check(!wait_read(listener, &zero), "TCP listener is not readable without connection");
    if (pthread_create(&worker, NULL, delayed_connect, &connector)) fatal("TCP worker");
    check(wait_read(listener, NULL), "TCP accept readiness wakes pselect");
    int accepted = accept(listener, NULL, NULL);
    if (accepted < 0) fatal("accept");
    check(wait_read(accepted, NULL), "TCP data readiness wakes pselect");
    if (read(accepted, &byte, 1) != 1) fatal("TCP read");
    pthread_join(worker, NULL);
    close(connector.fd); close(accepted); close(listener);
    printf("SELECTCHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}

int main(int argc, char **argv)
{
    if (argc > 2) return 2;
    const char *local_address = argc == 2 ? argv[1] : "127.0.0.1";
    // Brook's alarm syscall is not implemented. Keep the deadline in a
    // separate process so a stuck infinite readiness wait cannot disable it.
    pid_t child = fork();
    if (child < 0) fatal("watchdog fork");
    if (!child) exit(run_checks(local_address));
    uint64_t deadline = now_ms() + 15000;
    for (;;) {
        int status;
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            fprintf(stderr, "SELECTCHECK FAIL child terminated by signal %d\n",
                    WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            return 1;
        }
        if (result < 0 && errno != EINTR) fatal("watchdog waitpid");
        if (now_ms() >= deadline) {
            fprintf(stderr, "SELECTCHECK FAIL watchdog: readiness probe exceeded 15 seconds\n");
            if (kill(child, SIGKILL) < 0 && errno != ESRCH) fatal("watchdog kill");
            while (waitpid(child, &status, 0) < 0) {
                if (errno != EINTR) fatal("watchdog reap");
            }
            return 1;
        }
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
}
