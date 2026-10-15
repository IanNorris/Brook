#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// create PATH: persist seed 0, exercise fsync/fdatasync/O_SYNC/O_DSYNC.
// retry PATH [--wait]: truncate/refill an existing file with seed 1, require the
// first fsync to return EIO, then retry and verify. --wait pauses immediately
// before fsync: host sets brook_virtio_flush_failures[slot]=1 then sends SIGUSR1
// to the printed PID. The file remains for reboot/offline verification.
// verify PATH [SEED]: read-only verification, usable after a separate boot.
enum { FILE_BYTES = 65536 + 193 };
static unsigned char data[FILE_BYTES], actual[FILE_BYTES];
static volatile sig_atomic_t released;
static int failures;
static void release_wait(int signal) { (void)signal; released = 1; }
static void fatal(const char* what) { perror(what); exit(1); }
static void check(int ok, const char* label)
{
    printf("FLUSHCHECK %s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
static int64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) fatal("clock_gettime");
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void pattern(unsigned seed)
{
    for (unsigned i = 0; i < FILE_BYTES; ++i)
        data[i] = (unsigned char)((i * 37u) ^ (i >> 8) ^ (seed * 91u) ^ 0xa5u);
}
static void write_all(int fd)
{
    size_t off = 0;
    while (off < sizeof(data)) {
        ssize_t n = write(fd, data + off, sizeof(data) - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fatal("write");
        off += (size_t)n;
    }
}
static void verify(const char* path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) fatal("verify open");
    struct stat st;
    if (fstat(fd, &st)) fatal("verify stat");
    check(st.st_size == FILE_BYTES, "persisted file size");
    size_t off = 0;
    while (off < sizeof(actual)) {
        ssize_t n = read(fd, actual + off, sizeof(actual) - off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) fatal("verify read");
        if (!n) break;
        off += (size_t)n;
    }
    check(off == sizeof(actual) && !memcmp(data, actual, sizeof(data)), "all persisted bytes");
    if (close(fd)) fatal("verify close");
}
int main(int argc, char** argv)
{
    setbuf(stdout, NULL);
    if (argc == 3 && !strcmp(argv[1], "release")) {
        char *end;
        errno = 0;
        long pid = strtol(argv[2], &end, 10);
        if (errno || *end || pid < 2 || pid > 2147483647L) return 2;
        if (kill((pid_t)pid, SIGUSR1)) fatal("release signal");
        printf("FLUSHCHECK RELEASED pid=%ld\n", pid);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "cleanup")) {
        pattern(1);
        verify(argv[2]);
        if (failures) return 1;
        if (unlink(argv[2])) fatal("cleanup unlink");
        printf("FLUSHCHECK CLEANUP PASS path=%s\n", argv[2]);
        return 0;
    }
    if (argc < 3 || argc > 4 ||
        (strcmp(argv[1], "create") && strcmp(argv[1], "retry") && strcmp(argv[1], "verify"))) {
        fprintf(stderr, "usage: %s create PATH | retry PATH [--wait] | verify PATH [0|1] | release PID | cleanup PATH\n", argv[0]);
        return 2;
    }
    int retry = !strcmp(argv[1], "retry");
    int verify_only = !strcmp(argv[1], "verify");
    if (argc == 4 && ((!retry && !verify_only) ||
        (retry && strcmp(argv[3], "--wait")) ||
        (verify_only && strcmp(argv[3], "0") && strcmp(argv[3], "1")))) return 2;
    unsigned seed = retry || (verify_only && argc == 4 && !strcmp(argv[3], "1"));
    pattern(seed);
    if (verify_only) {
        verify(argv[2]);
        return failures ? 1 : 0;
    }
    struct sigaction action = {.sa_handler = release_wait};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL)) fatal("sigaction");
    // Truncate and reallocate on retry so bitmap/BGDT metadata, as well as
    // data, is dirty when the injected flush fails.
    int fd = open(argv[2], retry ? O_RDWR | O_TRUNC : O_RDWR | O_CREAT | O_EXCL, 0640);
    if (fd < 0) fatal("open");
    write_all(fd);
    if (retry && argc == 4 && !strcmp(argv[3], "--wait")) {
        printf("FLUSHCHECK READY pid=%ld path=%s inject flush then SIGUSR1\n", (long)getpid(), argv[2]);
        fflush(stdout);
        int64_t deadline = now_ms() + 120000;
        while (!released && now_ms() < deadline) {
            struct timespec pause = {0, 10000000};
            nanosleep(&pause, NULL);
        }
        if (!released) { fprintf(stderr, "FLUSHCHECK FAIL injection watchdog\n"); return 1; }
    }
    errno = 0;
    int result = fsync(fd), error = errno;
    if (retry) {
        printf("FLUSHCHECK first fsync result=%d errno=%d\n", result, error);
        check(result == -1 && error == EIO, "injected flush error reaches fsync");
        check(fsync(fd) == 0, "failed dirty flush can be retried");
    } else {
        check(result == 0, "fsync completes");
    }
    check(fdatasync(fd) == 0, "fdatasync completes");
    if (close(fd)) fatal("close");
    verify(argv[2]);
    if (!retry) {
        fd = open(argv[2], O_RDWR | O_SYNC);
        if (fd < 0) fatal("O_SYNC open");
        check(pwrite(fd, data + 17, 3079, 17) == 3079, "O_SYNC pwrite completes");
        if (close(fd)) fatal("O_SYNC close");
        fd = open(argv[2], O_RDWR | O_DSYNC);
        if (fd < 0) fatal("O_DSYNC open");
        check(write(fd, data, 1027) == 1027, "O_DSYNC write completes");
        if (close(fd)) fatal("O_DSYNC close");
        verify(argv[2]);
    }
    errno = 0;
    check(fsync(-1) == -1 && errno == EBADF, "invalid fd rejected");
    printf("FLUSHCHECK %s path=%s seed=%u failures=%d\n", failures ? "FAIL" : "PASS", argv[2], seed, failures);
    return failures ? 1 : 0;
}
