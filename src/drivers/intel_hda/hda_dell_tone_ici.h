#pragma once
#include <stdint.h>

namespace brook {
// Restricted codec-0 command transport for the separately gated Dell tone.
// The caller must have verified the Dell identity/topology, current controller
// idleness and exclusive ownership. ticks() returns monotonic milliseconds; the
// old format must come from that same fresh snapshot. On timeout this is terminal:
// even cleanup must not issue another ICI command on an unresolved transport.
template<class Io> class HdaDellToneIci {
    Io& io_;uint16_t oldFormat_;bool failed_=false;const bool allowMixerFormat_;
    bool wait(uint16_t mask,uint16_t expected) {
        const auto start=io_.ticks();
        for(unsigned i=0;i<100000;++i) {
            if((io_.read16(0x68)&mask)==expected)return true;
            const auto now=io_.ticks();if(now>=start && now-start>=50)break;io_.pause();
        }
        return false;
    }
    bool issue(uint32_t word,uint32_t& response) {
        if(failed_)return false;
        if(!wait(1,0)){failed_=true;return false;}
        io_.write16(0x68,2);
        if(!wait(3,0)){failed_=true;return false;}
        io_.write32(0x60,word);io_.write16(0x68,1);
        if(!wait(3,2)){failed_=true;return false;}
        response=io_.read32(0x64);return true;
    }
public:
    HdaDellToneIci(Io& io,uint16_t oldFormat,bool allowMixerFormat=false):io_(io),oldFormat_(oldFormat),allowMixerFormat_(allowMixerFormat){}
    bool failed()const{return failed_;}
    bool set8(uint8_t nid,uint16_t verb,uint8_t payload) {
        if(!((nid==0x14 && verb==0x701 && payload<=1) ||
             (nid==0x03 && verb==0x706 && (payload==0 || payload==0x10))))return false;
        uint32_t response=0;return issue((uint32_t(nid)<<20)|(uint32_t(verb)<<8)|payload,response);
    }
    bool get8(uint8_t nid,uint16_t verb,uint32_t& response) {
        if(!((nid==0x14 && verb==0xf01) || (nid==0x03 && verb==0xf06)))return false;
        return issue((uint32_t(nid)<<20)|(uint32_t(verb)<<8),response);
    }
    bool setAmp(uint8_t nid,bool output,bool left,uint8_t index,uint8_t value) {
        const bool dac=nid==3 && output;
        const bool fixed=(nid==0x14 && output) || (nid==0x0d && !output);
        if(index || (!dac && !fixed) || (dac && value>0x7f) || (fixed && value!=0 && value!=0x80))return false;
        const uint16_t payload=(output?0x8000:0x4000)|(left?0x2000:0x1000)|value;
        uint32_t response=0;return issue((uint32_t(nid)<<20)|(uint32_t(3)<<16)|payload,response);
    }
    bool getAmp(uint8_t nid,bool output,bool left,uint8_t index,uint32_t& response) {
        if(index || !((nid==3 && output) || (nid==0x14 && output) || (nid==0x0d && !output)))return false;
        const uint16_t payload=(output?0x8000:0)|(left?0x2000:0);
        return issue((uint32_t(nid)<<20)|(uint32_t(0x0b)<<16)|payload,response);
    }
    bool setFormat(uint16_t value) {
        if(value!=0x11 && value!=oldFormat_ && !(allowMixerFormat_ && value==0x4011))return false;
        uint32_t response=0;return issue((uint32_t(3)<<20)|(uint32_t(2)<<16)|value,response);
    }
    bool getFormat(uint32_t& response) {return issue((uint32_t(3)<<20)|(uint32_t(0x0a)<<16),response);}
};
}
