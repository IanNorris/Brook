#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static void fatal(const char *s) { perror(s); exit(2); }
static void check(int ok, const char *s)
{
    printf("FILELIFECHECK %s %s\n", ok ? "PASS" : "FAIL", s);
    failures += !ok;
}
static int create(const char *name, const char *contents)
{
    int fd = open(name, O_CREAT|O_EXCL|O_RDWR, 0600);
    if (fd < 0 || write(fd, contents, strlen(contents)) != (ssize_t)strlen(contents))
        fatal(name);
    return fd;
}
static int contents(int fd, const char *expected)
{
    char data[64] = {0};
    ssize_t n = pread(fd, data, sizeof(data), 0);
    return n == (ssize_t)strlen(expected) && !memcmp(data, expected, n);
}

int main(int argc, char **argv)
{
    if ((argc != 2 && argc != 3) || argv[1][0] != '/') return 2;
    int require_reuse = argc == 3 && !strcmp(argv[2], "--require-reuse");
    if (argc == 3 && !require_reuse) return 2;
    char original[512], fixture[256];
    if (!getcwd(original, sizeof(original))) fatal("getcwd");
    if (snprintf(fixture, sizeof(fixture), "%s/filelife-XXXXXX", argv[1]) >= (int)sizeof(fixture)) return 2;
    if (!mkdtemp(fixture) || chdir(fixture)) fatal("fixture");

    int old = create("old", "original");
    int second = open("old", O_RDONLY);
    if (second < 0) fatal("second open");
    struct stat old_stat, replacement_stat;
    if (fstat(old, &old_stat) || unlink("old")) fatal("unlink open file");
    int replacement = create("replacement", "replaced");
    if (fstat(replacement, &replacement_stat)) fatal("replacement stat");
    check(old_stat.st_ino != replacement_stat.st_ino, "unlinked open inode cannot be reused");
    check(contents(old, "original") && contents(second, "original"), "both open descriptions retain original data");
    check(fstat(old, &old_stat) == 0 && old_stat.st_nlink == 0, "unlinked open inode reports zero links");
    check(pwrite(old, "ORIGINAL", 8, 0) == 8 && contents(second, "ORIGINAL"), "unlinked file remains writable through independent opens");
    close(old);
    check(contents(second, "ORIGINAL"), "closing one description preserves the other");
    check(contents(replacement, "replaced"), "writes to orphan never alter replacement");
    close(second); close(replacement);
    if (unlink("replacement")) fatal("replacement cleanup");

    int mapped = create("mapped", "mapped-original");
    long page = sysconf(_SC_PAGESIZE);
    char *view = mmap(NULL, page, PROT_READ, MAP_PRIVATE, mapped, 0);
    if (view == MAP_FAILED || unlink("mapped")) fatal("mapped unlink");
    close(mapped); // The mapping, rather than a descriptor, now owns its lifetime.
    int other = create("other", "changed-content");
    check(!memcmp(view, "mapped-original", 15), "unfaulted mapping retains orphan after last descriptor closes");
    if (munmap(view, page)) fatal("munmap");
    close(other);
    if (unlink("other")) fatal("other cleanup");

    int src = create("src", "source");
    int dst = create("dst", "destination");
    if (rename("src", "dst")) fatal("replace rename");
    check(contents(dst, "destination"), "open overwritten destination retains its data");
    check(fstat(dst, &old_stat) == 0 && old_stat.st_nlink == 0, "overwritten destination loses directory link");
    int fresh = open("dst", O_RDONLY);
    check(fresh >= 0 && contents(fresh, "source"), "new destination names source inode");
    check(rename("dst", "dst") == 0 && contents(src, "source"), "same-inode rename preserves source");
    if (fresh >= 0) close(fresh);
    close(src); close(dst);
    if (unlink("dst")) fatal("rename cleanup");
    for (int i = 0; i < 32; ++i) {
        int fd = create("next", "replacement cycle"); close(fd);
        if (i == 0) { fd = create("target", "initial target"); close(fd); }
        if (rename("next", "target")) fatal("rename cycle");
    }
    if (unlink("target")) fatal("rename cycle cleanup");

    struct stat parent_before, parent_after;
    if (stat(".", &parent_before) || mkdir("dir", 0700)) fatal("mkdir");
    int item = create("dir/item", "occupied"); close(item);
    errno = 0;
    int result = rmdir("dir"), error = errno;
    check(result == -1 && error == ENOTEMPTY, "rmdir rejects nonempty directory");
    errno = 0; result = unlink("dir"); error = errno;
    check(result == -1 && error == EISDIR, "unlink cannot remove directory");
    if (unlink("dir/item")) fatal("directory item cleanup");
    int dir = open("dir", O_RDONLY|O_DIRECTORY);
    if (dir < 0 || rmdir("dir")) fatal("remove open empty directory");
    check(fstat(dir, &old_stat) == 0 && old_stat.st_nlink == 0, "removed open directory reports zero links");
    close(dir);
    check(stat(".", &parent_after) == 0 && parent_after.st_nlink == parent_before.st_nlink,
          "mkdir/rmdir restores parent link count");

    sync();
    if (mkdir("a", 0700) || mkdir("b", 0700) || mkdir("a/old", 0700)) fatal("reuse directories");
    struct stat old_dir, new_dir, actual_parent, expected_parent;
    if (stat("a/old", &old_dir) || stat("a/old/..", &actual_parent) || rmdir("a/old") ||
        mkdir("b/new", 0700) || stat("b/new", &new_dir) || stat("b", &expected_parent) ||
        stat("b/new/..", &actual_parent)) fatal("directory reuse");
    check(actual_parent.st_ino == expected_parent.st_ino, "directory inode reuse cannot retain cached old parent");
    if (require_reuse)
        check(old_dir.st_ino == new_dir.st_ino, "directory cache regression exercised an actually reused inode");
    printf("FILELIFECHECK directory reuse old=%llu new=%llu\n",
           (unsigned long long)old_dir.st_ino, (unsigned long long)new_dir.st_ino);
    if (rmdir("b/new") || mkdir("a/move", 0700) || mkdir("a/move/child", 0700)) fatal("move directories");
    errno = 0; result = rename("a/move", "a/move/child/cycle"); error = errno;
    check(result == -1 && error == EINVAL, "directory rename rejects descendant cycle");
    if (rename("a/move", "b/move") || stat("b/move/..", &actual_parent)) fatal("cross-parent move");
    check(actual_parent.st_ino == expected_parent.st_ino, "cross-parent directory rename updates dotdot");
    if (rmdir("b/move/child") || mkdir("a/victim", 0700)) fatal("directory replacement");
    dir = open("a/victim", O_RDONLY|O_DIRECTORY);
    if (dir < 0 || rename("b/move", "a/victim")) fatal("replace directory");
    check(fstat(dir, &old_stat) == 0 && old_stat.st_nlink == 0, "open replaced directory remains valid with zero links");
    close(dir);
    if (rmdir("a/victim") || rmdir("a") || rmdir("b")) fatal("directory move cleanup");

    char long_target[96]; memset(long_target, 'x', sizeof(long_target)-1); long_target[95] = 0;
    check(symlink("absent", "short-link") == 0 && unlink("short-link") == 0,
          "inline symlink removal never treats its bytes as block pointers");
    check(symlink(long_target, "long-link") == 0 && unlink("long-link") == 0,
          "block-backed symlink removal releases its storage");

    int retained = create("forked", "fork-retained"), rendezvous[2];
    if (pipe(rendezvous)) fatal("fork pipe");
    pid_t child = fork();
    if (child < 0) fatal("fork");
    if (!child) {
        close(rendezvous[1]); char byte;
        if (read(rendezvous[0], &byte, 1) != 1) _exit(3);
        _exit(contents(retained, "fork-retained") ? 0 : 4);
    }
    close(rendezvous[0]);
    if (unlink("forked")) fatal("fork unlink");
    close(retained);
    if (write(rendezvous[1], "S", 1) != 1) fatal("fork signal");
    close(rendezvous[1]); int status;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "forked descriptor preserves orphan after parent closes");

    int large = create("large", "");
    if (unlink("large")) fatal("large unlink");
    char chunk[4096]; memset(chunk, 0x5a, sizeof(chunk));
    for (int i = 0; i < 2048; ++i)
        if (write(large, chunk, sizeof(chunk)) != sizeof(chunk)) fatal("large orphan write");
    check(fstat(large, &old_stat) == 0 && old_stat.st_size == 8*1024*1024 && old_stat.st_nlink == 0,
          "unlinked file can grow across direct and indirect blocks");
    close(large); sync();
    if (chdir(original) || rmdir(fixture)) fatal("fixture cleanup");
    sync();
    printf("FILELIFECHECK %s failures=%d; run offline fsck after synced shutdown\n",
           failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
