#define _GNU_SOURCE
/* Run in an owned eight-vCPU VM. The host pauses/resumes that same VM after
 * PVCLOCK_RESUME_READY; compare libc's vDSO path with forced kernel reads. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "../../src/shared/pvclock_page.h"

static atomic_uint ready;
static atomic_int go;
struct result { uint64_t samples, backwards, bracket_errors, max_rewind_ns, cpu_mask; } results[8];

static uint64_t ns(struct timespec t) {
    assert(t.tv_sec >= 0 && t.tv_nsec >= 0 && t.tv_nsec < 1000000000);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void *worker(void *argument) {
    struct result *r = &results[(uintptr_t)argument];
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&go)) sched_yield();
    uint64_t start=0, previous=0;
    do {
        struct timespec before, value, after;
        assert(syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &before) == 0);
        assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
        assert(syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &after) == 0);
        uint64_t first=ns(before), current=ns(value), last=ns(after);
        if (!start) start=first;
        if (current < previous) {
            ++r->backwards;
            if (previous-current > r->max_rewind_ns) r->max_rewind_ns=previous-current;
        }
        if (current < first || current > last || last < first) ++r->bracket_errors;
        unsigned lo,hi,cpu;
        __asm__ volatile("rdtscp" : "=a"(lo),"=d"(hi),"=c"(cpu));
        (void)lo;(void)hi;
        cpu &= BROOK_PVCLOCK_CPU_MASK;
        assert(cpu < BROOK_PVCLOCK_CPUS);
        r->cpu_mask |= (uint64_t)1 << cpu;
        ++r->samples;previous=current;
        if (last >= start && last-start >= 12000000000ULL) break;
        if (!(r->samples % 128)) sched_yield();
    } while (1);
    return NULL;
}
int main(void) {
    uintptr_t base=getauxval(AT_SYSINFO_EHDR);
    assert(base);
    const struct BrookPvclockSlot *slots=(const void*)(base+4096);
    const struct BrookPvclockMetadata *meta=(const void*)slots[0].reserved;
    assert(meta->magic==BROOK_PVCLOCK_MAGIC && meta->version==BROOK_PVCLOCK_VERSION);
    assert(meta->stride==64 && meta->cpus==64 && (meta->features&BROOK_PVCLOCK_RDTSCP));
    pthread_t threads[8];
    for (uintptr_t i=0;i<8;++i) assert(pthread_create(&threads[i],NULL,worker,(void*)i)==0);
    while (atomic_load(&ready)!=8) sched_yield();
    puts("PVCLOCK_RESUME_READY threads=8 seconds=12");fflush(stdout);
    atomic_store(&go,1);
    uint64_t samples=0, backwards=0, errors=0, mask=0, rewind=0;
    for (unsigned i=0;i<8;++i) {
        assert(pthread_join(threads[i],NULL)==0);
        samples+=results[i].samples;backwards+=results[i].backwards;
        errors+=results[i].bracket_errors;mask|=results[i].cpu_mask;
        if(results[i].max_rewind_ns>rewind)rewind=results[i].max_rewind_ns;
    }
    printf("PVCLOCK_RESUME_RESULT samples=%llu backwards=%llu bracket_errors=%llu cpu_mask=%llu max_rewind_ns=%llu\n",
           (unsigned long long)samples,(unsigned long long)backwards,(unsigned long long)errors,
           (unsigned long long)mask,(unsigned long long)rewind);
    assert(samples && mask==255 && !backwards && !errors);
    puts("PVCLOCK_RESUME_PASS");
}
