#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
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
static void fatal(const char *what) { perror(what); exit(1); }
static void check(int ok, const char *label)
{
    printf("METACHECK %s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
static struct stat consistent(const char *path, int fd, const char *label)
{
    struct stat p, f;
    struct xstat xp = {0}, xf = {0};
    if (stat(path, &p) || fstat(fd, &f)) fatal("stat/fstat");
    if (syscall(SYS_statx, AT_FDCWD, path, 0, 0x7ff, &xp) ||
        syscall(SYS_statx, fd, "", 0x1000, 0x7ff, &xf)) fatal("statx");
    int ok = p.st_ino == f.st_ino && p.st_ino == xp.ino && p.st_ino == xf.ino &&
             p.st_mode == f.st_mode && p.st_mode == xp.mode && p.st_mode == xf.mode &&
             (uint64_t)p.st_size == xp.size && p.st_size == f.st_size && xp.size == xf.size;
#define SAME_TIME(name) \
    (p.st_##name##tim.tv_sec == f.st_##name##tim.tv_sec && \
     p.st_##name##tim.tv_nsec == f.st_##name##tim.tv_nsec && \
     p.st_##name##tim.tv_sec == xp.name##time.sec && \
     p.st_##name##tim.tv_nsec == xp.name##time.nsec && \
     xp.name##time.sec == xf.name##time.sec && xp.name##time.nsec == xf.name##time.nsec)
    ok = ok && SAME_TIME(a) && SAME_TIME(m) && SAME_TIME(c);
#undef SAME_TIME
    check(ok, label);
    return p;
}
static void set_times(const char *path, time_t atime, time_t mtime)
{
    struct timespec ts[2] = {{atime, 0}, {mtime, 0}};
    if (utimensat(AT_FDCWD, path, ts, 0)) fatal("utimensat");
}
int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s writable-directory [existing-executable]\n", argv[0]);
        return 2;
    }
    char path[512], linkpath[520];
    snprintf(path, sizeof(path), "%s/metacheck-XXXXXX", argv[1]);
    int fd = mkstemp(path);
    if (fd < 0) fatal("mkstemp");
    struct stat st = consistent(path, fd, "new file path/fd/statx agree");
    check(st.st_mtim.tv_sec > 0 && st.st_ctim.tv_sec > 0 && st.st_atim.tv_sec > 0,
          "new inode timestamps are nonzero");
    if (write(fd, "metadata", 8) != 8) fatal("write");
    set_times(path, 1600000000, 1600000100);
    st = consistent(path, fd, "explicit timestamps visible through already-open fd");
    check(st.st_atim.tv_sec == 1600000000 && st.st_mtim.tv_sec == 1600000100 &&
          st.st_atim.tv_nsec == 0 && st.st_mtim.tv_nsec == 0, "utimensat exact seconds");
    struct timespec ts[2] = {{1600000200, 0}, {1600000300, 0}};
    if (futimens(fd, ts)) fatal("futimens");
    st = consistent(path, fd, "futimens fd/path/statx agree");
    check(st.st_atim.tv_sec == 1600000200 && st.st_mtim.tv_sec == 1600000300,
          "futimens exact seconds");
    ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = 1600000400;
    if (utimensat(AT_FDCWD, path, ts, 0)) fatal("utimensat OMIT");
    st = consistent(path, fd, "UTIME_OMIT metadata consistency");
    check(st.st_atim.tv_sec == 1600000200 && st.st_mtim.tv_sec == 1600000400,
          "UTIME_OMIT preserves atime");
    if (pwrite(fd, "M", 1, 0) != 1) fatal("pwrite");
    st = consistent(path, fd, "write metadata consistency");
    check(st.st_mtim.tv_sec > 1600000400 && st.st_ctim.tv_sec > 1600000400,
          "write advances mtime/ctime");
    set_times(path, 1600000000, 1600000100);
    char c;
    if (pread(fd, &c, 1, 0) != 1) fatal("pread");
    st = consistent(path, fd, "read metadata consistency");
    check(st.st_atim.tv_sec > 1600000000 && st.st_mtim.tv_sec == 1600000100,
          "relatime read updates only atime");
    int truncated = open(path, O_WRONLY | O_TRUNC);
    if (truncated < 0) fatal("open O_TRUNC");
    close(truncated);
    st = consistent(path, fd, "truncate visible through existing fd");
    check(st.st_size == 0 && st.st_mtim.tv_sec > 1600000100, "O_TRUNC updates size/mtime");
    set_times(path, 1600000000, 1600000100);
    if (utimensat(AT_FDCWD, path, NULL, 0)) fatal("utimensat NOW");
    st = consistent(path, fd, "UTIME_NOW consistency");
    check(st.st_atim.tv_sec > 1600000000 && st.st_mtim.tv_sec > 1600000100,
          "NULL times repairs historical/zero timestamps");

    const char *executable = argc == 3 ? argv[2] : argv[0];
    snprintf(linkpath, sizeof(linkpath), "%s.link", path);
    if (symlink(executable, linkpath)) fatal("symlink");
    int execfd = open(linkpath, O_RDONLY);
    if (execfd < 0) fatal("open executable symlink");
    struct stat target = consistent(linkpath, execfd, "followed executable symlink path/fd/statx agree");
    check((target.st_mode & 0111) != 0, "executable permissions preserved");
    ts[0] = (struct timespec){1600000500, 0};
    ts[1] = (struct timespec){1600000600, 0};
    if (utimensat(AT_FDCWD, linkpath, ts, AT_SYMLINK_NOFOLLOW)) fatal("utimensat nofollow");
    struct stat linkst;
    if (lstat(linkpath, &linkst)) fatal("lstat");
    check(linkst.st_mtim.tv_sec == 1600000600 && S_ISLNK(linkst.st_mode),
          "nofollow changes symlink timestamp");
    st = consistent(linkpath, execfd, "nofollow leaves followed target consistent");
    check(st.st_mtim.tv_sec == target.st_mtim.tv_sec, "nofollow preserves target timestamp");
    close(execfd);
    unlink(linkpath);
    ts[0].tv_nsec = 1000000000;
    errno = 0;
    check(utimensat(AT_FDCWD, path, ts, 0) < 0 && errno == EINVAL,
          "invalid nanoseconds rejected");
    errno = 0;
    check(utimensat(AT_FDCWD, linkpath, NULL, 0) < 0 && errno == ENOENT,
          "missing pathname rejected");
    close(fd);
    unlink(path);
    printf("METACHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
