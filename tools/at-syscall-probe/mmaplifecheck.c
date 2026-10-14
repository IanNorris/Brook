#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { FILE_BYTES = 16 * 1024 * 1024, ROUNDS = 8 };
static int failures;
static void fatal(const char *label) { perror(label); exit(2); }
static void check(int ok, const char *label)
{
    printf("MMAPLIFECHECK %s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
static double now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) fatal("clock_gettime");
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}
static int wait_child(pid_t child)
{
    double deadline = now() + 30;
    int status;
    for (;;) {
        pid_t got = waitpid(child, &status, WNOHANG);
        if (got == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (got < 0 && errno != EINTR) return 0;
        if (now() > deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            return 0;
        }
        usleep(10000);
    }
}
static unsigned char marker(size_t page) { return (unsigned char)(page % 251 + 1); }
static int verify(volatile const unsigned char *map)
{
    for (size_t p = 0; p < FILE_BYTES / 4096; ++p)
        if (map[p * 4096] != marker(p) || map[p * 4096 + 4095] != (marker(p) ^ 0x5a)) return 0;
    return 1;
}
static void *map_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return MAP_FAILED;
    void *map = mmap(NULL, FILE_BYTES, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return map;
}
static int run_child(const char *self, const char *path, int exec_after)
{
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) fatal("fork");
    if (!child) {
        void *map = map_file(path);
        if (map == MAP_FAILED || !verify(map)) _exit(90);
        /* Deliberately do not munmap: exec/exit must release every PTE ref. */
        if (exec_after) {
            execl(self, self, "--empty", (char *)NULL);
            _exit(91);
        }
        _exit(0);
    }
    return wait_child(child);
}
static uint64_t settled_free(void)
{
    /* Allow the asynchronous reaper and frame quarantine to drain. Take the
       highest sample to avoid counting transient frees as persistent leaks. */
    uint64_t best = 0;
    for (int i = 0; i < 20; ++i) {
        usleep(50000);
        struct sysinfo info;
        if (sysinfo(&info)) fatal("sysinfo");
        uint64_t bytes = (uint64_t)info.freeram * info.mem_unit;
        if (bytes > best) best = bytes;
    }
    return best;
}
static void churn(const char *self, const char *path, int exec_after, int accounting)
{
    uint64_t before = settled_free();
    int ok = 1;
    for (int i = 0; i < ROUNDS; ++i) ok &= run_child(self, path, exec_after);
    uint64_t after = settled_free();
    uint64_t lost = before > after ? before - after : 0;
    check(ok, exec_after ? "all exec churn children finish" : "all exit churn children finish");
    printf("MMAPLIFECHECK %s free_before=%" PRIu64 " free_after=%" PRIu64 " lost=%" PRIu64 "\n",
           exec_after ? "exec" : "exit", before, after, lost);
    if (accounting)
        check(lost < FILE_BYTES, exec_after ? "exec releases 128 MiB of mapped pages" :
                                           "exit releases 128 MiB of mapped pages");
    else puts("MMAPLIFECHECK SKIP global free-memory assertion (--no-accounting)");
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--empty")) return 0;
    if (argc < 2 || argc > 3 || argv[0][0] != '/' || argv[1][0] != '/' ||
        (argc == 3 && strcmp(argv[2], "--no-accounting"))) {
        fprintf(stderr, "usage: /absolute/mmaplifecheck absolute-writable-directory [--no-accounting]\n");
        return 2;
    }
    char path[320];
    if (snprintf(path, sizeof(path), "%s/mmaplifecheck-XXXXXX", argv[1]) >= (int)sizeof(path)) return 2;
    int fd = mkstemp(path);
    if (fd < 0) fatal("mkstemp");
    unsigned char page[4096] = {0};
    for (size_t p = 0; p < FILE_BYTES / 4096; ++p) {
        page[0] = marker(p); page[4095] = marker(p) ^ 0x5a;
        size_t off = 0;
        while (off < sizeof(page)) {
            ssize_t n = write(fd, page + off, sizeof(page) - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) fatal("write fixture");
            off += (size_t)n;
        }
    }
    close(fd);
    check(run_child(argv[0], path, 0) && run_child(argv[0], path, 1), "warm exit and exec paths");
    if (argc == 2 && settled_free() < 512ULL * 1024 * 1024) {
        unlink(path);
        fprintf(stderr, "MMAPLIFECHECK needs 512 MiB free for a bounded old-kernel failure\n");
        return 2;
    }
    churn(argv[0], path, 0, argc == 2);
    churn(argv[0], path, 1, argc == 2);

    volatile const unsigned char *map = map_file(path);
    if ((const void *)map == MAP_FAILED) fatal("parent mmap");
    check(verify(map), "parent primes inherited readonly file mapping");
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) fatal("fork inherited mapping");
    if (!child) {
        if (!verify(map)) _exit(92);
        execl(argv[0], argv[0], "--empty", (char *)NULL);
        _exit(93);
    }
    check(wait_child(child), "child reads inherited mapping then execs");
    check(verify(map), "parent mapping survives child's exec and exit");
    if (munmap((void *)map, FILE_BYTES) || unlink(path)) fatal("cleanup");
    printf("MMAPLIFECHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
