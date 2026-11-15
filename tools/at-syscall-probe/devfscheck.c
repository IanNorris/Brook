// devfscheck: /dev device inodes and /proc/self/fd/N/<subpath> traversal,
// on the host namespace and inside a bubblewrap-style container.
//
// Build: $MUSL_CC -static -O2 -Wall -Wextra -Werror devfscheck.c -o devfscheck
// Run as an unprivileged user (the container uses a user namespace):
//   devfscheck            Linux or Brook
//   devfscheck --brook    also check Brook's reported /dev filesystem type
//   bwrap ... -- devfscheck --in-container DIR   device and proc-fd checks only
// Prints DEVFS PASS checks=N on success; the first failure exits 1.
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
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned checks;
static const char* phase = "host";
#define CK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "DEVFS FAIL phase=%s line=%d %s errno=%d\n", phase, __LINE__, #x, errno); \
    _exit(1); } } while (0)

static void map(const char* path, unsigned id)
{
    char text[64];
    int n = snprintf(text, sizeof(text), "0 %u 1\n", id);
    int f = open(path, O_WRONLY);
    CK(f >= 0);
    CK(write(f, text, n) == n);
    CK(close(f) == 0);
}

static void waitok(pid_t child)
{
    int status;
    CK(waitpid(child, &status, 0) == child);
    CK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void expectchr(const char* path, unsigned major, unsigned minor)
{
    struct stat st;
    CK(stat(path, &st) == 0);
    CK(S_ISCHR(st.st_mode));
    CK(major(st.st_rdev) == major && minor(st.st_rdev) == minor);
}

// The standard character devices, through whatever /dev is visible now.
static void devices(void)
{
    expectchr("/dev/null", 1, 3);
    expectchr("/dev/zero", 1, 5);
    expectchr("/dev/full", 1, 7);
    expectchr("/dev/random", 1, 8);
    expectchr("/dev/urandom", 1, 9);
    expectchr("/dev/tty", 5, 0);
    CK(access("/dev/null", R_OK | W_OK) == 0);
    CK(access("/dev/urandom", R_OK) == 0);
    CK(syscall(439 /* faccessat2 */, AT_FDCWD, "/dev/null", R_OK | W_OK, AT_EACCESS) == 0);
    CK(faccessat(AT_FDCWD, "/dev/urandom", R_OK, 0) == 0);

    char buffer[64];
    int f = open("/dev/null", O_RDWR | O_CLOEXEC);
    CK(f >= 0);
    CK(read(f, buffer, sizeof(buffer)) == 0);
    CK(write(f, "x", 1) == 1);
    struct stat st;
    CK(fstat(f, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == makedev(1, 3));
    CK(close(f) == 0);
    // Shell "> /dev/null": O_TRUNC|O_CREAT on a device is not a truncation.
    f = open("/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CK(f >= 0);
    CK(close(f) == 0);
    errno = 0;
    CK(open("/dev/null", O_RDONLY | O_DIRECTORY) == -1 && errno == ENOTDIR);
    f = open("/dev/null", O_PATH);
    CK(f >= 0);
    CK(fstat(f, &st) == 0 && S_ISCHR(st.st_mode));
    CK(close(f) == 0);

    f = open("/dev/zero", O_RDWR);
    CK(f >= 0);
    memset(buffer, 0x55, sizeof(buffer));
    CK(read(f, buffer, sizeof(buffer)) == (ssize_t)sizeof(buffer));
    for (unsigned i = 0; i < sizeof(buffer); ++i) CK(buffer[i] == 0);
    CK(write(f, buffer, 8) == 8);
    CK(close(f) == 0);
    f = open("/dev/full", O_RDWR);
    CK(f >= 0);
    errno = 0;
    CK(write(f, "x", 1) == -1 && errno == ENOSPC);
    buffer[0] = 1;
    CK(read(f, buffer, 1) == 1 && buffer[0] == 0);
    CK(close(f) == 0);
    f = open("/dev/urandom", O_RDONLY);
    CK(f >= 0);
    CK(read(f, buffer, 16) == 16);
    CK(close(f) == 0);
    // Without a controlling terminal Linux reports ENXIO; the name must exist.
    errno = 0;
    f = open("/dev/tty", O_RDWR | O_NOCTTY);
    CK(f >= 0 || errno == ENXIO);
    if (f >= 0) CK(close(f) == 0);
}

// /proc/self/fd/N is a magic link to the open directory itself: a suffix
// walks into it, for O_PATH and ordinary directory descriptors alike.
static void procfd(const char* dir)
{
    char sub[256], file[300], path[400];
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    snprintf(file, sizeof(file), "%s/sub/file", dir);
    if (mkdir(sub, 0755) != 0) CK(errno == EEXIST);
    int f = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    CK(f >= 0);
    CK(write(f, "PROCFD", 6) == 6);
    CK(close(f) == 0);
    const int flags[2] = {O_PATH | O_DIRECTORY | O_CLOEXEC, O_RDONLY | O_DIRECTORY | O_CLOEXEC};
    for (int i = 0; i < 2; ++i) {
        int d = open(dir, flags[i]);
        CK(d >= 0);
        snprintf(path, sizeof(path), "/proc/self/fd/%d/sub/file", d);
        f = open(path, O_RDONLY);
        CK(f >= 0);
        char text[8] = {0};
        CK(read(f, text, sizeof(text)) == 6 && !memcmp(text, "PROCFD", 6));
        CK(close(f) == 0);
        struct stat st, direct;
        CK(stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 6);
        CK(stat(file, &direct) == 0 && direct.st_ino == st.st_ino && direct.st_dev == st.st_dev);
        snprintf(path, sizeof(path), "/proc/self/fd/%d/sub", d);
        CK(syscall(SYS_newfstatat, AT_FDCWD, path, &st, 0) == 0 && S_ISDIR(st.st_mode));
        CK(access(path, X_OK) == 0);
        snprintf(path, sizeof(path), "/proc/self/fd/%d/missing", d);
        errno = 0;
        CK(stat(path, &st) == -1 && errno == ENOENT);
        // mkdir -p's fast path: a missing child under the magic link, then create it.
        CK(mkdir(path, 0755) == 0);
        CK(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
        CK(rmdir(path) == 0);
        snprintf(path, sizeof(path), "/proc/self/fd/%d/sub/../sub/file", d);
        CK(stat(path, &st) == 0 && st.st_ino == direct.st_ino);
        snprintf(path, sizeof(path), "/proc/self/fd/%d", d);
        CK(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
        // openat relative to a descriptor that itself names the magic link.
        int viaProc = open(path, O_PATH | O_DIRECTORY);
        CK(viaProc >= 0);
        f = openat(viaProc, "sub/file", O_RDONLY);
        CK(f >= 0);
        CK(close(f) == 0);
        CK(close(viaProc) == 0);
        CK(close(d) == 0);
    }
}

// bubblewrap: private mount namespace, tmpfs root, --dev-bind /dev /dev,
// --bind /proc /proc, pivot_root; then --dev's per-node binds onto a tmpfs.
static void container(const char* base, uid_t uid, gid_t gid)
{
    phase = "container";
    map("/proc/self/uid_map", uid);
    int f = open("/proc/self/setgroups", O_WRONLY);
    CK(f >= 0);
    CK(write(f, "deny\n", 5) == 5);
    CK(close(f) == 0);
    map("/proc/self/gid_map", gid);
    CK(mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) == 0);
    char root[200], dir[240];
    snprintf(root, sizeof(root), "%s/root", base);
    CK(mount("tmpfs", root, "tmpfs", MS_NODEV | MS_NOSUID, "mode=0755") == 0);
    const char* binds[] = {"dev", "proc", "tmp"};
    for (unsigned i = 0; i < 3; ++i) {
        char source[16];
        snprintf(source, sizeof(source), "/%s", binds[i]);
        snprintf(dir, sizeof(dir), "%s/%s", root, binds[i]);
        CK(mkdir(dir, 0755) == 0);
        CK(mount(source, dir, NULL, MS_BIND | MS_REC, NULL) == 0);
    }
    snprintf(dir, sizeof(dir), "%s/old", root);
    CK(mkdir(dir, 0755) == 0);
    CK(chdir(root) == 0);
    CK(syscall(SYS_pivot_root, ".", "old") == 0);
    CK(chdir("/") == 0);
    CK(umount2("/old", MNT_DETACH) == 0);
    devices();
    char work[256];
    snprintf(work, sizeof(work), "/tmp/%s", strrchr(base, '/') + 1);
    procfd(work);

    // --dev: a fresh NODEV tmpfs with each canonical node bound onto a file.
    phase = "container-dev";
    CK(mkdir("/newdev", 0755) == 0);
    CK(mount("tmpfs", "/newdev", "tmpfs", MS_NODEV | MS_NOSUID, "mode=0755") == 0);
    const char* nodes[] = {"null", "zero", "full", "random", "urandom", "tty"};
    for (unsigned i = 0; i < 6; ++i) {
        char source[32], target[64];
        snprintf(source, sizeof(source), "/dev/%s", nodes[i]);
        snprintf(target, sizeof(target), "/newdev/%s", nodes[i]);
        f = open(target, O_CREAT | O_EXCL | O_WRONLY, 0666);
        CK(f >= 0);
        CK(close(f) == 0);
        CK(mount(source, target, NULL, MS_BIND | MS_REC, NULL) == 0);
    }
    struct stat st;
    CK(stat("/newdev/null", &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == makedev(1, 3));
    f = open("/newdev/zero", O_RDONLY);
    CK(f >= 0);
    char c = 1;
    CK(read(f, &c, 1) == 1 && c == 0);
    CK(close(f) == 0);
    f = open("/newdev/null", O_RDWR);
    CK(f >= 0);
    CK(write(f, "x", 1) == 1 && read(f, &c, 1) == 0);
    CK(close(f) == 0);
    // A node created on that tmpfs itself would be refused: NODEV, and
    // unprivileged mknod of a device is not permitted.
    errno = 0;
    CK(mknod("/newdev/mine", S_IFCHR | 0666, makedev(1, 3)) == -1);
    // Plain --bind of /dev remounts NODEV: existing descriptors keep working.
    phase = "container-nodev";
    int held = open("/dev/null", O_WRONLY);
    CK(held >= 0);
    CK(mkdir("/nodev", 0755) == 0);
    CK(mount("/dev", "/nodev", NULL, MS_BIND | MS_REC, NULL) == 0);
    struct statvfs fs;
    CK(statvfs("/nodev", &fs) == 0);
    unsigned long locked = fs.f_flag & (ST_RDONLY | ST_NOSUID | ST_NODEV | ST_NOEXEC);
    CK(mount(NULL, "/nodev", NULL, MS_BIND | MS_REMOUNT | locked | MS_NOSUID | MS_NODEV, NULL) == 0);
    errno = 0;
    CK(open("/nodev/null", O_RDWR) == -1 && errno == EACCES);
    CK(stat("/nodev/null", &st) == 0 && S_ISCHR(st.st_mode));
    CK(write(held, "x", 1) == 1);
    CK(close(held) == 0);
    _exit(0);
}

int main(int argc, char** argv)
{
    setbuf(stdout, NULL);
    if (argc == 3 && !strcmp(argv[1], "--in-container")) {
        // Already inside a real bubblewrap container; argv[2] is a writable
        // directory visible there.
        phase = "bwrap";
        alarm(60);
        devices();
        procfd(argv[2]);
        printf("DEVFS IN-CONTAINER PASS checks=%u\n", checks);
        return 0;
    }
    int brook = argc == 2 && !strcmp(argv[1], "--brook");
    CK(argc == 1 || brook);
    CK(geteuid() != 0);
    alarm(60);
    devices();
    char target[64] = {0};
    CK(readlink("/dev/fd", target, sizeof(target) - 1) > 0 && !strcmp(target, "/proc/self/fd"));
    struct stat st;
    CK(lstat("/dev/stdin", &st) == 0 && S_ISLNK(st.st_mode));
    CK(stat("/dev/shm", &st) == 0 && S_ISDIR(st.st_mode));
    if (brook) {
        // Brook mounts /dev as a tmpfs of device inodes; Linux uses devtmpfs.
        int f = open("/proc/mounts", O_RDONLY);
        CK(f >= 0);
        static char mounts[16384];
        ssize_t n = read(f, mounts, sizeof(mounts) - 1);
        CK(n > 0);
        mounts[n] = 0;
        // The initrd's FAT directory reports its on-disk name, /DEV.
        CK(strstr(mounts, " /dev tmpfs ") != NULL || strstr(mounts, " /DEV tmpfs ") != NULL);
        CK(close(f) == 0);
    }
    char base[] = "/tmp/devfscheck-XXXXXX";
    CK(mkdtemp(base) != NULL);
    procfd(base);
    char root[200];
    snprintf(root, sizeof(root), "%s/root", base);
    CK(mkdir(root, 0700) == 0);
    const uid_t uid = geteuid();
    const gid_t gid = getegid();
    pid_t child = syscall(SYS_clone, (unsigned long)(CLONE_NEWUSER | CLONE_NEWNS | SIGCHLD), 0L, 0L, 0L, 0L);
    CK(child >= 0);
    if (!child) container(base, uid, gid);
    waitok(child);
    char path[300];
    snprintf(path, sizeof(path), "%s/sub/file", base);
    CK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/sub", base);
    CK(rmdir(path) == 0);
    CK(rmdir(root) == 0 && rmdir(base) == 0);
    printf("DEVFS PASS checks=%u brook=%d\n", checks, brook);
    return 0;
}
