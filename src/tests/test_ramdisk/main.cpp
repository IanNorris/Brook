#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "../../kernel/src/ramdisk.h"
static unsigned allocations=0,failAt=~0u,attempts=0;
namespace brook {
extern "C" void* kmalloc(uint64_t size){if(attempts++==failAt)return nullptr;void* p=malloc(size);if(p)allocations++;return p;}
extern "C" void kfree(void* p){if(p){assert(allocations);allocations--;free(p);}}
extern "C" void SerialPuts(const char*){}
extern "C" void SerialPrintf(const char*,...){}
}
using namespace brook;
int main() {
    uint8_t backing[1024]{},copy[1024]{};for(unsigned i=0;i<1024;i++)backing[i]=i;
    auto* ro=ImmutableRamdiskCreate(backing,sizeof(backing),512,"immutable");assert(ro);
    assert(ro->ops->ioctl(ro,DEVICE_IOCTL_READ_ONLY,nullptr)==1);
    assert(ro->ops->ioctl(ro,DEVICE_IOCTL_FLUSH,nullptr)==0);
    assert(ro->ops->write(ro,0,copy,512)==-30&&backing[1]==1);
    assert(ro->ops->read(ro,0,copy,1024)==1024&&!memcmp(backing,copy,1024));
    memset(copy,0,sizeof(copy));
    assert(ro->ops->read(ro,1000,copy,~uint64_t{0})==24&&!memcmp(backing+1000,copy,24));
    assert(ro->ops->read(ro,~uint64_t{0}-2,copy,8)==0);
    auto* blocks=reinterpret_cast<const BlockDeviceOps*>(ro->ops);assert(blocks->block_count(ro)==2&&blocks->block_size(ro)==512);
    RamdiskDestroyUnregistered(ro);assert(!allocations);
    auto* rw=RamdiskCreate(backing,sizeof(backing),512,"writable");assert(rw);
    assert(rw->ops->ioctl(rw,DEVICE_IOCTL_READ_ONLY,nullptr)==0);
    uint8_t data[4]={4,3,2,1};assert(rw->ops->write(rw,1022,data,~uint64_t{0})==2&&backing[1023]==3);
    RamdiskDestroyUnregistered(rw);assert(!allocations);
    assert(!ImmutableRamdiskCreate(backing,1023,512,"bad"));assert(!allocations);
    for(unsigned failure=0;failure<2;failure++){
        attempts=0;failAt=failure;assert(!ImmutableRamdiskCreate(backing,1024,512,"oom"));assert(!allocations);
    }
}
