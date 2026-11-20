#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include "../../drivers/intel_hda/hda_dell_audio_bridge.h"
using namespace brook;
struct Lock {std::atomic<bool> held{false};void lock(){while(held.exchange(true)){};}bool tryLock(){return !held.exchange(true);}void unlock(){held.store(false);}};
struct Owner {
    uint8_t cpu[65536]{},device[65536]{};uint64_t ns=1000000000;uint32_t pos=0;
    unsigned starts=0,stops=0,mutes=0,unmutes=0,retires=0,quarantines=0,retained=0,reprepares=0;
    bool running=false,failPosition=false,moveOnPublish=false,failReprepare=false,failUnmute=false,pinMuted=true;
    uint64_t nowNs(){ns+=1000000;return ns;}
    bool position(uint32_t& p,bool& error,bool& run){if(failPosition)return false;pos=(pos+192)%65536;p=pos;error=false;run=running;return true;}
    bool write(uint32_t o,const uint8_t* data,uint32_t n){assert(o<=65536 && n<=65536-o);if(data)std::memcpy(cpu+o,data,n);else std::memset(cpu+o,0,n);return true;}
    bool publish(uint32_t o,uint32_t n){assert(o<=65536 && n<=65536-o);if(n){const auto begin=o&~63u,end=(o+n+63)&~63u;std::memcpy(device+begin,cpu+begin,end-begin);}
        if(moveOnPublish){pos=(pos+8192)%65536;ns+=47000000;moveOnPublish=false;}return true;}
    bool fence(){return true;}bool start(){++starts;running=true;return true;}
    bool mute(){++mutes;pinMuted=true;return true;}bool unmute(){++unmutes;pinMuted=false;return !failUnmute;}
    bool stop(){++stops;running=false;return true;}
    bool retire(){++retires;return true;}void quarantine(){++quarantines;}
    bool retireRetained(){++retained;return true;}
    bool prepareRetained(){++reprepares;if(failReprepare)return false;pos=0;return true;}
};
// Recovery owner: verified register evidence, and retire/reprepare outcomes.
struct RecoveryOwner:Owner {
    bool failRetire=false,clockDead=false;uint8_t status=0;
    uint64_t nowNs(){if(clockDead)return 0;return Owner::nowNs();}
    void faultRegisters(HdaDellStreamRegisters& r){r={};r.valid=true;r.command=running?6:2;r.control=running?2:0;r.status=status;}
    bool retireRetained(){++retained;return !failRetire;}
};
static void Recovery(){
    uint8_t samples[128];std::memset(samples,0x5a,sizeof(samples));AudioDeviceProgress p{};
    // Default: an admitted-type fault is still quarantined and restart is
    // permanently refused without touching the owner.
    {RecoveryOwner o;Lock l;HdaDellAudioBridge<RecoveryOwner,Lock,true> b(o,l,64,4096);
        assert(b.submit(samples,sizeof(samples)).accepted && !b.restartPermanentlyRefused());o.ns+=AudioDmaClock::MaxSampleNs;
        assert(b.progress(&p) && p.discontinuity && o.quarantines==1 && b.faultCount()==1 && !b.recovery().enabled);
        assert(b.restartPermanentlyRefused() && !b.restart(p.epoch) && !o.retained && !o.reprepares);
        assert(b.lastFault().reason==HdaDellFaultReason::ServiceLate && !b.lastFault().recoverable);}
    // Enabled: one verified retire/reprepare per fault, then a new epoch.
    {RecoveryOwner o;Lock l;HdaDellAudioBridge<RecoveryOwner,Lock,true> b(o,l,64,4096);assert(b.enableFaultRecovery(2,10000000000ULL));
        auto r=b.submit(samples,sizeof(samples));assert(r.accepted);o.ns+=AudioDmaClock::MaxSampleNs;
        assert(b.progress(&p) && p.discontinuity && !o.quarantines && !o.running && b.recovery().pending && !b.restartPermanentlyRefused());
        auto submitted=b.submit(samples,sizeof(samples));assert(submitted.error && !submitted.accepted); // no data while faulted
        assert(!b.restart(p.epoch+1) && !o.retained && b.recovery().pending); // stale epoch consumes nothing
        assert(b.restart(p.epoch) && o.retained==1 && o.reprepares==1 && !o.quarantines);
        auto st=b.recovery();assert(st.successes==1 && st.attempts==1 && !st.pending && !st.permanent && st.lastRecoveryNs);
        assert(b.progress(&p) && !p.discontinuity && p.epoch==r.epoch+1);
        r=b.submit(samples,sizeof(samples));assert(r.accepted && r.epoch==p.epoch && o.starts==2 && o.running);
        // A second fault within the spacing window is terminal.
        o.ns+=AudioDmaClock::MaxSampleNs;assert(b.progress(&p) && p.discontinuity && b.recovery().pending);
        assert(!b.restart(p.epoch) && o.quarantines==1 && o.retained==1);
        st=b.recovery();assert(st.permanent && st.lastFailure==HdaDellRecoveryStep::Admission && st.attempts==2);
        assert(b.restartPermanentlyRefused() && !b.restart(p.epoch) && b.recovery().attempts==2);
        assert(b.firstFault().ordinal==1 && b.lastFault().ordinal==2 && b.faultCount()==2);}
    // Budget: after the last allowed recovery, the next fault quarantines at once.
    {RecoveryOwner o;Lock l;HdaDellAudioBridge<RecoveryOwner,Lock,true> b(o,l,64,4096);assert(b.enableFaultRecovery(1,1000000));
        assert(b.submit(samples,sizeof(samples)).accepted);o.ns+=AudioDmaClock::MaxSampleNs;assert(b.progress(&p) && b.restart(p.epoch));
        assert(b.submit(samples,sizeof(samples)).accepted);o.ns+=AudioDmaClock::MaxSampleNs;
        assert(b.progress(&p) && p.discontinuity && o.quarantines==1 && !b.recovery().pending && b.restartPermanentlyRefused());}
    // Spacing satisfied: a later fault recovers again.
    {RecoveryOwner o;Lock l;HdaDellAudioBridge<RecoveryOwner,Lock,true> b(o,l,64,4096);assert(b.enableFaultRecovery(3,1000000000ULL));
        for(unsigned i=0;i<3;++i){assert(b.submit(samples,sizeof(samples)).accepted);
            for(unsigned k=0;k<1200;++k)assert(b.progress(&p) && !p.discontinuity); // >1 s of healthy service
            o.ns+=AudioDmaClock::MaxSampleNs;assert(b.progress(&p) && p.discontinuity && b.restart(p.epoch));}
        assert(b.recovery().successes==3 && !o.quarantines);}
    // Each verification failure is terminal and quarantines.
    {RecoveryOwner o;Lock l;HdaDellAudioBridge<RecoveryOwner,Lock,true> b(o,l,64,4096);assert(b.enableFaultRecovery(2,0));o.failRetire=true;
        assert(b.submit(samples,sizeof(samples)).accepted);o.ns+=AudioDmaClock::MaxSampleNs;assert(b.progress(&p));
        assert(!b.restart(p.epoch) && o.quarantines==1 && !o.reprepares && b.recovery().lastFailure==HdaDellRecoveryStep::Retire);
        assert(b.restartPermanentlyRefused() && !b.restart(p.epoch) && o.retained==1);}
    {RecoveryOwner o;Lock l;HdaDellAudioBridge<RecoveryOwner,Lock,true> b(o,l,64,4096);assert(b.enableFaultRecovery(2,0));o.failReprepare=true;
        assert(b.submit(samples,sizeof(samples)).accepted);o.ns+=AudioDmaClock::MaxSampleNs;assert(b.progress(&p));
        assert(!b.restart(p.epoch) && o.quarantines==1 && b.recovery().lastFailure==HdaDellRecoveryStep::Reprepare);
        o.failReprepare=false;assert(!b.restart(p.epoch) && o.reprepares==1);}
    {RecoveryOwner o;Lock l;HdaDellAudioBridge<RecoveryOwner,Lock,true> b(o,l,64,4096);assert(b.enableFaultRecovery(2,0));
        assert(b.submit(samples,sizeof(samples)).accepted);o.ns+=AudioDmaClock::MaxSampleNs;assert(b.progress(&p));o.clockDead=true;
        assert(!b.restart(p.epoch) && o.quarantines==1 && !o.retained && b.recovery().lastFailure==HdaDellRecoveryStep::Admission);}
    // Hardware-reported faults are never admitted: DESE latched.
    {RecoveryOwner o;Lock l;HdaDellAudioBridge<RecoveryOwner,Lock,true> b(o,l,64,4096);assert(b.enableFaultRecovery(2,0));
        assert(b.submit(samples,sizeof(samples)).accepted);o.status=0x10;o.ns+=AudioDmaClock::MaxSampleNs;
        assert(b.progress(&p) && o.quarantines==1 && b.restartPermanentlyRefused() && !b.restart(p.epoch) && !o.retained);}
    // stop() or shutdown() while a recovery is pending quarantines instead.
    {RecoveryOwner o;Lock l;HdaDellAudioBridge<RecoveryOwner,Lock,true> b(o,l,64,4096);assert(b.enableFaultRecovery(2,0));
        assert(b.submit(samples,sizeof(samples)).accepted);o.ns+=AudioDmaClock::MaxSampleNs;assert(b.progress(&p) && b.recovery().pending);
        assert(!b.stop() && o.quarantines==1 && !b.restart(p.epoch) && !o.retained);}
    {RecoveryOwner o;Lock l;HdaDellAudioBridge<RecoveryOwner,Lock,true> b(o,l,64,4096);assert(b.enableFaultRecovery(2,0));
        assert(b.submit(samples,sizeof(samples)).accepted);o.ns+=AudioDmaClock::MaxSampleNs;assert(b.progress(&p));
        assert(!b.shutdown() && o.quarantines==1 && !b.restart(p.epoch) && !o.retires);}
    // Recovery cannot be enabled on a started or closed bridge, nor without clean restart.
    {RecoveryOwner o;Lock l;HdaDellAudioBridge<RecoveryOwner,Lock,true> b(o,l,64,4096);assert(b.submit(samples,sizeof(samples)).accepted);
        assert(!b.enableFaultRecovery(2,0));}
    {RecoveryOwner o;Lock l;HdaDellAudioBridge<RecoveryOwner,Lock> b(o,l,64,4096);assert(!b.enableFaultRecovery(2,0));}
}
int main(){
    Recovery();
    Owner owner;Lock lock;HdaDellAudioBridge<Owner,Lock> b(owner,lock,64,4096);AudioDeviceProgress p{};
    assert(b.progress(&p) && p.epoch==1 && p.capacity==65536 && !p.running && !p.discontinuity && !owner.starts);
    uint8_t samples[128];std::memset(samples,0x2d,sizeof(samples));
    auto invalid=b.submit(samples,3);assert(invalid.error==-22 && !owner.starts);
    assert(b.play(samples,sizeof(samples),48000,2,16)==-22 && !owner.starts);
    lock.lock();auto busy=b.submit(samples,sizeof(samples));lock.unlock();assert(!busy.accepted && !busy.error && !owner.starts);
    auto r=b.submit(samples,sizeof(samples));assert(!r.error && r.accepted==sizeof(samples) && owner.starts==1 && b.isPlaying());
    const auto clockBefore=b.drainClockNs();assert(clockBefore && b.drainClockNs()>clockBefore);
    for(unsigned i=0;i<sizeof(samples);++i)assert(owner.device[(r.begin+i)%65536]==0x2d);
    assert(b.progress(&p) && p.epoch==r.epoch && p.submitted>=r.begin+r.accepted && p.running);
    assert(b.stop() && owner.mutes==1 && owner.stops==1 && owner.retires==1 && !owner.quarantines);
    assert(!b.isPlaying() && b.pending()==0 && !b.drainClockNs() && b.progress(&p) && p.discontinuity && !b.restart(p.epoch));
    auto closed=b.submit(samples,sizeof(samples));assert(!closed.accepted && closed.error);
    Owner noStart;Lock noStartLock;HdaDellAudioBridge<Owner,Lock> idle(noStart,noStartLock,64,4096);
    assert(idle.stop() && !noStart.starts && noStart.mutes==1 && noStart.stops==1 && noStart.retires==1);
    Owner broken;Lock brokenLock;HdaDellAudioBridge<Owner,Lock> fault(broken,brokenLock,64,4096);
    r=fault.submit(samples,sizeof(samples));assert(r.accepted==sizeof(samples));broken.failPosition=true;
    assert(fault.progress(&p) && p.discontinuity && broken.quarantines==1 && broken.mutes==1 && broken.stops==1);
    assert(!fault.restart(p.epoch) && !fault.stop() && !broken.retires);
    Owner uncertain;Lock uncertainLock;HdaDellAudioBridge<Owner,Lock> legacy(uncertain,uncertainLock,64,4096);
    assert(legacy.play(samples,sizeof(samples),44100,2,16)==int(sizeof(samples)));uncertain.moveOnPublish=true;
    assert(legacy.play(samples,sizeof(samples),44100,2,16)==int(sizeof(samples)) && uncertain.quarantines==1);
    assert(legacy.play(samples,sizeof(samples),44100,2,16)<0);
    Owner invalidOwner;Lock invalidLock;HdaDellAudioBridge<Owner,Lock> invalidGeometry(invalidOwner,invalidLock,0,4096);
    assert(!invalidGeometry.progress(&p) && !invalidOwner.starts);
    Owner reusable;Lock reuseLock;HdaDellAudioBridge<Owner,Lock,true> reuse(reusable,reuseLock,64,4096);
    r=reuse.submit(samples,sizeof(samples));assert(r.accepted==sizeof(samples));const auto oldEpoch=r.epoch;
    assert(reuse.stop() && reusable.retained==1 && !reusable.retires);
    assert(reuse.progress(&p) && p.epoch==oldEpoch && p.discontinuity);
    assert(!reuse.restart(oldEpoch+1) && !reusable.reprepares);
    assert(reuse.restart(oldEpoch) && reusable.reprepares==1);
    assert(reuse.progress(&p) && p.epoch==oldEpoch+1 && !p.discontinuity && !p.running);
    r=reuse.submit(samples,sizeof(samples));assert(r.epoch==oldEpoch+1 && r.accepted==sizeof(samples));
    assert(reuse.stop() && reuse.restart(oldEpoch+1));
    assert(reuse.shutdown() && reusable.retires==1);assert(!reuse.restart(oldEpoch+2));
    Owner stoppedBefore;Lock stoppedBeforeLock;HdaDellAudioBridge<Owner,Lock,true> early(stoppedBefore,stoppedBeforeLock,64,4096);
    assert(early.stop() && !stoppedBefore.starts && early.restart(1));
    assert(early.submit(samples,sizeof(samples)).epoch==2);
    assert(early.stop() && early.shutdown() && stoppedBefore.retires==1 && !early.restart(2));
    Owner retryFailure;Lock retryLock;HdaDellAudioBridge<Owner,Lock,true> retry(retryFailure,retryLock,64,4096);
    assert(retry.submit(samples,sizeof(samples)).accepted && retry.stop());retryFailure.failReprepare=true;
    assert(!retry.restart(1) && retryFailure.quarantines==1 && retry.progress(&p) && p.epoch==1 && p.discontinuity);
    retryFailure.failReprepare=false;assert(!retry.restart(1) && retryFailure.reprepares==1);
    Owner output;Lock outputLock;HdaDellAudioBridge<Owner,Lock,true,true> controllable(output,outputLock,64,4096);
    HdaDellOutputRequest req{false,20};auto read=[&]{return req;};
    assert(controllable.reconcileOutput(read) && output.pinMuted && !output.unmutes);
    assert(controllable.submit(samples,sizeof(samples)).accepted && controllable.reconcileOutput(read) && output.pinMuted);
    req.authorized=true;assert(controllable.reconcileOutput(read) && !output.pinMuted && output.unmutes==1);
    req.control=BROOK_AUDIO_MUTED|20;assert(controllable.reconcileOutput(read) && output.pinMuted);
    req.control=20;assert(controllable.reconcileOutput(read) && !output.pinMuted && output.unmutes==2);
    assert(controllable.stop() && output.pinMuted && controllable.restart(1));
    assert(controllable.reconcileOutput(read) && output.pinMuted); // never unmute before new RUN
    assert(controllable.submit(samples,sizeof(samples)).accepted && controllable.reconcileOutput(read) && !output.pinMuted);
    assert(controllable.shutdown() && output.pinMuted && !controllable.reconcileOutput(read));
    Owner racing;Lock racingLock;HdaDellAudioBridge<Owner,Lock,true,true> race(racing,racingLock,64,4096);
    assert(race.submit(samples,sizeof(samples)).accepted);unsigned reads=0;
    auto racingControl=[&]{++reads;return HdaDellOutputRequest{true,reads==1?20u:uint32_t(BROOK_AUDIO_MUTED|20)};};
    assert(race.reconcileOutput(racingControl) && reads==2 && racing.unmutes==1 && racing.pinMuted && racing.mutes==1);
    Owner partialUnmute;Lock partialLock;HdaDellAudioBridge<Owner,Lock,true,true> partialBridge(partialUnmute,partialLock,64,4096);
    assert(partialBridge.submit(samples,sizeof(samples)).accepted);partialUnmute.failUnmute=true;
    assert(!partialBridge.reconcileOutput(read) && partialUnmute.pinMuted && !partialUnmute.running && partialUnmute.quarantines==1);
    assert(partialBridge.progress(&p) && p.discontinuity && !partialBridge.restart(p.epoch));
    for(unsigned i=0;i<32;++i){
        Owner concurrent;Lock concurrentLock;HdaDellAudioBridge<Owner,Lock,true> cycle(concurrent,concurrentLock,64,4096);
        assert(cycle.submit(samples,sizeof(samples)).accepted && cycle.stop());std::atomic<bool> go{false};
        std::thread restartThread([&]{while(!go.load()){}cycle.restart(1);});
        std::thread shutdownThread([&]{while(!go.load()){}assert(cycle.shutdown());});
        go.store(true);restartThread.join();shutdownThread.join();
        const unsigned starts=concurrent.starts,reprepares=concurrent.reprepares;
        assert(!cycle.restart(1) && !cycle.restart(2) && cycle.submit(samples,sizeof(samples)).error);
        assert(!concurrent.running && concurrent.retires==1 && !concurrent.quarantines &&
               concurrent.starts==starts && concurrent.reprepares==reprepares);
    }
}
