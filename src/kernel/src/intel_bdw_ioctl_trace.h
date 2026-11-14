#pragma once
#include <stdint.h>
namespace brook {
// Bounded trace of i915-compatible ioctls (VA-API phase 2 bring-up): which
// ioctls, GETPARAM ids and execbuffer flags a userspace driver (libva/i965)
// actually uses on Brook, and what Brook answered. Off by default; enabled
// with `gpu ioctl trace on`. Entries are written lock-free and may race;
// it is a diagnostic, not an audit log.
struct BdwIoctlTraceEntry {uint32_t seq,pid,nr,size;uint64_t detail;int64_t result;};
constexpr unsigned BdwIoctlTraceCapacity=256;
struct BdwIoctlTrace {
    uint32_t enabled=0,next=0,dropped=0;
    BdwIoctlTraceEntry entries[BdwIoctlTraceCapacity]{};
    void record(uint32_t pid,uint32_t nr,uint32_t size,uint64_t detail,int64_t result){
        if(!__atomic_load_n(&enabled,__ATOMIC_ACQUIRE))return;
        const uint32_t seq=__atomic_fetch_add(&next,1u,__ATOMIC_ACQ_REL);
        if(seq>=BdwIoctlTraceCapacity)__atomic_fetch_add(&dropped,1u,__ATOMIC_RELAXED);
        entries[seq%BdwIoctlTraceCapacity]={seq+1,pid,nr,size,detail,result};
    }
    void reset(){__atomic_store_n(&next,0u,__ATOMIC_RELEASE);__atomic_store_n(&dropped,0u,__ATOMIC_RELEASE);
        for(auto& e:entries)e={};}
};
BdwIoctlTrace& BdwIoctlTraceState();
// i915 ioctl numbers (DRM_COMMAND_BASE 0x40 + driver nr) and core DRM ones, for the dump.
inline const char* BdwIoctlName(uint32_t nr){
    switch(nr){
    case 0x00:return "VERSION";case 0x09:return "GEM_CLOSE";case 0x0a:return "GEM_FLINK";case 0x0b:return "GEM_OPEN";
    case 0x0c:return "GET_CAP";case 0x2d:return "PRIME_HANDLE_TO_FD";case 0x2e:return "PRIME_FD_TO_HANDLE";
    case 0x46:return "I915_GETPARAM";case 0x47:return "I915_SETPARAM";case 0x53:return "I915_GEM_EXECBUFFER";
    case 0x57:return "I915_GEM_BUSY";case 0x59:return "I915_GEM_THROTTLE";case 0x5b:return "I915_GEM_CREATE";
    case 0x5c:return "I915_GEM_PREAD";case 0x5d:return "I915_GEM_PWRITE";case 0x5e:return "I915_GEM_MMAP";
    case 0x5f:return "I915_GEM_SET_DOMAIN";case 0x60:return "I915_GEM_SW_FINISH";case 0x61:return "I915_GEM_SET_TILING";
    case 0x62:return "I915_GEM_GET_TILING";case 0x63:return "I915_GEM_GET_APERTURE";case 0x64:return "I915_GEM_MMAP_GTT/OFFSET";
    case 0x66:return "I915_GEM_MADVISE";case 0x69:return "I915_GEM_EXECBUFFER2";case 0x6c:return "I915_GEM_WAIT";
    case 0x6d:return "I915_GEM_CONTEXT_CREATE";case 0x6e:return "I915_GEM_CONTEXT_DESTROY";case 0x6f:return "I915_GEM_SET_CACHING";
    case 0x70:return "I915_GEM_GET_CACHING";case 0x71:return "I915_REG_READ";case 0x72:return "I915_GET_RESET_STATS";
    case 0x73:return "I915_GEM_USERPTR";case 0x74:return "I915_GEM_CONTEXT_GETPARAM";case 0x75:return "I915_GEM_CONTEXT_SETPARAM";
    case 0x79:return "I915_QUERY";
    case 0xbf:return "SYNCOBJ_CREATE";case 0xc0:return "SYNCOBJ_DESTROY";case 0xc1:return "SYNCOBJ_HANDLE_TO_FD";
    case 0xc2:return "SYNCOBJ_FD_TO_HANDLE";case 0xc3:return "SYNCOBJ_WAIT";case 0xc4:return "SYNCOBJ_RESET";
    case 0xc5:return "SYNCOBJ_SIGNAL";case 0xca:return "SYNCOBJ_TIMELINE_WAIT";case 0xcb:return "SYNCOBJ_QUERY";
    case 0xcc:return "SYNCOBJ_TRANSFER";case 0xcd:return "SYNCOBJ_TIMELINE_SIGNAL";
    default:return "?";
    }
}
}
