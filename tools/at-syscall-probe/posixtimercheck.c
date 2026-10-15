#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Validate the kernel ABI against the toolchain's actual Linux libc headers. */
_Static_assert(sizeof(struct sigevent) == 64, "x86-64 sigevent");
_Static_assert(offsetof(struct sigevent, sigev_value) == 0, "sigev_value");
_Static_assert(offsetof(struct sigevent, sigev_signo) == 8, "sigev_signo");
_Static_assert(offsetof(struct sigevent, sigev_notify) == 12, "sigev_notify");
_Static_assert(offsetof(struct sigevent, _sigev_un) == 16, "target tid");
_Static_assert(sizeof(struct itimerspec) == 32, "itimerspec");
_Static_assert(sizeof(siginfo_t) == 128, "siginfo");
_Static_assert(offsetof(siginfo_t, si_timerid) == 16, "si_timerid");
_Static_assert(offsetof(siginfo_t, si_overrun) == 20, "si_overrun");
_Static_assert(offsetof(siginfo_t, si_value) == 24, "si_value");
static int failures, require_reuse;
#define CHECK(c) do { if (!(c)) { printf("FAIL line=%d: %s errno=%d\n", __LINE__, #c, errno); ++failures; } } while (0)
static uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void pause_ms(unsigned ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
static int raw_create(int clock, int mode, int sig, uintptr_t value, int tid) {
    struct sigevent ev = {0};
    ev.sigev_notify = mode; ev.sigev_signo = sig; ev.sigev_value.sival_ptr = (void*)value;
    /* The first word of the union is the Linux SIGEV_THREAD_ID TID. */
    memcpy(&ev._sigev_un, &tid, sizeof(tid));
    int32_t id = -1;
    if (syscall(SYS_timer_create, clock, &ev, &id)) return -1;
    return id;
}
static int arm(int id, unsigned first_ms, unsigned interval_ms) {
    struct itimerspec ts = {{interval_ms / 1000, (long)(interval_ms % 1000) * 1000000},
                           {first_ms / 1000, (long)(first_ms % 1000) * 1000000}};
    return (int)syscall(SYS_timer_settime, id, 0, &ts, NULL);
}
static int wait_signal(int sig, siginfo_t *info, unsigned timeout_ms) {
    uint64_t mask = 1ULL << (sig - 1);
    struct timespec ts = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000};
    return (int)syscall(SYS_rt_sigtimedwait, &mask, info, &ts, sizeof(mask));
}
static void raw_tests(void) {
    int sig = SIGRTMIN + 3;
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, sig);
    CHECK(pthread_sigmask(SIG_BLOCK, &mask, NULL) == 0);
    int a = raw_create(CLOCK_MONOTONIC, SIGEV_SIGNAL, sig, 0x123456789abcdef0ULL, 0);
    int b = raw_create(CLOCK_MONOTONIC, SIGEV_SIGNAL, sig, 0x3141592653589793ULL, 0);
    CHECK(a >= 0 && b >= 0);
    if (a < 0 || b < 0) return;
    struct itimerspec current = {0}, previous = {0};
    CHECK(syscall(SYS_timer_gettime, a, &current) == 0);
    CHECK(current.it_value.tv_sec == 0 && current.it_value.tv_nsec == 0);
    uint64_t began = now_ms();
    CHECK(arm(a, 25, 0) == 0); CHECK(arm(b, 25, 0) == 0);
    unsigned seen = 0;
    for (int i = 0; i < 2; ++i) {
        siginfo_t info = {0};
        CHECK(wait_signal(sig, &info, 2000) == sig);
        CHECK(info.si_code == SI_TIMER && info.si_overrun == 0);
        if (info.si_timerid == a && (uintptr_t)info.si_value.sival_ptr == 0x123456789abcdef0ULL) seen |= 1;
        if (info.si_timerid == b && (uintptr_t)info.si_value.sival_ptr == 0x3141592653589793ULL) seen |= 2;
    }
    CHECK(seen == 3 && now_ms() - began >= 20);
    CHECK(arm(a, 10, 10) == 0); pause_ms(95);
    siginfo_t info = {0};
    CHECK(wait_signal(sig, &info, 2000) == sig);
    CHECK(info.si_timerid == a && info.si_overrun >= 3);
    CHECK(syscall(SYS_timer_getoverrun, a) == info.si_overrun);
    CHECK(syscall(SYS_timer_delete, a) == 0);
    errno = 0; CHECK(syscall(SYS_timer_gettime, a, &current) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_timer_delete, b) == 0);

    int none = raw_create(CLOCK_MONOTONIC, SIGEV_NONE, 0, 0, 0);
    CHECK(none >= 0);
    struct itimerspec value = {{0, 7000000}, {1, 0}};
    CHECK(syscall(SYS_timer_settime, none, 0, &value, &previous) == 0);
    CHECK(syscall(SYS_timer_gettime, none, &current) == 0);
    CHECK(current.it_interval.tv_nsec == 7000000 && current.it_value.tv_sec <= 1);
    value.it_value = (struct timespec){0, 0};
    CHECK(syscall(SYS_timer_settime, none, 0, &value, &previous) == 0);
    CHECK(previous.it_value.tv_sec || previous.it_value.tv_nsec);
    CHECK(syscall(SYS_timer_gettime, none, &current) == 0);
    /* Linux's SIGEV_NONE gettime path can still report the former hrtimer
     * countdown after disarm. Check the cleared interval here; verify zero
     * remaining time using a real signal timer in delivery_tests below. */
    CHECK(!current.it_interval.tv_sec && !current.it_interval.tv_nsec);
    value.it_value.tv_nsec = 1000000000;
    errno = 0; CHECK(syscall(SYS_timer_settime, none, 0, &value, NULL) == -1 && errno == EINVAL);
    value.it_value.tv_nsec = 0;
    /* Linux ignores flags other than TIMER_ABSTIME. */
    CHECK(syscall(SYS_timer_settime, none, 2, &value, NULL) == 0);
    errno = 0; CHECK(syscall(SYS_timer_gettime, none, (void*)1) == -1 && errno == EFAULT);
    CHECK(syscall(SYS_timer_delete, none) == 0);
    for (int clock = 0; clock <= 1; ++clock) {
        int id = raw_create(clock, SIGEV_SIGNAL, sig, 42, 0);
        CHECK(id >= 0);
        struct itimerspec abs = {0}; clock_gettime(clock, &abs.it_value);
        abs.it_value.tv_nsec += 30000000;
        if (abs.it_value.tv_nsec >= 1000000000) { ++abs.it_value.tv_sec; abs.it_value.tv_nsec -= 1000000000; }
        CHECK(syscall(SYS_timer_settime, id, TIMER_ABSTIME, &abs, NULL) == 0);
        CHECK(wait_signal(sig, &info, 2000) == sig && info.si_timerid == id);
        CHECK(syscall(SYS_timer_delete, id) == 0);
    }
}

