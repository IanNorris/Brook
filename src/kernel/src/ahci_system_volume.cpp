#include "ahci_system_volume.h"
#include "ahci_boot_selector.h"
#include "ahci_data_reader.h"
#include "ahci_readonly.h"
#include "device.h"
#include "fatfs_glue.h"
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
#include "ext2_vfs.h"
#include "large_data_session.h"
#include "serial.h"
#include "writable_desktop.h"
#endif
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
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
AhciDataInspection largeInspection{};bool largeInspectionClean=false;
bool largeDesktopReady=false;
uint8_t largeUuid[16]{};LargeDataSession largeSession{};
Range largeRange{};
int LargeRead(Device*,uint64_t offset,void* data,uint64_t length) {
    if(!largeRange.parent || offset>largeRange.bytes || length>largeRange.bytes-offset)return -22;
    return largeRange.parent->ops->read(largeRange.parent,largeRange.start+offset,data,length);
}
int LargeWrite(Device*,uint64_t offset,const void* data,uint64_t length) {
    if(!largeRange.parent || offset>largeRange.bytes || length>largeRange.bytes-offset)return -22;
    return DeviceWrite(largeRange.parent,largeRange.start+offset,data,length);
}
int LargeControl(Device*,uint32_t request,void*) {
    if(!largeRange.parent)return -19;
    if(request==DEVICE_IOCTL_FLUSH)return DeviceFlush(largeRange.parent);
    if(request==DEVICE_IOCTL_READ_ONLY)return largeRange.parent->ops->ioctl(largeRange.parent,request,nullptr);
    return -95;
}
uint64_t LargeBlocks(Device*) {return largeRange.bytes/512;}
uint32_t LargeBlockSize(Device*) {return 512;}
const BlockDeviceOps largeOps{LargeRead,LargeWrite,LargeControl,nullptr,LargeBlocks,LargeBlockSize};
Device largePartition{reinterpret_cast<const DeviceOps*>(&largeOps),"ahci-qemu-large",DeviceType::Block,nullptr};
WritableDesktopIo LargeIo() {return {&largePartition,
    [](void* p,uint64_t at,void* buf,uint32_t n)->int {auto* d=static_cast<Device*>(p);return d->ops->read(d,at,buf,n);},
    [](void* p,uint64_t at,const void* buf,uint32_t n)->int {return DeviceWrite(static_cast<Device*>(p),at,buf,n);},
    [](void* p)->int {return DeviceFlush(static_cast<Device*>(p));}};}
#endif

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

