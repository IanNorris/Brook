#include "usb_system.h"
#include "usb_system_layout.h"
#include "apic.h"
#include "audio.h"

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
#include "exec_image.h"
#include "lastlog.h"
#include "process.h"
#include "scheduler.h"
#include "shell.h"
#include "shutdown.h"
#include "usb_system_fsck.h"
#include "usb_system_fsck_policy.h"
#include "vfs_path.h"
#include "../../shared/update/payload.h"
#include "serial.h"
#include "strong_random.h"
#include "string.h"
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
UsbReadHistogram sources[UsbReadSourceCount]{};
uint64_t admissionUs,transportUs,transportIdentityUs,sessionUs,mountUs;

void RecordRead(UsbReadSource source,uint64_t bytes) {
    const unsigned index=unsigned(source);
    if(index>=UsbReadSourceCount)return;
    const unsigned bucket=bytes<=512?0:bytes<=4096?1:bytes<=16384?2:bytes<=65536?3:4;
    __atomic_fetch_add(&sources[index].requests[bucket],1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&sources[index].bytes,bytes,__ATOMIC_RELAXED);
}

int ReadTagged(uint64_t offset,void* data,uint64_t size,UsbReadSource source) {
    if(!RangeValid(offset,size))return -22;
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED);
    RecordRead(source,size);
    const int result=disk->ops->read(disk,systemWindow.start+offset,data,size);
    return result==int(size)?result:IoError(result);
}
int Read(Device*,uint64_t offset,void* data,uint64_t size) {
    return ReadTagged(offset,data,size,ready?UsbReadSource::Other:UsbReadSource::Boot);
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
    if(request==DEVICE_IOCTL_USB_TAGGED_READ) {
        if(!argument)return 1;
        const auto& read=*static_cast<const UsbTaggedRead*>(argument);
        return ReadTagged(read.offset,read.data,read.bytes,read.source);
    }
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
        RecordRead(relative.source,relative.bytes);
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

static uint64_t MountClockNs() {
    const auto precise = AudioClockNowNs();
    return precise ? precise : ApicTickCount() * 1000000;
}

static int PrepareProfileBoot(const VfsPath& root) {
    constexpr const char* directory = "/data/.brook-transfer";
    int openError = 0;
    auto* listing = VfsOpenAt(root, root, directory, VFS_O_READ, 0, 0, &openError);
    if (!listing) {
        KPrintf("usb-system: cannot list profile transfers (%d)\n", openError);
        return openError ? openError : -5;
    }
    uint32_t cookie = 0;
    DirEntry entry{};
    int count;
    int error = 0;
    while ((count = VfsReaddir(listing, &entry, &cookie)) > 0) {
        if (entry.isDir || strlen(entry.name) != 37 || strcmp(entry.name + 32, ".part")) continue;
        bool transfer = true;
        for (unsigned i = 0; i < 32; ++i) {
            const char c = entry.name[i];
            transfer &= (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }
        if (!transfer) continue;
        char path[80] = "/data/.brook-transfer/";
        memcpy(path + strlen(path), entry.name, 38);
        error = VfsRemoveAt(root, root, path, false);
        if (error) break;
        cookie = 0;
    }
    VfsClose(listing);
    if (error || count < 0) return error ? error : count;
    uint8_t random[16];
    if (!StrongRandomBytes(random, sizeof(random))) {
        KPrintf("usb-system: profile boot entropy unavailable\n");
        return -5;
    }
    char text[33];
    ShowHex(text, random);
    auto* writer = VfsOpenAt(root, root, "/data/.brook-profile-boot",
                             VFS_O_CREATE | VFS_O_EXCL | VFS_O_WRITE, 0644, 0, &openError);
    if (!writer) {
        KPrintf("usb-system: cannot create profile boot identity (%d)\n", openError);
        return openError ? openError : -5;
    }
    uint64_t offset = 0;
    const int written = VfsWrite(writer, text, 32, &offset);
    VfsClose(writer);
    return written == 32 ? 0 : -5;
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
    if (!error) error = PrepareProfileBoot(root);
    VfsPathRelease(root);
    boot.profileTransferReady = !error;
    return error;
}

namespace {
// Boot-time repair state (usb_system_fsck.h). The checker image is loaded and
// verified before the scheduler starts and released once the child exists.
ExecutableImage checker{};
LargeExt2Geometry admitted{};
uint8_t reservedDrive;
bool fsckPending;
int fsckExit=-1;

void ReleaseChecker() {if(checker.data)FreeExecutableImage(&checker);checker={};}

bool Refuse(const char* reason,int detail) {
    ReleaseChecker();
    if(!strcmp(boot.fsck,"running"))boot.fsck="refused";
    KPrintf("usb-system: BROOK_SYSTEM was not repaired: %s (%d). Run e2fsck -f on BROOK_SYSTEM from Linux.\n",
            reason,detail);
    return Fail("needs-filesystem-check",-117);
}

// The checker must be the pinned static e2fsck. Anything else keeps the
// pre-repair refusal, so a missing or replaced binary is never executed.
bool PrepareChecker() {
    VnodeStat marker{};
    if(VfsLstatPath(UsbSystemFsckOptOut,&marker)!=-2) {
        boot.fsck="disabled";
        KPrintf("usb-system: automatic repair disabled by %s\n",UsbSystemFsckOptOut);
        return false;
    }
    Vnode* node=nullptr;char resolved[256]{};VnodeStat stat{};
    int error=VfsOpenExecutable(UsbSystemFsckPath,&node,resolved,sizeof(resolved));
    if(!error)error=VfsStat(node,&stat);
    if(!error && (stat.isDir || stat.isSymlink || !stat.size || stat.size>UsbSystemFsckMaxBytes))error=-8;
    if(!error)error=ReadExecutableImage(node,&checker);
    if(node)VfsRelease(node);
    const char* problem=error==-2?"missing":"unreadable";
    if(!error) {
        uint8_t hash[32]{};char interpreter[256]{};
        BrookUpdateHash(checker.data,checker.size,hash);
        if(!UsbSystemFsckDigestMatches(hash)) {problem="digest mismatch";error=-8;}
        else if(!ElfMainInterpreter(checker.data,checker.size,interpreter,sizeof(interpreter)) || interpreter[0]) {
            problem="not a static executable";error=-8;
        }
    }
    if(error) {
        ReleaseChecker();
        boot.fsck="unavailable";
        KPrintf("usb-system: automatic repair unavailable: %s %s (%d)\n",UsbSystemFsckPath,problem,error);
        return false;
    }
    return true;
}

LargeExt2HeaderState ReadState() {
    uint8_t sb[1024];
    LargeExt2Geometry geometry{};
    if(!ReadExact(disk,systemWindow.start+1024,sb,sizeof(sb)))return LargeExt2HeaderState::Invalid;
    const auto state=LargeExt2ClassifyHeader(sb,sizeof(sb),systemWindow.length,record.uuid,geometry);
    if(state!=LargeExt2HeaderState::Invalid && geometry.groups!=admitted.groups)return LargeExt2HeaderState::Invalid;
    return state;
}

// Shared by the direct boot path and the post-repair path. The adapter is
// registered and leased; the extent's header has just been classified Clean.
bool Activate(uint8_t drive) {
    auto phase=MountClockNs();
    constexpr uint32_t gdtCapacity=4096*32;
    auto* gdt=static_cast<uint8_t*>(kmalloc(gdtCapacity));
    if(!gdt)return Fail("descriptor-buffer",-12);
    int error=__atomic_load_n(&open,__ATOMIC_ACQUIRE)?0:SetWindow(true);
    if(!error)error=DeviceFlush(&dataDevice);
    if(!error)error=MaybeTrim(gdt,gdtCapacity);
    if(!error && !session.dirty)
        error=session.Begin(SessionIo(),systemWindow.length,record.uuid,gdt,gdtCapacity);
    kfree(gdt);
    if(error)return Fail(error==-117?"needs-filesystem-check":"session-begin",error);
    sessionUs=(MountClockNs()-phase)/1000;phase=MountClockNs();
    if(!Ext2BindDevice(drive,&dataDevice))return Fail("ext2-bind",-16);
    if(!VfsMountConfigured("/nix","ext2",drive,6)) {
        VfsDrainMountCleanup();
        return Fail("mount",-5);
    }
    WritableDesktopFailure details{};
    if(WritableDesktopPublishDirect(&details)!=WritableDesktopPublish::Ready) {
        VfsUnmount("/nix");
        return Fail(details.check,details.status);
    }
    if (const int transferError = BindProfileTransfer())
        KPrintf("usb-system: profile transfer unavailable (bind error %d)\n", transferError);
    if(!session.Arm())__builtin_trap();
    mountUs=(MountClockNs()-phase)/1000;
    ready=true;
    boot.mounted=true;boot.dirty=true;boot.stage="ready";
    WritableDesktopMarkDirectReady(drive,systemWindow.length);
    SerialPrintf("USB_SYSTEM_READY disk=%d lba=%lu sectors=%lu groups=%u\n",
        boot.disk,record.first,record.sectors,admitted.groups);
    return true;
}

// The transcript was already in the boot log as the checker wrote it. Keep a
// copy beside lastlog and repeat it so the first lastlog record carries it.
void PublishTranscript() {
    uint32_t length=0;bool truncated=false;
    const char* text=UsbSystemFsckTranscriptText(&length,&truncated);
    KPrintf("usb-system: e2fsck exit=%d repaired BROOK_SYSTEM; transcript%s:\n",fsckExit,
            truncated?" (truncated)":"");
    for(uint32_t at=0;at<length;) {
        char line[200];uint32_t n=0;
        while(at<length && text[at]!='\n' && n<sizeof(line)-1)line[n++]=text[at++];
        if(at<length && text[at]=='\n')++at;
        line[n]=0;
        KPrintf("usb-system: e2fsck| %s\n",line);
    }
    VfsPath root{};
    int error=VfsPathRoot(&root);
    const char* directories[]={"/nix/var","/nix/var/log","/nix/var/log/brook"};
    for(const char* directory:directories) {
        if(error)break;
        const int made=VfsMkdirAt(root,root,directory,0755,0);
        if(made && made!=-17)error=made;
    }
    Vnode* file=nullptr;
    if(!error)file=VfsOpenAt(root,root,"/nix/var/log/brook/usb-system-fsck.log",
                             VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC,0644,0,&error);
    if(file) {
        char header[96]="brook usb-system: e2fsck -p -f exit=";
        int bytes=int(strlen(header));
        const int code=fsckExit<0?0:fsckExit;
        if(code>=100)header[bytes++]=char('0'+code/100);
        if(code>=10)header[bytes++]=char('0'+code/10%10);
        header[bytes++]=char('0'+code%10);
        const char* tail=truncated?" transcript truncated\n":"\n";
        memcpy(header+bytes,tail,strlen(tail)+1);bytes+=int(strlen(tail));
        uint64_t offset=0;
        int written=VfsWrite(file,header,uint32_t(bytes),&offset);
        if(written==bytes && length)written=VfsWrite(file,text,length,&offset)==int(length)?0:-5;
        else if(written==bytes)written=0;
        error=written<0?written:written?-5:0;
        if(!error)error=VfsFsync(file);
        VfsClose(file);
    } else if(!error) error=-5;
    VfsPathRelease(root);
    if(error)KPrintf("usb-system: cannot keep the e2fsck transcript (%d)\n",error);
    else KPrintf("usb-system: e2fsck transcript kept in /nix/var/log/brook/usb-system-fsck.log\n");
    LastLogRequestFlush();
}

// Runs e2fsck -p -f as root on /dev/brook-usb-system, the leased adapter of
// exactly the validated extent, then mounts, reboots or refuses.
bool Repair() {
    KPrintf("usb-system: running %s -p -f on BROOK_SYSTEM (lba %lu, %lu sectors)\n",
            UsbSystemFsckPath,record.first,record.sectors);
    boot.fsck="running";
    if(const int error=SetWindow(true))return Refuse("write window",error);
    const char* argv[]={UsbSystemFsckPath,"-p","-f",UsbSystemFsckDevice,nullptr};
    const char* env[]={"PATH=/boot","LANG=C",nullptr};
    FdEntry fds[3]{};fds[0].type=FdType::DevNull;fds[1].type=fds[2].type=FdType::DebugConsole;
    Process* child=ProcessCreateExecutable(checker,4,argv,2,env,fds,0,0);
    ReleaseChecker();
    if(!child)return Refuse("cannot start e2fsck",-12);
    child->parentPid=ProcessCurrent()->tgid;
    FdReleaseOwnedEntry(child->fds[3]);child->fds[3]={};
    const uint16_t pid=child->pid;const uint64_t incarnation=child->incarnation;
    if(!UsbSystemFsckSessionBegin(&dataDevice,child)) {
        ProcessDiscardUnpublished(child);
        return Refuse("device session",-13);
    }
    SchedulerAddProcess(child);child=nullptr;
    // A large store over USB can take minutes. A hung checker must not hold
    // the boot forever; the refusal keeps the extent for a host check.
    constexpr unsigned limitMs=15*60*1000,pollMs=100;
    ChildWaitSnapshot result{};int waited=0;
    for(unsigned elapsed=0;elapsed<limitMs && !waited;elapsed+=pollMs) {
        waited=SchedulerWaitChild(ProcessCurrent(),1,pid,4,&result);
        if(!waited)SchedulerSleepMs(pollMs);
    }
    if(waited==1 && result.incarnation!=incarnation)waited=-3;
    if(waited!=1) {
        const int failure=waited;
        if(auto* process=SchedulerFindProcessReference(pid)) {
            if(process->incarnation==incarnation)SchedulerRequestFatalSignal(process,9);
            ProcessUnref(process);
        }
        for(unsigned i=0;i<3000 && waited==0;++i) {
            SchedulerSleepMs(10);waited=SchedulerWaitChild(ProcessCurrent(),1,pid,4,&result);
        }
        UsbSystemFsckSessionEnd();
        return Refuse(failure?"e2fsck wait failed":"e2fsck timed out",failure);
    }
    // The child has been reaped, so its descriptors are closed.
    if(!UsbSystemFsckSessionEnd())return Refuse("device view still open",-16);
    const bool exited=result.code==1;
    fsckExit=exited?(result.status>>8)&0xff:-1;
    const auto action=UsbSystemFsckClassify(exited,fsckExit);
    KPrintf("USB_SYSTEM_FSCK exit=%d signal=%d action=%s\n",fsckExit,exited?0:result.status&0x7f,
            action==UsbSystemFsckAction::Continue?"mount":action==UsbSystemFsckAction::Reboot?"reboot":"refuse");
    if(action==UsbSystemFsckAction::Refuse) {
        boot.fsck="refused";
        return Refuse(exited?"e2fsck left errors uncorrected":"e2fsck did not exit normally",
                      exited?fsckExit:-(result.status&0x7f));
    }
    if(const int flushed=DeviceFlush(&dataDevice))return Refuse("flush after e2fsck",flushed);
    // Never continue, or reboot into another check, unless the checker
    // actually left the filesystem marked clean.
    if(ReadState()!=LargeExt2HeaderState::Clean) {
        boot.fsck="refused";
        return Refuse("e2fsck left the filesystem marked not clean",fsckExit);
    }
    if(action==UsbSystemFsckAction::Reboot) {
        SetWindow(false);
        boot.fsck="reboot";boot.stage="filesystem-check-reboot";
        const auto power=ShutdownGetPowerCapabilities();
        if(!power.gracefulQuiescence || !power.reset || !ShutdownLaunchAllowed()) {
            boot.fsck="refused";
            return Refuse("e2fsck asked for a reboot, which is unavailable",fsckExit);
        }
        KPrintf("usb-system: e2fsck repaired BROOK_SYSTEM and asked for a reboot; restarting\n");
        ShutdownDesktopRequest(true);
        return false;
    }
    boot.fsck="repaired";
    if(!Activate(reservedDrive))return false;
    PublishTranscript();
    return true;
}

void RepairWorker(void*) {
    const bool mounted=Repair();
    if(!mounted && !strcmp(boot.fsck,"reboot"))return;
    if(!ShutdownLaunchAllowed())return;
    if(mounted) {
        UsbSystemPrepareDesktopHome();
        const int script=ShellExecScript("/boot/INIT.RC");
        if(script)KPrintf("usb-system: boot script failed after repair (%d)\n",script);
    } else ShellExecString("set wm\nset vfb none\nkconsole\n");
}
}

bool UsbSystemBoot(uint8_t* nextDrive,bool scanComplete) {
    auto phase=MountClockNs();
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
    if(!ReadExact(disk,systemWindow.start+1024,sb,sizeof(sb)))return Fail("ext2-read",-5);
    const auto state=LargeExt2ClassifyHeader(sb,sizeof(sb),systemWindow.length,record.uuid,admitted);
    const bool needsCheck=state==LargeExt2HeaderState::NeedsCheck;
    if(needsCheck) {
        boot.dirty=true;
        KPrintf("usb-system: BROOK_SYSTEM was not cleanly unmounted\n");
        if(!PrepareChecker())return Fail("needs-filesystem-check",-117);
    } else if(state!=LargeExt2HeaderState::Clean)return Fail("ext2-identity",-22);
    WritableDesktopFailure details{};
    VnodeStat target{};
    if(!Ext2DriveAvailable(*nextDrive) || !WritableDesktopPrepareDirect(&details) ||
       VfsLstatPath("/nix",&target) || !target.isDir || target.isSymlink) {
        ReleaseChecker();
        return Fail("mount-target",details.status?details.status:-22);
    }

    admissionUs=(MountClockNs()-phase)/1000;phase=MountClockNs();
    const int transport=disk->ops->ioctl(disk,DEVICE_IOCTL_USB_SYSTEM_TRANSPORT,nullptr);
    const auto identifyStart=MountClockNs();
    if(transport && transport!=-95) {ReleaseChecker();return Fail("storage-transport",transport);}
    // The extent must classify exactly as it did before the transport change.
    if(!transport && (!ValidateGpt(disk,bootGuid,bootEntry,bootFirst,bootSectors) || ReadState()!=state)) {
        ReleaseChecker();
        return Fail("storage-transport-identity",-22);
    }

    transportIdentityUs=(MountClockNs()-identifyStart)/1000;
    transportUs=(MountClockNs()-phase)/1000;
    const BlockWriteLease lease{reinterpret_cast<uintptr_t>(disk),reinterpret_cast<uintptr_t>(&dataDevice),
        systemWindow.start,systemWindow.length};
    bool owned=DeviceRegister(&dataDevice);
    if(owned && !DeviceInstallBootWriteLeases(&lease,1)) {DeviceUnregister(&dataDevice);owned=false;}
    if(!owned) {ReleaseChecker();return Fail("session-begin",-16);}
    const uint8_t drive=(*nextDrive)++;
    if(needsCheck) {
        // e2fsck needs the scheduler. UsbSystemStartRepair runs it, then
        // mounts and starts the boot script, or keeps the refusal.
        reservedDrive=drive;fsckPending=true;
        boot.stage="filesystem-check";boot.fsck="pending";
        return false;
    }
    return Activate(drive);
}

bool UsbSystemRepairPending() {return fsckPending;}

bool UsbSystemStartRepair() {
    if(!fsckPending || !checker.data)return false;
    fsckPending=false;
    auto* worker=KernelThreadCreate("usb_system_fsck",RepairWorker,nullptr);
    if(!worker)return Refuse("cannot start the repair thread",-12);
    SchedulerAddProcess(worker);
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
    if(!disk || disk->ops->ioctl(disk,DEVICE_IOCTL_USB_STORAGE_TIMING,out))return false;
    for(unsigned i=0;i<UsbReadSourceCount;++i) {
        for(unsigned j=0;j<UsbReadBuckets;++j)
            out->readsBySource[i].requests[j]=__atomic_load_n(&sources[i].requests[j],__ATOMIC_RELAXED);
        out->readsBySource[i].bytes=__atomic_load_n(&sources[i].bytes,__ATOMIC_RELAXED);
    }
    out->admissionUs=admissionUs;out->transportUs=transportUs;
    out->transportIdentityUs=transportIdentityUs;
    out->sessionUs=sessionUs;out->mountUs=mountUs;
    return true;
}
int UsbSystemBenchRead(uint64_t at,void* data,uint32_t bytes) {
    if(!ready || IsUncertain())return -19;
    return ReadTagged(at,data,bytes,UsbReadSource::Bench);
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
    out.fsckExit=fsckExit;
    return out;
}
}
