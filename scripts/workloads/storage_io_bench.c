// Sequential storage benchmark for a disposable Brook QEMU guest.
//
//   storage_io_bench DIR [MiB]
//
// Phases: write MiB in 1 MiB writes + fsync; read it back in 1 MiB reads;
// write a second file in 64 KiB writes + fsync (downloader-like). Each phase
// reports wall time, this process's CPU (utime+stime, centiseconds) and the
// system-wide busy/idle centiseconds from /proc/stat, so busy-polling in the
// syscall path shows up as stime and as system busy time.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct Sample { double t; unsigned long long self, busy, idle; };

static double Now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static int ReadFile(const char* path, char* out, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, size - 1);
    close(fd);
    if (n < 0) return -1;
    out[n] = 0;
    return (int)n;
}
static struct Sample Take(void) {
    struct Sample s = {Now(), 0, 0, 0};
    char buf[4096];
    if (ReadFile("/proc/self/stat", buf, sizeof buf) > 0) {
        char* p = strrchr(buf, ')');
        // Fields after ')': state(3) ... utime(14) stime(15).
        for (int field = 2; p && field < 14; ++field) p = strchr(p + 1, ' ');
        unsigned long long ut = 0, st = 0;
        if (p && sscanf(p + 1, "%llu %llu", &ut, &st) == 2) s.self = ut + st;
    }
    if (ReadFile("/proc/stat", buf, sizeof buf) > 0) {
        unsigned long long user = 0, nice = 0, sys = 0, idle = 0;
        if (sscanf(buf, "cpu %llu %llu %llu %llu", &user, &nice, &sys, &idle) == 4) {
            s.busy = user + nice + sys;
            s.idle = idle;
        }
    }
    return s;
}
static void Report(const char* phase, uint64_t bytes, struct Sample a, struct Sample b) {
    const double seconds = b.t - a.t;
    printf("IOBENCH phase=%s bytes=%llu ms=%.0f mibps=%.1f self_cs=%llu sys_busy_cs=%llu sys_idle_cs=%llu\n",
           phase, (unsigned long long)bytes, seconds * 1000, seconds > 0 ? bytes / 1048576.0 / seconds : 0,
           b.self - a.self, b.busy - a.busy, b.idle - a.idle);
    fflush(stdout);
}
static void Dump(const char* path) {
    char buf[4096];
    if (ReadFile(path, buf, sizeof buf) < 0) return;
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) printf("IOBENCH %s: %s\n", path, line);
    fflush(stdout);
}
static int WriteFile(const char* path, uint64_t bytes, size_t chunk, unsigned char* buffer) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    for (uint64_t done = 0; done < bytes; done += chunk) {
        for (size_t i = 0; i < chunk; i += 4096) memcpy(buffer + i, &done, sizeof done);
        if (write(fd, buffer, chunk) != (ssize_t)chunk) { perror("write"); close(fd); return 1; }
    }
    if (fsync(fd)) { perror("fsync"); close(fd); return 1; }
    return close(fd) ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s DIR [MiB]\n", argv[0]); return 2; }
    const uint64_t mib = argc > 2 ? strtoull(argv[2], NULL, 10) : 256;
    const uint64_t bytes = mib << 20;
    const size_t big = 1 << 20, small = 64 << 10;
    unsigned char* buffer = malloc(big);
    if (!buffer) return 1;
    memset(buffer, 0x5a, big);
    char seq[512], dl[512];
    snprintf(seq, sizeof seq, "%s/iobench-seq.bin", argv[1]);
    snprintf(dl, sizeof dl, "%s/iobench-dl.bin", argv[1]);
    unlink(seq);
    unlink(dl);
    sync();
    Dump("/proc/diskstats");
    printf("IOBENCH_START mib=%llu\n", (unsigned long long)mib);
    fflush(stdout);
    sleep(2); // let the bridge observe an idle baseline sample

    struct Sample a = Take();
    if (WriteFile(seq, bytes, big, buffer)) return 1;
    struct Sample b = Take();
    Report("write1m", bytes, a, b);

    a = Take();
    int fd = open(seq, O_RDONLY);
    if (fd < 0) { perror("open read"); return 1; }
    uint64_t total = 0, mismatches = 0;
    for (;;) {
        ssize_t n = read(fd, buffer, big);
        if (n < 0) { perror("read"); return 1; }
        if (!n) break;
        for (ssize_t i = 0; i + 8 <= n; i += 4096) {
            uint64_t value;
            memcpy(&value, buffer + i, sizeof value);
            if (value != ((total + i) & ~(uint64_t)(big - 1))) ++mismatches;
        }
        total += (uint64_t)n;
    }
    close(fd);
    b = Take();
    Report("read1m", total, a, b);
    if (total != bytes || mismatches) {
        printf("IOBENCH_VERIFY_FAILED total=%llu mismatches=%llu\n", (unsigned long long)total,
               (unsigned long long)mismatches);
        return 1;
    }

    const uint64_t dlBytes = bytes / 2;
    a = Take();
    if (WriteFile(dl, dlBytes, small, buffer)) return 1;
    b = Take();
    Report("write64k", dlBytes, a, b);

    unlink(seq);
    unlink(dl);
    sync();
    Dump("/proc/diskstats");
    sleep(2);
    printf("IOBENCH_DONE\n");
    fflush(stdout);
    return 0;
}