void AhciSystemDataInspect() {
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    largeInspection={};largeInspectionClean=false;
#endif
    // The marker is a bounded selector, not an instruction to mount or write.
    VnodeStat st{};const int exists=VfsLstatPath("/etc/ahci-data-inspect",&st);
    if(exists==-2)return;
    if(exists || st.isDir || st.isSymlink || !st.size || st.size>160) {
        KPrintf("ahci data inspect: invalid selector metadata\n");return;
    }
    auto* node=VfsOpen("/etc/ahci-data-inspect",VFS_O_STREAM);
    if(!node){KPrintf("ahci data inspect: selector open failed\n");return;}
    char dataRecord[160]{};uint64_t offset=0;
    const bool dataRead=VfsRead(node,dataRecord,st.size,&offset)==int(st.size);
    VfsRelease(node);AhciDataSelector data{};
    if(!dataRead || !AhciDataSelectorParse(dataRecord,uint32_t(st.size),data)) {
        KPrintf("ahci data inspect: selector parse failed\n");return;
    }
    if(VfsLstatPath("/etc/ahci-boot-volume",&st) || st.isDir || st.isSymlink || !st.size || st.size>128) {
        KPrintf("ahci data inspect: boot selector metadata failed\n");return;
    }
    node=VfsOpen("/etc/ahci-boot-volume",VFS_O_STREAM);
    if(!node){KPrintf("ahci data inspect: boot selector open failed\n");return;}
    char bootRecord[128]{};offset=0;
    const bool bootRead=VfsRead(node,bootRecord,st.size,&offset)==int(st.size);
    VfsRelease(node);AhciBootSelector boot{};
    if(!bootRead || !AhciBootSelectorParse(bootRecord,uint32_t(st.size),boot)) {
        KPrintf("ahci data inspect: boot selector parse failed\n");return;
    }
    Device* disk=AhciWriteAdmittedDevice();
    if(!disk || !disk->ops || !disk->ops->read || DeviceBlockSize(disk)!=512) {
        KPrintf("ahci data inspect: admitted disk unavailable\n");return;
    }
    // Header validation bounds entry arrays to 1 MiB; the large ext2 helper
    // bounds descriptor bytes to 128 KiB. All scratch is private to boot.
    constexpr uint32_t EntryCapacity=1024*1024, GdtCapacity=4096*32;
    auto* entries=static_cast<uint8_t*>(kmalloc(EntryCapacity));
    auto* backup=static_cast<uint8_t*>(kmalloc(EntryCapacity));
    auto* gdt=static_cast<uint8_t*>(kmalloc(GdtCapacity));
    if(!entries || !backup || !gdt) {
        if(entries)kfree(entries);if(backup)kfree(backup);if(gdt)kfree(gdt);
        KPrintf("ahci data inspect: scratch allocation failed\n");return;
    }
    const AhciDataReadIo io{disk,[](void* context,uint64_t at,void* buffer,uint32_t length)->int {
        auto* dev=static_cast<Device*>(context);return dev->ops->read(dev,at,buffer,length);
    }};
    AhciDataInspection result{};
    const auto state=AhciReadAndInspectBootData(io,boot,data,DeviceBlockCount(disk),entries,backup,
                                                EntryCapacity,gdt,GdtCapacity,result);
    kfree(entries);kfree(backup);kfree(gdt);
    if(state==AhciDataInspectionState::Clean) {
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
        if(AhciDisposableLargeDataWritable()){
            largeInspection=result;memcpy(largeUuid,data.filesystemUuid,sizeof(largeUuid));largeInspectionClean=true;
        }
#endif
        KPrintf("ahci data inspect: format eligible entry=%u groups=%u (read only)\n",result.data.entry,result.geometry.groups);
    }
    else if(state==AhciDataInspectionState::NeedsCheck)
        KPrintf("ahci data inspect: offline filesystem check required (read only)\n");
    else KPrintf("ahci data inspect: identity or format rejected (read only)\n");
}
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
bool AhciSystemQemuLargeDesktopRequested() {
    VnodeStat st{};
    // An inaccessible or malformed marker still selects maintenance: do not
    // silently fall back to a different store after failed admission.
    return VfsLstatPath("/etc/ahci-qemu-large-desktop-test",&st)!=-2;
}
bool AhciSystemQemuLargeDataMount(uint8_t* nextDrive) {
    if(!AhciDisposableLargeDataWritable() || !largeInspectionClean || !nextDrive ||
       !Ext2DriveAvailable(*nextDrive) || DeviceFind(largePartition.name))return false;
    largeInspectionClean=false; // one-shot; a failed mount requires a fresh inspection
    Device* disk=AhciWriteAdmittedDevice();if(!disk)return false;
    VnodeStat st{};
    const int marker=VfsLstatPath("/etc/ahci-qemu-large-desktop-test",&st);
    const bool desktop=marker!=-2;
    if(desktop && (marker || st.isDir || st.isSymlink || !st.size || st.size>16))return false;
    const char* target=desktop?"/nix":"/mnt/ahcitest";
    if(desktop && !WritableDesktopTestPrepareDirect())return false;
    if(VfsLstatPath(target,&st) || !st.isDir || st.isSymlink)return false;
    auto* ns=VfsMountNamespaceInitial();
    for(uint32_t i=0;i<VfsMountNamespaceCount(ns);++i) {
        const auto mount=VfsMountNamespaceSnapshotAt(ns,i);
        if(mount.used && Prefix(mount.mountPoint,target))return false;
    }
    largeRange={disk,largeInspection.data.first*512,largeInspection.data.count*512};
    constexpr uint32_t GdtCapacity=4096*32;
    auto* gdt=static_cast<uint8_t*>(kmalloc(GdtCapacity));if(!gdt)return false;
    const int begin=largeSession.Begin(LargeIo(),largeRange.bytes,largeUuid,gdt,GdtCapacity);
    kfree(gdt);if(begin)return false;
    SerialPuts("AHCI_QEMU_LARGE_SESSION_DIRTY\n");
    if(!DeviceRegister(&largePartition))return false;
    if(!Ext2BindDevice(*nextDrive,&largePartition))return false;
    const uint8_t drive=(*nextDrive)++;
    if(!VfsMountConfigured(target,"ext2",drive,desktop?6:0)) {
        VfsDrainMountCleanup();
        if(VfsSync(nullptr) || VfsMountCleanupPending())
            KPrintf("ahci direct test: backing cleanup pending; maintenance only\n");
        return false;
    }
    if(desktop && !WritableDesktopTestPublishDirect()) {
        if(!VfsUnmount(target))KPrintf("ahci direct test: unmount failed; maintenance only\n");
        return false;
    }
    if(!largeSession.Arm())__builtin_trap();
    largeDesktopReady=desktop;
    return true;
}
bool AhciSystemQemuLargeDesktopReady() {return largeDesktopReady;}
int AhciSystemQemuLargeDataFinalSync() {
    const bool armed=largeSession.armed;
    const int result=largeSession.Finish(LargeIo(),DeviceFinalSyncOwner());
    if(armed)SerialPuts(result?"AHCI_QEMU_LARGE_SESSION_CLEAN_FAILED\n":"AHCI_QEMU_LARGE_SESSION_CLEAN\n");
    return result;
}
#endif
} // namespace brook
