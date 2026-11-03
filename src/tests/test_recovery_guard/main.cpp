#include <cassert>
#include <cstring>
#include "../../shared/update/recovery_guard.h"

static void Seal(uint8_t* p) { BrookUpdatePut32(p+124,BrookUpdateCrc(p,124)); }
static const char identity[] =
    "01000000000000000000000000000000"
    "02000000000000000000000000000000"
    "03000000000000000000000000000000";
int main() {
    uint8_t a[128]{},b[128]{};
    assert(!BrookRecoveryGuardAllowsNormal(a,sizeof(a),b,sizeof(b),identity));
    memcpy(a,"BRKRCV01",8);BrookUpdatePut64(a+8,1);
    BrookUpdatePut32(a+16,BROOK_RECOVERY_GUARD_CLEAN);
    a[24]=1;a[40]=2;a[56]=3;Seal(a);memcpy(b,a,sizeof(a));
    assert(BrookRecoveryGuardAllowsNormal(a,sizeof(a),b,sizeof(b),identity));
    assert(!BrookRecoveryGuardAllowsNormal(a,sizeof(a),b,sizeof(b),""));
    assert(!BrookRecoveryGuardAllowsNormal(a,sizeof(a),b,sizeof(b),nullptr));
    char wrong[sizeof(identity)];memcpy(wrong,identity,sizeof(wrong));wrong[0]='2';
    assert(!BrookRecoveryGuardAllowsNormal(a,sizeof(a),b,sizeof(b),wrong));
    assert(!BrookRecoveryGuardAllowsNormal(nullptr,0,b,sizeof(b),identity));
    assert(!BrookRecoveryGuardAllowsNormal(a,127,b,sizeof(b),identity));
    for(unsigned i=0;i<128;++i) { b[i]^=1;assert(!BrookRecoveryGuardAllowsNormal(a,sizeof(a),b,sizeof(b),identity));b[i]^=1; }
    BrookUpdatePut64(b+8,2);Seal(b);
    assert(!BrookRecoveryGuardAllowsNormal(a,sizeof(a),b,sizeof(b),identity));
    memcpy(b,a,sizeof(a));BrookUpdatePut32(b+16,BROOK_RECOVERY_GUARD_PENDING);Seal(b);
    assert(!BrookRecoveryGuardAllowsNormal(b,sizeof(b),b,sizeof(b),identity));
    BrookUpdatePut32(b+16,BROOK_RECOVERY_GUARD_FAILED);Seal(b);
    assert(!BrookRecoveryGuardAllowsNormal(b,sizeof(b),b,sizeof(b),identity));
    memcpy(b,a,sizeof(a));memset(b+24,0,16);Seal(b);
    assert(!BrookRecoveryGuardValid(b,sizeof(b)));
    memcpy(b,a,sizeof(a));b[72]=1;Seal(b);assert(!BrookRecoveryGuardValid(b,sizeof(b)));
}
