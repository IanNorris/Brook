#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static uint64_t bit(int sig) { return UINT64_C(1) << (sig - 1); }
static volatile sig_atomic_t caught, handler_error;
static uint64_t original_mask;
static void fail(const char *what) { perror(what); _exit(99); }
static uint64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) fail("clock_gettime");
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static uint64_t current_mask(void) {
    uint64_t mask = 0;
    if (syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &mask, 8)) fail("get mask");
    return mask;
}
static void child_signal(int signal, siginfo_t *info, void *context) {
    int saved = errno;
    (void)signal; (void)info;
    uint64_t restored;
    memcpy(&restored, &((ucontext_t *)context)->uc_sigmask, sizeof(restored));
    if (restored != original_mask ||
        current_mask() != (bit(SIGUSR2) | bit(SIGTERM) | bit(SIGCHLD)))
        handler_error = 1;
    ++caught;
    errno = saved;
}

/* Raw x86-64 interfaces, including the pselect6 pointer/size wrapper. */
static long wait_call(int api, int fd, int ep, int timeout_ms,
                      const void *mask, size_t size, int bad_argument) {
    struct timespec timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
    struct timespec *tp = timeout_ms < 0 ? NULL : &timeout;
    if (api == 3) return syscall(SYS_rt_sigsuspend, mask, size);
    if (api == 0) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        return syscall(SYS_ppoll, &p, bad_argument ? UINT64_MAX : 1, tp, mask, size);
    }
    if (api == 1) {
        fd_set set;
        FD_ZERO(&set); FD_SET(fd, &set);
        struct { const void *address; size_t size; } argument = {mask, size};
        return syscall(SYS_pselect6, bad_argument ? UINT64_MAX : (uint64_t)fd + 1,
                       &set, NULL, NULL, tp, &argument);
    }
    struct epoll_event event;
    return syscall(SYS_epoll_pwait, bad_argument ? -1 : ep, &event, 1,
                   timeout_ms, mask, size);
}

