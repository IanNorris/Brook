// Run only in a disposable guest. Exercise unlinkat through the real VFS
// after filling a private tmpfs with both file data and metadata.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { perror(#x); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "--disposable-vm") || geteuid() != 0) return 2;
    alarm(90);
    pid_t child = syscall(SYS_clone, (unsigned long)(CLONE_NEWNS | SIGCHLD), 0L, 0L, 0L, 0L);
    CHECK(child >= 0);
    if (child) {
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
        puts("TMPFS_FULL_UNLINK_PARENT_PASS");
        return 0;
    }
    CHECK(!mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL));
    char base[] = "/tmp/brook-full-unlink-XXXXXX";
    CHECK(mkdtemp(base));
    unsigned full = 0;
    for (unsigned trial = 0; trial < 480; ++trial) {
        CHECK(!mount("tmpfs", base, "tmpfs", MS_NODEV | MS_NOSUID, "size=64k"));
        int dir = open(base, O_RDONLY | O_DIRECTORY);
        CHECK(dir >= 0);
        int held = openat(dir, "held", O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(held >= 0 && write(held, "Q", 1) == 1);
        int fill = openat(dir, "fill", O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(fill >= 0);
        char data[4096] = {0};
        while (write(fill, data, sizeof(data)) > 0) {}
        CHECK(errno == ENOSPC);
        // Symlink targets use fine-grained metadata allocations. Different
        // lengths vary the residual slack instead of assuming page accounting.
        char target[481], name[32];
        memset(target, 'x', sizeof(target));
        unsigned count = 0;
        for (unsigned length = 480 - trial; length; --length) {
            target[length] = 0;
            snprintf(name, sizeof(name), "pad%u", count);
            int made = !symlinkat(target, dir, name);
            if (!made) CHECK(errno == ENOSPC || errno == ENOMEM);
            target[length] = 'x';
            if (made) { ++count; length = 481 - trial; }
        }
        struct statvfs space;
        CHECK(!fstatvfs(dir, &space));
        if (space.f_bfree == 0) ++full;
        errno = 0;
        int result = unlinkat(dir, "held", 0), failure = errno;
        struct stat st;
        CHECK(!fstat(held, &st));
        char value = 0;
        CHECK(pread(held, &value, 1, 0) == 1 && value == 'Q');
        if (result || st.st_nlink != 0) {
            fprintf(stderr, "TMPFS_FULL_UNLINK_FAIL trial=%u errno=%d links=%lu free_blocks=%lu\n",
                    trial, failure, (unsigned long)st.st_nlink, (unsigned long)space.f_bfree);
            return 1;
        }
        CHECK(!close(fill) && !close(held) && !close(dir));
        CHECK(!umount2(base, MNT_DETACH));
    }
    CHECK(!rmdir(base));
    CHECK(full == 480);
    puts("TMPFS_FULL_UNLINK_PASS trials=480");
    return 0;
}
