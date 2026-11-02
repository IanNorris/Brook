#pragma once
#include <stdint.h>

namespace brook {
// Admission and immediate-command state machine for a separately owned,
// non-playing HDA probe. The caller must establish PCI ownership, an
// appropriately mapped MMIO aperture and exclusive access for its lifetime.
// PCI identity checking does not validate BAR extent, physical-address width,
// decode, power state, mapping aliases or ownership.
enum class HdaProbeResult : uint8_t {
    Ready, InvalidIdentity, InvalidBar, InvalidCapabilities, ControllerReset,
    ActiveInterrupts, ActiveRing, ActiveStream, Busy, DisallowedVerb,
    Timeout, Failed
};
struct HdaProbePci {
    uint16_t vendor, device, subsystemVendor, subsystemDevice;
    uint8_t bus, slot, function, classCode, subclass;
    uint32_t bar0, bar1;
};
inline HdaProbeResult HdaProbePciCheck(const HdaProbePci& p,uint64_t& base) {
    base=0;
    if(p.vendor!=0x8086 || p.device!=0x9ca0 || p.subsystemVendor!=0x1028 ||
       p.subsystemDevice!=0x0665 || p.bus!=0 || p.slot!=0x1b || p.function!=0 ||
       p.classCode!=4 || p.subclass!=3)return HdaProbeResult::InvalidIdentity;
    // Intel 9-series PCH D27:F0 HdBAR is non-prefetchable, 64-bit and 16 KiB.
    // This syntactic check still does not establish resource ownership.
    if((p.bar0&15)!=4 || p.bar0==0xffffffff || p.bar1==0xffffffff)return HdaProbeResult::InvalidBar;
    const uint64_t address=(uint64_t(p.bar1)<<32)|(p.bar0&~uint32_t(15));
    if(!address || (address&16383) || address>~uint64_t(0)-16384)return HdaProbeResult::InvalidBar;
    base=address;return HdaProbeResult::Ready;
}
struct HdaProbeController {
    uint16_t gcap=0, state=0, immediate=0;
    uint32_t gctl=0, interrupts=0;
    uint8_t major=0, minor=0, corb=0, rirb=0, streams=0;
};
// read8/read16/read32 are bounded MMIO reads supplied by the caller. No writes
// are made here. Inspect all advertised stream descriptors before admitting.
template<class Io> HdaProbeResult HdaProbeInspect(Io& io,HdaProbeController& output) {
    output={};HdaProbeController s{};
    s.gcap=io.read16(0);s.minor=io.read8(2);s.major=io.read8(3);
    s.gctl=io.read32(8);s.state=io.read16(0x0e);s.interrupts=io.read32(0x20);
    s.corb=io.read8(0x4c);s.rirb=io.read8(0x5c);s.immediate=io.read16(0x68);
    const unsigned inputs=(s.gcap>>8)&15,outputs=(s.gcap>>12)&15,bidir=(s.gcap>>3)&31;
    const unsigned total=inputs+outputs+bidir;
    if(s.gcap==0xffff || !outputs || !s.major || s.major==0xff || total>30)return HdaProbeResult::InvalidCapabilities;
    s.streams=static_cast<uint8_t>(total);output=s;
    if(!(s.gctl&1))return HdaProbeResult::ControllerReset;
    if(s.interrupts)return HdaProbeResult::ActiveInterrupts;
    if((s.corb&2) || (s.rirb&2))return HdaProbeResult::ActiveRing;
    for(unsigned i=0;i<total;++i)if(io.read8(0x80+i*0x20)&2)return HdaProbeResult::ActiveStream;
    if(s.immediate&1)return HdaProbeResult::Busy;
    return HdaProbeResult::Ready;
}
inline bool HdaProbeAllowedGet(uint16_t verb,uint8_t payload) {
    if(verb==0xf00) {
        switch(payload) {case 0:case 1:case 2:case 4:case 5:case 8:case 9:case 10:case 11:
                         case 12:case 13:case 14:case 15:case 17:case 18:return true;default:return false;}
    }
    switch(verb) {case 0xf01:case 0xf02:case 0xf05:case 0xf06:case 0xf07:case 0xf0c:
                  case 0xf15:case 0xf16:case 0xf17:case 0xf1c:case 0xf20:return true;default:return false;}
}
// Immediate transport, valid only after Ready inspection and ownership.
// Failure is terminal for this instance; no retry or controller reset occurs.
template<class Io> class HdaReadOnlyProbe {
    Io& io_;bool failed_=false,admitted_=false;
    // ticks() must return elapsed milliseconds. The iteration bound is a
    // fallback for a stalled clock; it may expire before 50 milliseconds.
    bool wait(uint16_t mask,uint16_t expected) {
        const uint64_t start=io_.ticks();
        for(unsigned n=0;n<100000;++n) {
            if((io_.read16(0x68)&mask)==expected)return true;
            const uint64_t now=io_.ticks();if(now>=start && now-start>=50)break;
            io_.pause();
        }
        return false;
    }
    HdaProbeResult issue(uint32_t command,uint32_t& response) {
        if(!wait(1,0)){failed_=true;return HdaProbeResult::Timeout;}
        io_.write16(0x68,2); // response-valid is write-one-to-clear
        if(!wait(3,0)){failed_=true;return HdaProbeResult::Timeout;}
        io_.write32(0x60,command);
        io_.write16(0x68,1);
        if(!wait(3,2)){failed_=true;return HdaProbeResult::Timeout;}
        response=io_.read32(0x64);return HdaProbeResult::Ready;
    }
public:
    explicit HdaReadOnlyProbe(Io& io):io_(io) {}
    HdaProbeResult inspect(HdaProbeController& snapshot) {
        if(failed_ || admitted_)return HdaProbeResult::Failed;
        const auto result=HdaProbeInspect(io_,snapshot);
        if(result==HdaProbeResult::Ready)admitted_=true;
        else failed_=true;
        return result;
    }
    HdaProbeResult read(uint8_t codec,uint8_t nid,uint16_t verb,uint8_t payload,uint32_t& response) {
        if(failed_ || !admitted_)return HdaProbeResult::Failed;
        if(codec>=15 || nid>0x7f || !HdaProbeAllowedGet(verb,payload))return HdaProbeResult::DisallowedVerb;
        return issue((uint32_t(codec)<<28)|(uint32_t(nid)<<20)|(uint32_t(verb)<<8)|payload,response);
    }
    // GET amplifier gain/mute uses a four-bit verb and a 16-bit payload.
    // Only construct this GET verb; no caller-supplied verb or reserved bits.
    HdaProbeResult readAmp(uint8_t codec,uint8_t nid,bool output,bool left,uint8_t index,uint32_t& response) {
        if(failed_ || !admitted_)return HdaProbeResult::Failed;
        if(codec>=15 || nid>0x7f || index>15)return HdaProbeResult::DisallowedVerb;
        const uint16_t payload=(output?0x8000:0)|(left?0x2000:0)|index;
        return issue((uint32_t(codec)<<28)|(uint32_t(nid)<<20)|(uint32_t(0x0b)<<16)|payload,response);
    }
};
}
