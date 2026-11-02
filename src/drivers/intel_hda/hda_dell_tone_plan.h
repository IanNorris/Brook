#pragma once
#include "hda_probe_session.h"

namespace brook {
// A calculation only: this does not own, modify or start the controller.
// The caller must reacquire exclusive ownership and a fresh snapshot before
// executing any plan. Hardware gain and PCM peak are both conservatively
// bounded; they cannot predict acoustic loudness or replace user listening.
struct HdaDellTonePlan {
    uint8_t pin=0, mixer=0, dac=0, pinSelection=0, dacGain=0;
    uint16_t peakSample=0;
};
inline bool HdaDellMakeTonePlan(const HdaSessionReport& s,HdaDellTonePlan& out) {
    out={};
    if(s.result!=HdaSessionResult::Complete || s.probe!=HdaProbeResult::Ready ||
       s.vendor!=0x10ec0288 || s.subsystem!=0x10280665 || s.completed!=12 ||
       s.stateCompleted!=13 || s.ampCompleted!=6 || !s.formatCompleted || s.converterFormat>0xffff)return false;
    const auto* v=s.values;const auto* p=s.stateValues;
    // Require the exact analog speaker topology observed on this Dell.
    if((v[0]&0x00f0020d)!=0x0040000d || !(v[1]&0x10) ||
       ((v[2]>>20)&15)!=1 || ((v[2]>>30)&3)==1 || (v[3]&0x7f)!=2 ||
       (v[3]&0x80) || (v[4]&0xffff)!=0x0d0c || v[5]>1 || !(v[6]&0x40) || !(v[7]&2) ||
       (v[8]&0x00f0020b)!=0x0020000b || (v[9]&0xff)!=1 || (v[10]&0xff)!=3 ||
       (v[11]&0x00f00205)!=0x00000005)return false;
    // The observed DAC overrides format and amplifier capabilities. The
    // speaker pin must have a mute; mixer input 0 must also support mute.
    if(!(v[11]&(1u<<3)) || !(v[11]&(1u<<4)) ||
       !(p[7]&(1u<<6)) || !(p[7]&(1u<<17)) || !(p[8]&1) ||
       p[9]!=0x00017f7f || p[10]!=0x80000000 || p[12]!=0x80000000)return false;
    // No reported power error; requested and actual power state must be D0.
    if(((p[0]|p[5]|p[11])&0x1ff) || p[6]!=0)return false;
    for(unsigned i=0;i<6;++i)if(s.ampValues[i]>0xff)return false;
    if((s.ampValues[0]|s.ampValues[1])&0x80)return false; // DAC has no hardware mute
    for(unsigned i=2;i<6;++i)if(s.ampValues[i]&0x7f)return false; // fixed-gain mute amps
    const unsigned steps=(p[9]>>8)&0x7f;
    if(!steps)return false;
    out.pin=0x14;out.mixer=0x0d;out.dac=0x03;out.pinSelection=1;
    out.dacGain=static_cast<uint8_t>(steps/4); // gain 31 is -48 dB for the required capabilities
    out.peakSample=2048; // 6.25% of signed 16-bit full scale
    return true;
}
}
