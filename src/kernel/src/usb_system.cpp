#include "usb_system.h"
#include "usb_system_layout.h"

#include "block_write_leases.h"
#include "boot_stick.h"
#include "../../shared/brook_boot_stick.h"
#include "device.h"
#include "ext2_vfs.h"
#include "ext2_trim_plan.h"
#include "usb_trim_batch.h"
#include "gpt.h"
#include "memory/heap.h"
#include "mount_namespace.h"
#include "kprintf.h"
#include "large_data_session.h"
#include "serial.h"
#include "vfs.h"
#include "writable_desktop.h"

namespace brook {
namespace {
UsbSystemStatus boot;
UsbSystemRecord record;
LargeDataSession session;
Device* disk;
DeviceWriteWindow bootWindow{}, systemWindow{};
bool ready, open, uncertain, clean, finalAttempted;
int finalError;
uint64_t reads, writes, flushes, errors;

bool ReadExact(Device* device,uint64_t at,void* data,uint32_t bytes) {
    return device->ops->read(device,at,data,bytes)==int(bytes);
}

void ShowHex(char (&out)[33],const uint8_t (&id)[16]) {
    constexpr char hex[]="0123456789abcdef";
    for(unsigned i=0;i<16;++i){out[2*i]=hex[id[i]>>4];out[2*i+1]=hex[id[i]&15];}
}

int SetWindow(bool enable) {
    DeviceWriteWindow window=enable?systemWindow:DeviceWriteWindow{};
    const int error=disk->ops->ioctl(disk,DEVICE_IOCTL_USB_SYSTEM_WINDOW,&window);
    if(!error)__atomic_store_n(&open,enable,__ATOMIC_RELEASE);
    return error;
}

bool IsUncertain() {
    return __atomic_load_n(&uncertain,__ATOMIC_ACQUIRE) ||
        (disk && disk->ops->ioctl(disk,DEVICE_IOCTL_WRITE_FENCED,nullptr)>0);
}

int IoError(int error) {
    __atomic_add_fetch(&errors,1,__ATOMIC_RELAXED);
    if(!__atomic_exchange_n(&uncertain,true,__ATOMIC_ACQ_REL)) {
        SetWindow(false);
        KPrintf("usb-system: I/O failed (%d); write window closed, filesystem uncertain\n",error);
    }
    return error<0?error:-5;
}

bool RangeValid(uint64_t offset,uint64_t size) {
    return disk && offset<=systemWindow.length && size<=systemWindow.length-offset && size<=0x7fffffffu;
}
int Read(Device*,uint64_t offset,void* data,uint64_t size) {
    if(!RangeValid(offset,size))return -22;
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED);
    const int result=disk->ops->read(disk,systemWindow.start+offset,data,size);
    return result==int(size)?result:IoError(result);
}
int Write(Device* owner,uint64_t offset,const void* data,uint64_t size) {
    if(!RangeValid(offset,size))return -22;
    if(!size)return 0;
    if(IsUncertain() || !__atomic_load_n(&open,__ATOMIC_ACQUIRE))return -30;
    __atomic_add_fetch(&writes,1,__ATOMIC_RELAXED);
    const int result=DeviceWriteLeased(disk,owner,systemWindow.start+offset,data,size);
    return result==int(size)?result:IoError(result);
}
int Control(Device*,uint32_t request,void* argument) {
    if(request==DEVICE_IOCTL_READ_ONLY)return 0;
    if(request==DEVICE_IOCTL_PREFERRED_READ_BYTES)
        return disk->ops->ioctl(disk,request,nullptr);
    if(request==DEVICE_IOCTL_USB_READ_DMA) {
        if(!argument)return -22;
        auto& relative=*static_cast<UsbDmaRead*>(argument);
        if(!RangeValid(relative.offset,relative.bytes))return -22;
        if(IsUncertain())return -95;
        UsbDmaRead absolute{systemWindow.start+relative.offset,relative.physical,relative.bytes,false};
        const int result=disk->ops->ioctl(disk,request,&absolute);
        relative.retain=absolute.retain;
        if(result==-95)return result;
        __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED);
        return result==int(relative.bytes)?result:IoError(result);
    }
    if(request==DEVICE_IOCTL_WRITE_FENCED)return IsUncertain()?1:0;
    if(request!=DEVICE_IOCTL_FLUSH)return -95;
    if(IsUncertain() || !__atomic_load_n(&open,__ATOMIC_ACQUIRE))return -5;
    __atomic_add_fetch(&flushes,1,__ATOMIC_RELAXED);
    const int result=DeviceFlush(disk);
    return result?IoError(result):0;
}
uint64_t Blocks(Device*) {return systemWindow.length/512;}
uint32_t BlockSize(Device*) {return 512;}
const BlockDeviceOps ops{Read,Write,Control,nullptr,Blocks,BlockSize};
Device dataDevice{reinterpret_cast<const DeviceOps*>(&ops),"usb-system",DeviceType::Block,nullptr};

