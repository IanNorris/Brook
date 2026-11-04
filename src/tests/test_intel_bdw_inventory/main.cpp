#include <cassert>
#include "intel_bdw_inventory.h"
using namespace brook;
int main() {
    unsigned reads16=0, reads32=0, reads8=0;
    auto read16=[&](uint8_t offset) -> uint16_t { assert(offset==0x50); ++reads16; return 0x0840; };
    auto read32=[&](uint8_t offset) -> uint32_t { assert(offset==0x5c); ++reads32; return 0xabc00000; };
    auto read8=[&](uint8_t offset) -> uint8_t { assert(offset==0x62); ++reads8; return 1; };
    auto capture=[&](uint8_t bus, uint8_t dev, uint8_t fn, uint16_t vendor,
                     uint16_t device, uint8_t revision, uint16_t subvendor, uint16_t subsystem) {
        return BdwCaptureFirmwareGraphics(bus,dev,fn,vendor,device,revision,subvendor,subsystem,read16,read32,read8);
    };
    assert(!capture(1,2,0,0x8086,0x1616,9,0x1028,0x0665).captured);
    assert(!capture(0,3,0,0x8086,0x1616,9,0x1028,0x0665).captured);
    assert(!capture(0,2,1,0x8086,0x1616,9,0x1028,0x0665).captured);
    assert(!capture(0,2,0,0x1234,0x1616,9,0x1028,0x0665).captured);
    assert(!capture(0,2,0,0x8086,0x1617,9,0x1028,0x0665).captured);
    assert(!capture(0,2,0,0x8086,0x1616,8,0x1028,0x0665).captured);
    assert(!capture(0,2,0,0x8086,0x1616,9,0x1234,0x0665).captured);
    assert(!capture(0,2,0,0x8086,0x1616,9,0x1028,0x0666).captured);
    assert(!reads16 && !reads32 && !reads8);
    const auto result=capture(0,2,0,0x8086,0x1616,9,0x1028,0x0665);
    assert(result.captured && result.graphicsControl==0x0840 && result.stolenBase==0xabc00000 && result.apertureControl==1);
    assert(reads16==1 && reads32==1 && reads8==1);
    const auto unavailable=BdwCaptureFirmwareGraphics(0,2,0,0x8086,0x1616,9,0x1028,0x0665,
        [](uint8_t) -> uint16_t { return 0xffff; }, [](uint8_t) -> uint32_t { return 0xffffffff; }, [](uint8_t) -> uint8_t {return 0xff;});
    assert(unavailable.captured && unavailable.graphicsControl==0xffff && unavailable.stolenBase==0xffffffff);
    assert(!BdwDecodeStolenRange(unavailable).valid);
    assert(!BdwDecodeStolenRange({false,0x01c1,0xdd800001}).valid);
    const auto dell=BdwDecodeStolenRange({true,0x01c1,0xdd800001});
    assert(dell.valid && dell.base==0xdd800000ull && dell.size==32ull*1024*1024 && dell.end==0xdf800000ull);
    assert(!BdwDecodeStolenRange({true,0x00c1,0xdd800001}).valid);
    assert(!BdwDecodeStolenRange({true,0x01c1,0x00000001}).valid);
    assert(!BdwDecodeStolenRange({true,0xffc1,0xfff00001}).valid);
    assert(!BdwDecodeStolenRange({true,0x40c1,0x40000001}).valid);
}
