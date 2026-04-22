/*
 * profile — Brook userspace wrapper around the kernel sampling profiler.
 *
 * Usage:
 *   profile start [durationMs]   (default 10000ms; 0 = indefinite)
 *   profile stop
 *   profile status
 *
 * The kernel exposes profiler control as syscall 500 (Brook-specific range):
 *   rax=500, rdi=op, rsi=arg
 *       op=0  start(durationMs)
 *       op=1  stop
 *       op=2  isRunning
 *
 * Sample output streams to the serial port. On the host, capture the serial
 * log then run scripts/profiler_to_speedscope.py to produce a Speedscope JSON.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SYS_BROOK_PROFILE 500

static long brook_profile(long op, long arg) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "0"(SYS_BROOK_PROFILE), "D"(op), "S"(arg)
        : "rcx", "r11", "memory");
    return ret;
}

static void usage(void) {
    fprintf(stderr,
        "usage: profile start [durationMs]  (default 10000, 0 = indefinite)\n"
        "       profile stop\n"
        "       profile status\n");
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }

    if (!strcmp(argv[1], "start")) {
        long ms = 10000;
        if (argc >= 3) ms = strtol(argv[2], NULL, 10);
        long r = brook_profile(0, ms);
        if (r < 0) { fprintf(stderr, "profile start failed: %ld\n", r); return 1; }
        if (ms == 0) printf("profiler started (indefinite)\n");
        else         printf("profiler started for %ld ms\n", ms);
        return 0;
    }
    if (!strcmp(argv[1], "stop")) {
        long r = brook_profile(1, 0);
        if (r < 0) { fprintf(stderr, "profile stop failed: %ld\n", r); return 1; }
        printf("profiler stopped\n");
        return 0;
    }
    if (!strcmp(argv[1], "status")) {
        long r = brook_profile(2, 0);
        if (r < 0) { fprintf(stderr, "profile status failed: %ld\n", r); return 1; }
        printf("profiler %s\n", r ? "running" : "stopped");
        return 0;
    }
    usage();
    return 2;
}