WritableDesktopIo SessionIo() {
    return {&dataDevice,
        [](void* p,uint64_t at,void* data,uint32_t n){return Read(static_cast<Device*>(p),at,data,n);},
        [](void* p,uint64_t at,const void* data,uint32_t n){return DeviceWrite(static_cast<Device*>(p),at,data,n);},
        [](void* p){return DeviceFlush(static_cast<Device*>(p));}};
}

int MaybeTrim(uint8_t* gdt,uint32_t gdtCapacity) {
    auto* marker=VfsOpen("/etc/usb-system-trim",VFS_O_STREAM);
    if(!marker)return 0;
    VnodeStat stat{};
    char mode[32]{};
    uint64_t offset=0;
    const bool valid=!VfsStat(marker,&stat) && !stat.isDir && !stat.isSymlink &&
        stat.size<=sizeof(mode) && VfsRead(marker,mode,stat.size,&offset)==int(stat.size);
    VfsRelease(marker);
    SsdTrimRequest request{};
    if(!valid || !SsdTrimParseRequest(mode,uint32_t(stat.size),request) ||
       (request.run && !request.maximumMiB)) {
        KPrintf("USB_SYSTEM_TRIM {\"state\":\"invalid-marker\"}\n");
        return 0;
    }
    UsbUnmapLimits limits{};
    int result=disk->ops->ioctl(disk,DEVICE_IOCTL_USB_UNMAP_LIMITS,&limits);
    if(result) {
        KPrintf("USB_SYSTEM_TRIM {\"state\":\"probe\",\"error\":%d}\n",result);
        return result==-95?0:IoError(result);
    }
    UsbTrimBatchPlan plan(limits,uint64_t(request.maximumMiB)*2048);
    if(!plan.maxSectors || !plan.maxRanges)return 0;
    auto* scan=static_cast<Ext2TrimScan*>(kmalloc(sizeof(Ext2TrimScan)));
    if(!scan)return -12;
    const TrimRange bounds{systemWindow.start/512,systemWindow.length/512};
    uint32_t batches=0;
    auto submit=[&](const DeviceWriteWindow* ranges,uint32_t count) {
        if(request.run) {
            // The first scan pass has validated the whole bitmap and GDT.
            // Publish the dirty marker before issuing the first discard.
            if(!session.dirty)
                result=session.Begin(SessionIo(),systemWindow.length,record.uuid,gdt,gdtCapacity);
            UsbUnmapBatch batch{ranges,count};
            if(!result)result=DeviceUsbUnmapLeased(disk,&dataDevice,batch);
            if(result)return false;
        }
        for(uint32_t i=0;i<count;++i)
            KPrintf("USB_SYSTEM_TRIM_RANGE {\"mode\":\"%s\",\"lba\":%lu,\"sectors\":%lu}\n",
                request.run?"run":"dry-run",ranges[i].start/512,ranges[i].length/512);
        ++batches;
        return true;
    };
    const auto scanned=Ext2TrimScanClean(bounds,record.uuid,request.minimumMiB*256,*scan,
        [&](uint64_t at,void* data,uint32_t size) {
            return Read(&dataDevice,at,data,size)==int(size);
        },[&](TrimRange range) {
            return plan.Add(range,submit);
        });
    kfree(scan);
    if(scanned==Ext2TrimScanResult::Ok) {
        if(plan.Drain(submit) && request.run && batches)result=DeviceFlush(&dataDevice);
    } else if(scanned!=Ext2TrimScanResult::Cancelled) {
        result=scanned==Ext2TrimScanResult::IoError?-5:-117;
    }
    KPrintf("USB_SYSTEM_TRIM {\"state\":\"done\",\"mode\":\"%s\",\"batches\":%u,\"sectors\":%lu,\"error\":%d}\n",
        request.run?"run":"dry-run",batches,plan.total,result);
    return !request.run && scanned==Ext2TrimScanResult::Invalid?0:result;
}

