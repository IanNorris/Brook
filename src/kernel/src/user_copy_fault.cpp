#include "user_copy_fault.h"

extern "C" const char BrookCopyFromUserInstruction[],BrookCopyFromUserFixup[];
extern "C" const char BrookCopyToUserInstruction[],BrookCopyToUserFixup[];
namespace brook {
uint64_t UserCopyFaultFixup(const UserCopyFault& fault) {
    if((fault.cs&3) || (fault.error&(4|8|16)) || (fault.flags&(1ULL<<10)) || !fault.remaining)return 0;
    const bool from=fault.instruction==reinterpret_cast<uint64_t>(BrookCopyFromUserInstruction);
    const bool to=fault.instruction==reinterpret_cast<uint64_t>(BrookCopyToUserInstruction);
    if(!from && !to)return 0;
    if(((fault.error&2)!=0)!=to)return 0;
    const uint64_t user=from?fault.source:fault.destination;
    constexpr uint64_t limit=0x0000800000000000ULL;
    if(user>=limit || fault.remaining>limit-user || fault.address!=user)return 0;
    return reinterpret_cast<uint64_t>(from?BrookCopyFromUserFixup:BrookCopyToUserFixup);
}
}
