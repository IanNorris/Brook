// syscheck.c — Comprehensive syscall compatibility check for Brook OS
// Tests a wide range of Linux syscalls and reports pass/fail.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <grp.h>
#include <sys/syscall.h>

struct brook_linux_dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
};

static int g_pass = 0, g_fail = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("  PASS  %s\n", name); g_pass++; } \
    else      { printf("  FAIL  %s\n", name); g_fail++; } \
} while(0)

int main(void)
{
    printf("=== SYSCHECK: Brook OS Syscall Compatibility ===\n\n");

    // --- Process identity ---
    printf("[Process Identity]\n");
    CHECK("getpid",  getpid() > 0);
    CHECK("getppid", getppid() >= 0);
    CHECK("getuid",  getuid() == 0);
    CHECK("getgid",  getgid() == 0);
    CHECK("geteuid", geteuid() == 0);
    CHECK("getegid", getegid() == 0);
    printf("\n");

    // --- File I/O ---
    printf("[File I/O]\n");
    CHECK("write(stdout)", write(1, "", 0) >= 0);
    {
        int fd = open("/BROOK.CFG", O_RDONLY);
        CHECK("open(r)", fd >= 0);
        if (fd >= 0)
        {
            char buf[64];
            int n = read(fd, buf, sizeof(buf) - 1);
            CHECK("read", n > 0);
            if (n > 0) buf[n] = '\0';
            CHECK("close", close(fd) == 0);
        }
    }
    {
        struct stat st;
        CHECK("stat", stat("/BROOK.CFG", &st) == 0);
        CHECK("stat.size>0", st.st_size > 0);
    }
    printf("\n");

    // --- Working directory ---
    printf("[Working Directory]\n");
    {
        char cwd[256];
        CHECK("getcwd", getcwd(cwd, sizeof(cwd)) != NULL);
        CHECK("cwd='/boot'", strcmp(cwd, "/boot") == 0);
        CHECK("raw getcwd len", syscall(SYS_getcwd, cwd, sizeof(cwd)) == (long)strlen(cwd) + 1);
        CHECK("chdir", chdir("/") == 0);
    }
    printf("\n");

    // --- Memory ---
    printf("[Memory]\n");
    {
        void* p = malloc(4096);
        CHECK("malloc(4K)", p != NULL);
        if (p) { memset(p, 0xAB, 4096); CHECK("memset", 1); free(p); }

        void* big = malloc(1024 * 1024);
        CHECK("malloc(1M)", big != NULL);
        if (big) free(big);
    }
    printf("\n");

    // --- Time ---
    printf("[Time]\n");
    {
        struct timespec ts;
        CHECK("clock_gettime", clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
        CHECK("time>0", ts.tv_sec > 0 || ts.tv_nsec > 0);
    }
    printf("\n");

    // --- Pipe ---
    printf("[Pipe]\n");
    {
        int pfd[2];
        CHECK("pipe", pipe(pfd) == 0);
        if (pfd[0] >= 0)
        {
            const char* msg = "hello pipe";
            write(pfd[1], msg, 10);
            char buf[16] = {};
            int n = read(pfd[0], buf, sizeof(buf));
            CHECK("pipe r/w", n == 10 && memcmp(buf, msg, 10) == 0);
            close(pfd[0]);
            close(pfd[1]);
        }
    }
    printf("\n");

    // --- Fork + Exec + Wait ---
    printf("[Fork/Exec/Wait]\n");
    {
        pid_t child = fork();
        if (child < 0)
        {
            CHECK("fork", 0);
        }
        else if (child == 0)
        {
            _exit(42);
        }
        else
        {
            CHECK("fork", child > 0);
            int status = 0;
            pid_t w = waitpid(child, &status, 0);
            CHECK("waitpid", w == child);
            CHECK("exit(42)", WIFEXITED(status) && WEXITSTATUS(status) == 42);
        }
    }
    printf("\n");

    // --- Dup2 ---
    printf("[Dup2]\n");
    {
        int pfd[2];
        pipe(pfd);
        int old_stdout = dup(1);
        CHECK("dup", old_stdout >= 0);
        int d2 = dup2(pfd[1], 1);
        // Don't printf here — stdout is now the pipe!
        write(1, "redirected", 10);
        dup2(old_stdout, 1);  // restore stdout
        CHECK("dup2", d2 == 1);
        close(old_stdout);
        close(pfd[1]);
        char buf[16] = {};
        int n = read(pfd[0], buf, sizeof(buf));
        CHECK("dup2 data", n == 10 && memcmp(buf, "redirected", 10) == 0);
        close(pfd[0]);
    }
    printf("\n");

    // --- Uname ---
    printf("[Uname]\n");
    {
        struct utsname uts;
        CHECK("uname", uname(&uts) == 0);
        CHECK("sysname='Brook'", strcmp(uts.sysname, "Brook") == 0);
        printf("  info: %s %s %s\n", uts.sysname, uts.release, uts.machine);
    }
    printf("\n");

    // --- Groups ---
    printf("[Groups]\n");
    {
        gid_t groups[16];
        int ng = getgroups(16, groups);
        CHECK("getgroups", ng >= 0);
        if (ng > 0) CHECK("group[0]=0", groups[0] == 0);
    }
    printf("\n");

    // --- Fcntl ---
    printf("[Fcntl]\n");
    {
        int pfd[2];
        pipe(pfd);
        int fl = fcntl(pfd[0], F_GETFL);
        CHECK("F_GETFL", fl >= 0);
        int fd_flags = fcntl(pfd[0], F_GETFD);
        CHECK("F_GETFD", fd_flags >= 0);
        int r = fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
        CHECK("F_SETFD", r == 0);
        fd_flags = fcntl(pfd[0], F_GETFD);
        CHECK("FD_CLOEXEC set", fd_flags & FD_CLOEXEC);
        close(pfd[0]);
        close(pfd[1]);
    }
    printf("\n");

    // --- Synthetic files ---
    printf("[Synthetic Files]\n");
    {
        int fd = open("/etc/passwd", O_RDONLY);
        CHECK("/etc/passwd", fd >= 0);
        if (fd >= 0)
        {
            char buf[128] = {};
            int n = read(fd, buf, sizeof(buf) - 1);
            CHECK("passwd read", n > 0);
            if (n > 0) CHECK("passwd:root", strstr(buf, "root") != NULL);
            close(fd);
        }
    }
    printf("\n");

    // --- Process groups ---
    printf("[Process Groups]\n");
    {
        CHECK("getpgrp", getpgrp() >= 0);
        CHECK("getsid", getsid(0) >= 0);
    }
    printf("\n");

    // --- Directory listing ---
    printf("[Directory]\n");
    {
        struct stat dst;
        CHECK("stat(/boot)", stat("/boot", &dst) == 0);

        int dfd = open("/", O_RDONLY | O_DIRECTORY);
        CHECK("open(/)", dfd >= 0);
        if (dfd >= 0) {
            char dirbuf[2048];
            int n = syscall(SYS_getdents64, dfd, dirbuf, sizeof(dirbuf));
            int saw_boot = 0;
            for (int pos = 0; n > 0 && pos < n; ) {
                struct brook_linux_dirent64* de =
                    (struct brook_linux_dirent64*)(dirbuf + pos);
                if (strcmp(de->d_name, "boot") == 0)
                    saw_boot = 1;
                if (de->d_reclen == 0) break;
                pos += de->d_reclen;
            }
            CHECK("getdents64(/) includes boot", n > 0 && saw_boot);
            close(dfd);
        }
    }
    printf("\n");

    // --- Summary ---
    printf("=== SYSCHECK COMPLETE: %d passed, %d failed ===\n",
           g_pass, g_fail);

    // Small delay to let the async serial writer flush all output
    {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; // 100ms
        nanosleep(&ts, NULL);
    }

    return g_fail > 0 ? 1 : 0;
}
