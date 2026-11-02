#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include "device.h"
#include "block_write_leases.h"
#include <cstdarg>
namespace brook {
static bool retainAllowed=true;static unsigned retainedCount=0;
bool ModuleRetainAddresses(const uintptr_t* addresses,uint32_t count){retainedCount=count;for(unsigned i=0;i<count;++i)assert(addresses[i]);return retainAllowed;}
extern "C" void SerialPuts(const char*){}
extern "C" void SerialPrintf(const char*,...){}
struct Process;thread_local uintptr_t caller=1;Process* ProcessCurrent(){return reinterpret_cast<Process*>(caller);}}
namespace {
std::atomic<bool> entered=false,release=false,hold=false;
std::atomic<unsigned> writes=0;
int write(brook::Device*,uint64_t,const void*,uint64_t n) {
    ++writes;if(hold){entered=true;while(!release)std::this_thread::yield();}return int(n);
}
brook::DeviceOps ops{nullptr,write,nullptr,nullptr};
brook::Device dev{&ops,"owned-fixture",brook::DeviceType::Block,nullptr};
}
int main() {
    using namespace brook;char data=0;
    assert(!DeviceFinalSyncOwner());
    BlockDeviceOps registeredOps{nullptr,write,nullptr,nullptr,[](Device*)->uint64_t{return 1;},[](Device*)->uint32_t{return 512;}};
    Device fixture{reinterpret_cast<DeviceOps*>(&registeredOps),"module-storage",DeviceType::Block,nullptr};
    retainAllowed=false;assert(!DeviceRegister(&fixture) && !DeviceIsRegistered(&fixture) && retainedCount==3);
    retainAllowed=true;assert(DeviceRegister(&fixture) && DeviceIsRegistered(&fixture));assert(DeviceUnregister(&fixture));
    hold=true;std::thread writer([&]{caller=2;assert(DeviceWrite(&dev,0,&data,1)==1);});
    while(!entered)std::this_thread::yield();
    assert(!DeviceCloseWrites());release=true;writer.join();hold=false;
    assert(DeviceCloseWrites());
    assert(DeviceFinalSyncOwner());
    unsigned old=writes;std::thread late([&]{caller=2;assert(!DeviceFinalSyncOwner());for(int i=0;i<1000;++i)assert(DeviceWrite(&dev,0,&data,1)==-30);assert(!DeviceReopenWrites());});late.join();
    assert(writes==old);assert(DeviceWrite(&dev,0,&data,1)==1);assert(writes==old+1);
    assert(DeviceCloseWrites());assert(DeviceReopenWrites());assert(!DeviceFinalSyncOwner());assert(DeviceWrite(&dev,0,&data,1)==1);
    assert(DeviceWrite(nullptr,0,&data,1)==-19);
    // Nested partition calls are independently admitted. Their outer call
    // keeps closure from overtaking an in-progress nested write.
    DeviceOps partitionOps{nullptr,[](Device*,uint64_t offset,const void* p,uint64_t n){return DeviceWrite(&dev,offset,p,n);},nullptr,nullptr};
    Device partition{&partitionOps,"partition-fixture",DeviceType::Block,nullptr};
    assert(DeviceWrite(&partition,0,&data,1)==1);assert(DeviceCloseWrites());assert(DeviceWrite(&partition,0,&data,1)==1);assert(DeviceReopenWrites());
    Device parent{reinterpret_cast<DeviceOps*>(&registeredOps),"lease-parent",DeviceType::Block,nullptr};
    Device owner{reinterpret_cast<DeviceOps*>(&registeredOps),"lease-owner",DeviceType::Block,nullptr};
    assert(DeviceRegister(&parent));assert(DeviceRegister(&owner));
    const BlockWriteLease leases[]={{reinterpret_cast<uintptr_t>(&parent),0,0,100},
        {reinterpret_cast<uintptr_t>(&parent),reinterpret_cast<uintptr_t>(&owner),100,100}};
    hold=true;entered=false;release=false;
    std::thread pending([&]{caller=2;assert(DeviceWrite(&parent,300,&data,1)==1);});
    while(!entered)std::this_thread::yield();
    assert(!DeviceInstallBootWriteLeases(leases,2));
    release=true;pending.join();hold=false;
    assert(DeviceInstallBootWriteLeases(leases,2));
    assert(!DeviceUnregister(&parent));assert(!DeviceUnregister(&owner));
    assert(DeviceWrite(&parent,99,&data,1)==-30);
    assert(DeviceWrite(&parent,100,&data,1)==-30);
    assert(DeviceWrite(&parent,200,&data,1)==1);
    assert(DeviceWriteLeased(&parent,&owner,100,&data,1)==1);
    assert(DeviceWriteLeased(&parent,&owner,199,&data,2)==-30);
    assert(DeviceWriteLeased(&parent,&dev,100,&data,1)==-30);
    DeviceOps ownerOps{nullptr,[](Device* d,uint64_t at,const void* p,uint64_t n){
        return DeviceWriteLeased(static_cast<Device*>(d->priv),d,100+at,p,n);
    },nullptr,nullptr};
    DeviceOps aliasOps{nullptr,[](Device* d,uint64_t at,const void* p,uint64_t n){
        return DeviceWrite(static_cast<Device*>(d->priv),100+at,p,n);
    },nullptr,nullptr};
    owner.ops=&ownerOps;owner.priv=&parent;
    Device alias{&aliasOps,"lease-alias",DeviceType::Block,&parent};
    assert(DeviceWrite(&owner,0,&data,1)==1);
    assert(DeviceWrite(&alias,0,&data,1)==-30);
    assert(DeviceCloseWrites());
    std::thread blocked([&]{caller=2;assert(DeviceWriteLeased(&parent,&owner,100,&data,1)==-30);});blocked.join();
    assert(DeviceWriteLeased(&parent,&owner,100,&data,1)==1);assert(DeviceReopenWrites());
}
