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

    Io pending=Initial();
    assert(BrookGuardBeginRepair(pending,identity)==BrookGuardBeginResult::PendingDurable);
    uint8_t expected[128];memcpy(expected,pending.stable[0],128);
    pending.operation=0;
    assert(BrookGuardCompleteRepair(pending,identity,expected)==BrookGuardCompleteResult::CleanDurable);
    assert(pending.operation==11);
    pending.Crash();
    assert(BrookRecoveryGuardAllowsNormal(pending.visible[0],128,pending.visible[1],128,identity));
    assert(BrookUpdateGet64(pending.visible[0]+8)==42);
    Io retry=Initial();assert(BrookGuardBeginRepair(retry,identity)==BrookGuardBeginResult::PendingDurable);
    uint8_t attempt[128]{};retry.operation=0;
    assert(BrookGuardBeginAttempt(retry,identity,2,attempt)==BrookGuardAttemptResult::PendingDurable);
    assert(retry.operation==11 && BrookUpdateGet32(attempt+20)==1);
    retry.Crash();assert(BrookRecoveryGuardMatchesPending(retry.visible[0],128,retry.visible[1],128,identity));
    assert(!BrookRecoveryGuardAllowsNormal(retry.visible[0],128,retry.visible[1],128,identity));
    retry.operation=0;assert(BrookGuardBeginAttempt(retry,identity,2,attempt)==BrookGuardAttemptResult::PendingDurable);
    assert(BrookUpdateGet32(attempt+20)==2);
    retry.operation=0;uint8_t exhausted[128];memset(exhausted,0xaa,sizeof(exhausted));
    assert(BrookGuardBeginAttempt(retry,identity,2,exhausted)==BrookGuardAttemptResult::Exhausted);
    for(auto b:exhausted)assert(!b);
    retry.operation=0;assert(BrookGuardCompleteRepair(retry,identity,attempt)==BrookGuardCompleteResult::CleanDurable);
    assert(BrookUpdateGet32(retry.stable[0]+20)==0 && BrookRecoveryGuardAllowsNormal(retry.stable[0],128,retry.stable[1],128,identity));
    for(int failure=1;failure<=11;++failure)for(bool partial:{false,true})for(bool early:{false,true}) {
        Io io=Initial();assert(BrookGuardBeginRepair(io,identity)==BrookGuardBeginResult::PendingDurable);
        io.operation=0;io.fail=failure;io.partial=partial;io.early=early;uint8_t out[128];
        const auto result=BrookGuardBeginAttempt(io,identity,2,out);
        assert(result==(failure<=2?BrookGuardAttemptResult::Rejected:BrookGuardAttemptResult::Uncertain));
        io.Crash();assert(!BrookRecoveryGuardAllowsNormal(io.visible[0],128,io.visible[1],128,identity));
        for(auto b:out)assert(!b);
    }
    uint8_t cleanExpected[128];memcpy(cleanExpected,pending.visible[0],128);
    assert(BrookGuardCompleteRepair(pending,identity,expected)==BrookGuardCompleteResult::Rejected);
    for(int failure=1;failure<=11;++failure)for(bool partial:{false,true})for(bool early:{false,true}) {
        Io io=Initial();assert(BrookGuardBeginRepair(io,identity)==BrookGuardBeginResult::PendingDurable);
        io.operation=0;io.fail=failure;io.partial=partial;io.early=early;
        const auto result=BrookGuardCompleteRepair(io,identity,expected);
        assert(result==(failure<=2?BrookGuardCompleteResult::Rejected:BrookGuardCompleteResult::Uncertain));
        io.Crash();
        if(!partial && !early) {
            assert(BrookUpdateEqual(io.visible[0],failure<=4?expected:cleanExpected,128));
            assert(BrookUpdateEqual(io.visible[1],failure<=7?expected:cleanExpected,128));
        }
        if(BrookRecoveryGuardAllowsNormal(io.visible[0],128,io.visible[1],128,identity))
            assert(BrookUpdateGet64(io.visible[0]+8)==42);
    }
    Io stale=Initial();assert(BrookGuardBeginRepair(stale,identity)==BrookGuardBeginResult::PendingDurable);
    uint8_t unchanged[2][128];memcpy(unchanged,stale.stable,sizeof(unchanged));
    uint8_t badExpected[128];memcpy(badExpected,expected,128);
    BrookUpdatePut64(badExpected+8,43);BrookUpdatePut32(badExpected+124,BrookUpdateCrc(badExpected,124));
    assert(BrookGuardCompleteRepair(stale,identity,badExpected)==BrookGuardCompleteResult::Rejected);
    assert(!memcmp(unchanged,stale.stable,sizeof(unchanged)));
    assert(BrookGuardCompleteRepair(stale,"bad",expected)==BrookGuardCompleteResult::Rejected);
    assert(!memcmp(unchanged,stale.stable,sizeof(unchanged)));
    badExpected[124]^=1;
    assert(BrookGuardCompleteRepair(stale,identity,badExpected)==BrookGuardCompleteResult::Rejected);
    assert(!memcmp(unchanged,stale.stable,sizeof(unchanged)));
    stale.visible[1][40]^=1;
    assert(BrookGuardCompleteRepair(stale,identity,expected)==BrookGuardCompleteResult::Rejected);
    for(int slot=0;slot<2;++slot) {
        Io dropped=Initial();assert(BrookGuardBeginRepair(dropped,identity)==BrookGuardBeginResult::PendingDurable);
        dropped.operation=0;dropped.drop=slot;
        assert(BrookGuardCompleteRepair(dropped,identity,expected)==BrookGuardCompleteResult::Uncertain);
    }
}
