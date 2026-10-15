#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 1
#endif
static int failures;
#define CHECK(x) do { if (!(x)) { printf("FALLOCHECK FAIL line=%d errno=%d: %s\n", __LINE__, errno, #x); ++failures; } } while (0)
static long alloc(int fd, int mode, int64_t offset, int64_t length) {
    return syscall(SYS_fallocate, fd, mode, offset, length);
}
static int64_t size(int fd) { struct stat st; return fstat(fd, &st) ? -1 : st.st_size; }
static int64_t blocks(int fd) { struct stat st; return fstat(fd, &st) ? -1 : st.st_blocks; }
int main(int argc, char **argv)
{
    syscall(SYS_alarm, 30);
    int fd = memfd_create("fallocatecheck", MFD_CLOEXEC);
    CHECK(fd >= 0); if (fd < 0) return 1;
    CHECK(lseek(fd, 19, SEEK_SET) == 19);
    CHECK(alloc(fd, 0, 0, 2304) == 0);
    CHECK(size(fd) == 2304);
    CHECK(blocks(fd) >= 8);
    CHECK(lseek(fd, 0, SEEK_CUR) == 19);
    // Old false-success stub is a failure here. Avoid an intentional EOF fault
    // so the old-negative result remains readable and the guest stays healthy.
    if (size(fd) != 2304) { close(fd); return 1; }
    unsigned char *map = mmap(NULL, 2304, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED); if (map == MAP_FAILED) return 1;
    CHECK(map[32] == 0); map[32] = 0x6b;
    unsigned char value = 0;
    CHECK(pread(fd, &value, 1, 32) == 1 && value == 0x6b);
    CHECK(alloc(fd, 0, 17, 9000) == 0);
    CHECK(size(fd) == 9017 && map[32] == 0x6b);
    CHECK(alloc(fd, 0, 0, 1) == 0 && size(fd) == 9017);
    CHECK(alloc(fd, FALLOC_FL_KEEP_SIZE, 16381, 8197) == 0);
    CHECK(blocks(fd) >= 56); // seven allocated pages, including four beyond EOF
    CHECK(size(fd) == 9017 && lseek(fd, 0, SEEK_CUR) == 19);
    CHECK(ftruncate(fd, 24578) == 0);
    unsigned char zeros[256]; memset(zeros, 1, sizeof(zeros));
    CHECK(pread(fd, zeros, sizeof(zeros), 16381) == (ssize_t)sizeof(zeros));
    for (unsigned i = 0; i < sizeof(zeros); ++i) CHECK(zeros[i] == 0);
    CHECK(pwrite(fd, "K", 1, 24577) == 1);
    CHECK(alloc(fd, FALLOC_FL_KEEP_SIZE, 24577, 1) == 0);
    value = 0; CHECK(pread(fd, &value, 1, 24577) == 1 && value == 'K');
    CHECK(syscall(SYS_fallocate, ((uint64_t)0x12345678 << 32) | (uint32_t)fd, 0, 0, 1) == 0);
    char path[64]; snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    int ro = open(path, O_RDONLY); CHECK(ro >= 0);
    errno = 0; CHECK(alloc(ro, 0, 0, 1) == -1 && errno == EBADF);
    errno = 0; CHECK(alloc(-1, 0, 0, 1) == -1 && errno == EBADF);
    errno = 0; CHECK(alloc(fd, 0, -1, 1) == -1 && errno == EINVAL);
    errno = 0; CHECK(alloc(fd, 0, 0, -1) == -1 && errno == EINVAL);
    errno = 0; CHECK(alloc(fd, 0, 0, 0) == -1 && errno == EINVAL);
    errno = 0; CHECK(alloc(fd, 0, INT64_MAX-1, 4) == -1 && errno == EFBIG);
    errno = 0; CHECK(alloc(fd, 0x40000000, 0, 1) == -1 && errno == EOPNOTSUPP);
    CHECK(size(fd) == 24578 && lseek(fd, 0, SEEK_CUR) == 19 && map[32] == 0x6b);
    munmap(map, 2304); close(ro); close(fd);
    if (argc == 2 && !strcmp(argv[1], "--brook")) {
        const char *paths[] = {"/data/FALLOC.REG", "/boot/FALLOC.REG"};
        for (unsigned i = 0; i < 2; ++i) {
            int regular = open(paths[i], O_CREAT|O_RDWR|O_TRUNC, 0600);
            CHECK(regular >= 0);
            if (regular < 0) continue;
            CHECK(write(regular, "R", 1) == 1);
            errno = 0;
            CHECK(alloc(regular, 0, 0, 4096) == -1 && errno == EOPNOTSUPP);
            CHECK(size(regular) == 1 && lseek(regular, 0, SEEK_CUR) == 1);
            close(regular); unlink(paths[i]);
        }
    }
    printf("FALLOCHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
