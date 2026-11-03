#include <cassert>
#include <cstring>
#include <initializer_list>
#include "../../shared/update/recovery_guard_transaction.h"

static const char identity[] =
    "01000000000000000000000000000000"
    "02000000000000000000000000000000"
    "03000000000000000000000000000000";
struct Io {
    uint8_t visible[2][128]{},stable[2][128]{};
    int operation=0,fail=0,drop=-1;bool partial=false,early=false;
    bool Read(unsigned i,uint8_t* p,unsigned n) {
        if(++operation==fail)return false;
        memcpy(p,visible[i],n);return true;
    }
    bool Write(unsigned i,const uint8_t* p,unsigned n) {
        if(++operation==fail) {
            if(partial) {memcpy(visible[i],p,n/2);memcpy(stable[i],p,n/2);}
            return false;
        }
        if(int(i)==drop)return true; // device falsely reports a completed write
        memcpy(visible[i],p,n);if(early)memcpy(stable[i],p,n);return true;
    }
    bool Sync(unsigned i) {
        if(++operation==fail)return false;
        memcpy(stable[i],visible[i],128);return true;
    }
    bool Flush() {return ++operation!=fail;}
    void Crash() {memcpy(visible,stable,sizeof(visible));}
};
static Io Initial() {
    Io io{};auto* p=io.visible[0];memcpy(p,"BRKRCV01",8);
    BrookUpdatePut64(p+8,41);BrookUpdatePut32(p+16,BROOK_RECOVERY_GUARD_CLEAN);
    p[24]=1;p[40]=2;p[56]=3;BrookUpdatePut32(p+124,BrookUpdateCrc(p,124));
    memcpy(io.visible[1],p,128);memcpy(io.stable,io.visible,sizeof(io.stable));return io;
}
int main() {
    Io success=Initial();
    assert(BrookGuardBeginRepair(success,identity)==BrookGuardBeginResult::PendingDurable);
    assert(success.operation==11);
    assert(BrookUpdateGet64(success.stable[0]+8)==42);
    assert(BrookUpdateGet32(success.stable[0]+16)==BROOK_RECOVERY_GUARD_PENDING);
    assert(BrookUpdateEqual(success.stable[0],success.stable[1],128));
    success.Crash();assert(!BrookRecoveryGuardAllowsNormal(success.visible[0],128,success.visible[1],128,identity));
    for(int failure=1;failure<=11;++failure)for(bool partial:{false,true})for(bool early:{false,true}) {
        Io io=Initial();io.fail=failure;io.partial=partial;io.early=early;
        const auto result=BrookGuardBeginRepair(io,identity);
        assert(result!=BrookGuardBeginResult::PendingDurable);
        assert(result==(failure<=2?BrookGuardBeginResult::Rejected:BrookGuardBeginResult::Uncertain));
        io.Crash();
        // Both clean is safe here only because the caller was denied repair.
        if(BrookRecoveryGuardAllowsNormal(io.visible[0],128,io.visible[1],128,identity))
            assert(BrookUpdateGet64(io.visible[0]+8)==41);
    }
    Io mismatch=Initial();mismatch.visible[1][0]^=1;
    assert(BrookGuardBeginRepair(mismatch,identity)==BrookGuardBeginResult::Rejected);
    assert(mismatch.operation==2);
    Io wrong=Initial();assert(BrookGuardBeginRepair(wrong,"bad")==BrookGuardBeginResult::Rejected);
    for(int slot=0;slot<2;++slot) {
        Io dropped=Initial();dropped.drop=slot;
        assert(BrookGuardBeginRepair(dropped,identity)==BrookGuardBeginResult::Uncertain);
    }
    Io overflow=Initial();for(auto& r:overflow.visible) {BrookUpdatePut64(r+8,~uint64_t{0});BrookUpdatePut32(r+124,BrookUpdateCrc(r,124));}
    assert(BrookGuardBeginRepair(overflow,identity)==BrookGuardBeginResult::Rejected);
}
