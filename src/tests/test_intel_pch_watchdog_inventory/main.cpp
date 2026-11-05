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
    brook::PchWatchdogInventory live{};unsigned n=0;
    auto stable=[&](uint8_t offset) {++n;return offset==0?0x9cc38086u:offset==0x40?0x1801u:0x80u;};
    assert(!brook::PchWatchdogReadStable(brook::PchWatchdogInventory{},stable,live) && n==0);
    assert(brook::PchWatchdogReadStable(result,stable,live) && n==6 && live.pci40==0x1801 && live.pci44==0x80);
    n=0;auto changing=[&](uint8_t offset) {++n;return offset==0?0x9cc38086u:offset==0x40?n:0x80u;};
    assert(!brook::PchWatchdogReadStable(result,changing,live) && !live.captured);
    auto missing=[](uint8_t) {return 0xffffffffu;};
    assert(!brook::PchWatchdogReadStable(result,missing,live));
}
