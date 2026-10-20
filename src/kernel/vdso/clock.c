/* Linux x86-64 vDSO ABI. No libc, writable globals or kernel addresses. */
typedef unsigned long long u64;
typedef unsigned int u32;
struct timespec64 { long seconds, nanoseconds; };
#include "../../shared/pvclock_page.h"
extern volatile struct BrookPvclockSlot _brook_pvclock[BROOK_PVCLOCK_CPUS]
    __attribute__((visibility("hidden")));

static inline u64 ordered_tsc(u32* cpu) {
    u32 low, high, auxiliary;
    __asm__ volatile("lfence; rdtscp; lfence" : "=a"(low), "=d"(high), "=c"(auxiliary) :: "memory");
    *cpu = auxiliary & BROOK_PVCLOCK_CPU_MASK;
    return (u64)low | ((u64)high << 32);
}

__attribute__((visibility("default")))
int __vdso_clock_gettime(int clock_id, struct timespec64* output) {
    const volatile struct BrookPvclockMetadata* metadata =
        (const volatile struct BrookPvclockMetadata*)_brook_pvclock[0].reserved;
    if (clock_id == 1 && metadata->magic == BROOK_PVCLOCK_MAGIC &&
        metadata->version == BROOK_PVCLOCK_VERSION && metadata->stride == BROOK_PVCLOCK_STRIDE &&
        metadata->cpus == BROOK_PVCLOCK_CPUS && (metadata->features & BROOK_PVCLOCK_RDTSCP)) { /* CLOCK_MONOTONIC; all other IDs retain syscall semantics. */
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            u32 cpu, after_cpu;
            (void)ordered_tsc(&cpu);
            if (cpu >= BROOK_PVCLOCK_CPUS) break;
            const volatile struct BrookPvclockInfo* page = &_brook_pvclock[cpu].clock;
            const u32 first = __atomic_load_n(&page->version, __ATOMIC_ACQUIRE);
            if (first & 1) { __asm__ volatile("pause"); continue; }
            const u64 timestamp = page->tsc_timestamp;
            const u64 base = page->system_time;
            const u32 multiplier = page->tsc_to_system_mul;
            const int shift = page->tsc_shift;
            const unsigned flags = page->flags;
            const u64 tsc = ordered_tsc(&after_cpu);
            const u32 second = __atomic_load_n(&page->version, __ATOMIC_ACQUIRE);
            if (cpu != after_cpu || first != second || (second & 1)) continue;
            if (!(flags & 1) || !multiplier || tsc < timestamp || shift < -63 || shift > 63) break;
            u64 delta = tsc - timestamp;
            if (shift < 0) delta >>= -shift;
            else if (shift > 0) {
                if (delta > (~(u64)0 >> shift)) break;
                delta <<= shift;
            }
            const u64 elapsed = (u64)(((unsigned __int128)delta * multiplier) >> 32);
            if (base > ~(u64)0 - elapsed) break;
            const u64 value = base + elapsed;
            output->seconds = (long)(value / 1000000000);
            output->nanoseconds = (long)(value % 1000000000);
            return 0;
        }
    }
    long result;
    __asm__ volatile("syscall" : "=a"(result)
                     : "a"(228L), "D"((long)clock_id), "S"(output)
                     : "rcx", "r11", "memory");
    return (int)result;
}
extern __typeof(__vdso_clock_gettime) clock_gettime
    __attribute__((weak, alias("__vdso_clock_gettime"), visibility("default")));
