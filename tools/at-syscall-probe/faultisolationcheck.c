#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return t.tv_sec + t.tv_nsec / 1e9;
}
int main(int argc, char **argv) {
    int serial = argc == 1;
    if (!serial && (argc != 2 || strcmp(argv[1], "--no-serial"))) return 2;
    struct rlimit limit = {0, 0};
    (void)setrlimit(RLIMIT_CORE, &limit);
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) {
        __asm__ volatile("movq 0, %%rax" ::: "rax", "memory");
        _exit(99);
    }
    int status = 0;
    double deadline = now() + 15;
    for (;;) {
        pid_t got = waitpid(child, &status, WNOHANG);
        if (got == child) break;
        if (got < 0 && errno != EINTR) return 2;
        if (now() > deadline) {
            kill(child, SIGKILL);
            puts("FAULTISOLATIONCHECK FAIL child reap timeout");
            return 1;
        }
        usleep(10000);
    }
    int ok = WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
    printf("FAULTISOLATIONCHECK %s child status=0x%x SIGSEGV=%d\n",
           ok ? "PASS" : "FAIL", status, ok);
    const char marker[] = "FAULTISOLATIONCHECK SERIAL AFTER USER FAULT\n";
    if (serial && write(3, marker, sizeof(marker)-1) != sizeof(marker)-1) ok = 0;
    printf("FAULTISOLATIONCHECK %s\n", ok ? "PASS" : "FAIL");
    return !ok;
}
