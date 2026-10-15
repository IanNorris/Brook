#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { WORKERS = 8, FUTEX_WAIT_PRIVATE = 128, FUTEX_WAKE_PRIVATE = 129 };
static unsigned epoch, ready, started, stopping, waits, wakes;
static int pipefd[2];

static uint64_t millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *worker(void *arg)
{
    uintptr_t id = (uintptr_t)arg;
    __atomic_add_fetch(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&started, __ATOMIC_ACQUIRE)) sched_yield();
    if (id < 4) {
        struct timespec timeout = {0, 1000000};
        for (;;) {
            unsigned value = __atomic_load_n(&epoch, __ATOMIC_ACQUIRE);
            if (__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) break;
            __atomic_add_fetch(&waits, 1, __ATOMIC_RELAXED);
            long result = syscall(SYS_futex, &epoch, FUTEX_WAIT_PRIVATE, value,
                                  (id & 1) ? &timeout : NULL, NULL, 0);
            if (result < 0 && errno != EAGAIN && errno != EINTR &&
                errno != ETIMEDOUT) _exit(124);
        }
    } else if (id == 4) {
        while (!__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) {
            __atomic_add_fetch(&epoch, 1, __ATOMIC_RELEASE);
            if (syscall(SYS_futex, &epoch, FUTEX_WAKE_PRIVATE, INT_MAX,
                        NULL, NULL, 0) < 0) _exit(125);
            __atomic_add_fetch(&wakes, 1, __ATOMIC_RELAXED);
            sched_yield();
        }
    } else if (id == 5) {
        struct timespec timeout = {0, 1000000};
        while (!__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) nanosleep(&timeout, NULL);
    } else {
        char byte;
        while (read(pipefd[0], &byte, 1) < 0 && errno == EINTR) {}
    }
    return NULL;
}

static void child_run(unsigned round)
{
    pthread_t threads[WORKERS];
    setpgid(0, 0);
    if (pipe(pipefd)) _exit(120);
    for (uintptr_t i = 0; i < WORKERS; ++i)
        if (pthread_create(&threads[i], NULL, worker, (void *)i)) _exit(121);
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != WORKERS) sched_yield();
    __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&waits, __ATOMIC_ACQUIRE) < 16 ||
           __atomic_load_n(&wakes, __ATOMIC_ACQUIRE) < 8) sched_yield();
    /* Vary the phase relative to remote CPUs entering/blocking/waking. */
    for (unsigned i = 0; i < (round % 13); ++i) sched_yield();
    if ((round % 8) == 0) {
        /* Normal wake/removal must also release waiter-owned references. */
        __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&epoch, 1, __ATOMIC_RELEASE);
        syscall(SYS_futex, &epoch, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
        close(pipefd[1]);
        for (unsigned i = 0; i < WORKERS; ++i)
            if (pthread_join(threads[i], NULL)) _exit(122);
        close(pipefd[0]);
    }
    /* Other rounds deliberately retain live futex, pipe and nanosleep
     * workers. exit_group must quiesce/reap the entire group, including a
     * thread entering SchedulerBlock after terminal publication. */
    syscall(SYS_exit_group, round % 100);
    _exit(123);
}

int main(int argc, char **argv)
{
    unsigned rounds = 128;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 2) return 2;
    if (argc == 2) {
        char *end;
        unsigned long n = strtoul(argv[1], &end, 10);
        if (!*argv[1] || *end || n < 8 || n > 4096) return 2;
        rounds = (unsigned)n;
    }
    for (unsigned round = 0; round < rounds; ++round) {
        pid_t child = fork();
        if (!child) child_run(round);
        if (child < 0) { perror("fork"); return 1; }
        setpgid(child, child);
        uint64_t deadline = millis() + 5000;
        int status;
        for (;;) {
            pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child) break;
            if (result < 0 && errno != EINTR) { perror("waitpid"); return 1; }
            if (millis() >= deadline) {
                printf("EXITGROUPCHECK FAIL timeout round=%u pid=%ld\n", round, (long)child);
                kill(-child, SIGKILL);
                return 1; /* Never block again waiting for the broken group. */
            }
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != (int)(round % 100)) {
            printf("EXITGROUPCHECK FAIL status round=%u raw=%d\n", round, status);
            return 1;
        }
        if ((round + 1) % 32 == 0) printf("EXITGROUPCHECK progress=%u\n", round + 1);
    }
    printf("EXITGROUPCHECK PASS rounds=%u\n", rounds);
    return 0;
}
