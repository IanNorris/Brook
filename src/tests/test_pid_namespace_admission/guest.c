#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/nsfs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "crash_dump.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "PID_NAMESPACE_FAIL line=%d check=%s errno=%d\n", \
                __LINE__, #condition, errno); \
        _exit(2); \
    } \
} while (0)

static pid_t new_init(void)
{
    long result = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWPID | SIGCHLD,
                          0UL, 0UL, 0UL, 0UL);
    CHECK(result >= 0);
    if (result == 0) {
        CHECK(getpid() == 1);
        CHECK(syscall(SYS_gettid) == 1);
        CHECK(getppid() == 0);
    }
    return (pid_t)result;
}

static void wait_exit(pid_t child, int code)
{
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == code);
}

static void reparent_orphan(void)
{
    pid_t init = new_init();
    if (init == 0) {
        int start[2];
        CHECK(pipe(start) == 0);
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            CHECK(getpid() == 2);
            CHECK(getppid() == 1);
            close(start[1]);
            pid_t orphan = fork();
            CHECK(orphan >= 0);
            if (orphan == 0) {
                char mark;
                CHECK(read(start[0], &mark, 1) == 1 && mark == 'x');
                CHECK(getpid() == 3);
                CHECK(getppid() == 1);
                _exit(27);
            }
            CHECK(orphan == 3);
            _exit(0);
        }
        close(start[0]);
        CHECK(child == 2);
        wait_exit(child, 0);
        CHECK(write(start[1], "x", 1) == 1);
        close(start[1]);
        wait_exit(3, 27);
        _exit(0);
    }
    CHECK(init > 1);
    wait_exit(init, 0);
    fprintf(stderr, "PID_NAMESPACE_ORPHAN_PASS\n");
}

static void drain_namespace(void)
{
    int barrier[2];
    CHECK(pipe(barrier) == 0);
    pid_t init = new_init();
    if (init == 0) {
        close(barrier[0]);
        int ready[2];
        CHECK(pipe(ready) == 0);
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            close(ready[0]);
            CHECK(write(barrier[1], "x", 1) == 1);
            CHECK(write(ready[1], "r", 1) == 1);
            close(ready[1]);
            for (;;) pause();
        }
        close(ready[1]);
        char mark;
        CHECK(child == 2);
        CHECK(read(ready[0], &mark, 1) == 1 && mark == 'r');
        _exit(23);
    }
    close(barrier[1]);
    wait_exit(init, 23);
    char mark;
    CHECK(read(barrier[0], &mark, 1) == 1 && mark == 'x');
    struct pollfd drained = { .fd = barrier[0], .events = POLLIN };
    CHECK(poll(&drained, 1, 0) == 1 && (drained.revents & POLLHUP));
    CHECK(read(barrier[0], &mark, 1) == 0);
    close(barrier[0]);
    fprintf(stderr, "PID_NAMESPACE_DRAIN_PASS\n");
}

