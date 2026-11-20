// syscall_maintenance_block.cpp -- maintenance-only block device descriptors (write-test and installer builds).
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
#include "strict_gpt_inventory.h"
#endif

#include "ahci_readonly.h"
#include "syscall_internal.h"
#include "syscall_maintenance_block.h"
#include "syscall_rw.h"

namespace brook {

#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
// A session reserves one immutable range for one thread group. Separate opens
// in that group have independent offsets; inherited descriptors retain the
// reservation, but do not grant a child permission to open the disk again.
SpinLock maintenanceBlockLock{};
uint32_t maintenanceBlockHolders=0,maintenanceBlockPublished=0;
static uint16_t maintenanceBlockOwner=0;
uint64_t maintenanceBlockOwnerIncarnation=0,maintenanceBlockStart=0,maintenanceBlockBytes=0;
bool MaintenanceBlockReserve(Process* proc,uint32_t partition,bool& first,uint64_t& start,uint64_t& bytes) {
    const uint16_t owner=proc->tgid;
    const uint64_t incarnation=proc->pid==owner?proc->incarnation:proc->leaderIncarnation;
    if(!owner || !incarnation)return false;
    SpinLockAcquire(&maintenanceBlockLock);
    first=maintenanceBlockHolders==0;
    const bool allowed=first || (maintenanceBlockPublished==partition+1 &&
        maintenanceBlockOwner==owner && maintenanceBlockOwnerIncarnation==incarnation);
    if(allowed) {
        if(first){maintenanceBlockOwner=owner;maintenanceBlockOwnerIncarnation=incarnation;maintenanceBlockPublished=0;}
        ++maintenanceBlockHolders;start=maintenanceBlockStart;bytes=maintenanceBlockBytes;
    }
    SpinLockRelease(&maintenanceBlockLock);return allowed;
}
void MaintenanceBlockRelease() {
    SpinLockAcquire(&maintenanceBlockLock);
    if(--maintenanceBlockHolders==0){maintenanceBlockPublished=0;maintenanceBlockOwner=0;maintenanceBlockOwnerIncarnation=0;}
    SpinLockRelease(&maintenanceBlockLock);
}
Device* MaintenanceBlockDevice() {
#if defined(BROOK_SSD_INSTALLER)
    return AhciPhysicalBlockToolsDevice();
#else
    return AhciDisposableQemuBlockToolsDevice();
#endif
}
bool MaintenanceBlockPath(const char* path,uint32_t& partition) {
#if defined(BROOK_SSD_INSTALLER)
    static constexpr char prefix[]="/dev/brook-install-disk";
#else
    static constexpr char prefix[]="/dev/brook-test-disk";
#endif
    if(strncmp(path,prefix,sizeof(prefix)-1))return false;
    path+=sizeof(prefix)-1;partition=0;
    if(!*path)return true;
    if(*path<'1' || *path>'9')return false;
    while(*path>='0' && *path<='9') {
        partition=partition*10+uint32_t(*path++-'0');
        if(partition>128)return false;
    }
    return !*path;
}
// Caller holds the exclusive maintenance-disk admission, so the layout
// cannot change between validation and opening the resulting partition.
bool MaintenanceBlockRange(Device* device,uint32_t index,uint64_t& start,uint64_t& bytes) {
    const uint64_t sectors=DeviceBlockCount(device),size=DeviceBlockSize(device);
    if(size!=512 || sectors<68 || sectors>0x7fffffffffffffffULL/size)return false;
    start=0;bytes=sectors*size;if(!index)return true;
    uint8_t mbr[512],primary[512],backup[512];
    auto read=[&](uint64_t offset,void* buf,uint32_t len) {
        return device->ops->read(device,offset,buf,len)==int(len);
    };
    if(!read(0,mbr,512) || !read(512,primary,512) || !read((sectors-1)*512,backup,512) ||
       StrictGptProtectiveMbr(mbr,sectors)!=StrictMbrResult::Protective)return false;
    StrictGptHeader a{},b{};
    if(StrictGptCheckHeader(primary,512,sectors,false,a)!=StrictGptResult::Ok ||
       StrictGptCheckHeader(backup,512,sectors,true,b)!=StrictGptResult::Ok ||
       a.count!=b.count || a.entrySize!=b.entrySize || index>a.count)return false;
    const uint32_t length=a.count*a.entrySize;
    auto* entries=static_cast<uint8_t*>(kmalloc(length));
    auto* copy=static_cast<uint8_t*>(kmalloc(length));
    if(!entries || !copy) {if(entries)kfree(entries);if(copy)kfree(copy);return false;}
    const bool valid=read(a.entriesLba*512,entries,length) && read(b.entriesLba*512,copy,length) &&
        StrictGptCheckCopies(a,entries,b,copy,length)==StrictGptResult::Ok;
    bool found=false;
    if(valid) {
        const auto* entry=entries+uint64_t(index-1)*a.entrySize;
        if(!StrictAllZero(entry,16)) {
            const uint64_t first=StrictLe64(entry+32),last=StrictLe64(entry+40);
            start=first*512;bytes=(last-first+1)*512;found=true;
        }
    }
    kfree(entries);kfree(copy);return found;
}
int64_t MaintenanceBlockTransfer(MaintenanceBlockFd* file,uint64_t address,
                                 uint64_t count,uint64_t* position,
                                 bool write,bool userBuffer) {
    if(!file || !position)return -EBADF;
    if(!VfsCanAccessSystemFiles())return -EPERM;
    if(*position>=file->bytes)return write && count ? -28 : 0;
    if(count>file->bytes-*position)count=file->bytes-*position;
    if(count>VnodeWriteLimit)count=VnodeWriteLimit;
    if(!count)return 0;
    constexpr uint64_t chunkSize=16384;
    auto* buffer=static_cast<uint8_t*>(kmalloc(chunkSize));
    if(!buffer)return -ENOMEM;
    uint64_t completed=0;
    int error=0;
    while(completed<count) {
        const uint64_t n=count-completed<chunkSize?count-completed:chunkSize;
        if(write) {
            if(userBuffer) {
                if(!CopyFromUser(buffer,address+completed,n)){error=-EFAULT;break;}
            } else memcpy(buffer,reinterpret_cast<const void*>(address+completed),n);
            const int result=DeviceWrite(file->device,file->start+*position,buffer,n);
            if(result<=0 || uint64_t(result)>n){error=result<0?result:-EIO;break;}
            *position+=uint64_t(result);completed+=uint64_t(result);
            if(uint64_t(result)!=n)break;
        } else {
            const int result=file->device->ops->read(file->device,file->start+*position,buffer,n);
            if(result<=0 || uint64_t(result)>n){error=result<0?result:-EIO;break;}
            if(!CopyToUser(address+completed,buffer,uint64_t(result))){error=-EFAULT;break;}
            *position+=uint64_t(result);completed+=uint64_t(result);
            if(uint64_t(result)!=n)break;
        }
    }
    kfree(buffer);return completed?int64_t(completed):error;
}
#endif

} // namespace brook
