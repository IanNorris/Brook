#pragma once
#include "hda_dell_tone_plan.h"

namespace brook {
// Codec operations for the separately owned one-shot tone. Io must serialize
// commands, enforce bounded timeouts and expose only checked HDA SET/GETs.
// A false return can still mean a SET reached hardware; the owner must mute
// and quarantine rather than assume rollback. This type does not handle DMA.
// The plan and saved state must be derived together from a fresh validated
// snapshot while the caller has exclusive controller ownership. SET amp
// encoding uses direction bits 15/14, channel bits 13/12 and index bits 11:8;
// it is distinct from the GET amp encoding. An unresolved ICI command must not
// be retried blindly during cleanup.
template<class Io> class HdaDellToneCodec {
    Io& io_;
    bool readAmp(uint8_t nid,bool output,bool left,uint8_t& value) {
        uint32_t v=0;if(!io_.getAmp(nid,output,left,0,v) || v>0xff)return false;
        value=uint8_t(v);return true;
    }
    bool setAmp(uint8_t nid,bool output,bool left,uint8_t value) {
        uint8_t actual=0;
        return io_.setAmp(nid,output,left,0,value) && readAmp(nid,output,left,actual) && actual==value;
    }
    bool pinMuted(const HdaDellTonePlan& p) {
        uint8_t l=0,r=0;return readAmp(p.pin,true,true,l) && readAmp(p.pin,true,false,r) && l==0x80 && r==0x80;
    }
    bool selection(uint8_t nid,uint8_t value) {
        uint32_t actual=0;return io_.set8(nid,0x701,value) && io_.get8(nid,0xf01,actual) && actual==value;
    }
public:
    explicit HdaDellToneCodec(Io& io):io_(io){}
    bool muteAndVerify(const HdaDellTonePlan& p) {
        // Attempt both channels even if one command fails.
        const bool l=setAmp(p.pin,true,true,0x80),r=setAmp(p.pin,true,false,0x80);
        return l && r && pinMuted(p);
    }
    bool configureMuted(const HdaDellTonePlan& p) {
        if(!pinMuted(p))return false;
        return selection(p.pin,p.pinSelection) &&
               setAmp(p.dac,true,true,p.dacGain) && setAmp(p.dac,true,false,p.dacGain) &&
               setAmp(p.mixer,false,true,0) && setAmp(p.mixer,false,false,0) && pinMuted(p);
    }
    bool unmuteAndVerify(const HdaDellTonePlan& p) {
        if(!pinMuted(p))return false;
        return setAmp(p.pin,true,true,0) && setAmp(p.pin,true,false,0);
    }
    // Caller must have confirmed DMA stopped, converter detached and its
    // original format restored before invoking this. Restore pin mute last.
    bool restoreAndVerify(const HdaDellTonePlan& p,const HdaSessionReport& old) {
        if(!pinMuted(p) || !selection(p.pin,uint8_t(old.values[5])) ||
           !setAmp(p.dac,true,true,uint8_t(old.ampValues[0])) ||
           !setAmp(p.dac,true,false,uint8_t(old.ampValues[1])) ||
           !setAmp(p.mixer,false,true,uint8_t(old.ampValues[2])) ||
           !setAmp(p.mixer,false,false,uint8_t(old.ampValues[3])) || !pinMuted(p))return false;
        return setAmp(p.pin,true,true,uint8_t(old.ampValues[4])) &&
               setAmp(p.pin,true,false,uint8_t(old.ampValues[5]));
    }
};
}
