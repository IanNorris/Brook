#include "ahci_system_volume.h"
#include "ahci_boot_selector.h"
#include "ahci_data_reader.h"
#include "ahci_direct_layout.h"
#include "ahci_readonly.h"
#include "device.h"
#include "fatfs_glue.h"
#include "ext2_vfs.h"
#include "large_data_session.h"
#include "serial.h"
#include "writable_desktop.h"
#include "memory/heap.h"
#include "mount_namespace.h"
#include "vfs.h"
#include "kprintf.h"
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
#include "guard_checker_session.h"
#include "guard_fat_profile.h"
#endif
#if defined(BROOK_AHCI_PANIC_TEST)
#include "fatfs_vfs.h"
#include "panic_journal.h"
#endif
#if !defined(BROOK_HOST_TEST)
#include "string.h"
#endif

namespace brook {
namespace {
struct Range { Device* parent=nullptr;uint64_t start=0,bytes=0; } range;
bool attempted=false;
uint64_t systemVolumePublication=0;
uint8_t systemVolumeDrive=0;
Range guardRange{};bool guardReady=false;
int GuardRead(Device*,uint64_t offset,void* data,uint64_t length) {
    if(!guardReady || !guardRange.parent)return -19;
    if(offset>guardRange.bytes || length>guardRange.bytes-offset)return -22;
    return guardRange.parent->ops->read(guardRange.parent,guardRange.start+offset,data,length);
}
int GuardWrite(Device* owner,uint64_t offset,const void* data,uint64_t length) {
    (void)owner;(void)data;
    if(!guardReady || !guardRange.parent)return -19;
    if(offset>guardRange.bytes || length>guardRange.bytes-offset)return -22;
    // Checker completion and ownership handoff are not implemented yet.
    return -30;
}
int GuardControl(Device*,uint32_t request,void*) {
    if(!guardReady || !guardRange.parent)return -19;
    if(request==DEVICE_IOCTL_FLUSH)return DeviceFlush(guardRange.parent);
    if(request==DEVICE_IOCTL_READ_ONLY)return 1;
    return -95;
}
uint64_t GuardBlocks(Device*) {return guardReady?guardRange.bytes/512:0;}
uint32_t GuardBlockSize(Device*) {return 512;}
const BlockDeviceOps guardOps{GuardRead,GuardWrite,GuardControl,nullptr,GuardBlocks,GuardBlockSize};
Device guardPartition{reinterpret_cast<const DeviceOps*>(&guardOps),"ahci-guard-esp",DeviceType::Block,nullptr};
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
enum class GuardTestPhase : uint8_t {Disabled,Preparation,Writing,Finished,Failed};
GuardTestPhase guardTestPhase=GuardTestPhase::Disabled;
int GuardTestRead(Device*,uint64_t offset,void* data,uint64_t length) {
    if(guardTestPhase!=GuardTestPhase::Preparation && guardTestPhase!=GuardTestPhase::Writing)return -19;
    return GuardRead(nullptr,offset,data,length);
}
int GuardTestWrite(Device*,uint64_t offset,const void* data,uint64_t length) {
    if(guardTestPhase!=GuardTestPhase::Writing) {guardTestPhase=GuardTestPhase::Failed;return -30;}
    if(offset>guardRange.bytes || length>guardRange.bytes-offset) {guardTestPhase=GuardTestPhase::Failed;return -22;}
    const int result=DeviceWriteLeased(guardRange.parent,&guardPartition,guardRange.start+offset,data,length);
    if(result<0 || uint64_t(result)!=length)guardTestPhase=GuardTestPhase::Failed;
    return result;
}
int GuardTestControl(Device*,uint32_t request,void*) {
    if(request==DEVICE_IOCTL_READ_ONLY)return 0;
    if(request==DEVICE_IOCTL_FLUSH && (guardTestPhase==GuardTestPhase::Preparation || guardTestPhase==GuardTestPhase::Writing)) {
        const int error=DeviceFlush(guardRange.parent);if(error)guardTestPhase=GuardTestPhase::Failed;return error;
    }
    return -95;
}
uint64_t GuardTestBlocks(Device*) {return guardReady?guardRange.bytes/512:0;}
uint32_t GuardTestBlockSize(Device*) {return 512;}
const BlockDeviceOps guardTestOps{GuardTestRead,GuardTestWrite,GuardTestControl,nullptr,GuardTestBlocks,GuardTestBlockSize};
Device guardTestPartition{reinterpret_cast<const DeviceOps*>(&guardTestOps),"ahci-guard-test",DeviceType::Block,nullptr};
enum class GuardDataPhase : uint8_t {Disabled,Repair,Verify,Finished,Failed};
GuardDataPhase guardDataPhase=GuardDataPhase::Disabled;
Range guardDataRange{};
int GuardDataRead(Device*,uint64_t offset,void* data,uint64_t length) {
    if(guardDataPhase!=GuardDataPhase::Repair && guardDataPhase!=GuardDataPhase::Verify)return -19;
    if(offset>guardDataRange.bytes || length>guardDataRange.bytes-offset)return -22;
    return guardDataRange.parent->ops->read(guardDataRange.parent,guardDataRange.start+offset,data,length);
}
int GuardDataWrite(Device* adapter,uint64_t offset,const void* data,uint64_t length) {
    if(guardDataPhase!=GuardDataPhase::Repair)return -30;
    if(offset>guardDataRange.bytes || length>guardDataRange.bytes-offset) {guardDataPhase=GuardDataPhase::Failed;return -22;}
    const int result=DeviceWriteLeased(guardDataRange.parent,adapter,guardDataRange.start+offset,data,length);
    if(result<0 || uint64_t(result)!=length)guardDataPhase=GuardDataPhase::Failed;
    return result;
}
int GuardDataControl(Device*,uint32_t request,void*) {
    if(request==DEVICE_IOCTL_READ_ONLY)return 0;
    if(request==DEVICE_IOCTL_FLUSH && (guardDataPhase==GuardDataPhase::Repair || guardDataPhase==GuardDataPhase::Verify)) {
        const int error=DeviceFlush(guardDataRange.parent);if(error)guardDataPhase=GuardDataPhase::Failed;return error;
    }
    return -95;
}
uint64_t GuardDataBlocks(Device*) {return guardReady?guardDataRange.bytes/512:0;}
uint32_t GuardDataBlockSize(Device*) {return 512;}
const BlockDeviceOps guardDataOps{GuardDataRead,GuardDataWrite,GuardDataControl,nullptr,GuardDataBlocks,GuardDataBlockSize};
Device guardDataPartition{reinterpret_cast<const DeviceOps*>(&guardDataOps),"ahci-guard-data-test",DeviceType::Block,nullptr};
#endif
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
#endif
bool directPrepared=false,directLeased=false,directReady=false;
uint8_t largeUuid[16]{};LargeDataSession largeSession{};
uint8_t selectedDataDiskGuid[16]{},selectedDataPartGuid[16]{};
Range largeRange{};
int LargeRead(Device*,uint64_t offset,void* data,uint64_t length) {
    if(!largeRange.parent || offset>largeRange.bytes || length>largeRange.bytes-offset)return -22;
    return largeRange.parent->ops->read(largeRange.parent,largeRange.start+offset,data,length);
}
int LargeWrite(Device* dev,uint64_t offset,const void* data,uint64_t length) {
    if(!largeRange.parent || offset>largeRange.bytes || length>largeRange.bytes-offset)return -22;
    return directLeased?DeviceWriteLeased(largeRange.parent,dev,largeRange.start+offset,data,length):
        DeviceWrite(largeRange.parent,largeRange.start+offset,data,length);
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
bool SelectDirect(Device* d,const AhciBootSelector& boot,AhciDirectLayout& selected) {
    VnodeStat st{};
    if(VfsLstatPath("/etc/ahci-data-volume",&st) || st.isDir || st.isSymlink || !st.size || st.size>160)return false;
    auto* node=VfsOpen("/etc/ahci-data-volume",VFS_O_STREAM);if(!node)return false;
    char record[160]{};uint64_t offset=0;
    const bool read=VfsRead(node,record,st.size,&offset)==int(st.size);VfsRelease(node);
    AhciDataSelector data{};if(!read || !AhciDataSelectorParse(record,uint32_t(st.size),data))return false;
    const uint64_t sectors=DeviceBlockCount(d);
    if(DeviceBlockSize(d)!=512 || sectors<68 || sectors>~uint64_t{0}/512)return false;
    uint8_t mbr[512],primary[512],backup[512];
    if(!ReadExact(d,0,mbr,512) || !ReadExact(d,1,primary,512) || !ReadExact(d,sectors-1,backup,512))return false;
    StrictGptHeader a{},b{};
    if(StrictGptCheckHeader(primary,512,sectors,false,a)!=StrictGptResult::Ok ||
       StrictGptCheckHeader(backup,512,sectors,true,b)!=StrictGptResult::Ok ||
       a.count!=b.count || a.entrySize!=b.entrySize)return false;
    const uint32_t bytes=a.count*a.entrySize;
    auto* entries=static_cast<uint8_t*>(kmalloc(bytes));auto* copy=static_cast<uint8_t*>(kmalloc(bytes));
    const bool ok=entries && copy && ReadExact(d,a.entriesLba,entries,bytes) && ReadExact(d,b.entriesLba,copy,bytes) &&
        AhciDirectLayoutSelect(boot,data,sectors,mbr,primary,backup,entries,copy,bytes,selected);
    if(entries)kfree(entries);if(copy)kfree(copy);
    if(ok) {
        memcpy(largeUuid,data.filesystemUuid,sizeof(largeUuid));
        memcpy(selectedDataDiskGuid,data.diskGuid,sizeof(selectedDataDiskGuid));
        memcpy(selectedDataPartGuid,data.partitionGuid,sizeof(selectedDataPartGuid));
    }
    return ok;
}
}

bool AhciSystemDirectRequested() {
    VnodeStat st{};return VfsLstatPath("/etc/ahci-data-volume",&st)!=-2;
}
Device* AhciSystemGuardPrepare() {
    if(attempted)return nullptr;
    attempted=true;
    VnodeStat st{};
    if(VfsLstatPath("/etc/ahci-boot-volume",&st) || st.isDir || st.isSymlink || !st.size || st.size>128)return nullptr;
    auto* node=VfsOpen("/etc/ahci-boot-volume",VFS_O_STREAM);if(!node)return nullptr;
    char bytes[128]{};uint64_t offset=0;
    const bool read=VfsRead(node,bytes,st.size,&offset)==int(st.size);VfsRelease(node);
    AhciBootSelector wanted{};if(!read || !AhciBootSelectorParse(bytes,uint32_t(st.size),wanted))return nullptr;
    Device* disk=AhciWriteAdmittedDevice();AhciDirectLayout layout{};
    if(!disk || !SelectDirect(disk,wanted,layout) || DeviceFind(guardPartition.name))return nullptr;
    // GPT and the configured UUID alone do not identify the filesystem now
    // occupying the partition. Bind the recovery target to its actual primary
    // ext2 superblock before reserving anything or authorizing a checker.
    if(layout.data.count<4 || layout.data.first>~uint64_t{0}-2)return nullptr;
    uint8_t superblock[1024]{};
    if(!ReadExact(disk,layout.data.first+2,superblock,sizeof(superblock)) ||
       superblock[56]!=0x53 || superblock[57]!=0xef || memcmp(superblock+104,largeUuid,16))return nullptr;
    guardRange={disk,layout.esp.first*512,layout.esp.count*512};
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
    guardDataRange={disk,layout.data.first*512,layout.data.count*512};
#endif
    // Registered callbacks reject I/O until the complete batch is installed.
    if(!DeviceRegister(&guardPartition))return nullptr;
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
    if(!DeviceRegister(&guardDataPartition))return nullptr;
#endif
    BlockWriteLease leases[4]{};
    if(!AhciDirectGuardLeases(layout,reinterpret_cast<uintptr_t>(disk),
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
                              reinterpret_cast<uintptr_t>(&guardDataPartition),
#else
                              0,
#endif
                              reinterpret_cast<uintptr_t>(&guardPartition),leases) ||
       !DeviceInstallBootWriteLeases(leases,4))return nullptr;
    guardReady=true;
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
    if(!DeviceRegister(&guardTestPartition)) {guardReady=false;return nullptr;}
#endif
    return &guardPartition;
}
bool AhciSystemGuardIsReservedDevice(Device* device) {return device==&guardPartition;}
bool AhciSystemGuardIdentity(char (&out)[97]) {
    out[0]=0;if(!guardReady)return false;
    const uint8_t* fields[]={selectedDataDiskGuid,selectedDataPartGuid,largeUuid};
    constexpr char digits[]="0123456789abcdef";unsigned at=0;
    for(const auto* field:fields)for(unsigned i=0;i<16;++i) {
        out[at++]=digits[field[i]>>4];out[at++]=digits[field[i]&15];
    }
    out[at]=0;return true;
}
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
AhciGuardDataState AhciSystemGuardDataState(Device* reserved) {
    if(reserved!=&guardPartition || !guardReady || !guardDataRange.parent || guardDataRange.bytes<2048)
        return AhciGuardDataState::Invalid;
    uint8_t superblock[1024]{};
    if(guardDataRange.parent->ops->read(guardDataRange.parent,guardDataRange.start+1024,
                                       superblock,sizeof(superblock))!=int(sizeof(superblock)))
        return AhciGuardDataState::Invalid;
    LargeExt2Geometry geometry{};
    const auto result=LargeExt2ClassifyHeader(superblock,sizeof(superblock),guardDataRange.bytes,largeUuid,geometry);
    return result==LargeExt2HeaderState::NeedsCheck?AhciGuardDataState::NeedsCheck:
        result==LargeExt2HeaderState::Clean?AhciGuardDataState::Clean:AhciGuardDataState::Invalid;
}
Device* AhciSystemGuardTestPrepare(Device* reserved) {
    if(reserved!=&guardPartition || !guardReady || guardTestPhase!=GuardTestPhase::Disabled)return nullptr;
    // The same boot-lifetime reservation used by the checker remains in
    // force. Consume its drained, clean completion only from its original
    // controller, and repeat the common FAT geometry/copy profile before
    // making the preparation adapter available. A failure is final for this
    // boot; a raw reserved-device pointer alone grants no handoff.
    GuardFatProfile profile{};
    if(!GuardCheckerConsumeCompletion(reserved) || !GuardFatCheckDevice(reserved,profile)) {
        guardTestPhase=GuardTestPhase::Failed;return nullptr;
    }
    guardTestPhase=GuardTestPhase::Preparation;return &guardTestPartition;
}
bool AhciSystemGuardTestBeginWrites(Device* adapter) {
    if(adapter!=&guardTestPartition || guardTestPhase!=GuardTestPhase::Preparation)return false;
    guardTestPhase=GuardTestPhase::Writing;return true;
}
void AhciSystemGuardTestFinish(Device* adapter) {
    if(adapter==&guardTestPartition && guardTestPhase!=GuardTestPhase::Failed)guardTestPhase=GuardTestPhase::Finished;
}
Device* AhciSystemGuardTestDataArm(Device* adapter,int pendingOutcome) {
    if(adapter!=&guardTestPartition || guardTestPhase!=GuardTestPhase::Finished ||
       pendingOutcome!=2 || guardDataPhase!=GuardDataPhase::Disabled || !guardReady)return nullptr;
    guardDataPhase=GuardDataPhase::Repair;return &guardDataPartition;
}
bool AhciSystemGuardTestDataVerify(Device* adapter) {
    if(adapter!=&guardDataPartition || guardDataPhase!=GuardDataPhase::Repair || DeviceFlush(adapter))return false;
    guardDataPhase=GuardDataPhase::Verify;return true;
}
bool AhciSystemGuardTestDataIdentityValid(Device* adapter) {
    if(adapter!=&guardDataPartition || guardDataPhase!=GuardDataPhase::Verify)return false;
    uint8_t superblock[1024]{};
    return adapter->ops->read(adapter,1024,superblock,sizeof(superblock))==int(sizeof(superblock)) &&
        superblock[56]==0x53 && superblock[57]==0xef && !memcmp(superblock+104,largeUuid,16);
}
void AhciSystemGuardTestDataFinish(Device* adapter) {
    if(adapter==&guardDataPartition && guardDataPhase!=GuardDataPhase::Failed)guardDataPhase=GuardDataPhase::Finished;
}
#if defined(BROOK_SSD_GUARD_COMPLETE_TEST)
bool AhciSystemGuardTestBeginCompletion(Device* adapter,bool verified) {
    static bool completionAttempted=false;
    if(completionAttempted)return false;
    completionAttempted=true;
    if(adapter!=&guardTestPartition || guardTestPhase!=GuardTestPhase::Finished ||
       guardDataPhase!=GuardDataPhase::Finished || !verified)return false;
    guardTestPhase=GuardTestPhase::Writing;return true;
}
#endif
bool AhciSystemGuardIsTestData(Device* adapter) {return adapter==&guardDataPartition && guardReady;}
#endif
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
    const bool direct=AhciSystemDirectRequested();
    AhciBootSelection selected{};AhciDirectLayout layout{};
    if(direct) {
        if(!SelectDirect(disk,wanted,layout))return Fail("direct GPT validation or selection");
        selected=layout.esp;
    } else if(!Select(disk,wanted,selected))return Fail("GPT validation or selection");
    if(DeviceFind(partition.name))return Fail("partition name already registered");
    if(selected.first>~uint64_t{0}/512 || selected.count>~uint64_t{0}/512)return Fail("partition range");
    range={disk,selected.first*512,selected.count*512};
    if(!DeviceRegister(&partition))return Fail("partition registration");
    if(direct) {
        largePartition.name="ahci-data";
        if(DeviceFind(largePartition.name))return Fail("data partition name already registered");
        largeRange={disk,layout.data.first*512,layout.data.count*512};
        if(!DeviceRegister(&largePartition))return Fail("data partition registration");
        BlockWriteLease leases[3]{};
        if(!AhciDirectLayoutLeases(layout,reinterpret_cast<uintptr_t>(disk),reinterpret_cast<uintptr_t>(&largePartition),leases) ||
           !DeviceInstallBootWriteLeases(leases,3))return Fail("direct disk ownership");
        directLeased=true;directPrepared=true;
    }
    if(!FatFsBindDrive(*nextDrive,&partition))return Fail("FAT binding");
    const uint8_t drive=(*nextDrive)++; // binding remains pinned on failure
    uint64_t publication=0;
    if(!VfsMountConfiguredWithPublication("/media/system","fatfs",drive,0,&publication) || !publication)return Fail("FAT mount");
    systemVolumePublication=publication;
    systemVolumeDrive=drive;
    KPrintf("ahci system: verified GPT entry %u mounted at /media/system\n",selected.entry);
    return AhciSystemBoot::Ready;
}

uint64_t AhciSystemVolumePublication() { return systemVolumePublication; }
bool AhciSystemVolumeOwnsRoot(Vnode* retainedDirectory) {
    return systemVolumePublication && FatFsBoundDevice(systemVolumeDrive)==&partition &&
        VfsVnodeIsPublishedRoot(retainedDirectory,systemVolumePublication,systemVolumeDrive);
}

bool AhciSystemVolumeOwnsMember(Vnode* retainedDirectory) {
    return systemVolumePublication && FatFsBoundDevice(systemVolumeDrive)==&partition &&
        VfsVnodeIsPublishedMember(retainedDirectory,systemVolumePublication,systemVolumeDrive);
}

#if defined(BROOK_AHCI_PANIC_TEST)
void AhciPanicTestPrepareFat() {
    if(!AhciPanicTestFatCandidate())return;
    static FatImageLease* retained=nullptr;
    static bool journalAttempted=false;
    if(journalAttempted || retained)return;journalAttempted=true;
    auto* node=VfsOpen("/media/system/PANIC.BIN",VFS_O_WRITE|VFS_O_READWRITE);
    if(!node || !AhciSystemVolumeOwnsMember(node) || range.parent!=AhciWriteAdmittedDevice() ||
       (range.start&511) || (range.bytes&511)) {
        if(node)VfsRelease(node);SerialPuts("PANIC_TEST_FAT_REJECTED\n");return;
    }
    FatImageLease* lease=nullptr;
    const int opened=FatFsAcquireImageLease(node,uint64_t(PanicJournalSectors)*512,&lease);
    if(opened || !lease) {VfsRelease(node);SerialPuts("PANIC_TEST_FAT_REJECTED\n");return;}
    uint64_t sectors[PanicJournalSectors]{};
    const bool ready=FatFsImageLeaseDevice(lease)==&partition &&
        !FatFsJournalSectorMap(lease,sectors,PanicJournalSectors) &&
        AhciPanicTestMapSectors(sectors,PanicJournalSectors,range.start/512,range.bytes/512);
    if(!ready) {
        // Failure may poison a lease; retain it if checked release fails.
        if(FatFsReleaseImageLease(lease))retained=lease;
        VfsRelease(node);SerialPuts("PANIC_TEST_FAT_REJECTED\n");return;
    }
    retained=lease;VfsRelease(node);
    SerialPuts("PANIC_TEST_FAT_READY\n");
}
#endif

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
bool AhciSystemDirectDesktopMount(uint8_t* nextDrive) {
    if(!directPrepared || directReady || !nextDrive || !Ext2DriveAvailable(*nextDrive))return false;
    directPrepared=false; // failed admission is not replayed during this boot
    if(!WritableDesktopPrepareDirect())return false;
    VnodeStat st{};if(VfsLstatPath("/nix",&st) || !st.isDir || st.isSymlink)return false;
    constexpr uint32_t GdtCapacity=4096*32;
    auto* gdt=static_cast<uint8_t*>(kmalloc(GdtCapacity));if(!gdt)return false;
    const int begin=largeSession.Begin(LargeIo(),largeRange.bytes,largeUuid,gdt,GdtCapacity);
    kfree(gdt);
    if(begin) {KPrintf("ahci direct: filesystem admission failed error=%d; maintenance only\n",begin);return false;}
    if(!Ext2BindDevice(*nextDrive,&largePartition))return false;
    const uint8_t drive=(*nextDrive)++;
    if(!VfsMountConfigured("/nix","ext2",drive,6)) {
        VfsDrainMountCleanup();
        if(VfsSync(nullptr) || VfsMountCleanupPending())KPrintf("ahci direct: backing cleanup pending; maintenance only\n");
        return false;
    }
    if(!WritableDesktopPublishDirect()) {
        if(!VfsUnmount("/nix"))KPrintf("ahci direct: unmount failed; maintenance only\n");
        return false;
    }
    if(!largeSession.Arm())__builtin_trap();
    directReady=true;WritableDesktopMarkDirectReady(drive,largeRange.bytes);
    KPrintf("ahci direct: ready bytes=%lu drive=%u home=/data/home (persistent)\n",largeRange.bytes,drive);
    return true;
}
int AhciSystemDirectFinalSync() {
    if(!directReady)return 0;
    const int error=largeSession.Finish(LargeIo(),DeviceFinalSyncOwner());
    WritableDesktopDirectFinalResult(error,largeSession.cleanAttempted);
    SerialPuts(error?"AHCI_DIRECT_SESSION_CLEAN_FAILED\n":"AHCI_DIRECT_SESSION_CLEAN\n");
    return error;
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
    if(desktop && !WritableDesktopPrepareDirect())return false;
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
    if(desktop && !WritableDesktopPublishDirect()) {
        if(!VfsUnmount(target))KPrintf("ahci direct test: unmount failed; maintenance only\n");
        return false;
    }
    if(!largeSession.Arm())__builtin_trap();
    largeDesktopReady=desktop;
    return true;
}
bool AhciSystemQemuLargeDesktopReady() {return largeDesktopReady;}
int AhciSystemQemuLargeDataFinalSync() {
    if(directReady)return 0; // production direct session was finalized above
    const bool armed=largeSession.armed;
    const int result=largeSession.Finish(LargeIo(),DeviceFinalSyncOwner());
    if(armed)SerialPuts(result?"AHCI_QEMU_LARGE_SESSION_CLEAN_FAILED\n":"AHCI_QEMU_LARGE_SESSION_CLEAN\n");
    return result;
}
#endif
} // namespace brook
