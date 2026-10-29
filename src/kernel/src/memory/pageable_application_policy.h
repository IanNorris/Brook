#pragma once
#include <stdint.h>
namespace brook {
// Boot-only experiment, not executable authorization. Match the resolved path
// exactly; unsupported mapping shapes retain ordinary mmap behavior.
struct PageableApplicationPolicy {
    char executable[256]{};
    uint64_t residentPages=16384;
    uint64_t headroomPages=8192; // enter at 32 MiB, leave at twice that
    // Advisory hysteresis; direct victim handoff does not refill the band.
    bool ReclaimAllowed(bool& active,uint64_t freePages) const {
        if(!headroomPages){active=false;return true;} // legacy target-only mode
        active=active?freePages<headroomPages*2:freePages<=headroomPages;
        return active;
    }
    bool Configure(const char* path,uint32_t size,uint64_t targetPages,uint64_t minimumFreePages=8192) {
        executable[0]=0;
        if(!path || !size || size>=sizeof(executable) || path[0]!='/' ||
           !targetPages || targetPages>65536 || minimumFreePages>65536)return false;
        for(uint32_t i=0;i<size;++i)if(!path[i])return false;
        for(uint32_t i=0;i<size;++i)executable[i]=path[i];
        executable[size]=0;residentPages=targetPages;headroomPages=minimumFreePages;return true;
    }
    bool Matches(const char* path,uint64_t bytes,uint64_t protection,uint64_t flags) const {
        if(!executable[0] || !path || flags!=0x22 || protection!=3 ||
           bytes<65536 || bytes>16*1024*1024 || (bytes&4095))return false;
        for(unsigned i=0;i<sizeof(executable);++i) {
            if(path[i]!=executable[i])return false;
            if(!executable[i])return true;
        }
        return false;
    }
};
}
