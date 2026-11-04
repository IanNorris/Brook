#pragma once
#include <stdint.h>
namespace brook {
struct BdwBcsSample {uint32_t head=0,tail=0,mode=0;};
struct BdwForcewakeResult {
    bool requested=false,acquired=false,threadReady=false,sampled=false,idle=false,released=false;
    BdwBcsSample first{},second{};
};
// Io must hold exclusive ownership of request bit 0, provide verified UC
// mappings, and a verified monotonic millisecond clock. A successful clear
// write must drain all earlier potentially posted writes, including failed
// writes, before ACK is sampled. A successful
// release requires an ACK-clear observation; uncertain cleanup must quarantine
// further GT use. This operation does not hand off engines or grant GGTT slots.
template<class Io> BdwForcewakeResult BdwObserveBcs(Io& io) {
    constexpr uint32_t request=0xa188,ack=0x130044,thread=0x13805c;
    constexpr uint32_t head=0x22034,tail=0x22030,mode=0x2209c;
    BdwForcewakeResult result{};uint32_t value=0;
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
            auto sample=[&](BdwBcsSample& s){return io.read(head,s.head) && io.read(tail,s.tail) && io.read(mode,s.mode) &&
                s.head!=0xffffffffu && s.tail!=0xffffffffu && s.mode!=0xffffffffu;};
            if(sample(result.first) && sample(result.second)) {
                result.sampled=true;
                const auto idle=[](const BdwBcsSample& s){return (s.head&0x001ffffcu)==(s.tail&0x001ffff8u) && (s.mode&(1u<<9));};
                result.idle=idle(result.first) && idle(result.second) &&
                    result.first.head==result.second.head && result.first.tail==result.second.tail && result.first.mode==result.second.mode;
            }
        }
    }
    // Even a failed write may have reached the hardware: always clear our bit.
    const bool cleared=io.write(request,0x00010000u);
    result.released=cleared && wait(ack,1,0,50);
    return result;
}
}
