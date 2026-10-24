#pragma once
#include <stdint.h>

namespace brook {
// Stored configuration excludes dynamic SS_ONSTACK. Linux x86 stacks grow down:
// the initial top belongs to the stack, while its exhausted bottom does not.
inline int SignalAltstackFlags(uint64_t base,uint64_t size,uint32_t flags,uint64_t rsp) {
    if(!size || (flags&2))return 2; // SS_DISABLE (also canonicalize zeroed tasks)
    return rsp>base && rsp-base<=size ? 1 : 0;
}
}
