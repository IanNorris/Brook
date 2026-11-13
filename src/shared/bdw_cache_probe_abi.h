#pragma once
#include <stdint.h>

// Diagnostic build only. Allocate a fresh buffer and return its PRIME fd;
// query the exact fd to read the observed skipped page counts.
#define BROOK_BDW_CACHE_PROBE_IOCTL 0xc02864fau
#define BROOK_BDW_CACHE_PROBE_MAX_BYTES (2u * 1024u * 1024u)
#define BROOK_BDW_CACHE_PROBE_ALLOC 1u
#define BROOK_BDW_CACHE_PROBE_QUERY 2u
#define BROOK_BDW_CACHE_PROBE_SKIP_TO_DEVICE 1u
#define BROOK_BDW_CACHE_PROBE_SKIP_TO_CPU 2u

struct BrookBdwCacheProbe {
    uint32_t operation, omit;
    int32_t fd;
    uint32_t reserved;
    uint64_t bytes, skippedToDevice, skippedToCpu;
};
