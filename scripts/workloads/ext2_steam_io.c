#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { MiB = 1 << 20, Files = 4, FileBytes = 16 * MiB };
static unsigned char buffer[64 * 1024];

static void require(int ok, const char *operation)
{
    if (ok) return;
    perror(operation);
    printf("STEAM_IO_FAIL %s\n", operation);
    exit(1);
}

static double seconds(void)
{
    struct timespec now;
    require(clock_gettime(CLOCK_MONOTONIC, &now) == 0, "clock");
    return now.tv_sec + now.tv_nsec / 1e9;
}

static void print_counters(const char *path, const char *prefix)
{
    FILE *counters = fopen(path, "r");
    if (!counters) return;
    char line[1024];
    while (fgets(line, sizeof(line), counters)) printf("%s %s", prefix, line);
    fclose(counters);
}

static void report(const char *phase, unsigned requests, double started)
{
    double elapsed = seconds() - started;
    printf("STEAM_IO phase=%s bytes=%d requests=%u seconds=%.6f MiB_s=%.2f ops_s=%.0f\n",
           phase, Files * FileBytes, requests, elapsed, Files * FileBytes / (MiB * elapsed),
           requests / elapsed);
    print_counters("/proc/ext2writeback", "STEAM_IO_WRITEBACK");
    print_counters("/proc/diskstats", "STEAM_IO_DISKSTATS");
}

static void run_phase(const char *phase, int writing, unsigned char value)
{
    const size_t lengths[] = {1024, 8192, 4096, 16384, 2048, 65536};
    unsigned requests = 0;
    double started = seconds();
    memset(buffer, value, sizeof(buffer));
    for (unsigned i = 0; i < Files; ++i) {
        char path[24], temp[24];
        snprintf(path, sizeof(path), "pack-%u.dat", i);
        snprintf(temp, sizeof(temp), "pack-%u.part", i);
        int creating = writing && strcmp(phase, "create") == 0;
        int fd = open(creating ? temp : path, creating ? O_CREAT | O_EXCL | O_RDWR :
                      (writing ? O_RDWR : O_RDONLY), 0644);
        require(fd >= 0, "open");
        for (size_t at = 0; at < FileBytes; ++requests) {
            size_t count = lengths[requests % (sizeof(lengths) / sizeof(lengths[0]))];
            if (count > FileBytes - at) count = FileBytes - at;
            ssize_t result = writing ? write(fd, buffer, count) : read(fd, buffer, count);
            require(result == (ssize_t) count, writing ? "write" : "read");
            if (!writing)
                for (size_t j = 0; j < count; ++j)
                    require(buffer[j] == value, "verify");
            at += count;
        }
        if (writing) require(fdatasync(fd) == 0, "fdatasync");
        if (creating) {
            struct stat before, after;
            require(fstat(fd, &before) == 0, "stat before rename");
            require(rename(temp, path) == 0, "rename");
            require(stat(path, &after) == 0 && before.st_ino == after.st_ino, "rename inode");
        }
        require(close(fd) == 0, "close");
    }
    if (writing) {
        int dir = open(".", O_RDONLY | O_DIRECTORY);
        require(dir >= 0 && fsync(dir) == 0 && close(dir) == 0, "fsync directory");
    }
    report(phase, requests, started);
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    require(argc == 2 && chdir(argv[1]) == 0, "benchmark mount");
    print_counters("/proc/diskstats", "STEAM_IO_DISKSTATS");
    run_phase("create", 1, 0x5a);
    run_phase("read", 0, 0x5a);
    run_phase("reread", 0, 0x5a);
    run_phase("overwrite", 1, 0x39);
    run_phase("verify", 0, 0x39);
    // Put an O_SYNC positioned write on a separate file, then dirty an
    // existing data block. Cutting here exercises a mounted filesystem
    // with an uncommitted write, after the allocation metadata was synced.
    int witness = open("sync.witness", O_WRONLY | O_CREAT | O_EXCL | O_SYNC, 0644);
    const char durable[] = "ordered writeback witness\n";
    require(witness >= 0 && pwrite(witness, durable, sizeof(durable)-1, 0) ==
            (ssize_t)(sizeof(durable)-1) && close(witness) == 0, "O_SYNC witness");
    int fd = open("pack-3.dat", O_WRONLY);
    memset(buffer, 0x39, 4096);
    require(fd >= 0 && pwrite(fd, buffer, 4096, 4096) == 4096, "buffered overwrite");
    print_counters("/proc/ext2writeback", "STEAM_IO_AT_CUT");
    puts("STEAM_IO_CUT_READY");
    for (;;) sleep(60);
}
