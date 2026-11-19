#include <cassert>
#include <cstdint>
#include <cstring>
#include "../../drivers/intel_hda/hda_dell_stream_owner.h"
#include "../../drivers/intel_hda/hda_dell_stream_queue.h"
#include "../test_hda_dell_tone_integration/fake_hda_chip.h"
using namespace brook;
struct Platform:Chip {
    alignas(4096) uint8_t cpu[17][4096]{},device[17][4096]{};
    unsigned allocated=0,freed=0;uint64_t ns=1000000000;uint16_t pci=0;bool held=false,denyMaster=false,denyFinish=false,identity=true;
    bool identityStable(){return identity;}uint16_t command(){return pci;}
    void setCommand(uint16_t v){if(v==6){assert(published && reinterpret_cast<HdaToneBdlEntry*>(device[0])[0].address==0x2000);if(denyMaster)return;}
        if(v==0 && pci==2 && denyFinish)return;pci=v;}
    bool controllerIdle(){return !(read16(base)&2);}
    bool allocatePage(uint64_t& pa,void*& va){if(allocated==17)return false;va=cpu[allocated];pa=0x1000+uint64_t(allocated++)*4096;return true;}
    bool pageAllowed(uint64_t pa,void* va){return pa && va;}
    void clearPage(void* p){std::memset(p,0,4096);}void freePage(uint64_t){assert(!(pci&4));++freed;}
    void line(void* va){auto a=reinterpret_cast<uintptr_t>(va);for(unsigned i=0;i<17;++i){auto b=reinterpret_cast<uintptr_t>(cpu[i]);
        if(a>=b && a<b+4096){assert(!((a-b)&63));std::memcpy(device[i]+a-b,cpu[i]+a-b,64);return;}}assert(false);}
    bool fence(){published=true;return true;}
    uint64_t nowNs(){ns+=1000000;return ns;}
    void write16(uint32_t o,uint16_t v){Chip::write16(o,v);if(o==base && v==1 && (Chip::read16(o)&1))pos=0;}
    uint32_t read32(uint32_t o){if(o==base+4 && (read16(base)&2)){pos=(pos+192)%65536;return pos;}return Chip::read32(o);}
    void quarantine(){held=true;}
};
// Real owner + queue over the fake controller: the recovery path is the
// owner's verified stop, retire (SRST + descriptor clear) and reprepare.
static void FaultRecovery(const HdaSessionReport& snapshot){
    using Owner=HdaDellStreamOwner<Platform>;uint8_t samples[128];std::memset(samples,0x39,sizeof(samples));
    {Platform p;p.format=0x31;Owner owner(p,snapshot,64);assert(owner.prepare());
        HdaDellStreamQueue<Owner> q(owner,64,4096);q.allowRecovery(true);assert(q.start());
        assert(q.submit(samples,sizeof(samples)).accepted);for(unsigned i=0;i<10;++i)assert(q.service());
        p.ns+=AudioDmaClock::MaxSampleNs;assert(!q.service() && q.recoverable());
        auto f=q.lastFault();assert(f.reason==HdaDellFaultReason::ServiceLate && f.muted && f.stopped && f.recoverable);
        assert(f.atFault.valid && (f.atFault.control&2) && f.atFault.command==6 && f.atFault.bdpl==0x1000 && f.atFault.cbl==65536);
        assert(f.afterStop.valid && !(f.afterStop.control&3) && f.afterStop.command==2);
        assert(!p.held && p.pci==2 && p.pin[0]==0x80 && p.pin[1]==0x80 && !owner.failureStage());
        const auto epoch=q.progress().epoch;
        assert(owner.retireRetained() && owner.prepareRetained() && q.recoverAfterVerifiedFault(epoch));
        assert(p.allocated==17 && !p.freed && !p.held && p.pos==0);
        assert(q.start() && p.pci==6);for(unsigned i=1;i<17;++i)for(unsigned j=0;j<4096;++j)assert(p.device[i][j]==0);
        auto r=q.submit(samples,sizeof(samples));assert(r.accepted && r.epoch==epoch+1);for(unsigned i=0;i<10;++i)assert(q.service());
        assert(q.shutdown() && owner.retire() && p.freed==17 && p.pci==0 && !p.held);}
    // FIFOE alone (output underrun) is admitted; the reprepare clears it.
    {Platform p;p.format=0x31;Owner owner(p,snapshot,64);assert(owner.prepare());
        HdaDellStreamQueue<Owner> q(owner,64,4096);q.allowRecovery(true);assert(q.start());
        p.mem[Chip::base+3]|=0x08;assert(!q.service() && q.recoverable() && q.lastFault().reason==HdaDellFaultReason::DeviceError);
        assert(q.lastFault().atFault.status&0x08);
        assert(owner.retireRetained() && owner.prepareRetained() && q.recoverAfterVerifiedFault(q.progress().epoch));
        assert(!(p.mem[Chip::base+3]&0x18) && q.start() && q.service());}
    // DESE (descriptor error) is never admitted: quarantined, pages retained.
    {Platform p;p.format=0x31;Owner owner(p,snapshot,64);assert(owner.prepare());
        HdaDellStreamQueue<Owner> q(owner,64,4096);q.allowRecovery(true);assert(q.start());
        p.mem[Chip::base+3]|=0x10;assert(!q.service() && !q.recoverable() && p.held && !p.freed && p.pci==2);
        assert(owner.failureStage()==10 && !owner.retireRetained() && !owner.prepareRetained());}
    // A stop that never observes RUN clear is never admitted.
    {Platform p;p.format=0x31;Owner owner(p,snapshot,64);assert(owner.prepare());
        HdaDellStreamQueue<Owner> q(owner,64,4096);q.allowRecovery(true);assert(q.start());p.dropStop=true;
        p.ns+=AudioDmaClock::MaxSampleNs;assert(!q.service() && !q.recoverable() && p.held && p.pci==6 && !q.lastFault().stopped);}
    // A stream reset that does not complete during retire quarantines.
    {Platform p;p.format=0x31;Owner owner(p,snapshot,64);assert(owner.prepare());
        HdaDellStreamQueue<Owner> q(owner,64,4096);q.allowRecovery(true);assert(q.start());
        p.ns+=AudioDmaClock::MaxSampleNs;assert(!q.service() && q.recoverable());p.dropRetireReset=true;
        assert(!owner.retireRetained() && p.held && !p.freed && p.pin[0]==0x80 && owner.failureStage()==14);
        q.abandonRecovery();assert(!q.recoverAfterVerifiedFault(q.progress().epoch));}
    // Identity change before reprepare quarantines.
    {Platform p;p.format=0x31;Owner owner(p,snapshot,64);assert(owner.prepare());
        HdaDellStreamQueue<Owner> q(owner,64,4096);q.allowRecovery(true);assert(q.start());
        p.ns+=AudioDmaClock::MaxSampleNs;assert(!q.service() && owner.retireRetained());p.identity=false;
        assert(!owner.prepareRetained() && p.held && !p.freed && owner.failureStage()==20);}
}
int main(){
    auto snapshot=makeSnapshot();snapshot.converterFormat=0x31;FaultRecovery(snapshot);
    Platform p;p.format=0x31;HdaDellStreamOwner<Platform> owner(p,snapshot,64);assert(owner.prepare());
    HdaDellStreamQueue<HdaDellStreamOwner<Platform>> q(owner,64,4096);assert(q.start());
    assert(p.pci==6 && p.pin[0]==0x80 && p.pin[1]==0x80 && p.runConverterFormat==0x4011 && p.runStreamFormat==0x4011);
    uint8_t samples[128];std::memset(samples,0x39,sizeof(samples));auto r=q.submit(samples,sizeof(samples));assert(!r.error && r.accepted==sizeof(samples));
    for(unsigned i=0;i<900;++i)assert(q.service());
    assert(q.progress().RawCompleted()>2*65536 && q.shutdown() && p.pci==2 && owner.retire());
    assert(p.pci==0 && p.freed==17 && !p.held && p.format==0x31 && p.converter==0);
    Platform reused;reused.format=0x31;HdaDellStreamOwner<Platform> retained(reused,snapshot,64);assert(retained.prepare());
    HdaDellStreamQueue<HdaDellStreamOwner<Platform>> first(retained,64,4096);assert(first.start());
    assert(first.submit(samples,sizeof(samples)).accepted==sizeof(samples));assert(first.shutdown() && retained.retireRetained());
    assert(reused.allocated==17 && reused.freed==0 && reused.pci==2 && !reused.held && reused.pin[0]==0x80 && reused.pin[1]==0x80);
    const auto previousEpoch=first.progress().epoch;assert(retained.prepareRetained() && reused.allocated==17 && reused.freed==0);
    assert(first.restartAfterCleanStop(previousEpoch) && first.progress().epoch==previousEpoch+1);
    assert(reused.pos==0 && first.start());
    for(unsigned i=1;i<17;++i)for(unsigned j=0;j<4096;++j)assert(reused.device[i][j]==0);
    auto again=first.submit(samples,sizeof(samples));assert(again.accepted==sizeof(samples) && again.epoch==previousEpoch+1);
    assert(first.shutdown() && retained.retire());
    assert(reused.allocated==17 && reused.freed==17 && reused.pci==0 && !reused.held);
    Platform changed;changed.format=0x31;HdaDellStreamOwner<Platform> pinned(changed,snapshot,64);assert(pinned.prepare());
    HdaDellStreamQueue<HdaDellStreamOwner<Platform>> prior(pinned,64,4096);assert(prior.start() && prior.shutdown() && pinned.retireRetained());
    changed.identity=false;assert(!pinned.prepareRetained() && changed.held && changed.freed==0 && changed.pci==2);
    Platform release;release.format=0x31;HdaDellStreamOwner<Platform> released(release,snapshot,64);assert(released.prepare());
    HdaDellStreamQueue<HdaDellStreamOwner<Platform>> stopped(released,64,4096);assert(stopped.start() && stopped.shutdown() && released.retireRetained());
    assert(released.retire() && release.freed==17 && release.pci==0 && !release.held);
    Platform broken;broken.format=0x31;HdaDellStreamOwner<Platform> other(broken,snapshot,64);assert(other.prepare());
    HdaDellStreamQueue<HdaDellStreamOwner<Platform>> bad(other,64,4096);assert(bad.start());broken.dropStop=true;
    assert(!bad.shutdown() && broken.held && broken.freed==0 && broken.pci==6 && !other.retire());
    Platform active;active.pci=6;HdaDellStreamOwner<Platform> busy(active,snapshot,64);assert(!busy.prepare() && !active.allocated && !active.held);
    Platform mismatch;mismatch.identity=false;HdaDellStreamOwner<Platform> noIdentity(mismatch,snapshot,64);
    assert(!noIdentity.prepare() && !mismatch.allocated && !mismatch.held);
    Platform master;master.format=0x31;master.denyMaster=true;HdaDellStreamOwner<Platform> noMaster(master,snapshot,64);assert(noMaster.prepare());
    HdaDellStreamQueue<HdaDellStreamOwner<Platform>> rejected(noMaster,64,4096);assert(!rejected.start() && master.held && !master.freed && master.pci==2);
    Platform retire;retire.format=0x31;HdaDellStreamOwner<Platform> stuck(retire,snapshot,64);assert(stuck.prepare());
    HdaDellStreamQueue<HdaDellStreamOwner<Platform>> retiring(stuck,64,4096);assert(retiring.start() && retiring.shutdown());
    retire.dropBdlClear=true;assert(!stuck.retire() && retire.held && !retire.freed && retire.pci==2 && retire.pin[0]==0x80 && retire.pin[1]==0x80);
    assert(stuck.failureStage()==14 && stuck.failedStreamStage()==10 && (stuck.failedStreamDetail()&4));
    Platform partial;partial.format=0x31;HdaDellStreamOwner<Platform> pinFailure(partial,snapshot,64);assert(pinFailure.prepare());
    HdaDellStreamQueue<HdaDellStreamOwner<Platform>> pins(pinFailure,64,4096);assert(pins.start() && pins.shutdown());
    partial.dropPinRestoreRight=true;assert(!pinFailure.retire() && partial.held && !partial.freed && partial.pin[0]==0x80 && partial.pin[1]==0x80);
    Platform finish;finish.format=0x31;HdaDellStreamOwner<Platform> noFinish(finish,snapshot,64);assert(noFinish.prepare());
    HdaDellStreamQueue<HdaDellStreamOwner<Platform>> finishing(noFinish,64,4096);assert(finishing.start() && finishing.shutdown());
    finish.denyFinish=true;assert(!noFinish.retire() && finish.held && !finish.freed && finish.pci==2 && finish.pin[0]==0x80 && finish.pin[1]==0x80);
}
