#include <cassert>
#include "fatfs_glue.h"
#include "diskio.h"
namespace {bool readOnly=true;unsigned writes=0;}
namespace brook {
extern "C" bool DeviceIsRegistered(Device* d){return d && d->priv;}
extern "C" int DeviceFlush(Device*){return 0;}
extern "C" int DeviceWrite(Device*,uint64_t,const void*,uint64_t n){++writes;return n;}
uint32_t DeviceCountRaw(){return 2;}
bool DeviceRegistryCorrupted(){return false;}
void DeviceDumpRegistry(){}
extern "C" void SerialPrintf(const char*,...){}
}
int main(){
    using namespace brook;
    BlockDeviceOps ops{[](Device*,uint64_t,void*,uint64_t n){return int(n);},
        [](Device*,uint64_t,const void*,uint64_t n){return int(n);},
        [](Device*,uint32_t command,void*){return command==DEVICE_IOCTL_READ_ONLY?(readOnly?1:0):-95;},
        nullptr,[](Device*)->uint64_t{return 1024;},[](Device*)->uint32_t{return 512;}};
    Device a{reinterpret_cast<DeviceOps*>(&ops),"a",DeviceType::Block,&ops};
    Device b{reinterpret_cast<DeviceOps*>(&ops),"b",DeviceType::Block,&ops};
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
}
