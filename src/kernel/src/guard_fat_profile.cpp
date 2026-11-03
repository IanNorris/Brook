#include "guard_fat_profile.h"
#include "device.h"
#include "string.h"

namespace brook {
// Call only while the original reservation remains held. Exact reads from
// both copies are compared; an I/O error or difference denies admission.
bool GuardFatCheckDevice(Device* device,GuardFatProfile& profile) {
    profile={};
    if(!device || device->type!=DeviceType::Block || !device->ops || !device->ops->read)return false;
    const auto* ops=reinterpret_cast<const BlockDeviceOps*>(device->ops);
    if(!ops->block_size || !ops->block_count || DeviceBlockSize(device)!=512)return false;
    uint8_t first[4096],second[4096];
    GuardFatProfile checked{};
    if(device->ops->read(device,0,first,512)!=512 ||
       !GuardFatParse(first,512,DeviceBlockCount(device),checked))return false;
    if(checked.backupBoot &&
       (device->ops->read(device,uint64_t(checked.backupBoot)*512,second,512)!=512 || memcmp(first,second,512)))return false;
    if(checked.type==32) {
        if(device->ops->read(device,512,second,512)!=512 ||
           GuardFat32(second)!=0x41615252 || GuardFat32(second+484)!=0x61417272 ||
           GuardFat32(second+508)!=0xaa550000)return false;
        const uint32_t free=GuardFat32(second+488),next=GuardFat32(second+492);
        if((free!=0xffffffff && free>checked.clusters) ||
           (next!=0xffffffff && (next<2 || next>=checked.clusters+2)))return false;
    }
    for(uint64_t sector=0;sector<checked.fatSectors;) {
        const uint64_t remaining=checked.fatSectors-sector;
        const uint64_t count=remaining<8?remaining:8,bytes=count*512;
        const uint64_t left=(uint64_t(checked.fatStart)+sector)*512;
        const uint64_t right=(uint64_t(checked.fatStart)+checked.fatSectors+sector)*512;
        if(device->ops->read(device,left,first,bytes)!=int(bytes) ||
           device->ops->read(device,right,second,bytes)!=int(bytes) || memcmp(first,second,bytes))return false;
        sector+=count;
    }
    profile=checked;return true;
}
}
