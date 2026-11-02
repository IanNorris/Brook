#include <cassert>
#include <cstdint>
#include <cstring>
#include "../../drivers/intel_hda/hda_dma_publish.h"
using namespace brook;

struct DeviceView {
    alignas(4096) uint8_t cpu[8192]{};
    uint8_t device[8192]{};
    unsigned calls=0;unsigned lineSize=64;
    void line(void* p) {
        auto offset=static_cast<uint8_t*>(p)-cpu;
        assert(offset>=0 && offset<8192 && !(offset%lineSize));
        std::memcpy(device+offset,cpu+offset,lineSize);++calls;
    }
};
int main() {
    DeviceView d;
    d.cpu[63]=1;d.cpu[64]=2;d.cpu[128]=3;
    assert(HdaDmaFlushRange(d,d.cpu,8192,63,2,64));
    assert(d.calls==2 && d.device[63]==1 && d.device[64]==2 && !d.device[128]);
    d.cpu[8191]=4;d.cpu[0]=5;
    // The caller splits a wrapped interval and fences once after both pieces.
    assert(HdaDmaFlushRange(d,d.cpu,8192,8191,1,64));
    assert(HdaDmaFlushRange(d,d.cpu,8192,0,1,64));
    assert(d.calls==4 && d.device[8191]==4 && d.device[0]==5);
    d.lineSize=128;d.cpu[255]=6;
    assert(HdaDmaFlushRange(d,d.cpu,8192,255,1,128) && d.device[255]==6);
    d.lineSize=32;d.cpu[4095]=7;
    assert(HdaDmaFlushRange(d,d.cpu,8192,4095,1,32) && d.device[4095]==7);
    const unsigned before=d.calls;
    assert(HdaDmaFlushRange(d,d.cpu,8192,8192,0,64));
    assert(!HdaDmaFlushRange(d,d.cpu,8192,8192,1,64));
    assert(!HdaDmaFlushRange(d,d.cpu,8192,0,8193,64));
    assert(!HdaDmaFlushRange(d,d.cpu+1,8192,0,1,64));
    assert(!HdaDmaFlushRange(d,d.cpu,8192,0,1,48));
    assert(!HdaDmaFlushRange(d,d.cpu,8192,0,1,0));
    assert(!HdaDmaFlushRange(d,d.cpu,8191,0,1,64));
    assert(!HdaDmaFlushRange(d,reinterpret_cast<void*>(~uintptr_t(0)-4095),4096,0,1,64));
    assert(d.calls==before);
}
