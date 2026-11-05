#include "../../kernel/src/pci_config_access.h"
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>

struct Lock {
    std::mutex m;
    unsigned Acquire(){m.lock();return 7;}
    void Release(unsigned flags){assert(flags==7);m.unlock();}
};
struct Io {
    uint32_t selected=0,wordWrites=0,dwordWrites=0,lastPort=0,lastValue=0;
    void out32(uint16_t port,uint32_t value){
        if(port==0xcf8){selected=value;std::this_thread::yield();return;}
        assert(port==0xcfc);++dwordWrites;lastValue=value;
    }
    uint32_t in32(uint16_t port){assert(port==0xcfc);std::this_thread::yield();return selected;}
    void out16(uint16_t port,uint16_t value){++wordWrites;lastPort=port;lastValue=value;}
};
int main(){
    Lock lock;Io io;brook::PciConfigAccess<Lock,Io> pci(lock,io);
    auto worker=[&](uint8_t bus){
        for(unsigned n=0;n<10000;++n)
            assert(pci.Read32(bus,31,0,0x40)==(0x80000000u|(uint32_t(bus)<<16)|(31u<<11)|0x40));
    };
    std::thread a(worker,1),b(worker,2);a.join();b.join();
    pci.Write16(0,31,0,4,0x1234);
    assert(io.wordWrites==1 && io.dwordWrites==0 && io.lastPort==0xcfc && io.lastValue==0x1234);
    pci.Write16(0,31,0,6,0x5678);
    assert(io.wordWrites==2 && io.dwordWrites==0 && io.lastPort==0xcfe && io.lastValue==0x5678);
    pci.Write32(0,31,0,0x40,0xabcdef01);
    assert(io.dwordWrites==1 && io.lastValue==0xabcdef01);
}