bool Fail(const char* stage,int error) {
    if(__atomic_load_n(&open,__ATOMIC_ACQUIRE))SetWindow(false);
    boot.stage=stage;boot.error=error;boot.dirty=boot.dirty || session.dirtyAttempted;
    SerialPrintf("USB_SYSTEM_MAINTENANCE stage=%s error=%d\n",stage,error);
    KPrintf("usb-system: %s (%d). Desktop stopped. Check the system SSD from the host.\n",stage,error);
    return false;
}

bool ValidateGpt(Device* parent,const uint8_t* bootGuid,uint32_t bootEntry,uint64_t first,uint64_t count) {
    if(!parent || DeviceBlockSize(parent)!=512)return false;
    const uint64_t sectors=DeviceBlockCount(parent);
    if(sectors<68 || sectors>uint64_t(0xffffffffu)+1)return false;
    uint8_t sector[512];
    StrictGptHeader primary{},backup{};
    if(!ReadExact(parent,0,sector,sizeof(sector)) ||
       StrictGptProtectiveMbr(sector,sectors)!=StrictMbrResult::Protective ||
       !ReadExact(parent,512,sector,sizeof(sector)) ||
       StrictGptCheckHeader(sector,512,sectors,false,primary)!=StrictGptResult::Ok ||
       !ReadExact(parent,(sectors-1)*512,sector,sizeof(sector)) ||
       StrictGptCheckHeader(sector,512,sectors,true,backup)!=StrictGptResult::Ok ||
       primary.count!=backup.count || primary.entrySize!=backup.entrySize)return false;
    const uint32_t bytes=primary.count*primary.entrySize;
    auto* entries=static_cast<uint8_t*>(kmalloc(2*bytes));
    if(!entries)return false;
    const bool ok=ReadExact(parent,primary.entriesLba*512,entries,bytes) &&
        ReadExact(parent,backup.entriesLba*512,entries+bytes,bytes) &&
        StrictGptCheckCopies(primary,entries,backup,entries+bytes,bytes)==StrictGptResult::Ok &&
        UsbSystemEntriesMatch(primary,entries,bootEntry,bootGuid,first,count,record);
    kfree(entries);
    return ok;
}
}

bool UsbSystemRequested() {return boot.configured;}

void UsbSystemConfigure() {
    VnodeStat stat{};
    const int lookup=VfsLstatPath("/etc/usb-system",&stat);
    if(lookup==-2)return;
    boot.configured=true;
    boot.stage="record";
    if(lookup || stat.isDir || stat.isSymlink)return;
    auto* node=VfsOpen("/etc/usb-system",VFS_O_STREAM);
    if(!node)return;
    char data[128];
    uint64_t offset=0;
    const int length=VfsRead(node,data,sizeof(data),&offset);
    VfsClose(node);
    boot.parsed=length>0 && length<int(sizeof(data)) && UsbSystemParse(data,length,record);
    if(!boot.parsed)return;
    boot.firstLba=record.first;
    boot.sectors=record.sectors;
    ShowHex(boot.partitionGuid,record.guid);
    ShowHex(boot.fsUuid,record.uuid);
}

