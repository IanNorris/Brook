#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "TRUNCATECHECK FAIL line=%d %s errno=%d\n", __LINE__, #x, errno); ++failures; } } while (0)
static unsigned char buffer[65536];
static void zero_range(int fd, off_t start, off_t end)
{
    while (start < end) {
        size_t n = end - start > (off_t)sizeof(buffer) ? sizeof(buffer) : (size_t)(end - start);
        ssize_t got = pread(fd, buffer, n, start);
        CHECK(got == (ssize_t)n);
        if (got != (ssize_t)n) return;
        for (size_t i = 0; i < n; ++i) if (buffer[i]) {
            fprintf(stderr, "TRUNCATECHECK nonzero offset=%lld value=%u\n", (long long)(start + i), buffer[i]);
            ++failures; return;
        }
        start += n;
    }
}
static void size_is(int fd, const char *path, off_t size)
{
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == size);
    CHECK(stat(path, &st) == 0 && st.st_size == size);
}
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: truncatecheck PATH [--brook-gate|--verify]\n"); return 2; }
    const char *path = argv[1];
    int gate = argc > 2 && !strcmp(argv[2], "--brook-gate");
    if (argc > 2 && !strcmp(argv[2], "--verify")) {
        int fd = open(path, O_RDONLY);
        CHECK(fd >= 0);
        if (fd >= 0) { size_is(fd, path, 131119); zero_range(fd, 0, 131119); close(fd); }
        printf("TRUNCATECHECK VERIFY %s\n", failures ? "FAIL" : "PASS");
        return failures != 0;
    }
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    CHECK(fd >= 0);
    if (fd < 0) return 1;
    memset(buffer, 0xa7, sizeof(buffer));
    CHECK(write(fd, buffer, sizeof(buffer)) == sizeof(buffer));
    CHECK(write(fd, buffer, sizeof(buffer)) == sizeof(buffer));
    CHECK(write(fd, buffer, 47) == 47);
    CHECK(fsync(fd) == 0);
    int ro = open(path, O_RDONLY), alias = open(path, O_RDWR);
    CHECK(ro >= 0 && alias >= 0);
    CHECK(pread(ro, buffer, 1, 100000) == 1 && buffer[0] == 0xa7);
    CHECK(lseek(fd, 200000, SEEK_SET) == 200000);
    CHECK(ftruncate(fd, 4123) == 0);
    CHECK(lseek(fd, 0, SEEK_CUR) == 200000);
    size_is(alias, path, 4123); size_is(ro, path, 4123);
    CHECK(pread(ro, buffer, 1, 4123) == 0);
    CHECK(pread(ro, buffer, 1, 4122) == 1 && buffer[0] == 0xa7);
    CHECK(ftruncate(alias, 131119) == 0);
    CHECK(syscall(SYS_ftruncate, 0x1234567800000000ULL | (unsigned)alias, (off_t)131119) == 0);
    zero_range(ro, 4123, 131119);
    CHECK(read(fd, buffer, 1) == 0); // beyond-EOF read must not grow a writable FIL
    size_is(fd, path, 131119);
    errno = 0; CHECK(ftruncate(fd, -1) == -1 && errno == EINVAL);
    errno = 0; CHECK(ftruncate(-1, 0) == -1 && errno == EBADF);
    errno = 0; CHECK(ftruncate(ro, 0) == -1 && (errno == EINVAL || errno == EBADF));
    errno = 0; CHECK(truncate(path, -1) == -1 && errno == EINVAL);
    int dir = open("/", O_RDONLY | O_DIRECTORY);
    CHECK(dir >= 0);
    if (dir >= 0) { CHECK(ftruncate(dir, 0) == -1); close(dir); }
    CHECK(truncate("/", 0) == -1);
    CHECK(truncate(path, 17) == 0);
    size_is(ro, path, 17);
    CHECK(lseek(fd, 0, SEEK_CUR) == 200000);
    close(ro); close(alias);

    // Sparse markers straddle direct, single and double indirect boundaries
    // for 1, 2 and 4 KiB ext2 blocks. FAT also exercises cluster-chain removal.
    const unsigned blocks[] = {1024, 2048, 4096};
    for (unsigned b = 0; b < sizeof(blocks)/sizeof(blocks[0]); ++b) {
        off_t bs = blocks[b];
        off_t boundary[] = {12 * bs, (12 + bs/4) * bs};
        for (unsigned j = 0; j < 2; ++j) {
            CHECK(ftruncate(fd, 0) == 0);
            CHECK(pwrite(fd, "XYZ", 3, boundary[j] - 1) == 3);
            CHECK(ftruncate(fd, boundary[j] + 1) == 0);
            CHECK(ftruncate(fd, boundary[j] + bs + 3) == 0);
            CHECK(pread(fd, buffer, 2, boundary[j] - 1) == 2 && buffer[0] == 'X' && buffer[1] == 'Y');
            zero_range(fd, boundary[j] + 1, boundary[j] + bs + 3);
            CHECK(ftruncate(fd, boundary[j] - 1) == 0);
            CHECK(ftruncate(fd, boundary[j] + bs + 3) == 0);
            zero_range(fd, boundary[j] - 1, boundary[j] + bs + 3);
        }
    }
    CHECK(ftruncate(fd, 12288) == 0);
    if (gate) {
        ro = open(path, O_RDONLY);
        void *map = mmap(NULL, 12288, PROT_READ, MAP_PRIVATE, ro, 0);
        CHECK(map != MAP_FAILED);
        if (map != MAP_FAILED) {
            volatile unsigned char value = *(volatile unsigned char *)map; (void)value;
            errno = 0; CHECK(ftruncate(fd, 1) == -1 && errno == EBUSY);
            int rejected = open(path, O_WRONLY | O_TRUNC);
            CHECK(rejected == -1);
            if (rejected >= 0) close(rejected);
            size_is(fd, path, 12288);
            CHECK(mprotect((char *)map + 4096, 4096, PROT_NONE) == 0);
            CHECK(munmap((char *)map + 4096, 4096) == 0);
            errno = 0; CHECK(ftruncate(fd, 1) == -1 && errno == EBUSY);
            pid_t child = fork();
            CHECK(child >= 0);
            if (!child) _exit(0); // kernel exit teardown must release inherited pins
            if (child > 0) { int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0); }
            CHECK(munmap(map, 4096) == 0);
            CHECK(munmap((char *)map + 8192, 4096) == 0);
        }
        int truncfd = open(path, O_WRONLY | O_TRUNC);
        CHECK(truncfd >= 0);
        if (truncfd >= 0) close(truncfd);
        size_is(fd, path, 0); size_is(ro, path, 0);
        close(ro);
        CHECK(ftruncate(fd, 1) == 0); // no leaked split/fork/fault pins
        // Bounded cancellation stress. A ready pipe proves the child owns a
        // mapping; repeated cold VAs create faults while the parent kills it.
        // Timing does not prove cancellation at a particular I/O instruction.
        alarm(30);
        for (unsigned round = 0; round < 16; ++round) {
            CHECK(ftruncate(fd, 262144) == 0);
            int ready[2];
            if (pipe(ready)) { CHECK(0); break; }
            pid_t victim = fork();
            CHECK(victim >= 0);
            if (!victim) {
                close(ready[0]);
                for (unsigned iteration = 0;; ++iteration) {
                    void *pages = mmap(NULL, 262144, PROT_READ, MAP_PRIVATE, fd, 0);
                    if (pages == MAP_FAILED) _exit(2);
                    if (!iteration && write(ready[1], "R", 1) != 1) _exit(3);
                    volatile unsigned char sum = 0;
                    for (unsigned offset = 0; offset < 262144; offset += 4096)
                        sum ^= ((volatile unsigned char *)pages)[offset];
                    (void)sum;
                    if (munmap(pages, 262144)) _exit(4);
                }
            }
            close(ready[1]);
            if (victim > 0) {
                char started = 0;
                CHECK(read(ready[0], &started, 1) == 1 && started == 'R');
                usleep(50 + round * 50);
                CHECK(kill(victim, SIGKILL) == 0);
                int status = 0;
                CHECK(waitpid(victim, &status, 0) == victim && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
                CHECK(ftruncate(fd, 1) == 0);
            }
            close(ready[0]);
        }
        alarm(0);
    }
    CHECK(ftruncate(fd, 0) == 0);
    CHECK(ftruncate(fd, 131119) == 0);
    zero_range(fd, 0, 131119);
    CHECK(fsync(fd) == 0);
    CHECK(close(fd) == 0);
    fd = open(path, O_RDONLY); CHECK(fd >= 0);
    if (fd >= 0) { size_is(fd, path, 131119); zero_range(fd, 0, 131119); close(fd); }
    printf("TRUNCATECHECK %s path=%s gate=%d retained_bytes=131119\n", failures ? "FAIL" : "PASS", path, gate);
    return failures != 0;
}
