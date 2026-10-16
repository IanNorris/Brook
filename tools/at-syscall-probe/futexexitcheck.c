#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { WORKERS = 4, ROUNDS = 32, PRIVATE_WAIT = 128, FOOTPRINT = 32 * 1024 * 1024 };
static atomic_int tids[WORKERS];
static int futex_word;
static uint64_t millis(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) _exit(90);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void *waiter(void *arg)
{
    unsigned slot = (unsigned)(uintptr_t)arg;
    atomic_store_explicit(&tids[slot], (int)syscall(SYS_gettid), memory_order_release);
    /* No timeout and no normal waker: exit_group must unwind this wait. The
     * futex's user-buffer validation exercises the protected mapping latch. */
    syscall(SYS_futex, &futex_word, PRIVATE_WAIT, 0, NULL, NULL, 0);
    _exit(91); /* A terminal syscall boundary must prevent userspace resumption. */
}
static int sleeping(int tid)
{
    char path[96], buf[2048];
    snprintf(path, sizeof(path), "/proc/self/task/%d/status", tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, "State:\tS") != NULL;
}
static void child(unsigned round)
{
    unsigned char *pages = mmap(NULL, FOOTPRINT, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) _exit(92);
    for (unsigned i = 0; i < FOOTPRINT; i += 4096) pages[i] = (unsigned char)(i / 4096);
    pthread_t threads[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i)
        if (pthread_create(&threads[i], NULL, waiter, (void *)(uintptr_t)i)) _exit(93);
    uint64_t deadline = millis() + 3000;
    for (;;) {
        unsigned blocked = 0;
        for (unsigned i = 0; i < WORKERS; ++i) {
            int tid = atomic_load_explicit(&tids[i], memory_order_acquire);
            if (tid && sleeping(tid)) ++blocked;
        }
        if (blocked == WORKERS) break;
        if (millis() >= deadline) _exit(94);
        sched_yield();
    }
    syscall(SYS_exit_group, round % 80);
    _exit(95);
}
static unsigned long free_kib(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256]; unsigned long result = 0;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "MemFree: %lu kB", &result) == 1) break;
    fclose(f); return result;
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned long baseline = 0, after = 0;
    for (unsigned round = 0; round < ROUNDS; ++round) {
        pid_t pid = fork();
        if (!pid) child(round);
        if (pid < 0) { perror("fork"); return 1; }
        uint64_t deadline = millis() + 5000;
        int status;
        for (;;) {
            pid_t got = waitpid(pid, &status, WNOHANG);
            if (got == pid) break;
            if (got < 0 && errno != EINTR) { perror("waitpid"); return 1; }
            if (millis() >= deadline) {
                printf("FUTEXEXITCHECK FAIL group not reaped round=%u pid=%d\n", round, pid);
                kill(pid, SIGKILL); return 1;
            }
            usleep(1000);
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != (int)(round % 80)) {
            printf("FUTEXEXITCHECK FAIL round=%u status=%d\n", round, status); return 1;
        }
        /* Let the asynchronous task reaper run; warm up caches before comparing.
         * A retained 32-MiB address space per round exceeds this tolerance. */
        usleep(50000);
        after = free_kib();
        if (round == 3) baseline = after;
        if ((round + 1) % 8 == 0)
            printf("FUTEXEXITCHECK progress=%u free_kib=%lu\n", round + 1, after);
    }
    if (!baseline || !after || (baseline > after && baseline - after > FOOTPRINT / 1024)) {
        printf("FUTEXEXITCHECK FAIL memory baseline_kib=%lu after_kib=%lu\n", baseline, after);
        return 1;
    }
    printf("FUTEXEXITCHECK PASS rounds=%u workers=%u touched_mib=%u baseline_kib=%lu after_kib=%lu\n",
           ROUNDS, WORKERS, FOOTPRINT / 1024 / 1024, baseline, after);
    return 0;
}
