#include <cassert>
#include <atomic>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include "fatfs_glue.h"
#include "diskio.h"
namespace {bool readOnly=true,roError=false,failWrite=false;unsigned writes=0;}
namespace brook {
extern "C" bool DeviceIsRegistered(Device* d){return d && d->priv;}
extern "C" int DeviceFlush(Device*){return 0;}
extern "C" int DeviceWrite(Device*,uint64_t,const void*,uint64_t n){++writes;return failWrite?-5:int(n);}
uint32_t DeviceCountRaw(){return 2;}
bool DeviceRegistryCorrupted(){return false;}
void DeviceDumpRegistry(){}
extern "C" void SerialPrintf(const char*,...){}
}
int main(){
    using namespace brook;
    BlockDeviceOps ops{[](Device*,uint64_t,void*,uint64_t n){return int(n);},
        [](Device*,uint64_t,const void*,uint64_t n){return int(n);},
        [](Device*,uint32_t command,void*){return command==DEVICE_IOCTL_READ_ONLY?(roError?-5:readOnly?1:0):-95;},
        nullptr,[](Device*)->uint64_t{return 1024;},[](Device*)->uint32_t{return 512;}};
    Device a{reinterpret_cast<DeviceOps*>(&ops),"a",DeviceType::Block,&ops};
    Device b{reinterpret_cast<DeviceOps*>(&ops),"b",DeviceType::Block,&ops};
    Device c{reinterpret_cast<DeviceOps*>(&ops),"c",DeviceType::Block,&ops};
    bool ro=false;assert(!FatFsAcquireDrive(0,&ro));
    assert(FatFsBindDrive(0,&a));assert(FatFsAcquireDrive(0,&ro) && ro);
    assert(disk_status(0)==STA_PROTECT && disk_initialize(0)==STA_PROTECT);
    char data[512]{};assert(disk_write(0,reinterpret_cast<BYTE*>(data),0,1)==RES_WRPRT && !writes);
    assert(!FatFsBindDrive(0,&b) && !FatFsAcquireDrive(0,&ro));
    assert(FatFsBindDrive(0,&a)); // same-device bind cannot invalidate FILs
    assert(FatFsBindDrive(1,&a) && FatFsAcquireDrive(1,&ro) && ro); // alias retains device policy
    FatFsReleaseDrive(1);FatFsReleaseDrive(0);
    readOnly=false;assert(FatFsBindDrive(0,&b) && FatFsAcquireDrive(0,&ro) && !ro);
    assert(!disk_status(0) && disk_write(0,reinterpret_cast<BYTE*>(data),0,1)==RES_OK && writes==1);
    FatFsReleaseDrive(0);
    // Aliases stay disqualifying even after a normal mount is released.
    assert(!FatFsBindExclusiveDrive(2,&a));assert(!FatFsBindExclusiveDrive(0,&c));
    roError=true;assert(!FatFsBindExclusiveDrive(2,&c));roError=false;
    const pid_t child=fork();assert(child>=0);
    if(!child) {
        std::atomic<unsigned> ready{0};std::atomic<bool> go{false};bool x=false,y=false;
        auto race=[&](unsigned slot,bool& result) {++ready;while(!go.load())std::this_thread::yield();result=FatFsBindExclusiveDrive(slot,&c);};
        std::thread first(race,2,std::ref(x)),second(race,3,std::ref(y));
        while(ready.load()!=2)std::this_thread::yield();
        go=true;first.join();second.join();
        assert(x!=y);_exit(0);
    }
    int childStatus=0;assert(waitpid(child,&childStatus,0)==child && WIFEXITED(childStatus) && !WEXITSTATUS(childStatus));
    const pid_t mixedChild=fork();assert(mixedChild>=0);
    if(!mixedChild) {
        std::atomic<unsigned> ready{0};std::atomic<bool> go{false};bool x=false,y=false;
        std::thread first([&]{++ready;while(!go.load())std::this_thread::yield();x=FatFsBindExclusiveDrive(2,&c);});
        std::thread second([&]{++ready;while(!go.load())std::this_thread::yield();y=FatFsBindDrive(3,&c);});
        while(ready.load()!=2)std::this_thread::yield();
        go=true;first.join();second.join();
        assert(x!=y);_exit(0);
    }
    childStatus=0;assert(waitpid(mixedChild,&childStatus,0)==mixedChild && WIFEXITED(childStatus) && !WEXITSTATUS(childStatus));
    assert(FatFsBindExclusiveDrive(2,&c));
    assert(!FatFsBindDrive(2,&c) && !FatFsBindDrive(2,&a) && !FatFsBindDrive(3,&c));
    assert(!FatFsBindExclusiveDrive(3,&c));
    FatFsReleaseDrive(2);assert(!FatFsBindDrive(3,&c));
    assert(!FatFsAcquireDrive(2,&ro) && !FatFsAcquireExclusiveDrive(2,&a,&ro));
    readOnly=true; // the capability was pinned when the exclusive slot bound
    assert(FatFsAcquireExclusiveDrive(2,&c,&ro) && !ro);
    FatFsReleaseDrive(2);assert(!FatFsAcquireExclusiveDrive(2,&c,&ro));
    failWrite=true;assert(disk_write(2,reinterpret_cast<BYTE*>(data),0,1)==RES_ERROR);failWrite=false;
    assert(FatFsFlushDrive(2)==-5 && !FatFsBindDrive(2,&c) &&
           !FatFsBindExclusiveDrive(2,&c) && FatFsFlushDrive(2)==-5);
}