// The authenticated bulk helper and profile-stage capacity probe share this
// path. Bind it to the admitted system filesystem before userspace starts.
static int BindProfileTransfer() {
    VnodeStat marker{};
    const int markerResult = VfsLstatPath("/etc/usb-profile-plan.pub", &marker);
    if (markerResult == -2) return 0;
    if (markerResult) return markerResult;
    if (marker.isDir || marker.isSymlink) return -22;
    constexpr char source[] = "/nix/var/.brook-transfer";
    constexpr char target[] = "/data/.brook-transfer";
    VfsPath root{}, nix{}, var{}, transfer{};
    int error = VfsPathRoot(&root);
    if (!error) error = VfsPathResolve(root, root, "/nix", &nix, false);
    if (!error) error = VfsPathResolve(root, root, "/nix/var", &var, false);
    if (!error && (nix.attachment == root.attachment || nix.attachment != var.attachment)) error = -18;
    // WritableDesktopPrepareDirect already admitted /data as a tmpfs mount.
    const char* directories[] = {"/nix/var", "/data"};
    for (const char* path : directories) {
        VnodeStat stat{};
        if (!error) error = VfsStatAt(root, root, path, &stat, false);
        if (!error && (!stat.isDir || stat.isSymlink)) error = -20;
        if (error) {
            KPrintf("usb-system: profile transfer parent %s (%d)\n", path, error);
            break;
        }
    }
    const char* paths[] = {source, target};
    for (const char* path : paths) {
        if (error) break;
        const int created = VfsMkdirAt(root, root, path, 0755, 0);
        if (created && created != -17) {
            error = created;
            break;
        }
        VnodeStat stat{};
        error = VfsStatAt(root, root, path, &stat, false);
        if (!error && (!stat.isDir || stat.isSymlink || stat.uid != 0 || (stat.mode & 0022))) error = -13;
        if (error) KPrintf("usb-system: profile transfer directory %s (%d)\n", path, error);
    }
    if (!error) error = VfsPathResolve(root, root, source, &transfer, false);
    if (!error && nix.attachment != transfer.attachment) error = -18;
    if (!error)
        error = VfsMountNamespaceBindAt(VfsMountNamespaceInitial(), root, root, source, target);
    VfsPathRelease(transfer);
    VfsPathRelease(var);
    VfsPathRelease(nix);
    VfsPathRelease(root);
    boot.profileTransferReady = !error;
    return error;
}

bool UsbSystemBoot(uint8_t* nextDrive,bool scanComplete) {
    if(!boot.parsed || !nextDrive)return Fail("record",-22);
    if(!scanComplete)return Fail("boot-volume-scan",-19);
    int refusal=0;
    auto* bootPartition=BootStickPartition(&refusal);
    if(!bootPartition)return Fail(refusal==BROOK_BOOT_STICK_REFUSED_AMBIGUOUS?"boot-volume-ambiguous":"boot-volume",-19);
    boot.disk=BootStickDiskIndex();
    uint8_t bootGuid[16];
    uint32_t bootEntry;
    uint64_t bootFirst,bootSectors;
    if(boot.disk<0 || !PartitionGptIdentity(bootPartition,bootGuid,&bootEntry,&bootFirst,&bootSectors))
        return Fail("boot-volume",-19);
    auto* parent=PartitionParentDevice(bootPartition);
    if(!ValidateGpt(parent,bootGuid,bootEntry,bootFirst,bootSectors))return Fail("gpt-identity",-22);
    disk=parent;
    bootWindow={bootFirst*512,bootSectors*512};
    systemWindow={record.first*512,record.sectors*512};
    uint8_t sb[1024];
    LargeExt2Geometry geometry{};
    if(!ReadExact(disk,systemWindow.start+1024,sb,sizeof(sb)))return Fail("ext2-read",-5);
    const auto state=LargeExt2ClassifyHeader(sb,sizeof(sb),systemWindow.length,record.uuid,geometry);
    if(state==LargeExt2HeaderState::NeedsCheck) {
        boot.dirty=true;
        return Fail("needs-filesystem-check",-117);
    }
    if(state!=LargeExt2HeaderState::Clean)return Fail("ext2-identity",-22);
    WritableDesktopFailure details{};
    VnodeStat target{};
    if(!Ext2DriveAvailable(*nextDrive) || !WritableDesktopPrepareDirect(&details) ||
       VfsLstatPath("/nix",&target) || !target.isDir || target.isSymlink)
        return Fail("mount-target",details.status?details.status:-22);

    const int transport=disk->ops->ioctl(disk,DEVICE_IOCTL_USB_SYSTEM_TRANSPORT,nullptr);
    if(transport && transport!=-95)return Fail("storage-transport",transport);
    if(!transport && (!ValidateGpt(disk,bootGuid,bootEntry,bootFirst,bootSectors) ||
       !ReadExact(disk,systemWindow.start+1024,sb,sizeof(sb)) ||
       LargeExt2ClassifyHeader(sb,sizeof(sb),systemWindow.length,record.uuid,geometry)!=LargeExt2HeaderState::Clean))
        return Fail("storage-transport-identity",-22);

    constexpr uint32_t gdtCapacity=4096*32;
    auto* gdt=static_cast<uint8_t*>(kmalloc(gdtCapacity));
    if(!gdt)return Fail("descriptor-buffer",-12);
    const BlockWriteLease lease{reinterpret_cast<uintptr_t>(disk),reinterpret_cast<uintptr_t>(&dataDevice),
        systemWindow.start,systemWindow.length};
    int error=-16;
    if(DeviceRegister(&dataDevice)) {
        if(!DeviceInstallBootWriteLeases(&lease,1))DeviceUnregister(&dataDevice);
        else if(!(error=SetWindow(true)) && !(error=DeviceFlush(&dataDevice)) &&
                !(error=MaybeTrim(gdt,gdtCapacity)) && !session.dirty)
            error=session.Begin(SessionIo(),systemWindow.length,record.uuid,gdt,gdtCapacity);
    }
    kfree(gdt);
    if(error)return Fail(error==-117?"needs-filesystem-check":"session-begin",error);
    const uint8_t drive=*nextDrive;
    if(!Ext2BindDevice(drive,&dataDevice))return Fail("ext2-bind",-16);
    ++*nextDrive;
    if(!VfsMountConfigured("/nix","ext2",drive,6)) {
        VfsDrainMountCleanup();
        return Fail("mount",-5);
    }
    if(WritableDesktopPublishDirect(&details)!=WritableDesktopPublish::Ready) {
        VfsUnmount("/nix");
        return Fail(details.check,details.status);
    }
    if (const int transferError = BindProfileTransfer())
        KPrintf("usb-system: profile transfer unavailable (bind error %d)\n", transferError);
    if(!session.Arm())__builtin_trap();
    ready=true;
    boot.mounted=true;boot.dirty=true;boot.stage="ready";
    WritableDesktopMarkDirectReady(drive,systemWindow.length);
    SerialPrintf("USB_SYSTEM_READY disk=%d lba=%lu sectors=%lu groups=%u\n",
        boot.disk,record.first,record.sectors,geometry.groups);
    return true;
}

