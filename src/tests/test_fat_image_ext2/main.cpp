#include "../../kernel/src/process.h"
#include "../../kernel/src/process_user_namespace.h"
// Actual Ext2 backend bodies on a private mkfs.ext2 image. Host shims supply
// allocation, a memory disk, synchronization and a single credential identity;
// filesystem lookup/mutation/inode lifetime/open logic is production code.
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <map>
#include <memory>
#include <shared_mutex>
#include <unordered_set>
#include <vector>
#include <thread>
#include <atomic>
#include "retained_path_walk.h"
#include "vfs_path.h"
#include "fs_context.h"
#include "mount_namespace.h"
#include "../../kernel/src/ext2_vfs.cpp"
#include "fat_image_device.h"
#include "fatfs_vfs.h"
#include "fatfs_glue.h"
#include "diskio.h"
#include <unistd.h>
#include <sys/wait.h>

namespace {
std::map<brook::KRwLock*,std::unique_ptr<std::shared_mutex>> fsLocks;
std::map<brook::KMutex*,std::unique_ptr<std::mutex>> fatLocks;
bool driveAcquired=false,failLowerFlush=false;
brook::Device* imageRegistered=nullptr;
std::unordered_set<void*> allocations,frames;
std::mutex trackingLock;
thread_local long failAllocation=-1;
thread_local bool failTriggered=false;
unsigned requiredAccess=0;
brook::Process process{};
uint16_t legacyTestMask=022;
std::vector<unsigned char> disk;
uint64_t failWriteOffset=~uint64_t{0};
uint64_t nextFailWriteOffset=~uint64_t{0};
bool writeFailureInjected=false;
unsigned injectedWriteFailures=0;
uint64_t shortWriteOffset=~uint64_t{0};
int readDisk(brook::Device* device,uint64_t offset,void* out,uint64_t bytes){
    auto& storage=device->priv?*static_cast<std::vector<unsigned char>*>(device->priv):disk;
    if(offset>storage.size() || bytes>storage.size()-offset)return -5;
    std::memcpy(out,storage.data()+offset,bytes);return bytes;
}
int writeDisk(brook::Device* device,uint64_t offset,const void* in,uint64_t bytes){
    if(offset==failWriteOffset){failWriteOffset=nextFailWriteOffset;nextFailWriteOffset=~uint64_t{0};writeFailureInjected=true;++injectedWriteFailures;return -5;}
    auto& storage=device->priv?*static_cast<std::vector<unsigned char>*>(device->priv):disk;
    if(offset>storage.size() || bytes>storage.size()-offset)return -5;
    if(offset==shortWriteOffset) {
        shortWriteOffset=~uint64_t{0};
        // Data reached the disk, but the completion reports a short write.
        std::memcpy(storage.data()+offset,in,bytes);return bytes-1;
    }
    std::memcpy(storage.data()+offset,in,bytes);return bytes;
}
int ioctlDisk(brook::Device*,uint32_t,void*){return 0;}
unsigned flushCalls=0;bool failFlush=false;
const brook::DeviceOps diskOps={readDisk,writeDisk,ioctlDisk,nullptr};
void closeNode(brook::Vnode* node){assert(node);auto* object=node->backendObject;node->ops->close(node);brook::kfree(node);brook::VfsObjectRelease(object);}
}
namespace brook {
bool ProcessCapable(Process* p,UserNamespace* target,unsigned capability) {
    return p && UserNamespaceCapable({p->userNamespace,p->euid,p->egid,p->capabilities.effective},target,capability);
}

extern "C" [[noreturn]] void KernelPanic(const char*,...) {std::abort();}
extern "C" void* kmalloc(uint64_t size){
    if(failAllocation==0){failAllocation=-1;failTriggered=true;return nullptr;}
    if(failAllocation>0)--failAllocation;
    void* p=std::malloc(size);if(p){std::lock_guard<std::mutex> lock(trackingLock);allocations.insert(p);std::memset(p,0xcc,size);}return p;
}
extern "C" void kfree(void* p){if(p){std::lock_guard<std::mutex> lock(trackingLock);assert(allocations.erase(p)==1);std::free(p);}}
PhysicalAddress PmmAllocPage(MemTag,uint16_t){
    void* p=nullptr;assert(posix_memalign(&p,4096,4096)==0);{std::lock_guard<std::mutex> lock(trackingLock);frames.insert(p);}return PhysicalAddress(reinterpret_cast<uint64_t>(p));
}
void PmmFreePage(PhysicalAddress p){if(!p)return;void* memory=reinterpret_cast<void*>(p.raw());{std::lock_guard<std::mutex> lock(trackingLock);assert(frames.erase(memory)==1);}std::free(memory);}
void KRwLockInit(KRwLock* lock){*lock={};fsLocks[lock]=std::make_unique<std::shared_mutex>();}
void KRwLockReadLock(KRwLock* lock){fsLocks.at(lock)->lock_shared();}
void KRwLockReadUnlock(KRwLock* lock){fsLocks.at(lock)->unlock_shared();}
void KRwLockWriteLock(KRwLock* lock){fsLocks.at(lock)->lock();}
void KRwLockWriteUnlock(KRwLock* lock){fsLocks.at(lock)->unlock();}
void FileObjectInit(FileObject* object){*object={};}
// This backend fixture bypasses cache/coherence, which has dedicated tests.
int FileObjectRead(FileObject*,Vnode* vn,void* out,uint64_t bytes,uint64_t* offset){return vn->ops->read(vn,out,bytes,offset);}
int FileObjectWrite(FileObject*,Vnode* vn,const void* in,uint64_t bytes,uint64_t* offset){return vn->ops->write(vn,in,bytes,offset);}
int FileObjectTruncateUnmapped(FileObject*,Vnode*,uint64_t){assert(false);return -95;}
int FileObjectGrowMapped(FileObject*,Vnode*,uint64_t){assert(false);return -95;}
int FileObjectWriteback(FileObject* object,Vnode*,uint64_t,uint64_t){for(auto* page:object->pages)assert(!page);return 0;}
int FileObjectWritebackAll(){return 0;}
void FileObjectDispose(FileObject* object){assert(!object->mappings && !object->mappingRegistrations);for(auto* p:object->pages)assert(!p);}
Process* ProcessCurrent(){return &process;}
int ProcessFsMask(Process*){return legacyTestMask;}
CredentialIds ProcessReadIds(Process* p,bool group){return group?CredentialIds{p->gid,p->egid,p->sgid}:CredentialIds{p->uid,p->euid,p->suid};}
FileCredentialAccess ProcessFileAccess(Process* p,uint32_t owner,uint32_t group,uint16_t mode,bool realIds){
    auto uid=realIds?p->uid:p->euid;auto gid=realIds?p->gid:p->egid;
    bool root=uid==0;uint8_t bits=root?7:uid==owner?(mode>>6)&7:gid==group?(mode>>3)&7:mode&7;
    return {root,uid==owner,gid==group,bits};
}
int ProcessCheckFileAccess(Process* p,uint32_t owner,uint32_t group,uint16_t mode,uint8_t required,bool realIds){
    requiredAccess=required;auto access=ProcessFileAccess(p,owner,group,mode,realIds);
    if(access.privileged && (required&1) && (mode&0170000)==0100000 && !(mode&0111))return -13;
    return (access.permissions&required)==required?0:-13;
}
uint8_t ProcessFilePermissions(Process* p,uint32_t owner,uint32_t group,uint16_t mode){return ProcessFileAccess(p,owner,group,mode).permissions;}
uint32_t FilePageCacheAllocateNamespace(){static uint32_t next=0;return ++next;}
uint64_t RtcNow(){return 1792627200;}
uint64_t RtcNowNs(){static uint64_t n=0;return ++n;}
int DeviceFlush(Device* device){++flushCalls;return failFlush?-5:device->ops->ioctl(device,DEVICE_IOCTL_FLUSH,nullptr);}
extern "C" bool DeviceIsRegistered(Device* device){return device==imageRegistered;}
void KMutexInit(KMutex* mutex){*mutex={};fatLocks[mutex]=std::make_unique<std::mutex>();}
void KMutexLock(KMutex* mutex){fatLocks.at(mutex)->lock();}
void KMutexUnlock(KMutex* mutex){fatLocks.at(mutex)->unlock();}
VirtualAddress VmmAllocPages(uint64_t count,uint64_t,MemTag,uint16_t){
    void* p=nullptr;if(posix_memalign(&p,4096,count*4096))return VirtualAddress{};
    {std::lock_guard<std::mutex> lock(trackingLock);frames.insert(p);}return VirtualAddress(reinterpret_cast<uint64_t>(p));
}
void VmmFreePages(VirtualAddress address,uint64_t){void* p=reinterpret_cast<void*>(address.raw());{std::lock_guard<std::mutex> lock(trackingLock);assert(frames.erase(p)==1);}std::free(p);}
int FatFsFlushDrive(uint8_t drive){return drive||failLowerFlush?-5:0;}
bool FatFsAcquireDrive(uint8_t drive,bool* ro){if(drive||driveAcquired)return false;driveAcquired=true;*ro=false;return true;}
void FatFsReleaseDrive(uint8_t){assert(driveAcquired);driveAcquired=false;}
bool FatFsDriveReadOnly(uint8_t){return false;}
extern "C" int DeviceWrite(Device* device,uint64_t offset,const void* bytes,uint64_t size){return device->ops->write(device,offset,bytes,size);}
extern "C" void SerialPrintf(const char*,...){}
void SerialPuts(const char*){}
}