static volatile sig_atomic_t handler_calls, handler_code, handler_id, handler_value;
static void signal_handler(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)context;
    handler_code = info->si_code; handler_id = info->si_timerid;
    handler_value = info->si_value.sival_int;
    ++handler_calls;
}
static void delivery_tests(void) {
    int sig = SIGRTMIN + 3;
    int id = raw_create(CLOCK_MONOTONIC, SIGEV_SIGNAL, sig, 123, 0);
    CHECK(id >= 0); CHECK(arm(id, 100, 0) == 0); CHECK(arm(id, 0, 0) == 0);
    struct itimerspec disarmed = {0};
    CHECK(syscall(SYS_timer_gettime, id, &disarmed) == 0);
    CHECK(!disarmed.it_interval.tv_sec && !disarmed.it_interval.tv_nsec &&
          !disarmed.it_value.tv_sec && !disarmed.it_value.tv_nsec);
    siginfo_t info = {0};
    errno = 0; CHECK(wait_signal(sig, &info, 150) == -1 && errno == EAGAIN);
    CHECK(arm(id, 5, 0) == 0); pause_ms(30);
    uint64_t mask = 1ULL << (sig - 1); struct timespec zero = {0};
    errno = 0;
    CHECK(syscall(SYS_rt_sigtimedwait, &mask, (void*)1, &zero, sizeof(mask)) == -1 && errno == EFAULT);
    /* The EFAULT copyout happens after dequeue on Linux. */
    errno = 0; CHECK(wait_signal(sig, &info, 30) == -1 && errno == EAGAIN);
    CHECK(arm(id, 5, 0) == 0);
    CHECK(wait_signal(sig, &info, 100) == sig && info.si_timerid == id);
    CHECK(syscall(SYS_timer_delete, id) == 0);
    struct sigaction action = {0}; action.sa_sigaction = signal_handler;
    action.sa_flags = SA_SIGINFO; sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
    id = raw_create(CLOCK_MONOTONIC, SIGEV_SIGNAL, SIGUSR1, 567, 0);
    CHECK(id >= 0); CHECK(arm(id, 25, 0) == 0);
    errno = 0; CHECK(wait_signal(sig, &info, 2000) == -1 && errno == EINTR);
    CHECK(handler_calls == 1 && handler_code == SI_TIMER && handler_id == id && handler_value == 567);
    CHECK(syscall(SYS_timer_delete, id) == 0);
    CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    int32_t default_id = -1;
    CHECK(syscall(SYS_timer_create, CLOCK_MONOTONIC, NULL, &default_id) == 0);
    CHECK(arm(default_id, 25, 0) == 0);
    uint64_t deadline = now_ms() + 2000;
    while (handler_calls < 2 && now_ms() < deadline) pause_ms(1);
    CHECK(handler_calls == 2 && handler_code == SI_TIMER && handler_id == default_id && handler_value == default_id);
    CHECK(syscall(SYS_timer_delete, default_id) == 0);
}

