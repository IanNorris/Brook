#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Bounded metadata control; run only on disposable images or a private Linux
 * user/mount namespace. All mounts are in a newly unshared namespace. */
#define CK(test) do { if (!(test)) { \
    fprintf(stderr, "MOUNTFANOUT FAIL line=%d errno=%d %s\n", \
            __LINE__, errno, #test); exit(1); } } while (0)
enum { MOUNTS = 128, LOOKUPS = 500, REPEATS = 3, DEPTH = 6 };
static unsigned long long now(void) {
    struct timespec t;
    CK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (unsigned long long)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
static void value(int dir, const char *path, char expected) {
    int fd = openat(dir, path, O_RDONLY | O_CLOEXEC);
    char c = 0;
    CK(fd >= 0);
    CK(read(fd, &c, 1) == 1 && c == expected);
    CK(read(fd, &c, 1) == 0);
    CK(close(fd) == 0);
}
static void put(const char *path, char c) {
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CK(fd >= 0 && write(fd, &c, 1) == 1 && close(fd) == 0);
}
static void sample(const char *phase, int mounts, int dir, const char *absolute) {
    /* Check contents on every iteration, including EOF, to prevent a faster
     * failure or a stale/wrong mount from being counted as an improvement. */
    for (int repeat = 0; repeat < REPEATS; ++repeat) {
        unsigned long long a = now();
        for (int i = 0; i < LOOKUPS; ++i) value(AT_FDCWD, absolute, 'A');
        unsigned long long b = now();
        for (int i = 0; i < LOOKUPS; ++i) value(dir, "value", 'A');
        unsigned long long c = now();
        printf("MOUNTFANOUT_RESULT {\"phase\":\"%s\",\"unrelated_mounts\":%d,"
               "\"repeat\":%d,\"lookups\":%d,\"absolute_ns\":%llu,"
               "\"dirfd_ns\":%llu}\n", phase, mounts, repeat, LOOKUPS, b-a, c-b);
    }
}
int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    CK(argc == 3 && !strcmp(argv[1], "--disposable") && argv[2][0] == '/');
    CK(geteuid() == 0);
    /* Brook implements mount-namespace creation through clone; unshare is not
     * currently part of its syscall surface. The parent waits for ownership. */
    pid_t child = (pid_t)syscall(SYS_clone, (unsigned long)(CLONE_NEWNS | SIGCHLD),
                               0L, 0L, 0L, 0L);
    CK(child >= 0);
    if (child) {
        int status = 0;
        CK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        puts("MOUNTFANOUT PARENT PASS");
        return 0;
    }
    CK(mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) == 0);
    char base[PATH_MAX], absolute[PATH_MAX], target[64];
    CK(snprintf(base, sizeof(base), "%s/mountfanout-XXXXXX", argv[2]) < (int)sizeof(base));
    CK(mkdtemp(base) != NULL && chdir(base) == 0);
    CK(mkdir("source", 0700) == 0 && mkdir("view", 0700) == 0 && mkdir("other", 0700) == 0);
    char deep[128] = "source";
    for (int i = 0; i < DEPTH; ++i) { strcat(deep, "/d"); CK(mkdir(deep, 0700) == 0); }
    char file[160];
    CK(snprintf(file, sizeof(file), "%s/value", deep) < (int)sizeof(file));
    put(file, 'A'); put("other/value", 'B');
    CK(mount(deep, "view", NULL, MS_BIND, NULL) == 0);
    int dir = open("view", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CK(dir >= 0);
    CK(snprintf(absolute, sizeof(absolute), "%s/view/value", base) < (int)sizeof(absolute));
    sample("before", 0, dir, absolute);
    for (int i = 0; i < MOUNTS; ++i) {
        snprintf(target, sizeof(target), "m%03d", i);
        CK(mkdir(target, 0700) == 0 && mount("other", target, NULL, MS_BIND, NULL) == 0);
        char entry[80]; snprintf(entry, sizeof(entry), "%s/value", target);
        value(AT_FDCWD, entry, 'B');
    }
    sample("fanout", MOUNTS, dir, absolute);
    /* Overmount and held-FD checks exercise lookup authority independently of
     * timing; removing the top attachment must reveal the previous one. */
    CK(mount("other", "view", NULL, MS_BIND, NULL) == 0);
    value(AT_FDCWD, absolute, 'B'); value(dir, "value", 'A');
    CK(umount2("view", MNT_DETACH) == 0);
    value(AT_FDCWD, absolute, 'A');
    for (int i = MOUNTS-1; i >= 0; --i) {
        snprintf(target, sizeof(target), "m%03d", i);
        CK(umount2(target, MNT_DETACH) == 0 && rmdir(target) == 0);
    }
    sample("after", 0, dir, absolute);
    CK(close(dir) == 0 && umount2("view", MNT_DETACH) == 0);
    CK(unlink(file) == 0 && unlink("other/value") == 0);
    for (int i = DEPTH; i > 0; --i) {
        CK(rmdir(deep) == 0); *strrchr(deep, '/') = 0;
    }
    CK(rmdir("source") == 0 && rmdir("other") == 0 && rmdir("view") == 0);
    CK(chdir("/") == 0 && rmdir(base) == 0);
    sync();
    puts("MOUNTFANOUT PASS");
    return 0;
}
