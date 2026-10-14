#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t caught, handler_error;
static int notify_fd = -1, wake_fd = -1;
static void fatal(const char *s) { perror(s); exit(1); }
static uint64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) fatal("clock_gettime");
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void child_signal(int signo)
{
    int saved = errno;
    uint64_t one = 1;
    (void)signo;
    ++caught;
    if (write(notify_fd, &one, sizeof(one)) != sizeof(one)) handler_error = 1;
    /* Also complete a restarted read, or a read begun after early delivery. */
    if (wake_fd >= 0 && write(wake_fd, "S", 1) != 1) handler_error = 1;
    errno = saved;
}
static int reap(pid_t child, int code)
{
    int status;
    pid_t got;
    do { got = waitpid(child, &status, 0); } while (got < 0 && errno == EINTR);
    return got == child && WIFEXITED(status) && WEXITSTATUS(status) == code;
}
static pid_t spawn_exec(const char *self)
{
    pid_t child = fork();
    if (child < 0) fatal("child fork");
    if (!child) {
        execl(self, self, "--child", (char *)NULL);
        _exit(99);
    }
    return child;
}
static int run_case(const char *self, int which)
{
    int pair[2] = {-1, -1};
    int use_read = which != 0;
    int restart = which == 3;
    int ignored = which == 4;
    if (use_read) {
        if (which == 2) {
            if (socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0, pair)) fatal("socketpair");
        } else if (pipe2(pair, O_CLOEXEC)) fatal("pipe2");
        wake_fd = pair[1];
    }
    notify_fd = eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (notify_fd < 0 || ep < 0) fatal("eventfd/epoll");
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = 0x1234};
    if (epoll_ctl(ep, EPOLL_CTL_ADD, notify_fd, &event)) fatal("epoll_ctl");
    struct sigaction action = {0};
    action.sa_handler = ignored ? SIG_DFL : child_signal;
    action.sa_flags = restart ? SA_RESTART : 0;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGCHLD, &action, NULL)) fatal("sigaction");
    pid_t child = spawn_exec(self);
    int ok = 1;
    pid_t writer = -1;
    if (ignored) {
        writer = fork();
        if (writer < 0) fatal("writer fork");
        if (!writer) {
            usleep(400000);
            _exit(write(pair[1], "D", 1) == 1 ? 0 : 98);
        }
    }
    if (use_read) {
        char byte = 0;
        errno = 0;
        ssize_t got = read(pair[0], &byte, 1);
        int error = errno;
        if (ignored) ok = got == 1 && byte == 'D' && caught == 0;
        else if (restart) ok = got == 1 && byte == 'S' && caught == 1;
        else ok = ((got == -1 && error == EINTR) || (got == 1 && byte == 'S')) && caught == 1;
    }
    if (!ignored) {
        int ready;
        do { ready = epoll_pwait(ep, &event, 1, -1, NULL); }
        while (ready < 0 && errno == EINTR);
        uint64_t count = 0;
        ok &= ready == 1 && event.data.u64 == 0x1234 && (event.events & EPOLLIN) &&
              read(notify_fd, &count, sizeof(count)) == sizeof(count) && count == 1 &&
              caught == 1 && !handler_error;
    }
    ok &= reap(child, 23);
    if (writer > 0) ok &= reap(writer, 0);
    close(ep); close(notify_fd);
    if (use_read) { close(pair[0]); close(pair[1]); }
    return !ok;
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--child")) {
        usleep(100000);
        _exit(23);
    }
    if (argc != 1 || argv[0][0] != '/') {
        fprintf(stderr, "invoke this probe by its absolute path\n");
        return 2;
    }
    const char *labels[] = {
        "exec child SIGCHLD handler wakes epoll eventfd",
        "caught SIGCHLD interrupts pipe read then wakes epoll",
        "caught SIGCHLD interrupts AF_UNIX read then wakes epoll",
        "SA_RESTART pipe read resumes after SIGCHLD handler",
        "default-ignored SIGCHLD does not interrupt pipe read"
    };
    int failures = 0;
    for (int which = 0; which < 5; ++which) {
        fflush(NULL);
        pid_t worker = fork();
        if (worker < 0) fatal("watchdog fork");
        if (!worker) _exit(run_case(argv[0], which));
        uint64_t deadline = now_ms() + 15000;
        int status = 0, timed_out = 0;
        for (;;) {
            pid_t got = waitpid(worker, &status, WNOHANG);
            if (got == worker) break;
            if (got < 0 && errno != EINTR) fatal("watchdog waitpid");
            if (now_ms() >= deadline) {
                timed_out = 1;
                if (kill(worker, SIGKILL) && errno != ESRCH) fatal("watchdog kill");
                while (waitpid(worker, &status, 0) < 0)
                    if (errno != EINTR) fatal("watchdog reap");
                break;
            }
            usleep(10000);
        }
        int ok = !timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        printf("CHILDSIGNALCHECK %s %s%s\n", ok ? "PASS" : "FAIL", labels[which],
               timed_out ? " (15-second watchdog)" : "");
        failures += !ok;
    }
    printf("CHILDSIGNALCHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