static void ignored_test(void) {
    struct sigaction action = {0}; action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR2, &action, NULL) == 0);
    int id = raw_create(CLOCK_MONOTONIC, SIGEV_SIGNAL, SIGUSR2, 0, 0);
    CHECK(id >= 0); CHECK(arm(id, 5, 5) == 0);
    struct timespec sleep = {0, 60000000};
    /* Ignored timer notifications must not cause spurious EINTR. */
    CHECK(nanosleep(&sleep, NULL) == 0);
    CHECK(syscall(SYS_timer_delete, id) == 0);
    action.sa_handler = SIG_DFL;
    CHECK(sigaction(SIGUSR2, &action, NULL) == 0);
}

static unsigned callbacks;
static int callback_bad;
static void callback(union sigval value) {
    if (value.sival_ptr != &callbacks) __atomic_store_n(&callback_bad, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&callbacks, 1, __ATOMIC_RELEASE);
}
static void glibc_callback_test(void) {
    struct sigevent event = {0};
    event.sigev_notify = SIGEV_THREAD; event.sigev_notify_function = callback;
    event.sigev_value.sival_ptr = &callbacks;
    timer_t timer;
    int result = timer_create(CLOCK_MONOTONIC, &event, &timer);
    CHECK(result == 0); if (result) return;
    for (unsigned round = 1; round <= 16; ++round) {
        struct itimerspec ts = {{0,0}, {0,20000000}};
        CHECK(timer_settime(timer, 0, &ts, NULL) == 0);
        uint64_t deadline = now_ms() + 2000;
        while (__atomic_load_n(&callbacks, __ATOMIC_ACQUIRE) < round && now_ms() < deadline) pause_ms(1);
        CHECK(__atomic_load_n(&callbacks, __ATOMIC_ACQUIRE) == round);
        if (__atomic_load_n(&callbacks, __ATOMIC_ACQUIRE) != round) break;
    }
    CHECK(!__atomic_load_n(&callback_bad, __ATOMIC_ACQUIRE));
    CHECK(timer_delete(timer) == 0);
    printf("POSIXTIMERCHECK glibc_callbacks=%u\n", callbacks);
}