static int run_case(const char *self, int api, int which) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) fail("pipe2");
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) fail("epoll_create1");
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = 0x1234};
    if (epoll_ctl(ep, EPOLL_CTL_ADD, pipefd[0], &event)) fail("epoll_ctl");
    struct sigaction action = {0};
    action.sa_sigaction = child_signal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask); sigaddset(&action.sa_mask, SIGTERM);
    if (sigaction(SIGCHLD, &action, NULL)) fail("sigaction");
    original_mask = bit(SIGCHLD) | bit(SIGUSR1);
    if (syscall(SYS_rt_sigprocmask, SIG_SETMASK, &original_mask, NULL, 8)) fail("set mask");
    uint64_t temporary = bit(SIGUSR2) | bit(SIGKILL) | bit(SIGSTOP);
    const void *mask = &temporary;
    size_t size = 8;
    int timeout = 0, expected = 0, expected_errno = 0, bad_argument = 0;
    pid_t child = -1;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    char *guard = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (guard == MAP_FAILED || munmap(guard + page_size, page_size)) fail("unmapped guard");
    if (which == 0) {
        /* Deterministically pending before entering the atomic-mask wait. */
        if (kill(getpid(), SIGCHLD)) fail("queue SIGCHLD");
        timeout = -1; expected = -1; expected_errno = EINTR;
    } else if (which == 1) {
        child = fork();
        if (child < 0) fail("fork console child");
        if (!child) { execl(self, self, "--child", (char *)NULL); _exit(98); }
        /* Parent retains the writer: child exit cannot produce pipe EOF.
           Only SIGCHLD can end this infinite wait, as with Ninja's console job. */
        timeout = -1; expected = -1; expected_errno = EINTR;
    } else if (which == 2) timeout = 30;
    else if (which == 3) { size = 7; expected = -1; expected_errno = EINVAL; }
    else if (which == 4) { mask = guard + page_size - 4; expected = -1; expected_errno = EFAULT; }
    else if (which == 5) { bad_argument = 1; expected = -1; expected_errno = api == 2 ? EBADF : EINVAL; }
    else if (which == 6) { if (write(pipefd[1], "R", 1) != 1) fail("ready pipe"); expected = 1; }
    else if (which == 7) {
        mask = NULL;
        if (api == 3) { expected = -1; expected_errno = EFAULT; }
        else size = 123; /* Poll waits ignore size with a NULL mask. */
    }
    else if (which == 8) { expected = -1; expected_errno = EFAULT; }
    errno = 0;
    uint64_t start = now_ms();
    long result;
    if (which == 8 && api == 0) {
        struct timespec zero = {0};
        result = syscall(SYS_ppoll, guard + page_size, 1, &zero, mask, size);
    } else if (which == 8 && api == 1) {
        struct timespec zero = {0};
        struct { const void *address; size_t size; } argument = {mask, size};
        result = syscall(SYS_pselect6, 1, guard + page_size, NULL, NULL, &zero, &argument);
    } else if (which == 8 && api == 2) {
        expected_errno = EINVAL;
        result = syscall(SYS_epoll_pwait, ep, &event, 0, 0, mask, size);
    } else result = wait_call(api, pipefd[0], ep, timeout, mask, size, bad_argument);
    int error = errno;
    uint64_t elapsed_ms = now_ms() - start;
    int ok = result == expected && (result >= 0 || error == expected_errno) &&
             current_mask() == original_mask && !handler_error &&
             (which > 1 ? caught == 0 : caught >= 1);
    if (which == 2) ok &= elapsed_ms >= 30;
    if (child > 0) {
        int status = 0;
        pid_t got;
        do { got = waitpid(child, &status, 0); } while (got < 0 && errno == EINTR);
        ok &= got == child && WIFEXITED(status) && WEXITSTATUS(status) == 23;
    }
    if (!ok) fprintf(stderr, "api=%d case=%d result=%ld errno=%d caught=%d handler_error=%d mask=%llx elapsed_ms=%llu\n",
                     api, which, result, error, caught, handler_error,
                     (unsigned long long)current_mask(), (unsigned long long)elapsed_ms);
    close(ep); close(pipefd[0]); close(pipefd[1]); munmap(guard, page_size);
    return !ok;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--child")) { usleep(50000); _exit(23); }
    int delivery_only = argc == 2 && !strcmp(argv[1], "--sigsuspend-delivery");
    if ((argc != 1 && !delivery_only) || argv[0][0] != '/') { fprintf(stderr, "invoke by absolute path\n"); return 2; }
    const char *apis[] = {"ppoll", "pselect6", "epoll_pwait", "sigsuspend"};
    const char *cases[] = {"queued SIGCHLD + SA_RESTART", "console exec child SIGCHLD",
        "timeout restores mask", "invalid sigset size", "mask crosses unmapped page",
        "invalid wait argument restores mask", "ready fd restores mask",
        "NULL mask handling", "post-install error restores mask"};
    int failures = 0;
    for (int api = 0; api < 4; ++api) for (int which = 0; which < 9; ++which) {
        if (delivery_only && (api != 3 || which > 1)) continue;
        if (api == 3 && (which == 2 || which == 5 || which == 6 || which == 8)) continue;
        fflush(NULL);
        pid_t worker = fork();
        if (worker < 0) fail("watchdog fork");
        if (!worker) _exit(run_case(argv[0], api, which));
        uint64_t deadline = now_ms() + 15000;
        int status = 0, timed_out = 0;
        for (;;) {
            pid_t got = waitpid(worker, &status, WNOHANG);
            if (got == worker) break;
            if (got < 0 && errno != EINTR) fail("watchdog waitpid");
            if (now_ms() >= deadline) {
                timed_out = 1;
                if (kill(worker, SIGKILL) && errno != ESRCH) fail("watchdog kill");
                while (waitpid(worker, &status, 0) < 0) if (errno != EINTR) fail("watchdog reap");
                break;
            }
            usleep(10000);
        }
        int ok = !timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        printf("WAITMASKCHECK %s %s: %s%s\n", ok ? "PASS" : "FAIL", apis[api], cases[which],
               timed_out ? " (15-second watchdog)" : "");
        failures += !ok;
    }
    printf("WAITMASKCHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