extern "C" DSTATUS disk_initialize(BYTE drive){return drive?STA_NOINIT:0;}
extern "C" DSTATUS disk_status(BYTE drive){return drive?STA_NOINIT:0;}
extern "C" DRESULT disk_read(BYTE drive,BYTE* out,LBA_t sector,UINT count){
    brook::Device outer{&diskOps,"outer",brook::DeviceType::Block,nullptr};
    const auto bytes=uint64_t(count)*512;
    return !drive&&readDisk(&outer,uint64_t(sector)*512,out,bytes)==int(bytes)?RES_OK:RES_ERROR;
}
extern "C" DRESULT disk_write(BYTE drive,const BYTE* in,LBA_t sector,UINT count){
    brook::Device outer{&diskOps,"outer",brook::DeviceType::Block,nullptr};
    const auto bytes=uint64_t(count)*512;
    return !drive&&writeDisk(&outer,uint64_t(sector)*512,in,bytes)==int(bytes)?RES_OK:RES_ERROR;
}
extern "C" DRESULT disk_ioctl(BYTE drive,BYTE command,void* out){
    if(drive)return RES_PARERR;
    if(command==CTRL_SYNC)return failLowerFlush?RES_ERROR:RES_OK;
    if(command==GET_SECTOR_COUNT){*static_cast<LBA_t*>(out)=disk.size()/512;return RES_OK;}
    if(command==GET_SECTOR_SIZE){*static_cast<WORD*>(out)=512;return RES_OK;}
    if(command==GET_BLOCK_SIZE){*static_cast<DWORD*>(out)=1;return RES_OK;}
    return RES_PARERR;
}
extern "C" DWORD get_fattime(){return (DWORD(46)<<25)|(DWORD(10)<<21)|(DWORD(30)<<16);}
int main(int argc,char** argv){
    assert(argc==2);std::ifstream input(argv[1],std::ios::binary);assert(input);
    disk.assign(std::istreambuf_iterator<char>(input),{});input.close();
    using namespace brook;
    process.uid=process.euid=process.suid=0;process.gid=process.egid=process.sgid=0;
    VfsInit();FatFsVfsRegister();Ext2VfsRegister();g_ext2WritebackMiB=0;
    assert(VfsMount("/","fatfs",0));assert(!VfsMkdir("/nix",0755));
    auto* source=VfsOpen("/STORE.IMG",VFS_O_WRITE|VFS_O_READWRITE);assert(source);
    VnodeStat sourceStat{};assert(!VfsStat(source,&sourceStat));int error=0;
    auto* device=FatImageDeviceCreate(source,sourceStat.size,"usb-nix-image",&error);assert(device&&!error);
    VfsRelease(source);imageRegistered=device;assert(Ext2BindDevice(0,device));
    Device replacement{&diskOps,"replacement",DeviceType::Block,nullptr};
    assert(Ext2BindDevice(0,device) && !Ext2BindDevice(0,&replacement) && g_ext2Devices[0]==device);
    assert(VfsMount("/nix","ext2",0));
    assert(!VfsOpen("/STORE.IMG",VFS_O_READ));
    assert(VfsUnlink("/STORE.IMG")==-16);
    assert(!VfsMkdir("/nix/packages",0755));
    auto* file=VfsOpen("/nix/packages/persist",VFS_O_CREATE|VFS_O_WRITE|VFS_O_READWRITE);assert(file);
    std::vector<uint8_t> expected(131077),actual(expected.size());
    for(unsigned i=0;i<expected.size();++i)expected[i]=static_cast<uint8_t>(i*37+(i>>8));
    uint64_t offset=0;assert(VfsWrite(file,expected.data(),expected.size(),&offset)==int(expected.size()));
    // Actual 128B inode updates and4B indirect block-map accesses pass through
    // the byte-oriented backing Device, in addition to these unaligned writes.
    const uint8_t patch[7]={1,2,3,4,5,6,7};offset=509;
    assert(VfsWrite(file,patch,sizeof(patch),&offset)==7);std::memcpy(expected.data()+509,patch,7);
    assert(!VfsFsync(file));offset=0;
    assert(VfsRead(file,actual.data(),actual.size(),&offset)==int(actual.size())&&actual==expected);
    const pid_t failure=fork();assert(failure>=0);
    if(!failure){
        alarm(3);failLowerFlush=true;assert(VfsFsync(file)==-5);failLowerFlush=false;
        assert(DeviceFlush(device)==-5 && VfsFsync(file)==-5);
        assert(VfsSync(nullptr)==-5);_exit(0);
    }
    int status=0;assert(waitpid(failure,&status,0)==failure && WIFEXITED(status) && !WEXITSTATUS(status));
    VfsRelease(file);assert(!VfsSync(nullptr));
    assert(VfsUnmount("/nix"));VfsDrainMountCleanup();assert(!g_ext2Mounts[0]);
    assert(VfsMount("/nix","ext2",0));file=VfsOpen("/nix/packages/persist");assert(file);
    offset=0;assert(VfsRead(file,actual.data(),actual.size(),&offset)==int(actual.size())&&actual==expected);
    VfsRelease(file);assert(!VfsSync(nullptr));assert(VfsUnmount("/nix"));VfsDrainMountCleanup();assert(!g_ext2Mounts[0]);
    imageRegistered=nullptr;assert(!FatImageDeviceDestroyUnregistered(device));
    source=VfsOpen("/STORE.IMG",VFS_O_STREAM);assert(source);VfsRelease(source);
    assert(!VfsSync(nullptr));
    std::ofstream output(argv[1],std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<char*>(disk.data()),disk.size());assert(output);output.close();
    std::puts("FAT_IMAGE_EXT2_PASS retained-source mount create indirect-blocks partial-write fsync lower-flush-failure remount persistence");
}