static void init_job_control(void)
{
    int ready[2], go[2];
    CHECK(pipe(ready) == 0 && pipe(go) == 0);
    pid_t init = new_init();
    if (!init) {
        close(ready[0]);
        close(go[1]);
        CHECK(setsid() == 1 && write(ready[1], "r", 1) == 1);
        char mark;
        CHECK(read(go[0], &mark, 1) == 1 && mark == 'x');
        _exit(0);
    }
    close(ready[1]);
    close(go[0]);
    char mark;
    CHECK(read(ready[0], &mark, 1) == 1 && mark == 'r');
    CHECK(kill(-init, SIGINT) == 0 && kill(-init, SIGTSTP) == 0);
    CHECK(write(go[1], "x", 1) == 1);
    int status;
    CHECK(waitpid(init, &status, WUNTRACED) == init &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(ready[0]);
    close(go[1]);
    fprintf(stderr, "PID_NAMESPACE_INIT_SIGNALS_PASS\n");
}

static void crash_writer(const BrookCrashCtx *context)
{
    const int valid = context && context->magic == BROOK_CRASH_CTX_MAGIC &&
        context->version == BROOK_CRASH_CTX_VER && context->tid == 1 &&
        getpid() == 1 && syscall(SYS_gettid) == 2;
    if (valid) {
        const char message[] = "PID_NAMESPACE_CRASH_WRITER_PASS\n";
        if (write(STDERR_FILENO, message, sizeof(message) - 1) != sizeof(message) - 1)
            _exit(3);
    }
    (void)syscall(503, valid ? 77 : 3);
    _exit(3);
}

static void crash_writer_after_unshare(void)
{
    pid_t init = new_init();
    if (!init) {
        CHECK(unshare(CLONE_NEWPID) == 0);
        CHECK(syscall(502, (uintptr_t)crash_writer) == 0);
        // Exercise the registered user exception writer in this guest.
        __builtin_trap();
    }
    wait_exit(init, 77);
}

static void check_bwrap_proc(void)
{
    FILE *proc = fopen("/proc/self/stat", "r");
    int self = 0;
    CHECK(proc && fscanf(proc, "%d", &self) == 1 && self == getpid());
    fclose(proc);
    proc = fopen("/proc/1/stat", "r");
    int init = 0;
    CHECK(proc && fscanf(proc, "%d", &init) == 1 && init == 1);
    fclose(proc);
    CHECK(getpid() == 1 || getppid() == 1);
    fprintf(stderr, "PID_NAMESPACE_BWRAP_PROC_PASS pid=%ld ppid=%ld\n",
            (long)getpid(), (long)getppid());
}

static ino_t namespace_inode(const char *name, int type)
{
    char path[64], link[80], expected[80];
    CHECK(snprintf(path, sizeof(path), "/proc/self/ns/%s", name) > 0);
    struct stat metadata;
    CHECK(stat(path, &metadata) == 0);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0 && ioctl(fd, NS_GET_NSTYPE) == type);
    CHECK(close(fd) == 0);
    ssize_t n = readlink(path, link, sizeof(link));
    CHECK(n > 0 && n < (ssize_t)sizeof(link));
    link[n] = 0;
    CHECK(snprintf(expected, sizeof(expected), "%s:[%llu]", name,
                   (unsigned long long)metadata.st_ino) == n);
    CHECK(strcmp(link, expected) == 0);
    return metadata.st_ino;
}

static void namespace_descriptors(void)
{
    const ino_t user = namespace_inode("user", CLONE_NEWUSER);
    const ino_t pid = namespace_inode("pid", CLONE_NEWPID);
    const ino_t net = namespace_inode("net", CLONE_NEWNET);
    CHECK(user != pid && pid != net && user != net);
    const long child = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | SIGCHLD,
                               0UL, 0UL, 0UL, 0UL);
    CHECK(child >= 0);
    if (!child) {
        CHECK(getpid() == 1);
        CHECK(namespace_inode("user", CLONE_NEWUSER) != user);
        CHECK(namespace_inode("pid", CLONE_NEWPID) != pid);
        CHECK(namespace_inode("net", CLONE_NEWNET) != net);
        _exit(0);
    }
    wait_exit((pid_t)child, 0);
    fprintf(stderr, "PROC_NAMESPACE_DESCRIPTORS_PASS\n");
}

static int seal_shared_root(void *unused)
{
    (void)unused;
    CHECK(chroot("/proc/self/fdinfo") == 0);
    CHECK(chdir("/") == 0);
    return 0;
}

static void retained_fdinfo_chroot(void)
{
    CHECK(chroot("/proc/self/fdinfo") == -1 && errno == EPERM);
    pid_t init = new_init();
    if (!init) {
        char stack[32768];
        const int child = clone(seal_shared_root, stack + sizeof(stack), CLONE_FS | SIGCHLD, NULL);
        CHECK(child > 1);
        wait_exit(child, 0);
        char cwd[64];
        struct stat root, parent;
        CHECK(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/") == 0);
        CHECK(stat("/", &root) == 0 && S_ISDIR(root.st_mode));
        CHECK(stat("/..", &parent) == 0 && root.st_ino == parent.st_ino);
        CHECK(open("/dev/null", O_RDONLY) == -1 && errno == ENOENT);
        CHECK(mkdir("/new", 0700) == -1);
        CHECK(unshare(CLONE_NEWUSER) == -1 && errno == EPERM);
        CHECK(syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, 0UL, 0UL, 0UL, 0UL) == -1 && errno == EPERM);
        _exit(0);
    }
    wait_exit(init, 0);
    fprintf(stderr, "PROC_FDINFO_CHROOT_PASS\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--sandbox-primitives") == 0) {
        namespace_descriptors();
        retained_fdinfo_chroot();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--inside-bwrap") == 0) {
        check_bwrap_proc();
        return 0;
    }
    CHECK(argc == 1);
    reparent_orphan();
    drain_namespace();
    init_job_control();
    namespace_descriptors();
    retained_fdinfo_chroot();
    crash_writer_after_unshare();
    return 0;
}
