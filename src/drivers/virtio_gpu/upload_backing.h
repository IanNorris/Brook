#pragma once
#include <stdint.h>
namespace brook {
// Serialized by GpuCmdGuard. Host resource and context incarnation identify
// the allocation independently of context-slot reuse. Published means a
// backing-attach command may have reached the host, not that it succeeded.
struct GpuUploadBacking {
    bool used = false;
    bool published = false;
    bool quarantined = false;
    int32_t ctxId = 0;
    uint64_t incarnation = 0;
    uint32_t resource = 0;
    uint64_t vaddr = 0;
    uint64_t pages = 0;
};
}
