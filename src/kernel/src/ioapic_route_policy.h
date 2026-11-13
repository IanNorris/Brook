#pragma once
#include <stdint.h>
namespace brook {
// Boot-selected trigger overrides. Other route attributes come from the live
// entry; a later mask/unmask must not silently undo the selected trigger mode.
struct IoApicRoutePolicy {
    uint32_t levelPins=0, publishedPins=0, polarityPins=0, activeLowPins=0;
    bool SelectBootLevel(uint8_t pin) {
        if(pin>=32 || (publishedPins&(uint32_t{1}<<pin)))return false;
        levelPins|=uint32_t{1}<<pin;return true;
    }
    bool SelectBootLevelAndPolarity(uint8_t pin,bool activeLow) {
        if(pin>=32)return false;
        const uint32_t bit=uint32_t{1}<<pin;
        if((polarityPins&bit) && bool(activeLowPins&bit)!=activeLow)return false;
        if(!SelectBootLevel(pin))return false;
        polarityPins|=bit;
        if(activeLow)activeLowPins|=bit;
        return true;
    }
    bool Published(uint8_t pin)const{return pin<32 && (publishedPins&(uint32_t{1}<<pin));}
    uint32_t UnmaskLow(uint8_t pin,uint32_t old,uint8_t vector) {
        if(pin<32)publishedPins|=uint32_t{1}<<pin;
        uint32_t result=(old&~uint32_t(0xff|0x10000))|vector;
        if(pin<32) {
            const uint32_t bit=uint32_t{1}<<pin;
            if(levelPins&bit)result|=0x8000;
            if(polarityPins&bit)result=(result&~uint32_t{0x2000})|((activeLowPins&bit)?0x2000u:0u);
        }
        return result;
    }
};
}
