// remote_thread_test.c — exercise the CreateRemoteThread kernel primitive.
//
// Invokes the TEMPORARY debug syscall 501 to inject a new thread into the
// current process.  The injected thread runs at __remote_entry with RDI
// pointing at our arg buffer, prints a message via raw write(2), and
// calls sys_exit to terminate just itself.  Main waits for a visible
// success marker via a shared flag, then exits.
//
// This test app will be removed once the user-mode crash-dump path is in
// place and the debug syscall goes away.

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/syscall.h>

#define SYS_BROOK_REMOTE_THREAD_DEBUG  501

struct arg_t {
    volatile int*  done_flag;
    volatile int   tid_self;   // set by thread
    const char*    msg;
};

static inline long raw_syscall3(long n, long a, long b, long c)
{
    long ret;
    __asm__ volatile(
        "mov %1, %%rax\n\t"
        "mov %2, %%rdi\n\t"
        "mov %3, %%rsi\n\t"
        "mov %4, %%rdx\n\t"
        "syscall\n\t"
        "mov %%rax, %0\n\t"
        : "=r"(ret)
        : "r"(n), "r"(a), "r"(b), "r"(c)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

static inline long raw_syscall1(long n, long a)
{
    long ret;
    __asm__ volatile(
        "mov %1, %%rax\n\t"
        "mov %2, %%rdi\n\t"
        "syscall\n\t"
        "mov %%rax, %0\n\t"
        : "=r"(ret)
        : "r"(n), "r"(a)
        : "rax", "rdi", "rcx", "r11", "memory"
    );
    return ret;
}

// Injected thread entry. Must use only raw syscalls — the kernel hands us a
// fresh stack, no TLS, no per-thread runtime setup from musl.
__attribute__((used, noinline))
static void __remote_entry(struct arg_t* arg)
{
    // Length of message
    const char* msg = arg->msg;
    long len = 0;
    while (msg[len]) ++len;

    raw_syscall3(SYS_write, 1, (long)msg, len);

    // Signal completion to main
    *arg->done_flag = 1;

    // Exit just this thread (SYS_exit = 60 on Linux x86_64)
    raw_syscall1(SYS_exit, 0);

    for (;;) { /* unreachable */ }
}

int main(void)
{
    printf("remote_thread_test: pid=%d\n", getpid());

    static volatile int done = 0;
    static struct arg_t arg;
    arg.done_flag = &done;
    arg.tid_self  = 0;
    arg.msg       = "hello from injected remote thread!\n";

    long r = syscall(SYS_BROOK_REMOTE_THREAD_DEBUG,
                     (long)0,                  // 0 = self
                     (long)__remote_entry,
                     (long)&arg,
                     (long)sizeof(arg));

    if (r < 0) {
        printf("remote_thread_test: FAIL: injection syscall returned %ld\n", r);
        return 1;
    }
    printf("remote_thread_test: injected tid=%ld\n", r);

    // Wait for the thread to set the flag (bounded so we don't hang CI).
    for (int i = 0; i < 500 && !done; ++i) {
        struct timespec ts = { 0, 10000000 }; // 10ms
        nanosleep(&ts, NULL);
    }

    if (done) {
        printf("remote_thread_test: PASS — remote thread observed to run\n");
        return 0;
    } else {
        printf("remote_thread_test: FAIL — remote thread never ran\n");
        return 1;
    }
}
