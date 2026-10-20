#pragma once

// Brook's sanitized public pvclock page. KVM owns only the 32-byte clock
// record in each cache line. The first line's padding carries pointer-free
// format metadata; all other padding remains zero. Shared with the C vDSO.
typedef __UINT64_TYPE__ BrookClockU64;
typedef __UINT32_TYPE__ BrookClockU32;
typedef __UINT16_TYPE__ BrookClockU16;
enum {
    BROOK_PVCLOCK_CPUS = 64,
    BROOK_PVCLOCK_STRIDE = 64,
    BROOK_PVCLOCK_MAGIC = 0x31435042,
    BROOK_PVCLOCK_VERSION = 1,
    BROOK_PVCLOCK_RDTSCP = 1,
    BROOK_PVCLOCK_CPU_MASK = 0xfff
};
struct BrookPvclockInfo {
    BrookClockU32 version, pad0;
    BrookClockU64 tsc_timestamp, system_time;
    BrookClockU32 tsc_to_system_mul;
    signed char tsc_shift;
    unsigned char flags, pad[2];
};
struct __attribute__((aligned(64))) BrookPvclockSlot {
    struct BrookPvclockInfo clock;
    unsigned char reserved[32];
};
struct BrookPvclockMetadata {
    BrookClockU32 magic;
    BrookClockU16 version, stride;
    BrookClockU32 cpus, features;
    BrookClockU64 reserved[2];
};
#ifdef __cplusplus
static_assert(sizeof(BrookPvclockInfo) == 32);
static_assert(sizeof(BrookPvclockSlot) == 64);
static_assert(sizeof(BrookPvclockMetadata) == 32);
static_assert(sizeof(BrookPvclockSlot) * BROOK_PVCLOCK_CPUS == 4096);
#else
_Static_assert(sizeof(struct BrookPvclockInfo) == 32, "KVM pvclock ABI");
_Static_assert(sizeof(struct BrookPvclockSlot) == 64, "Brook pvclock stride");
_Static_assert(sizeof(struct BrookPvclockMetadata) == 32, "Brook pvclock metadata");
#endif