bool UsbSystemAllowsBootUpdates(Device* parent,uint64_t start,uint64_t length) {
    return !boot.configured || (disk && parent==disk && start==bootWindow.start && length==bootWindow.length);
}

int UsbSystemFinalSync(int syncError) {
    if(!ready)return syncError;
    const int error=syncError?syncError:IsUncertain()?-5:session.Finish(SessionIo(),DeviceFinalSyncOwner());
    __atomic_store_n(&finalError,error,__ATOMIC_RELEASE);
    __atomic_store_n(&finalAttempted,true,__ATOMIC_RELEASE);
    if(!error)__atomic_store_n(&clean,true,__ATOMIC_RELEASE);
    WritableDesktopDirectFinalResult(error,session.cleanAttempted);
    SerialPrintf("USB_SYSTEM_FINAL_SYNC result=%d clean=%u\n",error,error?0u:1u);
    return error;
}

bool UsbSystemTiming(UsbStorageTiming* out) {
    return disk && disk->ops->ioctl(disk,DEVICE_IOCTL_USB_STORAGE_TIMING,out)==0;
}
int UsbSystemBenchRead(uint64_t at,void* data,uint32_t bytes) {
    if(!ready || IsUncertain())return -19;
    return Read(&dataDevice,at,data,bytes);
}

UsbSystemStatus UsbSystemRead() {
    auto out=boot;
    out.window=__atomic_load_n(&open,__ATOMIC_ACQUIRE);
    out.uncertain=IsUncertain();
    out.window=out.window && !out.uncertain;
    out.finalError=__atomic_load_n(&finalError,__ATOMIC_ACQUIRE);
    out.finalAttempted=__atomic_load_n(&finalAttempted,__ATOMIC_ACQUIRE);
    out.clean=__atomic_load_n(&clean,__ATOMIC_ACQUIRE);
    out.dirty=out.dirty && !out.clean;
    out.reads=__atomic_load_n(&reads,__ATOMIC_RELAXED);
    out.writes=__atomic_load_n(&writes,__ATOMIC_RELAXED);
    out.flushes=__atomic_load_n(&flushes,__ATOMIC_RELAXED);
    out.errors=__atomic_load_n(&errors,__ATOMIC_RELAXED);
    return out;
}
}
