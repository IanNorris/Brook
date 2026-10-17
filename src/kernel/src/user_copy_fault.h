#pragma once
#include <stdint.h>

namespace brook {
struct UserCopyFault {
    uint64_t instruction,address,source,destination,remaining,error,cs,flags;
};
// Pure, nonblocking classifier for #PF only. Zero means ordinary fault handling;
// otherwise redirect RIP to the corresponding assembly-local residual return.
uint64_t UserCopyFaultFixup(const UserCopyFault& fault);
}
extern "C" uint64_t BrookCopyFromUserRaw(void*,const void*,uint64_t);
extern "C" uint64_t BrookCopyToUserRaw(void*,const void*,uint64_t);
