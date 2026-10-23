#pragma once
#include <stdint.h>
namespace brook {
// Reported inode numbers are independent of transient VfsObject identities.
// Reserve the high domain for task incarnations; globals use the low domain.
// Checked arithmetic rejects exhaustion rather than hashing or aliasing IDs.
static constexpr uint32_t ProcInodeIndexes=1024,ProcInodeRoles=32;
static inline bool ProcInodeNumber(uint64_t incarnation,uint32_t role,
                                   bool member,uint32_t index,uint64_t& out)
{
    if(role>=ProcInodeRoles || index>=ProcInodeIndexes)return false;
    constexpr uint64_t Domain=uint64_t{1}<<63;
    constexpr uint64_t Stride=uint64_t{ProcInodeRoles}*2*ProcInodeIndexes;
    const uint64_t slot=uint64_t{role}*2*ProcInodeIndexes+uint64_t{member}*ProcInodeIndexes+index;
    if(!incarnation){out=slot+1;return true;}
    if(incarnation>(Domain-1-slot)/Stride)return false;
    out=Domain|(incarnation*Stride+slot);return true;
}
}
