#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
// Linux statx ABI, independent of whether the probe libc exposes statx().
struct stamp { int64_t sec; uint32_t nsec; int32_t reserved; };
struct xstat {
    uint32_t mask, blksize;
    uint64_t attributes;
    uint32_t nlink, uid, gid;
    uint16_t mode, pad;
    uint64_t ino, size, blocks, attributes_mask;
    struct stamp atime, btime, ctime, mtime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
    uint64_t mnt_id;
    uint32_t dio_mem_align, dio_offset_align;
    uint64_t spare[12];
};
_Static_assert(sizeof(struct xstat) == 256, "statx ABI size");

static int failures;
static void check(int ok, const char *label) {
    printf("DEVICECHECK %s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
static struct stat inspect(const char *path) {
    struct stat p, f, l, a;
    struct xstat xp = {0}, xf = {0};
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0 || stat(path, &p) || fstat(fd, &f) || lstat(path, &l) ||
        fstatat(AT_FDCWD, path, &a, 0) ||
        syscall(SYS_statx, AT_FDCWD, path, 0, 0x7ff, &xp) ||
        syscall(SYS_statx, fd, "", 0x1000, 0x7ff, &xf)) {
        perror(path); exit(2);
    }
    check(p.st_dev == f.st_dev && p.st_dev == l.st_dev && p.st_dev == a.st_dev,
          "stat/fstat/lstat/fstatat agree on device");
    check(major(p.st_dev) == xp.dev_major && minor(p.st_dev) == xp.dev_minor &&
          xp.dev_major == xf.dev_major && xp.dev_minor == xf.dev_minor,
          "path and descriptor statx agree with encoded stat device");
    printf("DEVICECHECK identity %s dev=%llu ino=%llu major=%u minor=%u\n",
           path, (unsigned long long)p.st_dev, (unsigned long long)p.st_ino,
           xp.dev_major, xp.dev_minor);
    close(fd);
    return p;
}
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: devicecheck mount1 mount2\n"); return 2; }
    struct stat a = inspect(argv[1]), b = inspect(argv[2]);
    check(a.st_dev != b.st_dev, "distinct mounts have distinct device identities");
    printf("DEVICECHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
