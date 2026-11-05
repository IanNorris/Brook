#include "../../kernel/src/intel_pch_watchdog_inventory.h"
#include <cassert>

int main() {
    unsigned reads=0;
    auto read=[&](uint8_t offset) {++reads;assert(offset==0x40 || offset==0x44);return uint32_t(offset*0x101u);};
    auto capture=[&](uint8_t bus,uint8_t dev,uint8_t fn,uint16_t vendor,uint16_t device) {
        return brook::PchWatchdogCapture(bus,dev,fn,vendor,device,read);
    };
    assert(!capture(1,31,0,0x8086,0x9cc3).captured);
    assert(!capture(0,30,0,0x8086,0x9cc3).captured);
    assert(!capture(0,31,1,0x8086,0x9cc3).captured);
    assert(!capture(0,31,0,0x1234,0x9cc3).captured);
    assert(!capture(0,31,0,0x8086,0x9cc2).captured);
    assert(reads==0);
    auto result=capture(0,31,0,0x8086,0x9cc3);
    assert(result.captured && result.pci40==0x4040 && result.pci44==0x4444 && reads==2);
}
