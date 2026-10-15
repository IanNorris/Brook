#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Keep mappings after the allocating thread exits, then reuse that thread's
 * PID for an unrelated process. Reaping it must not reclaim the mappings. */
enum { BYTES = 128 * 1024, KINDS = 4 };
static unsigned char *pages[KINDS];
static const char *names[KINDS] = {"anonymous", "mprotect", "file fault", "COW fault"};
static int input_fd, worker_error;
static pid_t worker_tid;

static unsigned char marker(int kind, size_t pos)
{
    return (unsigned char)(0x51 + kind * 31 + pos * 17 + (pos >> 12));
}

static void *allocate_pages(void *unused)
{
    (void)unused;
    worker_tid = (pid_t)syscall(SYS_gettid);
    pages[0] = mmap(NULL, BYTES, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pages[1] = mmap(NULL, BYTES, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pages[2] = mmap(NULL, BYTES, PROT_READ, MAP_PRIVATE, input_fd, 0);
    if (pages[0] == MAP_FAILED || pages[1] == MAP_FAILED ||
        pages[2] == MAP_FAILED || mprotect(pages[1], BYTES, PROT_READ | PROT_WRITE)) {
        worker_error = 1;
        return NULL;
    }
    for (size_t i = 0; i < BYTES; ++i) {
        pages[0][i] = marker(0, i);
        pages[1][i] = marker(1, i);
        if (((volatile unsigned char *)pages[2])[i] != marker(2, i))
            worker_error = 1;
        pages[3][i] = marker(3, i); /* CPU write fault while fork child retains old page. */
    }
    return NULL;
}

static int verify(const char *phase)
{
    int failed = 0;
    for (int kind = 0; kind < KINDS; ++kind) {
        size_t bad = 0, first = 0;
        for (size_t i = 0; i < BYTES; ++i)
            if (((volatile unsigned char *)pages[kind])[i] != marker(kind, i)) {
                if (!bad) first = i;
                ++bad;
            }
        printf("THREADPAGECHECK %s %s %s changed=%zu first=%zu\n",
               bad ? "FAIL" : "PASS", phase, names[kind], bad, first);
        failed += bad != 0;
    }
    return failed;
}

static int child_ok(pid_t child)
{
    int status;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status);
}

static int run(const char *directory, const char *self, int require_reuse)
{
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/threadpagecheck-%ld", directory, (long)getpid()) >= (int)sizeof(path))
        return 2;
    input_fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (input_fd < 0) { perror("create fixture"); return 2; }
    /* Unlink immediately: every exit path releases the fixture. */
    if (unlink(path)) { perror("unlink fixture"); close(input_fd); return 2; }
    unsigned char block[4096];
    for (size_t offset = 0; offset < BYTES; offset += sizeof(block)) {
        for (size_t j = 0; j < sizeof(block); ++j) block[j] = marker(2, offset + j);
        if (write(input_fd, block, sizeof(block)) != sizeof(block)) {
            perror("write fixture"); close(input_fd); return 2;
        }
    }
    pages[3] = mmap(NULL, BYTES, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages[3] == MAP_FAILED) { perror("COW mmap"); close(input_fd); return 2; }
    memset(pages[3], 0x26, BYTES);
    int gate[2], reply[2];
    if (pipe(gate) || pipe(reply)) return 2;
    pid_t keeper = fork();
    if (!keeper) {
        close(gate[1]);
        close(reply[0]);
        pid_t tid;
        ssize_t got;
        do { got = read(gate[0], &tid, sizeof(tid)); } while (got < 0 && errno == EINTR);
        if (got != sizeof(tid)) _exit(4);
        int reused = 0;
        /* This peer predates the worker mappings. Forking from the victim
         * would share its pages and could detach their PMM ownership lists,
         * accidentally concealing the stale-thread-PID reclamation bug. */
        for (int batch = 0; batch < 3; ++batch) {
            int release[2];
            pid_t children[16];
            int count = 0, batch_failed = 0;
            if (pipe(release)) _exit(5);
            /* Reserve lower free PIDs together. Sequential fork/reap can
             * repeatedly reuse a lower PID and never reach the worker TID. */
            for (int slot = 0; slot < 16; ++slot) {
                pid_t child = fork();
                if (!child) {
                    close(release[1]);
                    close(gate[0]);
                    close(reply[1]);
                    char byte;
                    ssize_t result;
                    do { result = read(release[0], &byte, 1); }
                    while (result < 0 && errno == EINTR);
                    close(release[0]);
                    if (result != 0) _exit(112);
                    execl(self, self, "--child", (char *)NULL);
                    _exit(111);
                }
                if (child < 0) { batch_failed = 1; break; }
                children[count++] = child;
                reused |= child == tid;
            }
            close(release[0]);
            close(release[1]); /* EOF releases every reserved child. */
            for (int i = 0; i < count; ++i)
                if (!child_ok(children[i])) batch_failed = 1;
            if (batch_failed) _exit(5);
        }
        for (size_t i = 0; i < BYTES; ++i)
            if (((volatile unsigned char *)pages[3])[i] != 0x26) _exit(3);
        if (write(reply[1], &reused, sizeof(reused)) != sizeof(reused)) _exit(6);
        _exit(0);
    }
    close(gate[0]);
    close(reply[1]);
    if (keeper < 0) { close(gate[1]); return 2; }
    pthread_t thread;
    int failed = 0;
    int error = pthread_create(&thread, NULL, allocate_pages, NULL);
    if (error) { fprintf(stderr, "pthread_create: %s\n", strerror(error)); failed = 1; }
    else if (pthread_join(thread, NULL) || worker_error) failed = 1;
    else {
        failed += verify("after join");
        int reused = 0;
        ssize_t got;
        if (write(gate[1], &worker_tid, sizeof(worker_tid)) != sizeof(worker_tid)) ++failed;
        do { got = read(reply[0], &reused, sizeof(reused)); } while (got < 0 && errno == EINTR);
        if (got != sizeof(reused)) ++failed;
        printf("THREADPAGECHECK PID reuse tid=%ld observed=%d required=%d\n",
               (long)worker_tid, reused, require_reuse);
        if (require_reuse && !reused) ++failed;
        failed += verify("after fork/exec/reap");
    }
    close(gate[1]);
    close(reply[0]);
    if (!child_ok(keeper)) { puts("THREADPAGECHECK FAIL fork keeper changed"); ++failed; }
    else puts("THREADPAGECHECK PASS fork keeper preserved");
    close(input_fd);
    for (int kind = 0; kind < KINDS; ++kind)
        if (pages[kind] && pages[kind] != MAP_FAILED) munmap(pages[kind], BYTES);
    printf("THREADPAGECHECK %s failures=%d\n", failed ? "FAIL" : "PASS", failed);
    return failed != 0;
}

static uint64_t milliseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--child")) {
        unsigned char *p = mmap(NULL, BYTES * 4, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return 2;
        memset(p, 0xb7, BYTES * 4);
        munmap(p, BYTES * 4);
        return 0;
    }
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--require-reuse"))) {
        fprintf(stderr, "usage: %s ABSOLUTE_DIRECTORY [--require-reuse]\n", argv[0]);
        return 2;
    }
    if (argv[0][0] != '/' || argv[1][0] != '/') {
        fputs("Use absolute executable and directory paths\n", stderr); return 2;
    }
    pid_t child = fork();
    if (!child) { setpgid(0, 0); _exit(run(argv[1], argv[0], argc == 3)); }
    if (child < 0) return 2;
    setpgid(child, child);
    uint64_t deadline = milliseconds() + 30000;
    for (;;) {
        int status;
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (result < 0 && errno != EINTR) return 2;
        if (milliseconds() >= deadline) {
            kill(-child, SIGKILL);
            waitpid(child, &status, 0);
            fputs("THREADPAGECHECK FAIL watchdog\n", stderr);
            return 1;
        }
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
}
