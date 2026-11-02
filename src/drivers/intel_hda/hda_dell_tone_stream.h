#pragma once
#include <stdint.h>

namespace brook {
// Polling-only HDA output stream control. Io supplies bounded register
// accesses to the already-owned controller, a monotonic millisecond clock and
// a bounded delayUs() that returns false unless it waited at least the requested
// microseconds; a stalled or unavailable clock must never be treated as success.
// No PCI, codec, DMA allocation or memory freeing is performed here.
enum class HdaToneStreamState : uint8_t {Idle,Prepared,Running,Stopped,Unknown};
template<class Io> class HdaDellToneStream {
    Io& io_;uint32_t base_=0,detail_=0,progressFirst_=0,progressLast_=0,ringBytes_=0;uint8_t stage_=0,priority_=0,progressReason_=0,progressStatus_=0;uint16_t progressControl_=0,fifoSize_=0;bool fifoSampled_=false;HdaToneStreamState state_=HdaToneStreamState::Idle;
    bool waitControl(uint16_t mask,uint16_t value) {
        const auto start=io_.ticks();
        for(unsigned i=0;i<1000000;++i) {
            if((io_.read16(base_)&mask)==value)return true;
            const auto now=io_.ticks();if(now>=start && now-start>=100)break;io_.pause();
        }
        return false;
    }
public:
    explicit HdaDellToneStream(Io& io):io_(io){}
    HdaToneStreamState state()const{return state_;}
    uint8_t stage()const{return stage_;}uint32_t detail()const{return detail_;}
    uint8_t progressReason()const{return progressReason_;}uint8_t progressStatus()const{return progressStatus_;}
    uint16_t progressControl()const{return progressControl_;}uint32_t progressFirst()const{return progressFirst_;}uint32_t progressLast()const{return progressLast_;}
    uint16_t fifoSize()const{return fifoSize_;}
    bool fifoSampled()const{return fifoSampled_;}
    bool prepare(uint16_t gcap,uint64_t bdlPhysical,uint16_t format=0x11,uint32_t ringBytes=12288) {
        stage_=1;detail_=gcap;
        if(state_!=HdaToneStreamState::Idle || gcap==0xffff || !((gcap>>12)&15) ||
           (format!=0x11 && format!=0x4011) || (ringBytes!=12288 && ringBytes!=65536) ||
           !bdlPhysical || (bdlPhysical&127))return false;
        const uint64_t bdlLast=uint64_t(ringBytes/4096)*16-1;
        if(bdlPhysical>~uint64_t(0)-bdlLast || (!(gcap&1) && bdlPhysical>0xffffffffULL-bdlLast))return false;
        const unsigned input=(gcap>>8)&15;const unsigned total=input+((gcap>>12)&15)+((gcap>>3)&31);
        if(total>30)return false;
        base_=0x80+input*0x20;ringBytes_=ringBytes;
        // Require an idle descriptor without interrupt enables or a tag.
        stage_=2;const uint16_t initialControl=io_.read16(base_);const uint8_t initialTag=io_.read8(base_+2);
        detail_=uint32_t(initialControl)|(uint32_t(initialTag)<<16);
        if(initialControl!=0 || (initialTag&~uint8_t(4)))return false;priority_=initialTag&4;
        stage_=3;io_.write16(base_,1);if(!io_.delayUs(3) || !waitControl(1,1)){detail_=io_.read16(base_);state_=HdaToneStreamState::Unknown;return false;}
        stage_=4;io_.write16(base_,0);if(!io_.delayUs(3) || !waitControl(1,0)){detail_=io_.read16(base_);state_=HdaToneStreamState::Unknown;return false;}
        io_.write8(base_+3,0x1c); // clear only W1C status bits
        io_.write16(base_+0x12,format);
        fifoSize_=io_.read16(base_+0x10);fifoSampled_=true; // raw format-dependent FIFO, diagnostic only
        io_.write32(base_+0x18,uint32_t(bdlPhysical));
        io_.write32(base_+0x1c,uint32_t(bdlPhysical>>32));
        io_.write16(base_+0x0c,uint16_t(ringBytes_/4096-1));io_.write32(base_+0x08,ringBytes_);
        io_.write8(base_+2,uint8_t(0x10|priority_)); // tag 1; preserve traffic priority
        stage_=5;detail_=0;
        if(io_.read16(base_)!=0)detail_|=1;if(io_.read8(base_+2)!=uint8_t(0x10|priority_))detail_|=2;
        if(io_.read16(base_+0x12)!=format)detail_|=4;if(io_.read32(base_+0x18)!=uint32_t(bdlPhysical))detail_|=8;
        if(io_.read32(base_+0x1c)!=uint32_t(bdlPhysical>>32))detail_|=16;
        if(io_.read16(base_+0x0c)!=ringBytes_/4096-1)detail_|=32;if(io_.read32(base_+0x08)!=ringBytes_)detail_|=64;
        if(detail_) {
            state_=HdaToneStreamState::Unknown;return false;
        }
        stage_=6;state_=HdaToneStreamState::Prepared;return true;
    }
    bool start() {
        if(state_!=HdaToneStreamState::Prepared)return false;
        if(!io_.publishDma())return false; // flush and fence before RUN
        state_=HdaToneStreamState::Unknown; // a failed RUN write can still take effect
        io_.write16(base_,2);
        if(!waitControl(2,2) || (io_.read8(base_+3)&0x18))return false;
        state_=HdaToneStreamState::Running;return true;
    }
    bool progress() {
        progressReason_=1;progressStatus_=0;progressControl_=0;if(state_!=HdaToneStreamState::Running)return false;
        const uint32_t rawFirst=io_.read32(base_+4);progressFirst_=progressLast_=rawFirst;
        if(rawFirst>ringBytes_){progressReason_=2;return false;}
        const uint32_t first=rawFirst==ringBytes_?0:rawFirst;const auto start=io_.ticks();
        for(unsigned i=0;i<2000000;++i) {
            progressStatus_=io_.read8(base_+3);if(progressStatus_&0x18){progressReason_=3;return false;}
            progressControl_=io_.read16(base_);if(!(progressControl_&2)){progressReason_=4;return false;}
            const uint32_t rawCurrent=io_.read32(base_+4);progressLast_=rawCurrent;if(rawCurrent>ringBytes_){progressReason_=5;return false;}
            const uint32_t current=rawCurrent==ringBytes_?0:rawCurrent;
            if(current!=first){progressReason_=6;return true;}
            const auto now=io_.ticks();if(now>=start && now-start>=150){progressReason_=7;return false;}io_.pause();
        }
        progressReason_=8;return false; // bounded-iteration exhaustion
    }
    bool stop() {
        if(state_!=HdaToneStreamState::Running && state_!=HdaToneStreamState::Unknown &&
           state_!=HdaToneStreamState::Prepared)return false;
        state_=HdaToneStreamState::Unknown;io_.write16(base_,0);
        if(!waitControl(3,0))return false;
        state_=HdaToneStreamState::Stopped;return true;
    }
    // RUN-clear alone does not retire the controller's descriptor references.
    // Call only with exclusive ownership, before releasing any DMA pages.
    bool retire() {
        stage_=7;detail_=0;
        if(state_!=HdaToneStreamState::Stopped)return false;
        if(io_.read16(base_)&3){detail_=io_.read16(base_);state_=HdaToneStreamState::Unknown;return false;}
        // PCH requires another completed reset before CBL can be rewritten
        // after RUN. Leave state unknown and retain pages on any failure.
        state_=HdaToneStreamState::Unknown;
        stage_=8;io_.write16(base_,1);if(!io_.delayUs(3)){detail_=0x10000|io_.read16(base_);return false;}
        if(!waitControl(1,1)){detail_=io_.read16(base_);return false;}
        stage_=9;io_.write16(base_,0);if(!io_.delayUs(3)){detail_=0x10000|io_.read16(base_);return false;}
        if(!waitControl(3,0)){detail_=io_.read16(base_);return false;}
        io_.write8(base_+2,priority_);io_.write32(base_+0x18,0);io_.write32(base_+0x1c,0);
        io_.write32(base_+0x08,0);io_.write16(base_+0x0c,0);
        stage_=10;detail_=0;if(io_.read16(base_)&3)detail_|=1;if(io_.read8(base_+2)!=priority_)detail_|=2;
        if(io_.read32(base_+0x18)!=0)detail_|=4;if(io_.read32(base_+0x1c)!=0)detail_|=8;
        if(io_.read32(base_+0x08)!=0)detail_|=16;if(io_.read16(base_+0x0c)!=0)detail_|=32;
        if(detail_) {
            state_=HdaToneStreamState::Unknown;return false;
        }
        stage_=11;state_=HdaToneStreamState::Idle;return true;
    }
};
}
