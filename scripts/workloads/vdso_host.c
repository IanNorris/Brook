#define _GNU_SOURCE
/* Run against the actual freestanding ELF, including its symbol versions.
 * This exercises synthetic clock data only; guest mapping/lifetime tests are
 * separate. Usage: vdso-host /absolute/path/to/brook-vdso.so */
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct clock_page {
    uint32_t version, padding;
    uint64_t timestamp, nanoseconds;
    uint32_t multiplier;
    int8_t shift;
    uint8_t flags, reserved[2];
};
typedef int (*clock_fn)(int, struct timespec *);
static uint64_t tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return (uint64_t)lo | (uint64_t)hi << 32;
}
static uint64_t ns(struct timespec value) {
    assert(value.tv_sec >= 0 && value.tv_nsec >= 0 && value.tv_nsec < 1000000000);
    return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}
static void fallback(clock_fn fn, int id) {
    struct timespec before, value, after;
    assert(syscall(SYS_clock_gettime, id, &before) == 0);
    assert(fn(id, &value) == 0);
    assert(syscall(SYS_clock_gettime, id, &after) == 0);
    assert(ns(before) <= ns(value) && ns(value) <= ns(after));
}
int main(int argc, char **argv) {
    assert(argc == 2);
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    clock_fn fn = (clock_fn)dlvsym(handle, "__vdso_clock_gettime", "LINUX_2.6");
    assert(fn && dlvsym(handle, "clock_gettime", "LINUX_2.6") == (void *)fn);
    assert(!dlvsym(handle, "__vdso_clock_gettime", "LINUX_9.9"));
    Dl_info info;
    assert(dladdr((void *)fn, &info) && info.dli_fbase);
    struct clock_page *page = (void *)((char *)info.dli_fbase + 4096);
    assert(mprotect(page, 4096, PROT_READ | PROT_WRITE) == 0);
    memset(page, 0, 4096);
    fallback(fn, CLOCK_MONOTONIC); /* Unavailable clock page. */
    page->nanoseconds = 1234567890123ULL;
    page->multiplier = 1;
    page->shift = -63;
    page->flags = 1;
    struct timespec value;
    assert(fn(CLOCK_MONOTONIC, &value) == 0);
    assert(ns(value) == page->nanoseconds); /* Must be fast, not host time. */
    const int shifts[] = {-63, -1, 0, 1, 8};
    for (unsigned i = 0; i < sizeof(shifts)/sizeof(shifts[0]); ++i) {
        page->timestamp = tsc();
        page->shift = shifts[i];
        page->multiplier = 0x80000000U;
        uint64_t lo = tsc() - page->timestamp;
        assert(fn(CLOCK_MONOTONIC, &value) == 0);
        uint64_t hi = tsc() - page->timestamp;
        if (shifts[i] < 0) { lo >>= -shifts[i]; hi >>= -shifts[i]; }
        else { lo <<= shifts[i]; hi <<= shifts[i]; }
        assert(ns(value) >= page->nanoseconds + lo / 2);
        assert(ns(value) <= page->nanoseconds + hi / 2);
    }
    page->flags = 0; fallback(fn, CLOCK_MONOTONIC);
    page->flags = 1;
    page->version = 1; fallback(fn, CLOCK_MONOTONIC); /* Bounded retry -> syscall. */
    page->version = 2;
    page->multiplier = 0; fallback(fn, CLOCK_MONOTONIC);
    page->multiplier = UINT32_MAX;
    page->timestamp = UINT64_MAX; fallback(fn, CLOCK_MONOTONIC);
    page->timestamp = 0;
    page->shift = -64; fallback(fn, CLOCK_MONOTONIC);
    page->shift = 64; fallback(fn, CLOCK_MONOTONIC);
    page->shift = 63; fallback(fn, CLOCK_MONOTONIC); /* Shift overflow. */
    page->shift = 0;
    page->nanoseconds = UINT64_MAX; fallback(fn, CLOCK_MONOTONIC); /* Add overflow. */
    fallback(fn, CLOCK_REALTIME);
    assert(fn(-1, &value) == -EINVAL);
    page->flags = 0;
    assert(fn(CLOCK_MONOTONIC, NULL) == -EFAULT);
    assert(mprotect(page, 4096, PROT_READ) == 0);
    assert(dlclose(handle) == 0);
    puts("VDSO_HOST_PASS: versioned ELF, arithmetic, bounded helper retry and raw syscall fallback");
    return 0;
}