struct target_state { int tid, release, wait, received, code; uintptr_t value; };
static void *target_worker(void *arg) {
    struct target_state *state = arg;
    __atomic_store_n(&state->tid, (int)syscall(SYS_gettid), __ATOMIC_RELEASE);
    while (!__atomic_load_n(&state->release, __ATOMIC_ACQUIRE)) pause_ms(1);
    if (state->wait) {
        siginfo_t info = {0};
        state->received = wait_signal(SIGRTMIN + 3, &info, 100);
        state->code = info.si_code; state->value = (uintptr_t)info.si_value.sival_ptr;
    }
    return NULL;
}
static void target_tests(void) {
    pthread_t thread; struct target_state state = {0}; state.wait = 1;
    CHECK(pthread_create(&thread, NULL, target_worker, &state) == 0);
    while (!__atomic_load_n(&state.tid, __ATOMIC_ACQUIRE)) pause_ms(1);
    int id = raw_create(CLOCK_MONOTONIC, 4, SIGRTMIN + 3, 0xfeed, state.tid);
    CHECK(id >= 0); CHECK(arm(id, 25, 0) == 0);
    __atomic_store_n(&state.release, 1, __ATOMIC_RELEASE);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(state.received == SIGRTMIN + 3 && state.code == SI_TIMER && state.value == 0xfeed);
    int oldtid = state.tid, observed = 0;
    for (int round = 0; round < 16 && !observed; ++round) {
        struct target_state next = {0}; next.wait = 1;
        CHECK(pthread_create(&thread, NULL, target_worker, &next) == 0);
        while (!__atomic_load_n(&next.tid, __ATOMIC_ACQUIRE)) pause_ms(1);
        observed = next.tid == oldtid;
        CHECK(arm(id, 5, 0) == 0); // Must never target the replacement incarnation.
        __atomic_store_n(&next.release, 1, __ATOMIC_RELEASE);
        CHECK(pthread_join(thread, NULL) == 0);
        CHECK(next.received == -1);
    }
    CHECK(!require_reuse || observed);
    CHECK(syscall(SYS_timer_delete, id) == 0);
    printf("POSIXTIMERCHECK target_tid_reuse=%d required=%d\n", observed, require_reuse);
}

