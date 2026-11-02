#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "../../drivers/intel_hda/hda_dell_audio_bridge.h"
using namespace brook;
struct Lock {std::atomic<bool> held{false};void lock(){while(held.exchange(true)){};}bool tryLock(){return !held.exchange(true);}void unlock(){held.store(false);}};
struct Owner {
    uint8_t cpu[65536]{},device[65536]{};uint64_t ns=1000000000;uint32_t pos=0;
    unsigned starts=0,stops=0,mutes=0,retires=0,quarantines=0;bool running=false,failPosition=false,moveOnPublish=false;
    uint64_t nowNs(){ns+=1000000;return ns;}
    bool position(uint32_t& p,bool& error,bool& run){if(failPosition)return false;pos=(pos+192)%65536;p=pos;error=false;run=running;return true;}
    bool write(uint32_t o,const uint8_t* data,uint32_t n){assert(o<=65536 && n<=65536-o);if(data)std::memcpy(cpu+o,data,n);else std::memset(cpu+o,0,n);return true;}
    bool publish(uint32_t o,uint32_t n){assert(o<=65536 && n<=65536-o);if(n){const auto begin=o&~63u,end=(o+n+63)&~63u;std::memcpy(device+begin,cpu+begin,end-begin);}
        if(moveOnPublish){pos=(pos+8192)%65536;ns+=47000000;moveOnPublish=false;}return true;}
    bool fence(){return true;}bool start(){++starts;running=true;return true;}
    bool mute(){++mutes;return true;}bool stop(){++stops;running=false;return true;}
    bool retire(){++retires;return true;}void quarantine(){++quarantines;}
};
int main(){
    Owner owner;Lock lock;HdaDellAudioBridge<Owner,Lock> b(owner,lock,64,4096);AudioDeviceProgress p{};
    assert(b.progress(&p) && p.epoch==1 && p.capacity==65536 && !p.running && !p.discontinuity && !owner.starts);
    uint8_t samples[128];std::memset(samples,0x2d,sizeof(samples));
    auto invalid=b.submit(samples,3);assert(invalid.error==-22 && !owner.starts);
    assert(b.play(samples,sizeof(samples),48000,2,16)==-22 && !owner.starts);
    lock.lock();auto busy=b.submit(samples,sizeof(samples));lock.unlock();assert(!busy.accepted && !busy.error && !owner.starts);
    auto r=b.submit(samples,sizeof(samples));assert(!r.error && r.accepted==sizeof(samples) && owner.starts==1 && b.isPlaying());
    for(unsigned i=0;i<sizeof(samples);++i)assert(owner.device[(r.begin+i)%65536]==0x2d);
    assert(b.progress(&p) && p.epoch==r.epoch && p.submitted>=r.begin+r.accepted && p.running);
    assert(b.stop() && owner.mutes==1 && owner.stops==1 && owner.retires==1 && !owner.quarantines);
    assert(!b.isPlaying() && b.pending()==0 && b.progress(&p) && p.discontinuity && !b.restart(p.epoch));
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
}
