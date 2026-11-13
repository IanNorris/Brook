/* Diagnostic only: compare grouped cache publication with its setup cost.
 * The reduced-CPUID case is a timing control, not a safe kernel algorithm:
 * a task may migrate to a CPU with different CLFLUSH properties. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { PAGE = 4096, GROUP = 16, PAGES = 1024, REPEATS = 32 };

static uint32_t flush_line(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    uint32_t line = ((b >> 8) & 255u) * 8u;
    return (d & (1u << 19)) && line >= 16 && line <= PAGE &&
           !(line & (line - 1)) && !(PAGE % line) ? line : 0;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t run(uint8_t *buf, uint32_t line, int query, int flush) {
    uint64_t start = now_ns();
    for (unsigned rep = 0; rep < REPEATS; ++rep) {
        for (unsigned p = 0; p < PAGES; p += GROUP) {
            uint32_t current = query ? flush_line() : line;
            if (!current || current != line) { fputs("CLFLUSH properties changed\n", stderr); exit(1); }
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (flush)
                for (unsigned i = 0; i < GROUP * PAGE; i += current)
                    __asm__ volatile("clflush (%0)" :: "r"(buf + p * PAGE + i) : "memory");
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
        }
    }
    return now_ns() - start;
}

int main(void) {
    uint32_t line = flush_line();
    if (!line) { fputs("CLFLUSH unavailable or unsupported line size\n", stderr); return 1; }
    uint8_t *buf = NULL;
    if (posix_memalign((void **)&buf, PAGE, PAGES * PAGE)) return 1;
    for (unsigned i = 0; i < PAGES * PAGE; ++i) buf[i] = (uint8_t)i;
    printf("line=%u pages=%u group=%u repeats=%u\n", line, PAGES, GROUP, REPEATS);
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t setup = run(buf, line, 1, 0);
        uint64_t full = run(buf, line, 1, 1);
        uint64_t reduced = run(buf, line, 0, 1);
        printf("round=%u setup_ns=%llu full_ns=%llu reduced_cpuid_ns=%llu\n", i,
               (unsigned long long)setup, (unsigned long long)full, (unsigned long long)reduced);
    }
    free(buf);
    return 0;
}
