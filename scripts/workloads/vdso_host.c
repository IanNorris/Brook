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
#include <sched.h>
#include <cpuid.h>
#include "../../src/shared/pvclock_page.h"
#include <unistd.h>

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
    struct BrookPvclockSlot *slots = (void *)((char *)info.dli_fbase + 4096);
    unsigned a,b,c,d;
    assert(__get_cpuid(0x80000001,&a,&b,&c,&d) && (d&(1u<<27)));
    cpu_set_t allowed, selected;
    assert(sched_getaffinity(0,sizeof(allowed),&allowed)==0);
    unsigned cpu=BROOK_PVCLOCK_CPUS;
    /* Prefer an AP slot to catch accidentally continuing to read CPU0. */
    for(unsigned i=1;i<BROOK_PVCLOCK_CPUS;++i)if(CPU_ISSET(i,&allowed)){cpu=i;break;}
    if(cpu==BROOK_PVCLOCK_CPUS && CPU_ISSET(0,&allowed))cpu=0;
    assert(cpu<BROOK_PVCLOCK_CPUS);
    CPU_ZERO(&selected);CPU_SET(cpu,&selected);
    assert(sched_setaffinity(0,sizeof(selected),&selected)==0);
    unsigned lo,hi,aux;
    __asm__ volatile("rdtscp" : "=a"(lo),"=d"(hi),"=c"(aux));
    assert((aux&BROOK_PVCLOCK_CPU_MASK)==cpu);
    struct BrookPvclockInfo *page=&slots[cpu].clock;
    struct BrookPvclockMetadata *metadata=(void*)slots[0].reserved;
    assert(mprotect(slots, 4096, PROT_READ | PROT_WRITE) == 0);
    memset(slots, 0, 4096);
    fallback(fn, CLOCK_MONOTONIC); /* Unavailable clock page. */
    metadata->magic=BROOK_PVCLOCK_MAGIC;metadata->version=BROOK_PVCLOCK_VERSION;
    metadata->stride=BROOK_PVCLOCK_STRIDE;metadata->cpus=BROOK_PVCLOCK_CPUS;
    metadata->features=BROOK_PVCLOCK_RDTSCP;
    page->system_time = 1234567890123ULL;
    page->tsc_to_system_mul = 1;
    page->tsc_shift = -63;
    page->flags = 1;
    struct timespec value;
    assert(fn(CLOCK_MONOTONIC, &value) == 0);
    assert(ns(value) == page->system_time); /* Must be fast, not host time. */
    metadata->features=0;fallback(fn,CLOCK_MONOTONIC);metadata->features=BROOK_PVCLOCK_RDTSCP;
    metadata->magic=0;fallback(fn,CLOCK_MONOTONIC);metadata->magic=BROOK_PVCLOCK_MAGIC;
    metadata->version=2;fallback(fn,CLOCK_MONOTONIC);metadata->version=BROOK_PVCLOCK_VERSION;
    metadata->stride=32;fallback(fn,CLOCK_MONOTONIC);metadata->stride=BROOK_PVCLOCK_STRIDE;
    metadata->cpus=65;fallback(fn,CLOCK_MONOTONIC);metadata->cpus=BROOK_PVCLOCK_CPUS;
    const int shifts[] = {-63, -1, 0, 1, 8};
    for (unsigned i = 0; i < sizeof(shifts)/sizeof(shifts[0]); ++i) {
        page->tsc_timestamp = tsc();
        page->tsc_shift = shifts[i];
        page->tsc_to_system_mul = 0x80000000U;
        uint64_t lo = tsc() - page->tsc_timestamp;
        assert(fn(CLOCK_MONOTONIC, &value) == 0);
        uint64_t hi = tsc() - page->tsc_timestamp;
        if (shifts[i] < 0) { lo >>= -shifts[i]; hi >>= -shifts[i]; }
        else { lo <<= shifts[i]; hi <<= shifts[i]; }
        assert(ns(value) >= page->system_time + lo / 2);
        assert(ns(value) <= page->system_time + hi / 2);
    }
    page->flags = 0; fallback(fn, CLOCK_MONOTONIC);
    page->flags = 1;
    page->version = 1; fallback(fn, CLOCK_MONOTONIC); /* Bounded retry -> syscall. */
    page->version = 2;
    page->tsc_to_system_mul = 0; fallback(fn, CLOCK_MONOTONIC);
    page->tsc_to_system_mul = UINT32_MAX;
    page->tsc_timestamp = UINT64_MAX; fallback(fn, CLOCK_MONOTONIC);
    page->tsc_timestamp = 0;
    page->tsc_shift = -64; fallback(fn, CLOCK_MONOTONIC);
    page->tsc_shift = 64; fallback(fn, CLOCK_MONOTONIC);
    page->tsc_shift = 63; fallback(fn, CLOCK_MONOTONIC); /* Shift overflow. */
    page->tsc_shift = 0;
    page->system_time = UINT64_MAX; fallback(fn, CLOCK_MONOTONIC); /* Add overflow. */
    fallback(fn, CLOCK_REALTIME);
    assert(fn(-1, &value) == -EINVAL);
    page->flags = 0;
    assert(fn(CLOCK_MONOTONIC, NULL) == -EFAULT);
    assert(mprotect(slots, 4096, PROT_READ) == 0);
    assert(dlclose(handle) == 0);
    printf("VDSO_HOST_PASS: CPU slot %u, metadata guards, versioned ELF, arithmetic, bounded retry and syscall fallback\n",cpu);
    return 0;
}
