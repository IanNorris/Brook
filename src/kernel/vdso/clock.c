/* Linux x86-64 vDSO ABI. No libc, writable globals or kernel addresses. */
typedef unsigned long long u64;
typedef unsigned int u32;
struct timespec64 { long seconds, nanoseconds; };
struct pvclock_info {
    u32 version, padding;
    u64 timestamp, nanoseconds;
    u32 multiplier;
    signed char shift;
    unsigned char flags, reserved[2];
};
_Static_assert(sizeof(struct pvclock_info) == 32, "pvclock ABI");
extern volatile struct pvclock_info _brook_pvclock __attribute__((visibility("hidden")));

static inline u64 ordered_tsc(void) {
    u32 low, high;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) :: "memory");
    return (u64)low | ((u64)high << 32);
}

__attribute__((visibility("default")))
int __vdso_clock_gettime(int clock_id, struct timespec64* output) {
    if (clock_id == 1) { /* CLOCK_MONOTONIC; all other IDs retain syscall semantics. */
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            const u32 first = __atomic_load_n(&_brook_pvclock.version, __ATOMIC_ACQUIRE);
            if (first & 1) { __asm__ volatile("pause"); continue; }
            const u64 timestamp = _brook_pvclock.timestamp;
            const u64 base = _brook_pvclock.nanoseconds;
            const u32 multiplier = _brook_pvclock.multiplier;
            const int shift = _brook_pvclock.shift;
            const unsigned flags = _brook_pvclock.flags;
            const u64 tsc = ordered_tsc();
            const u32 second = __atomic_load_n(&_brook_pvclock.version, __ATOMIC_ACQUIRE);
            if (first != second || (second & 1)) continue;
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
