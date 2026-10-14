#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static void fatal(const char *s) { perror(s); exit(1); }
static void check(int ok, const char *label)
{
    printf("MODECHECK %s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
static void mode_is(const char *path, int fd, mode_t mode, const char *label)
{
    struct stat p, f;
    if (stat(path, &p) || fstat(fd, &f)) fatal("stat/fstat");
    check((p.st_mode & 07777) == mode && p.st_mode == f.st_mode &&
          p.st_ino == f.st_ino && S_ISREG(p.st_mode) && p.st_ctim.tv_sec > 0, label);
}
static void fixture(const char *directory)
{
    char path[512], name[64], linkpath[520];
    snprintf(name, sizeof(name), "modecheck-%ld", (long)getpid());
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path))
        exit(2);
    printf("MODECHECK directory=%s\n", directory);
    unlink(path);
    mode_t oldmask = umask(0022);
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0755);
    if (fd < 0) fatal("open executable");
    mode_is(path, fd, 0755, "O_CREAT preserves requested executable bits");
    int existing = open(path, O_CREAT | O_RDWR, 0600);
    if (existing < 0) fatal("open existing");
    mode_is(path, existing, 0755, "O_CREAT existing inode preserves mode");
    close(existing);
    if (chmod(path, 0640)) fatal("chmod");
    mode_is(path, fd, 0640, "path chmod visible through existing fd");
    if (fchmod(fd, 0511)) fatal("fchmod");
    mode_is(path, fd, 0511, "fd chmod visible through pathname");
    int dir = open(directory, O_RDONLY | O_DIRECTORY);
    if (dir < 0) fatal("open directory");
    if (syscall(SYS_fchmodat, dir, name, 0705)) fatal("fchmodat relative");
    mode_is(path, fd, 0705, "relative dirfd chmod uses correct mount");
    int cwd = open(".", O_RDONLY | O_DIRECTORY);
    if (cwd < 0 || chdir(directory)) fatal("chdir");
    if (chmod(name, 0740)) fatal("relative chmod");
    if (fchdir(cwd)) fatal("restore cwd");
    close(cwd);
    mode_is(path, fd, 0740, "relative chmod resolves cwd");
    snprintf(linkpath, sizeof(linkpath), "%s.link", path);
    unlink(linkpath);
    if (symlink(path, linkpath) || chmod(linkpath, 0555)) fatal("chmod symlink");
    mode_is(path, fd, 0555, "chmod follows symlink and preserves inode type");
    unlink(linkpath);
    errno = 0;
    check(chmod(linkpath, 0600) < 0 && errno == ENOENT, "missing path reports ENOENT");
    errno = 0;
    check(fchmod(-1, 0600) < 0 && errno == EBADF, "invalid fd reports EBADF");
    errno = 0;
    check(syscall(SYS_fchmodat, fd, name, 0600) < 0 && errno == ENOTDIR,
          "relative non-directory fd reports ENOTDIR");
    errno = 0;
    check(syscall(SYS_fchmodat, -1, name, 0600) < 0 && errno == EBADF,
          "relative invalid dirfd reports EBADF");
    if (syscall(SYS_fchmodat, -1, path, 0555)) fatal("absolute fchmodat");
    mode_is(path, fd, 0555, "absolute fchmodat ignores dirfd");
    if (geteuid() == 0) {
        pid_t child = fork();
        if (child < 0) fatal("fork");
        if (!child) {
            if (setgid(12345) || setuid(12345)) _exit(2);
            errno = 0;
            _exit(fchmod(fd, 0777) == -1 && errno == EPERM ? 0 : 3);
        }
        int status;
        if (waitpid(child, &status, 0) != child) fatal("waitpid");
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "non-owner fd chmod rejected with EPERM");
    }
    close(fd);
    unlink(path);
    umask(0027);
    fd = openat(dir, name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd < 0) fatal("openat umask");
    mode_is(path, fd, 0640, "openat creation applies umask");
    close(fd);
    unlink(path);
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0000);
    if (fd < 0) fatal("open zero mode");
    mode_is(path, fd, 0000, "new zero-mode inode can be opened by creator");
    close(fd);
    unlink(path);
    close(dir);
    umask(oldmask);
}
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s absolute-writable-directory [other-mounted-directory ...]\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '/') return 2;
        fixture(argv[i]);
    }
    printf("MODECHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
