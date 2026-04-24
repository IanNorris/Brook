// crashdump_check.c — Drive __brook_crash_entry with a synthetic CrashCtx,
// then read back the dump and verify key fields.  Independent of the
// fault-path wiring (commit d), so this verifies (c) in isolation.
//
// The writer's sys_crash_complete call would terminate this process, so
// we run the writer in a fresh thread via syscall 501 (CreateRemoteThread
// debug) instead of calling it directly.  That way the parent survives
// to open the dump file and assert.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <time.h>

#include "../shared/inc_um/crash_dump.h"

static long syscall4(long n, long a, long b, long c, long d) {
    register long r10 __asm__("r10") = d;
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static int run_child_injection(void) {
    BrookCrashCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.magic = BROOK_CRASH_CTX_MAGIC;
    ctx.version = BROOK_CRASH_CTX_VER;
    ctx.signum = 11;         // SIGSEGV
    ctx.vector = 14;         // #PF
    ctx.errorCode = 0x6;
    ctx.faultAddr = 0x0;
    ctx.rip = 0xdeadbeef1000ULL;
    ctx.rsp = 0xcafebabe0000ULL;
    ctx.rbp = 0;
    ctx.rax = 0x1111111111111111ULL;
    ctx.rbx = 0x2222222222222222ULL;
    ctx.tid = (uint64_t)getpid();
    const char* nm = "crashdump_check";
    for (int i = 0; i < (int)sizeof(ctx.commName) - 1 && nm[i]; ++i)
        ctx.commName[i] = nm[i];

    long tid = syscall4(501, 0,
                        (long)(void*)__brook_crash_entry,
                        (long)&ctx,
                        (long)sizeof(ctx));
    if (tid < 0) {
        printf("child: CreateRemoteThread failed (%ld)\n", tid);
        _exit(2);
    }
    // Writer will call sys_crash_complete which terminates the tgid with
    // exit code 128+11 = 139.  Wait here to be killed.
    for (int i = 0; i < 300; ++i) {
        struct timespec ts = { 0, 10 * 1000 * 1000L };
        nanosleep(&ts, NULL);
    }
    _exit(3); // shouldn't reach — writer should have killed us
}

int main(void) {
    pid_t pid = fork();
    if (pid < 0) {
        printf("crashdump_check: FAIL — fork: %m\n");
        return 1;
    }
    if (pid == 0) {
        run_child_injection();
        _exit(99);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    // Give ext2 a moment to settle — the writer thread is in a doomed tgid
    // being torn down; its close(fd) should be synchronous but let the
    // scheduler fully reap the thread before we poke at the filesystem.
    struct timespec settle = { 0, 100 * 1000 * 1000L };
    nanosleep(&settle, NULL);

    int exited = WIFEXITED(status);
    int code = exited ? WEXITSTATUS(status) : -1;
    int termsig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    printf("crashdump_check: child exited=%d code=%d signal=%d\n",
           exited, code, termsig);

    // The child should have been killed by sys_crash_complete with
    // exit code 128+11 = 139.  Brook's wait4 decodes that as a signal
    // death (signum=11), which matches the POSIX shell convention.
    if (!(!exited && termsig == 11)) {
        printf("crashdump_check: WARN — unexpected child exit "
               "(exited=%d code=%d signal=%d; expected signal=11)\n",
               exited, code, termsig);
    }

    // Look for a dump file that mentions crashdump_check.
    DIR* d = opendir("/data/crashes");
    if (!d) {
        printf("crashdump_check: /data/crashes opendir failed: %m\n");
        // Try /data to see what's there
        d = opendir("/data");
        if (d) {
            struct dirent* de;
            printf("crashdump_check: /data contents:\n");
            while ((de = readdir(d)) != NULL) {
                printf("  %s\n", de->d_name);
            }
            closedir(d);
        }
        return 1;
    }
    struct dirent* de;
    int found = 0;
    printf("crashdump_check: /data/crashes contents:\n");
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        printf("  %s\n", de->d_name);
        char path[256];
        snprintf(path, sizeof(path), "/data/crashes/%s", de->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char buf[1024] = { 0 };
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        if (strstr(buf, "Brook User Crash Dump v1") &&
            strstr(buf, "crashdump_check") &&
            strstr(buf, "Signal:     11")) {
            printf("crashdump_check: found dump at %s (%zd bytes read)\n", path, n);
            found = 1;
            break;
        }
    }
    closedir(d);

    if (!found) {
        printf("crashdump_check: FAIL — no matching dump in /data/crashes\n");
        return 1;
    }
    printf("crashdump_check: PASS\n");
    return 0;
}
