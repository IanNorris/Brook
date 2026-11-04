#pragma once
#include <stdint.h>
namespace brook {
struct BdwBcsSample {uint32_t head=0,tail=0,mode=0,start=0,control=0,execlistLo=0,execlistHi=0,submissionMode=0;};
struct BdwEngineObservation {bool sampled=false,idle=false,submissionQuiescent=false;BdwBcsSample first{},second{};};
struct BdwForcewakeResult {
    bool requested=false,acquired=false,threadReady=false,sampled=false,idle=false,submissionQuiescent=false,released=false;
    bool otherEnginesRequested=false,otherEnginesSampled=false,otherEnginesQuiescent=false;
    BdwBcsSample first{},second{};BdwEngineObservation other[3]{};
};
// A point-in-time sample only. The caller must separately exclude future
// submissions and account for display consumers before claiming the GGTT.
template<class Io> BdwEngineObservation BdwObserveEngine(Io& io,uint32_t base) {
    BdwEngineObservation r{};
    auto read=[&](uint32_t offset,uint32_t& value){return io.read(offset,value) && value!=0xffffffffu;};
    auto sample=[&](BdwBcsSample& s){return read(base+0x34,s.head) && read(base+0x30,s.tail) &&
        read(base+0x9c,s.mode) && read(base+0x38,s.start) && read(base+0x3c,s.control) &&
        read(base+0x234,s.execlistLo) && read(base+0x238,s.execlistHi) && read(base+0x29c,s.submissionMode);};
    if(!sample(r.first) || !sample(r.second))return r;
    r.sampled=true;
    const auto idle=[](const BdwBcsSample& s){return (s.head&0x001ffffcu)==(s.tail&0x001ffff8u) && (s.mode&(1u<<9));};
    r.idle=idle(r.first) && idle(r.second) && r.first.head==r.second.head && r.first.tail==r.second.tail &&
        r.first.mode==r.second.mode && r.first.start==r.second.start && r.first.control==r.second.control &&
        r.first.execlistLo==r.second.execlistLo && r.first.execlistHi==r.second.execlistHi &&
        r.first.submissionMode==r.second.submissionMode;
    constexpr uint32_t busy=(1u<<2)|(3u<<3)|(3u<<14)|(3u<<17);
    const bool listMode=(r.first.submissionMode&(1u<<15))!=0;
    r.submissionQuiescent=r.idle && !(r.first.control&1u) && (!listMode || !(r.first.execlistLo&busy));
    return r;
}
// Necessary but not sufficient submission evidence. Caller must obtain this
// observation from BCS and retain exclusive engine and CSB ownership through
// submission; neither the sample nor the CSB Boolean establishes ownership.
inline bool BdwExeclistObservationReady(const BdwEngineObservation& r,bool csbOwned) {
    constexpr uint32_t runList=1u<<15,stopRing=1u<<8;
    return csbOwned && r.sampled && r.idle && r.submissionQuiescent &&
        (r.first.submissionMode&runList) && (r.second.submissionMode&runList) &&
        !(r.first.mode&stopRing) && !(r.second.mode&stopRing);
}
// Io must hold exclusive ownership of request bit 0, provide verified UC
// mappings, and a verified monotonic millisecond clock. A successful clear
// write must drain all earlier potentially posted writes, including failed
// writes, before ACK is sampled. A successful
// release requires an ACK-clear observation; uncertain cleanup must quarantine
// further GT use. This operation does not hand off engines or grant GGTT slots.
template<class Io> BdwForcewakeResult BdwObserveBcs(Io& io,bool otherEngines=false) {
    constexpr uint32_t request=0xa188,ack=0x130044,thread=0x13805c;
    BdwForcewakeResult result{};result.otherEnginesRequested=otherEngines;uint32_t value=0;
    // ACK is always-on. REQ is not used to establish ownership: a pre-wake
    // request-register read is not evidence that another actor cannot use it.
    if(!io.owned())return result;
    if(!io.read(ack,value) || value==0xffffffffu || (value&1))return result;
    uint64_t preflight=0;if(!io.monotonicMs(preflight))return result;
    auto wait=[&](uint32_t reg,uint32_t mask,uint32_t expected,uint64_t ms){
        uint64_t start=0;if(!io.monotonicMs(start))return false;
        uint64_t previous=start;
        for(unsigned i=0;i<100000;++i){
            uint32_t v=0;uint64_t now=0;
            if(!io.read(reg,v) || v==0xffffffffu || !io.monotonicMs(now) || now<previous)return false;
            previous=now;
            if(now-start>=ms)return false;
            if((v&mask)==expected)return true;
            io.relax();
        }return false;
    };
    result.requested=true;
    const bool writeOk=io.write(request,0x00010001u);
    if(writeOk && wait(ack,1,1,50)) {
        result.acquired=true;
        if(wait(thread,7,0,5)) {
            result.threadReady=true;
            const auto bcs=BdwObserveEngine(io,0x22000);
            result.first=bcs.first;result.second=bcs.second;result.sampled=bcs.sampled;
            result.idle=bcs.idle;result.submissionQuiescent=bcs.submissionQuiescent;
            if(otherEngines && bcs.sampled) {
                // Broadwell GT2 has RCS0, VCS0 and VECS0 in addition to BCS0.
                constexpr uint32_t bases[]={0x2000,0x12000,0x1a000};
                result.otherEnginesSampled=true;result.otherEnginesQuiescent=true;
                for(unsigned i=0;i<3;++i) {
                    result.other[i]=BdwObserveEngine(io,bases[i]);
                    result.otherEnginesSampled &= result.other[i].sampled;
                    result.otherEnginesQuiescent &= result.other[i].submissionQuiescent;
                    if(!result.other[i].sampled)break;
                }
            }
        }
    }
    // Even a failed write may have reached the hardware: always clear our bit.
    const bool cleared=io.write(request,0x00010000u);
    result.released=cleared && wait(ack,1,0,50);
    return result;
}
}
