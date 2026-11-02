#pragma once
#include <stdint.h>

namespace brook {
// Polling-only HDA output stream control. Io supplies bounded register
// accesses to the already-owned controller, a monotonic millisecond clock and
// a bounded delayUs() that waits at least the requested microseconds.
// No PCI, codec, DMA allocation or memory freeing is performed here.
enum class HdaToneStreamState : uint8_t {Idle,Prepared,Running,Unknown};
template<class Io> class HdaDellToneStream {
    Io& io_;uint32_t base_=0;HdaToneStreamState state_=HdaToneStreamState::Idle;
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
    bool prepare(uint16_t gcap,uint64_t bdlPhysical) {
        if(state_!=HdaToneStreamState::Idle || gcap==0xffff || !((gcap>>12)&15) ||
           !bdlPhysical || (bdlPhysical&127) || (!(gcap&1) && bdlPhysical>0xffffffffULL))return false;
        const unsigned input=(gcap>>8)&15;const unsigned total=input+((gcap>>12)&15)+((gcap>>3)&31);
        if(total>30)return false;
        base_=0x80+input*0x20;
        // Require an idle descriptor without interrupt enables or a tag.
        if(io_.read16(base_)!=0 || io_.read8(base_+2)!=0)return false;
        io_.write16(base_,1);io_.delayUs(3);if(!waitControl(1,1)){state_=HdaToneStreamState::Unknown;return false;}
        io_.write16(base_,0);io_.delayUs(3);if(!waitControl(1,0)){state_=HdaToneStreamState::Unknown;return false;}
        io_.write8(base_+3,0x1c); // clear only W1C status bits
        io_.write16(base_+0x12,0x11); // 48 kHz, 16-bit stereo
        io_.write32(base_+0x18,uint32_t(bdlPhysical));
        io_.write32(base_+0x1c,uint32_t(bdlPhysical>>32));
        io_.write16(base_+0x0c,2);io_.write32(base_+0x08,12288);
        io_.write8(base_+2,0x10); // stream tag 1; no interrupts
        if(io_.read16(base_)!=0 || io_.read8(base_+2)!=0x10 ||
           io_.read16(base_+0x12)!=0x11 || io_.read32(base_+0x18)!=uint32_t(bdlPhysical) ||
           io_.read32(base_+0x1c)!=uint32_t(bdlPhysical>>32) ||
           io_.read16(base_+0x0c)!=2 || io_.read32(base_+0x08)!=12288) {
            state_=HdaToneStreamState::Unknown;return false;
        }
        state_=HdaToneStreamState::Prepared;return true;
    }
    bool start() {
        if(state_!=HdaToneStreamState::Prepared)return false;
        io_.publishDma(); // adapter supplies the platform DMA publication barrier
        state_=HdaToneStreamState::Unknown; // a failed RUN write can still take effect
        io_.write16(base_,2);
        if(!waitControl(2,2) || (io_.read8(base_+3)&0x18))return false;
        state_=HdaToneStreamState::Running;return true;
    }
    bool progress() {
        if(state_!=HdaToneStreamState::Running)return false;
        const uint32_t first=io_.read32(base_+4);if(first>=12288)return false;const auto start=io_.ticks();
        for(unsigned i=0;i<2000000;++i) {
            if(io_.read8(base_+3)&0x18)return false;
            if(!(io_.read16(base_)&2))return false;
            const uint32_t current=io_.read32(base_+4);if(current>=12288)return false;
            if(current!=first)return true;
            const auto now=io_.ticks();if(now>=start && now-start>=150)break;io_.pause();
        }
        return false;
    }
    bool stop() {
        if(state_!=HdaToneStreamState::Running && state_!=HdaToneStreamState::Unknown &&
           state_!=HdaToneStreamState::Prepared)return false;
        state_=HdaToneStreamState::Unknown;io_.write16(base_,0);
        if(!waitControl(3,0))return false;
        state_=HdaToneStreamState::Idle;return true;
    }
};
}
