#pragma once
#include "payload.h"

// Two identical, CRC-protected records are required. A pending, failed,
// missing, corrupt or disagreeing record never authorizes normal boot.
// This codec alone does not authenticate the disk or implement a repair.
#define BROOK_RECOVERY_GUARD_SIZE 128u
#define BROOK_RECOVERY_GUARD_CLEAN 1u
#define BROOK_RECOVERY_GUARD_PENDING 2u
#define BROOK_RECOVERY_GUARD_FAILED 3u

static inline int BrookRecoveryGuardValid(const uint8_t* p, BrookUpdateSize n) {
    static const uint8_t magic[8]={'B','R','K','R','C','V','0','1'};
    if(!p || n!=BROOK_RECOVERY_GUARD_SIZE || !BrookUpdateEqual(p,magic,8) ||
       !BrookUpdateGet64(p+8) || BrookUpdateGet32(p+20))return 0;
    const uint32_t state=BrookUpdateGet32(p+16);
    if(state<BROOK_RECOVERY_GUARD_CLEAN || state>BROOK_RECOVERY_GUARD_FAILED)return 0;
    for(unsigned id=0;id<3;++id) {
        unsigned nonzero=0;for(unsigned i=0;i<16;++i)nonzero|=p[24+id*16+i];
        if(!nonzero)return 0;
    }
    for(unsigned i=72;i<124;++i)if(p[i])return 0;
    return BrookUpdateGet32(p+124)==BrookUpdateCrc(p,124);
}

static inline int BrookRecoveryGuardAllowsNormal(const uint8_t* a,BrookUpdateSize an,
                                                  const uint8_t* b,BrookUpdateSize bn,
                                                  const char* identity) {
    if(!BrookRecoveryGuardValid(a,an) || !BrookRecoveryGuardValid(b,bn) ||
       !BrookUpdateEqual(a,b,BROOK_RECOVERY_GUARD_SIZE) ||
       BrookUpdateGet32(a+16)!=BROOK_RECOVERY_GUARD_CLEAN || !identity)return 0;
    for(unsigned i=0;i<48;++i) {
        unsigned value=0;
        for(unsigned j=0;j<2;++j) {
            const char c=*identity++;unsigned digit;
            if(c>='0' && c<='9')digit=unsigned(c-'0');
            else if(c>='a' && c<='f')digit=unsigned(c-'a'+10);
            else return 0;
            value=value*16+digit;
        }
        if(a[24+i]!=value)return 0;
    }
    return *identity=='\0';
}
