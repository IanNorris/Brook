#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>

_Static_assert(sizeof(struct statfs) == 120, "x86-64 Linux statfs ABI");
static int host;
static void sane(const struct statfs *s) {
    assert(s->f_type && s->f_bsize && s->f_frsize && s->f_blocks);
    assert(s->f_bavail <= s->f_bfree && s->f_bfree <= s->f_blocks);
    assert(s->f_ffree <= s->f_files && s->f_namelen == 255);
    assert(s->f_flags & 0x20); /* Linux ST_VALID. */
}
static void same(const struct statfs *a, const struct statfs *b) {
    assert(a->f_type == b->f_type && a->f_bsize == b->f_bsize && a->f_blocks == b->f_blocks);
    assert(!memcmp(&a->f_fsid, &b->f_fsid, sizeof(a->f_fsid)));
}
static void check(const char *parent) {
    char directory[512], file[512], link[512];
    assert(snprintf(directory, sizeof(directory), "%s/brook-space-%ld-XXXXXX", parent, (long)getpid()) > 0);
    assert(mkdtemp(directory));
    assert(snprintf(file, sizeof(file), "%s/data", directory) > 0);
    assert(snprintf(link, sizeof(link), "%s/to-nix", directory) > 0);
    struct statfs before, after, via_fd;
    assert(statfs(directory, &before) == 0);sane(&before);
    if (!host) assert(before.f_type == 0xef53 || before.f_type == 0x4d44);
    assert(statfs(file, &after) == -1 && errno == ENOENT);
    int fd = open(file, O_CREAT | O_EXCL | O_RDWR, 0600);assert(fd >= 0);
    int copy = dup(fd);assert(copy >= 0);
    assert(fstatfs(copy, &via_fd) == 0);same(&before, &via_fd);
    char bytes[4096];memset(bytes, 0x5a, sizeof(bytes));
    for (unsigned i = 0; i < 64; ++i) assert(write(fd, bytes, sizeof(bytes)) == sizeof(bytes));
    assert(fsync(fd) == 0);
    assert(fstatfs(fd, &after) == 0);sane(&after);same(&before, &after);
    if (!host) {
        uint64_t blocks = (sizeof(bytes)*64 + before.f_bsize-1)/before.f_bsize;
        assert(before.f_bfree >= after.f_bfree + blocks);
        if (before.f_type == 0xef53) assert(before.f_ffree == after.f_ffree + 1);
    }
    assert(close(copy) == 0);
    assert(close(fd) == 0);
    fd = open(file, O_RDONLY);assert(fd >= 0); /* Exercises FAT's cached vnode. */
    assert(fstatfs(fd, &via_fd) == 0);same(&before, &via_fd);assert(close(fd) == 0);
    if (before.f_type == 0xef53 || host) {
        assert(chmod(file, 0000) == 0);
        assert(statfs(file, &via_fd) == 0);same(&before, &via_fd); /* Metadata needs no read permission. */
        assert(chmod(file, 0600) == 0);
        fd = open(file, O_RDONLY);assert(fd >= 0);
        assert(unlink(file) == 0);
        assert(fstatfs(fd, &via_fd) == 0);same(&before, &via_fd);
        assert(close(fd) == 0);
    } else assert(unlink(file) == 0);
    // Brook defers zero-holder orphan reclamation until a filesystem writer
    // or sync. Compare released capacity after that reclamation boundary.
    assert(syscall(SYS_sync) == 0);
    assert(statfs(directory, &after) == 0);
    if (!host) assert(after.f_bfree == before.f_bfree && after.f_ffree == before.f_ffree);
    if (!host && before.f_type == 0xef53) {
        assert(symlink("/nix", link) == 0);
        assert(statfs(link, &via_fd) == 0 && statfs("/nix", &after) == 0);same(&via_fd, &after);
        assert(unlink(link) == 0);
    }
    char cwd[512];assert(getcwd(cwd, sizeof(cwd)));assert(chdir(directory) == 0);
    assert(statfs(".", &via_fd) == 0);same(&before, &via_fd);assert(chdir(cwd) == 0);
    assert(syscall(SYS_statfs, parent, NULL) == -1 && errno == EFAULT);
    assert(syscall(SYS_statfs, (void *)1, &via_fd) == -1 && errno == EFAULT);
    assert(syscall(SYS_statfs, "", &via_fd) == -1 && errno == ENOENT);
    assert(syscall(SYS_fstatfs, -1, &via_fd) == -1 && errno == EBADF);
    assert(syscall(SYS_fstatfs, 100000, &via_fd) == -1 && errno == EBADF);
    char long_name[600];memset(long_name, 'a', sizeof(long_name));long_name[599]=0;
    assert(syscall(SYS_statfs, long_name, &via_fd) == -1 && errno == ENAMETOOLONG);
    char *pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(pages != MAP_FAILED && mprotect(pages+4096, 4096, PROT_NONE) == 0);
    size_t length = strlen(parent)+1;
    memcpy(pages+4096-length, parent, length);
    assert(syscall(SYS_statfs, pages+4096-length, &via_fd) == 0); /* NUL at page end. */
    memset(pages+4096-4, 'a', 4);
    assert(syscall(SYS_statfs, pages+4096-4, &via_fd) == -1 && errno == EFAULT);
    assert(syscall(SYS_statfs, parent, pages+4096-64) == -1 && errno == EFAULT);
    assert(munmap(pages, 8192) == 0);
    if (rmdir(directory) != 0) {
        // Existing FAT policy prevents directory removal while any file on
        // that volume is open/mapped. The controller removes this empty,
        // private fixture offline, after checked VM shutdown.
        assert(!host && before.f_type == 0x4d44 && errno == EBUSY);
        printf("FS_SPACE_FAT_CLEANUP_DEFERRED path=%s errno=EBUSY\n", directory);
    }
    printf("FS_SPACE_PASS path=%s type=%lx block=%ld total=%llu free=%llu available=%llu files=%llu\n",
        parent,(unsigned long)before.f_type,(long)before.f_bsize,(unsigned long long)before.f_blocks,
        (unsigned long long)before.f_bfree,(unsigned long long)before.f_bavail,(unsigned long long)before.f_files);
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "--host")) {host = 1;first = 2;}
    assert(argc > first);
    for (int i = first; i < argc; ++i) check(argv[i]);
    puts("FS_SPACE_ALL_PASS");
    return 0;
}