struct leader_state { int tid, release, timer, output; };
static void *leader_survivor(void *argument) {
    struct leader_state *s = argument;
    __atomic_store_n(&s->tid, (int)syscall(SYS_gettid), __ATOMIC_RELEASE);
    while (!__atomic_load_n(&s->release, __ATOMIC_ACQUIRE)) pause_ms(1);
    pause_ms(100); // Allow the leader's raw SYS_exit to retire.
    struct itimerspec ts;
    int result = 1;
    if (syscall(SYS_timer_gettime, s->timer, &ts) == 0 && arm(s->timer, 20, 0) == 0) {
        siginfo_t info = {0};
        result = !(wait_signal(SIGRTMIN + 3, &info, 1000) == SIGRTMIN + 3 &&
            info.si_code == SI_TIMER && info.si_timerid == s->timer &&
            (uintptr_t)info.si_value.sival_ptr == 0xcafe);
    }
    if (syscall(SYS_timer_delete, s->timer)) result = 2;
    /* A surviving sibling can also create and delete a new owned timer. */
    int fresh = raw_create(CLOCK_MONOTONIC, SIGEV_NONE, 0, 0, 0);
    if (fresh < 0 || syscall(SYS_timer_delete, fresh)) result = 3;
    if (write(s->output, &result, sizeof(result)) != (ssize_t)sizeof(result)) _exit(4);
    return NULL;
}
static void leader_exit_test(void) {
    int fds[2]; CHECK(pipe(fds) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        close(fds[0]);
        /* Heap storage outlives the leader's userspace stack. */
        struct leader_state *state = calloc(1, sizeof(*state));
        if (!state) _exit(5);
        state->output = fds[1];
        pthread_t worker;
        if (pthread_create(&worker, NULL, leader_survivor, state)) _exit(6);
        while (!__atomic_load_n(&state->tid, __ATOMIC_ACQUIRE)) pause_ms(1);
        state->timer = raw_create(CLOCK_MONOTONIC, 4, SIGRTMIN + 3, 0xcafe, state->tid);
        if (state->timer < 0) _exit(7);
        __atomic_store_n(&state->release, 1, __ATOMIC_RELEASE);
        syscall(SYS_exit, 0); _exit(8);
    }
    close(fds[1]);
    int result = -1, status = -1;
    CHECK(read(fds[0], &result, sizeof(result)) == (ssize_t)sizeof(result) && result == 0);
    close(fds[0]);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void lifecycle_tests(const char *exe) {
    int parent = raw_create(CLOCK_MONOTONIC, SIGEV_NONE, 0, 0, 0);
    CHECK(parent >= 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        struct itimerspec ts;
        if (syscall(SYS_timer_gettime, parent, &ts) != -1 || errno != EINVAL) _exit(1);
        int own = raw_create(CLOCK_MONOTONIC, SIGEV_NONE, 0, 0, 0);
        if (own < 0) _exit(2);
        char id[24]; snprintf(id, sizeof(id), "%d", own);
        execl(exe, exe, "--after-exec", id, (char*)NULL); _exit(3);
    }
    int status = -1; CHECK(waitpid(child, &status, 0) == child && status == 0);
    CHECK(syscall(SYS_timer_delete, parent) == 0);
    /* Child exit without delete must reclaim a whole table, repeatedly. */
    for (int round = 0; round < 4; ++round) {
        child = fork(); CHECK(child >= 0);
        if (!child) {
            int count = 0;
            while (count < 256 && raw_create(CLOCK_MONOTONIC, SIGEV_NONE, 0, 0, 0) >= 0) ++count;
            _exit(count < 64 ? 4 : 0);
        }
        CHECK(waitpid(child, &status, 0) == child && status == 0);
    }
    int ids[256], count = 0;
    while (count < 256) {
        int id = raw_create(CLOCK_MONOTONIC, SIGEV_NONE, 0, 0, 0);
        if (id < 0) { CHECK(errno == EAGAIN); break; }
        ids[count++] = id;
    }
    CHECK(count >= 64);
    for (int i = 0; i < count; ++i) CHECK(syscall(SYS_timer_delete, ids[i]) == 0);
    int recovered = raw_create(CLOCK_MONOTONIC, SIGEV_NONE, 0, 0, 0);
    CHECK(recovered >= 0); CHECK(syscall(SYS_timer_delete, recovered) == 0);
    printf("POSIXTIMERCHECK capacity=%d\n", count);
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "--after-exec")) {
        struct itimerspec ts;
        return !(syscall(SYS_timer_gettime, atoi(argv[2]), &ts) == -1 && errno == EINVAL);
    }
    require_reuse = argc == 2 && !strcmp(argv[1], "--require-reuse");
    if (argc > 2 || (argc == 2 && !require_reuse)) return 2;
    pid_t child = fork(); if (child < 0) return 2;
    if (!child) {
        setpgid(0, 0);
        raw_tests(); delivery_tests(); ignored_test(); target_tests();
        leader_exit_test(); lifecycle_tests(argv[0]); glibc_callback_test();
        printf("POSIXTIMERCHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
        _exit(failures != 0);
    }
    setpgid(child, child);
    uint64_t deadline = now_ms() + 30000;
    for (;;) {
        int status; pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (result < 0 && errno != EINTR) return 1;
        if (now_ms() >= deadline) {
            printf("POSIXTIMERCHECK FAIL watchdog\n"); kill(-child, SIGKILL); return 1;
        }
        pause_ms(10);
    }
}
