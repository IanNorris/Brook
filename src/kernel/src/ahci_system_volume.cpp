#include "ahci_system_volume.h"
#include "ahci_boot_selector.h"
#include "ahci_readonly.h"
#include "device.h"
#include "fatfs_glue.h"
#include "memory/heap.h"
#include "mount_namespace.h"
#include "vfs.h"
#include "kprintf.h"
#if !defined(BROOK_HOST_TEST)
#include "string.h"
#endif

namespace brook {
namespace {
struct Range { Device* parent=nullptr;uint64_t start=0,bytes=0; } range;
bool attempted=false;
int Read(Device*,uint64_t offset,void* data,uint64_t length) {
    if(offset>range.bytes || length>range.bytes-offset)return -22;
    return range.parent->ops->read(range.parent,range.start+offset,data,length);
}
int Write(Device*,uint64_t offset,const void* data,uint64_t length) {
    if(offset>range.bytes || length>range.bytes-offset)return -22;
    return DeviceWrite(range.parent,range.start+offset,data,length);
}
int Control(Device*,uint32_t request,void*) {
    if(request==DEVICE_IOCTL_FLUSH)return DeviceFlush(range.parent);
    if(request==DEVICE_IOCTL_READ_ONLY)return range.parent->ops->ioctl(range.parent,request,nullptr);
    return -95;
}
uint64_t Blocks(Device*) {return range.bytes/512;}
uint32_t BlockSize(Device*) {return 512;}
const BlockDeviceOps ops{Read,Write,Control,nullptr,Blocks,BlockSize};
Device partition{reinterpret_cast<const DeviceOps*>(&ops),"ahci-system",DeviceType::Block,nullptr};

AhciSystemBoot Fail(const char* stage) {
    KPrintf("ahci system: %s failed; maintenance only\n",stage);
    return AhciSystemBoot::Maintenance;
}
bool Prefix(const char* p,const char* prefix) {
    while(*prefix){char c=*p++;if(c>='A'&&c<='Z')c+=32;if(c!=*prefix++)return false;}
    return !*p || *p=='/';
}
bool TargetClear() {
    VnodeStat st{};if(VfsLstatPath("/media/system",&st) || !st.isDir || st.isSymlink)return false;
    auto* ns=VfsMountNamespaceInitial();
    for(uint32_t i=0;i<VfsMountNamespaceCount(ns);++i){const auto m=VfsMountNamespaceSnapshotAt(ns,i);
        if(m.used && Prefix(m.mountPoint,"/media/system"))return false;}
    return true;
}
bool ReadExact(Device* d,uint64_t lba,void* data,uint32_t bytes) {
    return lba<=~uint64_t{0}/512 && d->ops->read(d,lba*512,data,bytes)==int(bytes);
}
bool Select(Device* d,const AhciBootSelector& wanted,AhciBootSelection& selected) {
    const uint64_t sectors=DeviceBlockCount(d);
    if(DeviceBlockSize(d)!=512 || sectors<68 || sectors>~uint64_t{0}/512)return false;
    uint8_t mbr[512],primary[512],backup[512];
    if(!ReadExact(d,0,mbr,512) || !ReadExact(d,1,primary,512) || !ReadExact(d,sectors-1,backup,512))return false;
    StrictGptHeader a{},b{};
    if(StrictGptCheckHeader(primary,512,sectors,false,a)!=StrictGptResult::Ok ||
       StrictGptCheckHeader(backup,512,sectors,true,b)!=StrictGptResult::Ok ||
       a.count!=b.count || a.entrySize!=b.entrySize)return false;
    const uint32_t bytes=a.count*a.entrySize; // checked <= 1 MiB by headers
    auto* entries=static_cast<uint8_t*>(kmalloc(bytes));
    auto* copy=static_cast<uint8_t*>(kmalloc(bytes));
    bool ok=entries && copy && ReadExact(d,a.entriesLba,entries,bytes) && ReadExact(d,b.entriesLba,copy,bytes) &&
        AhciBootSelect(wanted,sectors,mbr,primary,backup,entries,copy,bytes,selected);
    if(entries)kfree(entries);if(copy)kfree(copy);return ok;
}
}

AhciSystemBoot AhciSystemVolumeMount(uint8_t* nextDrive) {
    if(attempted)return Fail("repeated admission");attempted=true;
    VnodeStat st{};const int exists=VfsLstatPath("/etc/ahci-boot-volume",&st);
    if(exists==-2)return AhciSystemBoot::Absent;
    if(exists || st.isDir || st.isSymlink || !st.size || st.size>128)return Fail("selector metadata");
    auto* node=VfsOpen("/etc/ahci-boot-volume",VFS_O_STREAM);if(!node)return Fail("selector open");
    char bytes[128]{};uint64_t offset=0;const bool read=VfsRead(node,bytes,st.size,&offset)==int(st.size);
    VfsRelease(node);AhciBootSelector wanted{};
    if(!read || !AhciBootSelectorParse(bytes,uint32_t(st.size),wanted))return Fail("selector parse");
    if(!nextDrive || !TargetClear())return Fail("mount namespace");
    Device* disk=AhciWriteAdmittedDevice();if(!disk)return Fail("disk identity or writable state");
    AhciBootSelection selected{};if(!Select(disk,wanted,selected))return Fail("GPT validation or selection");
    if(DeviceFind(partition.name))return Fail("partition name already registered");
    if(selected.first>~uint64_t{0}/512 || selected.count>~uint64_t{0}/512)return Fail("partition range");
    range={disk,selected.first*512,selected.count*512};
    if(!DeviceRegister(&partition))return Fail("partition registration");
    if(!FatFsBindDrive(*nextDrive,&partition))return Fail("FAT binding");
    const uint8_t drive=(*nextDrive)++; // binding remains pinned on failure
    if(!VfsMountConfigured("/media/system","fatfs",drive,0))return Fail("FAT mount");
    KPrintf("ahci system: verified GPT entry %u mounted at /media/system\n",selected.entry);
    return AhciSystemBoot::Ready;
}
} // namespace brook
