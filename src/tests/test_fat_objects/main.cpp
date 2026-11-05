#include <cassert>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <cstdlib>
#include "retained_path_walk.h"
#include "../../kernel/src/fatfs_vfs.cpp"
#include "fatfs_object.h"
#include "fat_image_device.h"
#include "guard_fat_profile.h"
#include "diskio.h"
#include "../../shared/update/recovery_guard_transaction.h"
#include <unistd.h>
#include <sys/wait.h>
static std::vector<unsigned char> disk;
static bool guardDrive=false;
static uint64_t failReadSector=~uint64_t{0};
static bool failRead=false, failWrite=false, failSync=false, failDriveFlush=false, intrinsicReadOnly=false, driveAcquired=false;
static brook::Device* exclusiveBound=nullptr;
static brook::Device* reservedRecoveryDevice=nullptr;
static unsigned diskReads=0,diskWrites=0;
static int driveFlushes=0,failDriveFlushAt=-1;
static LBA_t lastReadSector=0;
extern "C" DSTATUS disk_initialize(BYTE drive){return drive!=(guardDrive?1:0)?STA_NOINIT:0;}
extern "C" DSTATUS disk_status(BYTE drive){return drive!=(guardDrive?1:0)?STA_NOINIT:0;}
extern "C" DRESULT disk_read(BYTE drive,BYTE* out,LBA_t sector,UINT count){
    ++diskReads;
    lastReadSector=sector;
    uint64_t offset=uint64_t(sector)*512,bytes=uint64_t(count)*512;
    if(drive!=(guardDrive?1:0) || failRead || (sector<=failReadSector && failReadSector-sector<count) ||
       offset>disk.size() || bytes>disk.size()-offset)return RES_ERROR;
    std::memcpy(out,disk.data()+offset,bytes);return RES_OK;
}
extern "C" DRESULT disk_write(BYTE drive,const BYTE* in,LBA_t sector,UINT count){
    ++diskWrites;
    uint64_t offset=uint64_t(sector)*512,bytes=uint64_t(count)*512;
    if(drive!=(guardDrive?1:0) || failWrite || offset>disk.size() || bytes>disk.size()-offset)return RES_ERROR;
    std::memcpy(disk.data()+offset,in,bytes);return RES_OK;
}
extern "C" DRESULT disk_ioctl(BYTE drive,BYTE command,void* out){
    if(drive!=(guardDrive?1:0))return RES_PARERR;
    if(command==CTRL_SYNC)return failSync?RES_ERROR:RES_OK;
    if(command==GET_SECTOR_COUNT){*static_cast<LBA_t*>(out)=disk.size()/512;return RES_OK;}
    if(command==GET_SECTOR_SIZE){*static_cast<WORD*>(out)=512;return RES_OK;}
    if(command==GET_BLOCK_SIZE){*static_cast<DWORD*>(out)=1;return RES_OK;}
    return RES_PARERR;
}
extern "C" DWORD get_fattime(){return (DWORD(46)<<25)|(DWORD(10)<<21)|(DWORD(22)<<16);}

static void closeNode(brook::Vnode* node);
namespace {
std::mutex fatMutex,allocationMutex;
std::unordered_set<void*> allocations,pages;
thread_local long failAllocation=-1;
}
namespace brook {
Device* registeredImage=nullptr;
bool AhciSystemGuardIsReservedDevice(Device* device){return device && device==reservedRecoveryDevice;}
extern "C" bool DeviceIsRegistered(Device* device){return device==registeredImage;}
extern "C" void* kmalloc(uint64_t bytes){
    if(failAllocation==0){failAllocation=-1;return nullptr;}if(failAllocation>0)--failAllocation;
    void* p=std::malloc(bytes);if(p){std::memset(p,0xcc,bytes);std::lock_guard<std::mutex> lock(allocationMutex);allocations.insert(p);}return p;
}
extern "C" void kfree(void* p){if(p){std::lock_guard<std::mutex> lock(allocationMutex);assert(allocations.erase(p)==1);std::free(p);}}
void KMutexInit(KMutex* mutex){*mutex={};}
void KMutexLock(KMutex*){fatMutex.lock();}
void KMutexUnlock(KMutex*){fatMutex.unlock();}
VirtualAddress VmmAllocPages(uint64_t count,uint64_t,MemTag,uint16_t){
    void* p=nullptr;if(posix_memalign(&p,4096,count*4096))return VirtualAddress{};
    {std::lock_guard<std::mutex> lock(allocationMutex);pages.insert(p);}return VirtualAddress(reinterpret_cast<uint64_t>(p));
}
void VmmFreePages(VirtualAddress address,uint64_t){
    void* p=reinterpret_cast<void*>(address.raw());{std::lock_guard<std::mutex> lock(allocationMutex);assert(pages.erase(p)==1);}std::free(p);
}
int FatFsFlushDrive(uint8_t){++driveFlushes;return failSync||failDriveFlush||driveFlushes==failDriveFlushAt?-5:0;}
void VfsRelease(Vnode* node){if(__atomic_fetch_sub(&node->refCount,1,__ATOMIC_ACQ_REL)==1)::closeNode(node);}
bool FatFsAcquireDrive(uint8_t drive,bool* ro){if(drive || driveAcquired || exclusiveBound)return false;driveAcquired=true;*ro=intrinsicReadOnly;return true;}
bool FatFsBindExclusiveDrive(uint8_t drive,Device* device){if(drive!=(guardDrive?1:0) || !device || driveAcquired || exclusiveBound)return false;exclusiveBound=device;return true;}
bool FatFsAcquireExclusiveDrive(uint8_t drive,Device* expected,bool* ro){if(drive!=(guardDrive?1:0) || !ro || driveAcquired || !exclusiveBound || expected!=exclusiveBound)return false;driveAcquired=true;*ro=intrinsicReadOnly;return true;}
void FatFsReleaseDrive(uint8_t){assert(driveAcquired);if(!exclusiveBound)driveAcquired=false;}
bool FatFsDriveReadOnly(uint8_t){return intrinsicReadOnly;}
extern "C" void SerialPrintf(const char*,...){}
extern "C" void SerialPuts(const char*){}
}
static void closeNode(brook::Vnode* node){
    assert(node);auto* object=node->backendObject;
    node->ops->close(node);brook::kfree(node->directoryPath);brook::kfree(node);brook::VfsObjectRelease(object);
}
int main(int argc,char** argv){
    assert(argc==2);std::ifstream input(argv[1],std::ios::binary);assert(input);
    disk.assign(std::istreambuf_iterator<char>(input),{});input.close();
    using namespace brook;
    void* privateMount=nullptr;assert(FatFsMount(0,&privateMount));auto* mount=static_cast<FatFsMountData*>(privateMount);
    GuardFatProfile profile{};
    if(mount->fs.volbase==0) {
        assert(GuardFatParse(disk.data(),disk.size(),disk.size()/512,profile));
        assert(mount->fs.fs_type==(profile.type==16?FS_FAT16:FS_FAT32));
        assert(mount->fs.fatbase==profile.fatStart && mount->fs.fsize==profile.fatSectors);
        assert(mount->fs.database==profile.dataStart && mount->fs.csize==profile.sectorsPerCluster);
        assert(mount->fs.n_fatent==profile.clusters+2 && mount->fs.n_rootdir==profile.rootEntries && mount->fs.n_fats==2);
        assert(mount->fs.dirbase==(profile.type==32?profile.rootCluster:profile.fatStart+2*profile.fatSectors));
    } else {
        // A nested MBR volume may be mountable generally but is never a
        // valid reserved-ESP recovery target.
        assert(!GuardFatParse(disk.data(),disk.size(),disk.size()/512,profile));
    }
    auto create=[&](const char* path,const char* contents){
        Vnode* node=FatFsOpen(mount,0,path,VFS_O_CREATE|VFS_O_WRITE);assert(node);
        uint64_t offset=0;assert(node->ops->write(node,contents,std::strlen(contents),&offset)==int(std::strlen(contents)));closeNode(node);
    };
    assert(!FatFsMkdir(mount,0,"/dir",0755));assert(!FatFsMkdir(mount,0,"/parent",0755));
    create("/dir/meaningfully-long-name.txt","original");create("/dir/empty","");
    VfsObject *root=nullptr,*held=nullptr,*file=nullptr,*empty=nullptr,*after=nullptr;
    assert(!FatRootObject(mount,0,&root));Vnode* legacyRoot=FatFsOpen(mount,0,"/",VFS_O_READ);
    assert(legacyRoot && legacyRoot->backendObject==root);closeNode(legacyRoot);
    assert(!FatObjectChild(root,"dir",&held));
    assert(!FatObjectChild(held,"meaningfully-long-name.txt",&file));assert(!FatObjectChild(held,"empty",&empty));
    assert(file->identity!=empty->identity);
    assert(!FatFsRename(mount,0,"/dir","/parent/moved"));assert(!FatFsMkdir(mount,0,"/dir",0755));
    create("/dir/meaningfully-long-name.txt","replacement");
    char currentName[128];assert(!held->ops->directory_path(held,currentName,sizeof(currentName))&&!std::strcmp(currentName,"/parent/moved"));
    assert(!root->ops->directory_path(root,currentName,sizeof(currentName))&&!std::strcmp(currentName,"/"));
    assert(held->ops->directory_path(held,currentName,4)==-36);
    assert(file->ops->directory_path(file,currentName,sizeof(currentName))==-20);
    assert(!file->ops->object_path(file,currentName,sizeof(currentName)) &&
           !std::strcmp(currentName,"/parent/moved/meaningfully-long-name.txt"));
    assert(!empty->ops->object_path(empty,currentName,sizeof(currentName)) &&
           !std::strcmp(currentName,"/parent/moved/empty"));
    assert(file->ops->object_path(file,currentName,4)==-36);
    VfsObject* entryParent=nullptr;char leaf[256];
    assert(file->ops->entry_parent(file,&entryParent,leaf,4)==-36 && !entryParent);
    assert(!file->ops->entry_parent(file,&entryParent,leaf,sizeof(leaf)) && entryParent==held);
    assert(!strcmp(leaf,"meaningfully-long-name.txt"));VfsObjectRelease(entryParent);entryParent=nullptr;
    assert(!empty->ops->entry_parent(empty,&entryParent,leaf,sizeof(leaf)) && entryParent==held);
    assert(!strcmp(leaf,"empty"));VfsObjectRelease(entryParent);
    assert(!FatObjectChild(held,"meaningfully-long-name.txt",&after));assert(after==file);VfsObjectRelease(after);
    assert(FatFsUnlink(mount,0,"/parent/moved/empty")==-16);
    assert(FatFsRename(mount,0,"/parent/moved/empty","/new-empty")==-16);
    int error=0;Vnode* first=FatObjectOpen(file,VFS_O_READ,&error);Vnode* second=FatObjectOpen(file,VFS_O_WRITE|VFS_O_READWRITE,&error);
    for(long failure=0;failure<3;++failure){
        const auto before=allocations.size();failAllocation=failure;
        assert(!FatObjectCreate(held,"new retained.txt",VFS_O_WRITE,0600,&error) && error==-12);failAllocation=-1;
        assert(allocations.size()==before);assert(FatObjectChild(held,"new retained.txt",&after)==-2 && !after);
    }
    Vnode* made=FatObjectCreate(held,"new retained.txt",VFS_O_WRITE|VFS_O_READWRITE,0600,&error);
    assert(made && made->backendObject && !error);uint64_t atMade=0;
    assert(made->ops->write(made,"ok",2,&atMade)==2);assert(!made->ops->fsync(made));closeNode(made);
    assert(!FatObjectCreate(held,"NEW RETAINED.TXT",VFS_O_WRITE,0600,&error) && error==-17);
    assert(!FatObjectChild(held,"new retained.txt",&after));VnodeStat madeStat{};
    assert(!FatObjectStat(after,&madeStat) && madeStat.size==2);VfsObjectRelease(after);after=nullptr;
    // Model the VFS mutation's owned target reference, then an extra pin.
    assert(!FatObjectChild(held,"new retained.txt",&after));
    VfsObjectRetain(after);
    assert(FatObjectRemove(held,"new retained.txt",false)==-16);
    VfsObjectRelease(after);
    assert(FatObjectRemove(held,"new retained.txt",true)==-20);
    assert(!FatObjectRemove(held,"new retained.txt",false));
    VfsObjectRelease(after);after=nullptr;
    assert(FatObjectChild(held,"new retained.txt",&after)==-2);
    assert(!FatObjectMkdir(held,"remove-dir",0755));
    assert(!FatObjectChild(held,"remove-dir",&after));
    Vnode* nested=FatObjectCreate(after,"child",VFS_O_WRITE,0600,&error);
    assert(nested && !error);closeNode(nested);VfsObjectRelease(after);after=nullptr;
    assert(!FatObjectChild(held,"remove-dir",&after));
    assert(FatObjectRemove(held,"remove-dir",true)==-39);
    VfsObject* nestedObject=nullptr;assert(!FatObjectChild(after,"child",&nestedObject));
    assert(!FatObjectRemove(after,"child",false));VfsObjectRelease(nestedObject);
    VfsObjectRelease(after);after=nullptr;
    assert(!FatObjectChild(held,"remove-dir",&after));
    assert(!FatObjectRemove(held,"remove-dir",true));VfsObjectRelease(after);after=nullptr;
    Vnode* readCreator=FatObjectCreate(held,"readonly-create",VFS_O_READ,0600,&error);
    assert(readCreator && !error);VfsObject* createdObject=readCreator->backendObject;
    Vnode* writer=FatObjectOpen(createdObject,VFS_O_WRITE|VFS_O_READWRITE,&error);assert(writer && !error);
    uint64_t writerAt=0;assert(writer->ops->write(writer,"safe",4,&writerAt)==4);assert(!writer->ops->fsync(writer));
    closeNode(readCreator);closeNode(writer);
    assert(!FatObjectChild(held,"readonly-create",&after));Vnode* checkCreated=FatObjectOpen(after,VFS_O_READ,&error);assert(checkCreated);
    char createdBytes[4];uint64_t checkAt=0;assert(checkCreated->ops->read(checkCreated,createdBytes,4,&checkAt)==4 && !memcmp(createdBytes,"safe",4));
    closeNode(checkCreated);VfsObjectRelease(after);after=nullptr;assert(!FatFsUnlink(mount,0,"/parent/moved/readonly-create"));
    Vnode* failedCreator=FatObjectCreate(held,"readonly-failure",VFS_O_READ,0600,&error);assert(failedCreator);
    failSync=true;closeNode(failedCreator);assert(mount->deferredCloseError!=0);
    failSync=false;assert(FatFsSync(mount)!=0);assert(!FatFsSync(mount));
    assert(!FatFsUnlink(mount,0,"/parent/moved/readonly-failure"));
    Vnode* racers[2]{};int raceErrors[2]{};
    std::thread left([&]{racers[0]=FatObjectCreate(held,"race-create",VFS_O_WRITE,0600,&raceErrors[0]);});
    std::thread right([&]{racers[1]=FatObjectCreate(held,"RACE-CREATE",VFS_O_WRITE,0600,&raceErrors[1]);});
    left.join();right.join();assert(bool(racers[0])!=bool(racers[1]));
    assert(racers[0]?raceErrors[1]==-17:raceErrors[0]==-17);
    closeNode(racers[0]?racers[0]:racers[1]);assert(!FatFsUnlink(mount,0,"/parent/moved/race-create"));
    assert(first && second && first->priv!=second->priv);
    char bytes[32]{};uint64_t offset=0;assert(first->ops->read(first,bytes,8,&offset)==8 && !std::memcmp(bytes,"original",8));
    uint64_t append=8;assert(second->ops->write(second,"-tail",5,&append)==5);assert(!second->ops->fsync(second));
    VnodeStat stat{};assert(!FatObjectStat(file,&stat) && stat.size==13);
    closeNode(second);closeNode(first);
    // File coordinates remain pinned even after all FILs have closed.
    assert(FatFsUnlink(mount,0,"/parent/moved/meaningfully-long-name.txt")==-16);
    Vnode* converted=FatObjectOpen(held,VFS_O_READ,&error);assert(converted);
    assert(!FatObjectFromVnode(converted,&after));closeNode(converted);
    // Explicit directory handles below exercise legacy pins and independent cursors.
    Vnode* a=FatFsOpen(mount,0,"/parent/moved",VFS_O_READ);
    Vnode* b=FatObjectOpen(held,VFS_O_READ,&error);assert(a && b && a->backendObject==held && b->backendObject==held);
    DirEntry entryA{},entryB{};uint32_t ca=0,cb=0;
    assert(a->ops->readdir(a,&entryA,&ca)==1 && b->ops->readdir(b,&entryB,&cb)==1 && !std::strcmp(entryA.name,entryB.name));
    closeNode(a);closeNode(b);
    VfsObjectRelease(after);
    // Object-open failure after each allocation leaves original pins usable.
    for(auto* object : {file,held})for(long failure=0;failure<2;++failure){
        const auto before=allocations.size();failAllocation=failure;
        assert(!FatObjectOpen(object,VFS_O_READ,&error) && error==-12);failAllocation=-1;
        assert(allocations.size()==before);assert(!FatObjectStat(object,&stat));
    }
    // Legacy directory open must unwind a newly acquired identity pin even if
    // vnode allocation fails. Each case starts without a pin for that object.
    assert(!FatFsMkdir(mount,0,"/legacy-empty",0755));
    for(long failure=0;failure<4;++failure){
        const auto before=allocations.size();failAllocation=failure;
        assert(!FatFsOpen(mount,0,"/legacy-empty",VFS_O_READ));failAllocation=-1;
        assert(allocations.size()==before);
    }
    assert(!FatFsUnlink(mount,0,"/legacy-empty"));
    VfsObjectRelease(empty);assert(!FatFsUnlink(mount,0,"/parent/moved/empty"));
    VfsObjectRelease(file);assert(!FatFsUnlink(mount,0,"/parent/moved/meaningfully-long-name.txt"));
    assert(FatFsUnlink(mount,0,"/parent/moved")==-16); // Empty but retained.
    assert(!FatFsMkdir(mount,0,"/target",0755));
    assert(FatFsRename(mount,0,"/target","/parent/moved")==-16);
    assert(!FatFsUnlink(mount,0,"/target"));
    VfsObjectRelease(held);assert(!FatFsUnlink(mount,0,"/parent/moved"));
    // A zero-count object waiting for the registry mutex must not be revived.
    assert(!FatFsMkdir(mount,0,"/retiring",0755));VfsObject* retiring=nullptr;
    assert(!FatObjectChild(root,"retiring",&retiring));
    FF_OBJECT location=static_cast<FatPathObject*>(retiring->priv)->location;
    KMutexLock(&g_fatLock);
    std::thread release([&]{VfsObjectRelease(retiring);});
    while(__atomic_load_n(&retiring->references,__ATOMIC_ACQUIRE))std::this_thread::yield();
    VfsObject* fresh=nullptr;assert(!FatInternObjectLocked(mount,location,&fresh) && fresh!=retiring && fresh->identity==retiring->identity);
    KMutexUnlock(&g_fatLock);release.join();assert(!FatObjectStat(fresh,&stat));VfsObjectRelease(fresh);
    assert(!FatFsUnlink(mount,0,"/retiring"));
    assert(FatFsUnmount(mount)==-16);VfsObjectRelease(root);assert(!FatFsUnmount(mount));
    assert(allocations.empty() && pages.empty() && !driveAcquired);
    // A large backing file must open without preloading or consulting an old
    // whole-file cache. Exercise the actual production FAT adapter and FIL.
    assert(FatFsMount(0,&privateMount));mount=static_cast<FatFsMountData*>(privateMount);
    auto* large=FatFsOpen(mount,0,"/image.brk",VFS_O_WRITE|VFS_O_CREATE);assert(large);
    std::vector<char> block(4096,'x');uint64_t at=0;
    for(unsigned i=0;i<256;++i)assert(large->ops->write(large,block.data(),block.size(),&at)==4096);
    closeNode(large);assert(!FatFsUnmount(mount));
    intrinsicReadOnly=true;
    assert(FatFsMount(0,&privateMount));mount=static_cast<FatFsMountData*>(privateMount);
    void* duplicate=nullptr;assert(!FatFsMount(0,&duplicate));
    const auto diskBefore=disk;const auto readsBefore=diskReads;
    auto* stream=FatFsOpen(mount,0,"/image.brk",VFS_O_STREAM);assert(stream && FatFsFileSourceReadOnly(stream));
    assert(pages.empty() && diskReads-readsBefore<32); // no 1 MiB preload
    char probe[7];at=900000;assert(stream->ops->read(stream,probe,sizeof(probe),&at)==7);
    for(char c:probe)assert(c=='x');
    at=0;assert(stream->ops->write(stream,"bad",3,&at)==-30);
    assert(stream->ops->truncate(stream,0)==-30);
    assert(!FatFsOpen(mount,0,"/image.brk",VFS_O_WRITE));
    assert(!FatFsOpen(mount,0,"/image.brk",VFS_O_STREAM|VFS_O_TRUNC));
    assert(FatFsUnlink(mount,0,"/image.brk")==-30);
    assert(FatFsMkdir(mount,0,"/bad",0755)==-30);
    assert(FatFsRename(mount,0,"/image.brk","/bad")==-30);
    assert(!FatRootObject(mount,0,&root));assert(!FatObjectChild(root,"image.brk",&file));
    assert(FatObjectAccess(file,2,false)==-30);
    assert(!FatObjectOpen(file,VFS_O_WRITE,&error) && error==-30);
    assert(!FatObjectCreate(root,"forbidden",VFS_O_WRITE,0600,&error) && error==-30);
    assert(FatObjectMkdir(root,"forbidden-dir",0755)==-30);
    auto* other=FatObjectOpen(file,VFS_O_STREAM,&error);assert(other && FatFsFileSourceReadOnly(other));closeNode(other);
    VfsObjectRelease(file);VfsObjectRelease(root);
    assert(FatFsUnmount(mount)==-16);closeNode(stream);
    assert(!FatFsUnmount(mount) && !driveAcquired);
    assert(disk==diskBefore && allocations.empty() && pages.empty());
    intrinsicReadOnly=false;
    // An open, otherwise idle writer must reach disk without fsync or close.
    assert(FatFsMount(0,&privateMount));mount=static_cast<FatFsMountData*>(privateMount);
    assert(!FatFsBackgroundSync());const auto idleFlushes=driveFlushes;
    assert(!FatFsBackgroundSync() && driveFlushes==idleFlushes);
    auto* idleWriter=FatFsOpen(mount,0,"/IDLE.BIN",VFS_O_CREATE|VFS_O_WRITE);assert(idleWriter);
    uint64_t idleAt=0;assert(idleWriter->ops->write(idleWriter,"persist",7,&idleAt)==7);
    failDriveFlush=true;assert(FatFsBackgroundSync()==-5 && mount->backgroundDirty);
    failDriveFlush=false;assert(!FatFsBackgroundSync() && !mount->backgroundDirty);
    const auto flushed=driveFlushes;assert(!FatFsBackgroundSync() && driveFlushes==flushed);
    // Fresh FatFs directory lookup observes the updated size while the writer
    // remains open, rather than relying on its in-memory FIL.
    FILINFO idleInfo{};assert(f_stat("0:/IDLE.BIN",&idleInfo)==FR_OK && idleInfo.fsize==7);
    closeNode(idleWriter);
    VfsObject* idleRoot=nullptr;assert(!FatRootObject(mount,0,&idleRoot));
    int idleError=0;auto* emptyCreated=FatObjectCreate(idleRoot,"idle-empty",VFS_O_READ,0600,&idleError);
    assert(emptyCreated && !idleError && mount->backgroundDirty);
    assert(!FatFsBackgroundSync() && !mount->backgroundDirty);
    closeNode(emptyCreated);VfsObjectRelease(idleRoot);assert(!FatFsUnmount(mount));
    assert(!FatFsBackgroundSync() && allocations.empty() && pages.empty());
    // Ordinary readers can still populate a cache, but streaming must bypass it.
    assert(FatFsMount(0,&privateMount));mount=static_cast<FatFsMountData*>(privateMount);
    auto* cached=FatFsOpen(mount,0,"/image.brk",VFS_O_READ);assert(cached && cached->ops==&g_cachedFileOps);
    stream=FatFsOpen(mount,0,"/image.brk",VFS_O_STREAM);assert(stream && stream->ops==&g_fatFileOps && !FatFsFileSourceReadOnly(stream));
    closeNode(stream);closeNode(cached);assert(!FatFsUnmount(mount));
    assert(allocations.empty() && pages.empty() && !driveAcquired);
    // A writable fixed-size image must have one exclusive, uncached owner.
    assert(FatFsMount(0,&privateMount));mount=static_cast<FatFsMountData*>(privateMount);
    {
        Vnode* guard=FatFsOpen(mount,0,"/recovery-guard.bin",VFS_O_CREATE|VFS_O_WRITE|VFS_O_READWRITE);assert(guard);
        uint8_t record[128]{};record[0]=17;uint64_t position=0;
        assert(guard->ops->write(guard,record,sizeof(record),&position)==128);
        FatImageLease* fixed=nullptr;
        assert(FatFsAcquireImageLease(guard,128,&fixed)==-22 && !fixed);
        Vnode* alias=FatFsOpen(mount,0,"/recovery-guard.bin",VFS_O_STREAM);assert(alias);
        assert(FatFsAcquireGuardLease(guard,&fixed)==-16 && !fixed);closeNode(alias);
        assert(!FatFsAcquireGuardLease(guard,&fixed) && fixed && guard->ops->deny_mapping);
        Vnode* guard2=FatFsOpen(mount,0,"/recovery-guard-two.bin",VFS_O_CREATE|VFS_O_WRITE|VFS_O_READWRITE);assert(guard2);
        position=0;assert(guard2->ops->write(guard2,record,sizeof(record),&position)==128);closeNode(guard2);
        const pid_t crossChild=fork();assert(crossChild>=0);
        if(!crossChild) {
            // Corrupt the second on-disk directory entry to point at the
            // first file's EOC cluster; the names and identities stay distinct.
            Vnode* other=FatFsOpen(mount,0,"/recovery-guard-two.bin",VFS_O_WRITE|VFS_O_READWRITE);assert(other);
            auto* otherFile=static_cast<FatOpenFile*>(other->priv);
            const auto sector=otherFile->fil.dir_sect;
            const auto offset=otherFile->fil.dir_ptr-mount->fs.win;
            const auto cluster=static_cast<FatOpenFile*>(guard->priv)->fil.obj.sclust;
            assert(offset>=0 && offset+32<=512 && uint64_t(sector)*512+offset+32<=disk.size());
            closeNode(other);
            auto* entry=disk.data()+uint64_t(sector)*512+offset;
            entry[20]=uint8_t(cluster>>16);entry[21]=uint8_t(cluster>>24);
            entry[26]=uint8_t(cluster);entry[27]=uint8_t(cluster>>8);
            mount->fs.winsect=~LBA_t{0};
            other=FatFsOpen(mount,0,"/recovery-guard-two.bin",VFS_O_WRITE|VFS_O_READWRITE);assert(other);
            FatImageLease* overlap=nullptr;assert(!FatFsAcquireGuardLease(other,&overlap));
            assert(FatFsGuardPairValidate(fixed,overlap)==-22);_exit(0);
        }
        int crossStatus=0;assert(waitpid(crossChild,&crossStatus,0)==crossChild && WIFEXITED(crossStatus) && !WEXITSTATUS(crossStatus));
        guard2=FatFsOpen(mount,0,"/recovery-guard-two.bin",VFS_O_WRITE|VFS_O_READWRITE);assert(guard2);
        FatImageLease* fixed2=nullptr;assert(!FatFsAcquireGuardLease(guard2,&fixed2));
        assert(!FatFsGuardPairValidate(fixed,fixed2) && !FatFsGuardPairValidate(fixed2,fixed));
        assert(FatFsGuardPairValidate(fixed,fixed)==-22);
        record[4]=66;assert(FatFsImageWrite(fixed,0,record,sizeof(record))==128);
        assert(!FatFsGuardPairValidate(fixed,fixed2));
        uint8_t pairRead[128]{};assert(!FatFsGuardFlushReadback(fixed,pairRead));
        const pid_t pairChild=fork();assert(pairChild>=0);
        if(!pairChild) {
            fixed2->linkMap[1]=2;assert(FatFsGuardPairValidate(fixed,fixed2)==-22);
            fixed2->linkMap[1]=1;
            assert(FatFsGuardPairValidate(fixed,fixed2)==-22 && FatFsReleaseImageLease(fixed2)==-22);_exit(0);
        }
        int pairStatus=0;assert(waitpid(pairChild,&pairStatus,0)==pairChild && WIFEXITED(pairStatus) && !WEXITSTATUS(pairStatus));
        assert(!FatFsReleaseImageLease(fixed2));closeNode(guard2);
        assert(!FatFsOpen(mount,0,"/recovery-guard.bin",VFS_O_STREAM));
        FILINFO info{};assert(f_stat("0:/recovery-guard.bin",&info)==FR_OK && info.altname[0]);
        std::string aliasName="/"+std::string(info.altname);
        assert(!FatFsOpen(mount,0,aliasName.c_str(),VFS_O_STREAM));
        assert(FatFsRename(mount,0,"/recovery-guard.bin","/changed.bin")==-16);
        assert(FatFsUnlink(mount,0,"/recovery-guard.bin")==-16);
        uint8_t got[128]{};assert(FatFsImageRead(fixed,0,got,sizeof(got))==128 && got[0]==17);
        record[1]=33;assert(FatFsImageWrite(fixed,0,record,sizeof(record))==128);
        assert(FatFsImageWrite(fixed,127,record,2)==-22);
        const unsigned reads=diskReads;
        assert(!FatFsGuardFlushReadback(fixed,got) && !std::memcmp(got,record,128));
        assert(diskReads>reads);const LBA_t guardSector=lastReadSector;
        assert(uint64_t(guardSector)*512+512<=disk.size());
        uint8_t tail[384];std::memcpy(tail,disk.data()+uint64_t(guardSector)*512+128,sizeof(tail));
        record[3]=55;assert(FatFsImageWrite(fixed,0,record,sizeof(record))==128);
        assert(!FatFsGuardFlushReadback(fixed,got) && !std::memcmp(got,record,128));
        assert(!std::memcmp(tail,disk.data()+uint64_t(guardSector)*512+128,sizeof(tail)));
        // A retained FIL buffer may be stale. The readback must see the
        // changed underlying sector, not return its cached 128 bytes.
        disk[uint64_t(guardSector)*512+10]=99;
        assert(FatFsImageRead(fixed,0,got,sizeof(got))==128 && got[10]==0);
        assert(!FatFsGuardFlushReadback(fixed,got) && got[10]==99);
        disk[uint64_t(guardSector)*512+10]=0;
        assert(FatFsGuardFlushReadback(fixed,nullptr)==-22);
        const pid_t child=fork();assert(child>=0);
        if(!child) {
            record[2]=44;assert(FatFsImageWrite(fixed,0,record,sizeof(record))==128);
            failSync=true;assert(FatFsGuardFlushReadback(fixed,got)==-5);failSync=false;
            assert(FatFsImageFlush(fixed)==-5 && FatFsReleaseImageLease(fixed)==-5);
            assert(!FatFsOpen(mount,0,"/recovery-guard.bin",VFS_O_STREAM));_exit(0);
        }
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
        const pid_t flushChild=fork();assert(flushChild>=0);
        if(!flushChild) {
            failDriveFlush=true;assert(FatFsGuardFlushReadback(fixed,got)==-5);failDriveFlush=false;
            assert(FatFsGuardFlushReadback(fixed,got)==-5 && FatFsReleaseImageLease(fixed)==-5);_exit(0);
        }
        status=0;assert(waitpid(flushChild,&status,0)==flushChild && WIFEXITED(status) && !WEXITSTATUS(status));
        const pid_t readChild=fork();assert(readChild>=0);
        if(!readChild) {
            failRead=true;assert(FatFsGuardFlushReadback(fixed,got)==-5);failRead=false;
            assert(FatFsGuardFlushReadback(fixed,got)==-5 && FatFsReleaseImageLease(fixed)==-5);
            assert(!FatFsOpen(mount,0,"/recovery-guard.bin",VFS_O_STREAM));_exit(0);
        }
        status=0;assert(waitpid(readChild,&status,0)==readChild && WIFEXITED(status) && !WEXITSTATUS(status));
        const pid_t mapChild=fork();assert(mapChild>=0);
        if(!mapChild) {
            fixed->linkMap[2]^=1;assert(FatFsGuardFlushReadback(fixed,got)==-22);
            fixed->linkMap[2]^=1;
            assert(FatFsGuardFlushReadback(fixed,got)==-22 && FatFsReleaseImageLease(fixed)==-22);_exit(0);
        }
        status=0;assert(waitpid(mapChild,&status,0)==mapChild && WIFEXITED(status) && !WEXITSTATUS(status));
        assert(!FatFsReleaseImageLease(fixed));closeNode(guard);
        guard=FatFsOpen(mount,0,"/recovery-guard.bin",VFS_O_WRITE|VFS_O_READWRITE);assert(guard);
        assert(FatFsAcquireGuardLease(guard,&fixed)==0);
        assert(FatFsImageRead(fixed,0,got,sizeof(got))==128 && !std::memcmp(got,record,128));
        assert(!FatFsReleaseImageLease(fixed));closeNode(guard);
        for(unsigned size:{127u,129u,512u}) {
            guard=FatFsOpen(mount,0,"/recovery-guard.bin",VFS_O_WRITE|VFS_O_READWRITE|VFS_O_TRUNC);assert(guard);
            uint8_t bytes[512]{};position=0;assert(guard->ops->write(guard,bytes,size,&position)==int(size));
            assert(FatFsAcquireGuardLease(guard,&fixed)==-22 && !fixed);closeNode(guard);
        }
    }
    {
        // Exercise the actual transaction primitive through retained FAT
        // records. The isolated host fixture supplies exclusive ownership;
        // this does not provide a production ESP exclusion mechanism.
        const char* names[2]={"/guard-transaction-0.bin","/guard-transaction-1.bin"};
        const char* identity="01000000000000000000000000000000"
                             "02000000000000000000000000000000"
                             "03000000000000000000000000000000";
        uint8_t record[128]{};std::memcpy(record,"BRKRCV01",8);BrookUpdatePut64(record+8,1);
        BrookUpdatePut32(record+16,BROOK_RECOVERY_GUARD_CLEAN);
        record[24]=1;record[40]=2;record[56]=3;BrookUpdatePut32(record+124,BrookUpdateCrc(record,124));
        Vnode* nodes[2]{};FatImageLease* leases[2]{};
        for(unsigned i=0;i<2;++i) {
            nodes[i]=FatFsOpen(mount,0,names[i],VFS_O_CREATE|VFS_O_WRITE|VFS_O_READWRITE);assert(nodes[i]);
            uint64_t pos=0;assert(nodes[i]->ops->write(nodes[i],record,128,&pos)==128);
            assert(!FatFsAcquireGuardLease(nodes[i],&leases[i]));
        }
        assert(!FatFsGuardPairValidate(leases[0],leases[1]));
        struct GuardIo {
            FatImageLease** leases;FatFsMountData* mount;
            int failAfterWrite=-1,failWriteOnSync=-1,failReadAt=-1,reads=0;bool failFinal=false;
            bool Read(unsigned i,uint8_t* out,unsigned size) {
                if(++reads==failReadAt)failRead=true;
                const bool ok=i<2 && size==128 && !FatFsGuardFlushReadback(leases[i],out);
                failRead=false;return ok;
            }
            bool Write(unsigned i,const uint8_t* data,unsigned size) {
                return i<2 && size==128 && FatFsImageWrite(leases[i],0,data,size)==128;
            }
            bool Sync(unsigned i) {
                if(int(i)==failAfterWrite)failSync=true;
                if(int(i)==failWriteOnSync)failWrite=true;
                const bool ok=i<2 && !FatFsImageFlush(leases[i]);failSync=false;failWrite=false;return ok;
            }
            bool Flush() {if(failFinal)failDriveFlush=true;const bool ok=!FatFsSync(mount);failDriveFlush=false;return ok;}
        } io{leases,mount};
        for(unsigned mode=0;mode<7;++mode) {
            const pid_t failed=fork();assert(failed>=0);
            if(!failed) {
                const unsigned slot=mode/3;
                if(mode<6 && mode%3==0)io.failAfterWrite=int(slot);
                if(mode<6 && mode%3==1)io.failWriteOnSync=int(slot);
                if(mode<6 && mode%3==2)io.failReadAt=3+int(slot);
                if(mode==6)io.failFinal=true;
                assert(BrookGuardBeginRepair(io,identity)==BrookGuardBeginResult::Uncertain);
                if(mode<6)assert(FatFsImageFlush(leases[slot])==-5 && FatFsReleaseImageLease(leases[slot])==-5);
                if(mode==6)for(auto* lease:leases)
                    assert(FatFsImageFlush(lease)==-5 && FatFsReleaseImageLease(lease)==-5);
                _exit(0);
            }
            int failureStatus=0;assert(waitpid(failed,&failureStatus,0)==failed && WIFEXITED(failureStatus) && !WEXITSTATUS(failureStatus));
        }
        assert(BrookGuardBeginRepair(io,identity)==BrookGuardBeginResult::PendingDurable);
        uint8_t a[128]{},b[128]{};assert(io.Read(0,a,128) && io.Read(1,b,128));
        assert(BrookRecoveryGuardValid(a,128) && BrookRecoveryGuardValid(b,128));
        assert(BrookUpdateEqual(a,b,128) && BrookUpdateGet64(a+8)==2 &&
               BrookUpdateGet32(a+16)==BROOK_RECOVERY_GUARD_PENDING);
        assert(!BrookRecoveryGuardAllowsNormal(a,128,b,128,identity));
        for(unsigned i=0;i<2;++i) {assert(!FatFsReleaseImageLease(leases[i]));closeNode(nodes[i]);}
        for(unsigned i=0;i<2;++i) {
            Vnode* verify=FatFsOpen(mount,0,names[i],VFS_O_STREAM);assert(verify);
            uint8_t actual[128]{};uint64_t pos=0;
            assert(verify->ops->read(verify,actual,128,&pos)==128 && BrookUpdateEqual(actual,a,128));
            closeNode(verify);
        }
    }
    assert(!FatFsRename(mount,0,"/image.brk","/network-storage-image.img"));
    const char* image="/network-storage-image.img";
    auto* owner=FatFsOpen(mount,0,image,VFS_O_WRITE|VFS_O_READWRITE);assert(owner);
    FatImageLease* lease=nullptr;
    auto* alias=FatFsOpen(mount,0,image,VFS_O_STREAM);assert(alias);
    assert(FatFsAcquireImageLease(owner,1024*1024,&lease)==-16 && !lease && owner->refCount==1);
    closeNode(alias);
    VfsRetain(owner);assert(FatFsAcquireImageLease(owner,1024*1024,&lease)==-16 && !lease);VfsRelease(owner);
    assert(FatFsAcquireImageLease(owner,512,&lease)==-22 && !lease);
    for(int allocation=0;allocation<2;++allocation){
        const auto before=allocations.size();failAllocation=allocation;
        assert(FatFsAcquireImageLease(owner,1024*1024,&lease)==-12 && !lease);failAllocation=-1;
        assert(allocations.size()==before && owner->ops==&g_fatFileOps && owner->refCount==1);
    }
    failSync=true;assert(FatFsAcquireImageLease(owner,1024*1024,&lease)==-5 && !lease);failSync=false;
    assert(owner->ops==&g_fatFileOps && owner->refCount==1);
    assert(!FatFsAcquireImageLease(owner,1024*1024,&lease) && lease && owner->ops->deny_mapping);
    uint8_t guardProbe[128]{};assert(FatFsGuardFlushReadback(lease,guardProbe)==-22);
    assert(owner->refCount==2 && static_cast<FatOpenFile*>(owner->priv)->fil.cltbl && pages.empty());
    assert(!FatFsOpen(mount,0,image,VFS_O_READ));
    assert(!FatFsOpen(mount,0,image,VFS_O_METADATA));
    assert(!FatFsOpen(mount,0,image,VFS_O_STREAM));
    assert(!FatFsOpen(mount,0,image,VFS_O_WRITE|VFS_O_TRUNC));
    assert(!FatFsOpen(mount,0,"/NETWORK-STORAGE-IMAGE.IMG",VFS_O_WRITE));
    FILINFO imageInfo{};assert(f_stat("0:/network-storage-image.img",&imageInfo)==FR_OK);
    assert(imageInfo.altname[0]);std::string shortAlias="/"+std::string(imageInfo.altname);
    assert(!FatFsOpen(mount,0,shortAlias.c_str(),VFS_O_WRITE|VFS_O_TRUNC));
    assert(!FatRootObject(mount,0,&root));assert(!FatObjectChild(root,"network-storage-image.img",&file));
    assert(!FatObjectOpen(file,VFS_O_READ,&error) && error==-16);
    assert(!FatObjectOpen(file,VFS_O_WRITE|VFS_O_READWRITE,&error) && error==-16);
    assert(!FatObjectMkdir(root,"slot-dir-while-leased",0755));
    VfsObject* slotDir=nullptr;assert(!FatObjectChild(root,"slot-dir-while-leased",&slotDir));
    VfsObjectRelease(slotDir);
    at=0;assert(owner->ops->read(owner,probe,sizeof(probe),&at)==-16);
    assert(owner->ops->write(owner,"bad",3,&at)==-16 && at==0);
    assert(owner->ops->truncate(owner,0)==-16);
    assert(FatFsUnlink(mount,0,image)==-16);
    assert(FatFsRename(mount,0,image,"/renamed.img")==-16);
    create("/replace.img","preserve");assert(FatFsRename(mount,0,"/replace.img",image)==-16);
    std::vector<uint8_t> sector(512),readback(512);
    assert(FatFsImageRead(lease,1,readback.data(),512)==512);
    const uint8_t partial[]={1,2,3,4,5,6,7};
    assert(FatFsImageWrite(lease,509,partial,sizeof(partial))==int(sizeof(partial)));
    assert(FatFsImageRead(lease,507,readback.data(),11)==11);
    assert(readback[0]=='x' && readback[1]=='x' && readback[9]=='x' && readback[10]=='x');
    assert(!std::memcmp(readback.data()+2,partial,sizeof(partial)));
    assert(FatFsImageWrite(lease,1024*1024,sector.data(),512)==-22);
    assert(FatFsImageRead(lease,~uint64_t{0},readback.data(),512)==-22);
    assert(FatFsImageRead(lease,0,nullptr,512)==-22);
    assert(FatFsImageRead(lease,1024*1024,nullptr,0)==0);
    for(unsigned i=0;i<32;++i){
        for(unsigned j=0;j<512;++j)sector[j]=static_cast<uint8_t>(i*17+j);
        const uint64_t offset=uint64_t((i*61)%2048)*512;
        assert(FatFsImageWrite(lease,offset,sector.data(),512)==512);
    }
    assert(!FatFsImageFlush(lease));const auto beforeRandomReads=diskReads;
    for(unsigned i=0;i<32;++i){
        const uint64_t offset=uint64_t((i*61)%2048)*512;
        assert(FatFsImageRead(lease,offset,readback.data(),512)==512);
        for(unsigned j=0;j<512;++j)assert(readback[j]==static_cast<uint8_t>(i*17+j));
    }
    assert(diskReads-beforeRandomReads<=36); // retained extent map, not repeated FAT walks
    // Source I/O errors are sticky and never turn into a successful fsync or
    // release. Fork isolates deliberately unreleasable error-state fixtures.
    for(unsigned fault=0;fault<7;++fault){
        const pid_t child=fork();assert(child>=0);
        if(!child){
            if(fault==0){failRead=true;assert(FatFsImageRead(lease,512*500,readback.data(),512)==-5);failRead=false;}
            if(fault==1){failWrite=true;assert(FatFsImageWrite(lease,512*500,sector.data(),512)==-5);failWrite=false;}
            if(fault==2){failSync=true;assert(FatFsImageFlush(lease)==-5);failSync=false;}
            if(fault==3){failSync=true;assert(FatFsSync(mount)==-5);failSync=false;}
            if(fault==4){failSync=true;assert(owner->ops->fsync(owner)==-5);failSync=false;}
            if(fault==5){VfsObject* rootObj=nullptr;assert(!FatRootObject(mount,0,&rootObj));
                failSync=true;assert(FatObjectMkdir(rootObj,"mkdir-sync-failure",0755)==-5);failSync=false;
                VfsObjectRelease(rootObj);}
            if(fault==6){VfsObject* rootObj=nullptr;assert(!FatRootObject(mount,0,&rootObj));
                failWrite=true;assert(FatObjectMkdir(rootObj,"mkdir-write-failure",0755)==-5);failWrite=false;
                VfsObjectRelease(rootObj);}
            assert(FatFsImageFlush(lease)==-5 && FatFsReleaseImageLease(lease)==-5);
            assert(owner->ops->deny_mapping && static_cast<FatOpenFile*>(owner->priv)->imageLease==lease);
            assert(FatFsImageRead(lease,0,readback.data(),512)==-5);
            assert(FatFsImageWrite(lease,0,sector.data(),512)==-5);
            assert(FatFsSync(mount)==-5 && FatFsUnmount(mount)==-5);
            assert(driveAcquired && mount->allFiles);
            _exit(0);
        }
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    // Corrupted FAT chains and an over-capacity extent map must fail
    // admission without publishing a lease or hanging the global FAT mutex.
    for(unsigned malformed=0;malformed<8;++malformed){
        if(malformed==7 && mount->fs.fs_type!=FS_FAT32)continue;
        const pid_t child=fork();assert(child>=0);
        if(!child){
            alarm(2);
            assert(!FatFsReleaseImageLease(lease));
            auto* handle=static_cast<FatOpenFile*>(owner->priv);
            const DWORD first=handle->fil.obj.sclust;
            const uint64_t clusterBytes=uint64_t(mount->fs.csize)*512;
            auto putFat=[&](DWORD cluster,DWORD next){
                const unsigned width=mount->fs.fs_type==FS_FAT16?2:4;
                uint64_t pos=uint64_t(mount->fs.fatbase)*512+uint64_t(cluster)*width;
                assert(pos+width<=disk.size());
                for(unsigned b=0;b<width;++b)disk[pos+b]=static_cast<unsigned char>(next>>(8*b));
            };
            const DWORD eoc=mount->fs.fs_type==FS_FAT16?0xffff:0x0fffffff;
            int expected=-5;
            if(malformed==0)putFat(first,first);
            if(malformed==1){putFat(first,first+1);putFat(first+1,first);}
            if(malformed==2)putFat(first,eoc); // short chain
            if(malformed==3)handle->fil.obj.objsize=clusterBytes; // excess allocation tail
            if(malformed==4)putFat(first,mount->fs.fs_type==FS_FAT16?0xfff7:0x0ffffff7);
            if(malformed==5){
                // Even a tiny caller map must stop before walking the tail.
                putFat(first,first+2);putFat(first+2,first+4);
                struct Guard {DWORD before=0x12345678,map[4]{},after=0x87654321;} guard;
                mount->fs.winsect=static_cast<LBA_t>(-1);
                assert(f_object_image_map(&handle->fil,guard.map,4)==FR_NOT_ENOUGH_CORE);
                assert(guard.before==0x12345678 && guard.after==0x87654321 && !handle->fil.cltbl);
                _exit(0);
            }
            if(malformed==6){putFat(first,first+1);putFat(first+1,first+2);putFat(first+2,first);}
            if(malformed==7){
                assert(first+16384<mount->fs.n_fatent);
                for(unsigned i=0;i<8192;++i)putFat(first+2*i,i==8191?eoc:first+2*(i+1));
                handle->fil.obj.objsize=clusterBytes*8192;expected=-27;
            }
            mount->fs.winsect=static_cast<LBA_t>(-1);
            const auto before=allocations.size();FatImageLease* rejected=nullptr;
            assert(FatFsAcquireImageLease(owner,handle->fil.obj.objsize,&rejected)==expected && !rejected);
            assert(owner->ops==&g_fatFileOps && owner->refCount==1 && !handle->imageLease && !handle->fil.cltbl);
            assert(allocations.size()==before);
            _exit(0);
        }
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    assert(!FatFsReleaseImageLease(lease));assert(owner->refCount==1 && !owner->ops->deny_mapping);
    VfsObjectRelease(file);VfsObjectRelease(root);
    for(int allocation=0;allocation<3;++allocation){
        const auto before=allocations.size();failAllocation=allocation;
        assert(!FatImageDeviceCreate(owner,1024*1024,"usb-image",&error) && error==-12);failAllocation=-1;
        assert(allocations.size()==before && owner->ops==&g_fatFileOps && owner->refCount==1);
    }
    char deviceName[]="usb-image";
    auto* device=FatImageDeviceCreate(owner,1024*1024,deviceName,&error);assert(device && !error);
    deviceName[0]='X';assert(!std::strcmp(device->name,"usb-image"));
    assert(device->type==DeviceType::Block && owner->refCount==2);
    VfsRelease(owner); // Only the device now pins the source/mount.
    const auto* blockOps=reinterpret_cast<const BlockDeviceOps*>(device->ops);
    assert(blockOps->block_size(device)==512 && blockOps->block_count(device)==2048);
    assert(device->ops->ioctl(device,DEVICE_IOCTL_READ_ONLY,nullptr)==0);
    assert(device->ops->ioctl(device,12345,nullptr)==-95);
    std::vector<uint8_t> bulk(262144),got(bulk.size());
    for(unsigned i=0;i<bulk.size();++i)bulk[i]=static_cast<uint8_t>(i*29+7);
    assert(device->ops->write(device,65536,bulk.data(),bulk.size())==int(bulk.size()));
    assert(!device->ops->ioctl(device,DEVICE_IOCTL_FLUSH,nullptr));
    assert(device->ops->read(device,65536,got.data(),got.size())==int(got.size()) && got==bulk);
    const uint8_t four[]={0xaa,0xbb,0xcc,0xdd};
    assert(device->ops->write(device,65536+1007,four,sizeof(four))==4);
    std::memcpy(bulk.data()+1007,four,4);
    std::vector<uint8_t> metadata(128,0x67),crossing(70000,0x42);
    assert(device->ops->write(device,65536+508,metadata.data(),metadata.size())==128);
    std::memcpy(bulk.data()+508,metadata.data(),metadata.size());
    assert(device->ops->write(device,65536+65530,crossing.data(),crossing.size())==int(crossing.size()));
    std::memcpy(bulk.data()+65530,crossing.data(),crossing.size());
    std::thread partialA([&]{for(unsigned i=0;i<100;++i)assert(device->ops->write(device,65536+3000,four,4)==4);});
    std::thread partialB([&]{for(unsigned i=0;i<100;++i)assert(device->ops->write(device,65536+3004,four,4)==4);});
    partialA.join();partialB.join();std::memcpy(bulk.data()+3000,four,4);std::memcpy(bulk.data()+3004,four,4);
    assert(!device->ops->ioctl(device,DEVICE_IOCTL_FLUSH,nullptr));
    assert(device->ops->read(device,65536,got.data(),got.size())==int(got.size()) && got==bulk);
    assert(device->ops->read(device,1,got.data(),512)==512);
    assert(device->ops->write(device,1024*1024,bulk.data(),512)==-22);
    assert(!FatFsOpen(mount,0,image,VFS_O_STREAM));
    registeredImage=device;assert(FatImageDeviceDestroyUnregistered(device)==-16);registeredImage=nullptr;
    const pid_t failedDevice=fork();assert(failedDevice>=0);
    if(!failedDevice){
        failSync=true;assert(device->ops->ioctl(device,DEVICE_IOCTL_FLUSH,nullptr)==-5);failSync=false;
        assert(FatImageDeviceDestroyUnregistered(device)==-5 && mount->allFiles && driveAcquired);
        _exit(0);
    }
    int deviceStatus=0;assert(waitpid(failedDevice,&deviceStatus,0)==failedDevice && WIFEXITED(deviceStatus) && !WEXITSTATUS(deviceStatus));
    assert(!FatImageDeviceDestroyUnregistered(device) && !g_imageLeaseMounts[0]);
    alias=FatFsOpen(mount,0,image,VFS_O_STREAM);assert(alias);closeNode(alias);
    {
        constexpr uint32_t sectors=PanicJournalSectors;
        std::vector<uint8_t> content(sectors*512);
        for(size_t i=0;i<content.size();++i)content[i]=uint8_t(i*31+(i>>9));
        std::fill_n(content.begin(),512,0);
        std::fill_n(content.begin()+PanicJournalSlotSectors*512,512,0);
        Vnode* created=FatFsOpen(mount,0,"/PANIC.BIN",VFS_O_CREATE|VFS_O_WRITE);assert(created);
        uint64_t pos=0;unsigned paddingCount=0;
        for(size_t offset=0;offset<content.size();) {
            const size_t count=std::min(size_t(32768),content.size()-offset);
            assert(created->ops->write(created,content.data()+offset,count,&pos)==int(count));offset+=count;
            if(offset<content.size()) {
                char name[32];std::snprintf(name,sizeof(name),"/PANPAD%u.BIN",paddingCount++);
                const std::string padding(mount->fs.csize*512,'p');create(name,padding.c_str());
            }
        }
        assert(!created->ops->fsync(created));closeNode(created);
        Vnode* journal=FatFsOpen(mount,0,"/PANIC.BIN",VFS_O_WRITE|VFS_O_READWRITE);assert(journal);
        FatImageLease* retained=nullptr;
        assert(!FatFsAcquireImageLease(journal,content.size(),&retained));
        assert(retained->linkMap[0]>4); // interleaved files force fragmentation
        uint64_t mapping[sectors]{};
        assert(!FatFsJournalSectorMap(retained,mapping,sectors));
        for(uint32_t i=0;i<sectors;++i) {
            assert(mapping[i]<disk.size()/512);
            assert(!std::memcmp(disk.data()+mapping[i]*512,content.data()+i*512,512));
            for(uint32_t j=0;j<i;++j)assert(mapping[i]!=mapping[j]);
        }
        assert(!FatFsOpen(mount,0,"/PANIC.BIN",VFS_O_WRITE));
        // Exercise the actual journal through the mapped, leased FAT file.
        // Verify with an independent memory-disk view that no sector outside
        // that file changed and that its data can be recovered after writes.
        const auto beforeJournal=disk;
        struct MappedJournalIo {
            const uint64_t* sectors;
            bool read(uint32_t i,uint8_t* out) {
                return i<PanicJournalSectors && disk_read(0,out,sectors[i],1)==RES_OK;
            }
            bool write(uint32_t i,const uint8_t* in) {
                return i<PanicJournalSectors && disk_write(0,in,sectors[i],1)==RES_OK;
            }
            bool flush() {return disk_ioctl(0,CTRL_SYNC,nullptr)==RES_OK;}
        } mappedIo{mapping};
        const uint8_t packet[]={0x2d,0x02,0x00,0x01,0x0d,0xf0,0xfe,0xca,0x42};
        assert(PanicJournalWrite(mappedIo,packet,sizeof(packet))==PanicJournalResult::Ok);
        uint8_t recovered[sizeof(packet)]{};PanicJournalRecord result;
        assert(PanicJournalReadLatest(mappedIo,recovered,sizeof(recovered),result)==PanicJournalResult::Ok);
        assert(result.sequence==1 && !std::memcmp(packet,recovered,sizeof(packet)));
        std::unordered_set<uint64_t> owned(mapping,mapping+sectors);
        for(uint64_t i=0;i<disk.size()/512;++i)
            if(!owned.count(i))assert(!std::memcmp(disk.data()+i*512,beforeJournal.data()+i*512,512));
        // A damaged extent map must never publish a partial sector list.
        const pid_t malformed=fork();assert(malformed>=0);
        if(!malformed) {
            const DWORD first=retained->linkMap[2];
            const DWORD needed=(sectors+mount->fs.csize-1)/mount->fs.csize;
            assert(needed>1);
            retained->linkMap[0]=6;retained->linkMap[1]=1;retained->linkMap[2]=first;
            retained->linkMap[3]=needed-1;retained->linkMap[4]=first;retained->linkMap[5]=0;
            for(auto& entry:mapping)entry=~uint64_t(0);
            assert(FatFsJournalSectorMap(retained,mapping,sectors)==-5);
            for(auto entry:mapping)assert(entry==0);
            _exit(0);
        }
        int status=0;assert(waitpid(malformed,&status,0)==malformed && WIFEXITED(status) && !WEXITSTATUS(status));
        assert(!FatFsReleaseImageLease(retained));closeNode(journal);
        assert(!FatFsUnlink(mount,0,"/PANIC.BIN"));
        for(unsigned i=0;i<paddingCount;++i) {
            char name[32];std::snprintf(name,sizeof(name),"/PANPAD%u.BIN",i);
            assert(!FatFsUnlink(mount,0,name));
        }
    }
    assert(!FatFsMkdir(mount,0,"/RECOVERY",0755));
    for(unsigned i=0;i<3;++i) {
        const char* path=i==0?"/RECOVERY/GUARD0.BIN":i==1?"/RECOVERY/GUARD1.BIN":"/RECOVERY/THIRD.BIN";
        Vnode* file=FatFsOpen(mount,0,path,VFS_O_CREATE|VFS_O_WRITE);assert(file);
        uint8_t bytes[128]{};bytes[0]=uint8_t(50+i);uint64_t position=0;
        assert(file->ops->write(file,bytes,sizeof(bytes),&position)==128);closeNode(file);
    }
    assert(!FatFsUnmount(mount));assert(allocations.empty() && pages.empty() && !driveAcquired);
    if(profile.total)for(unsigned mode=0;mode<5;++mode) {
        const pid_t child=fork();assert(child>=0);
        if(!child) {
            if(mode==2) {
                void* temporary=nullptr;assert(FatFsMount(0,&temporary));
                assert(!FatFsUnlink(temporary,0,"/RECOVERY/GUARD1.BIN"));assert(!FatFsUnmount(temporary));
            }
            uint64_t secondSector=~uint64_t{0};
            if(mode==4) {
                void* temporary=nullptr;assert(FatFsMount(0,&temporary));
                auto* file=FatFsOpen(temporary,0,"/RECOVERY/GUARD1.BIN",VFS_O_STREAM);assert(file);
                const auto* fil=static_cast<const FIL*>(file->priv);assert(fil->obj.sclust>=2);
                secondSector=profile.dataStart+uint64_t(profile.sectorsPerCluster)*(fil->obj.sclust-2);
                closeNode(file);assert(!FatFsUnmount(temporary));
            }
            BlockDeviceOps block{};
            block.ioctl=[](Device*,uint32_t op,void*) {return op==DEVICE_IOCTL_READ_ONLY?1:-95;};
            block.block_size=[](Device*)->uint32_t{return 512;};
            block.block_count=[](Device*)->uint64_t{return disk.size()/512;};
            Device reserved{reinterpret_cast<const DeviceOps*>(&block),"reserved",DeviceType::Block,nullptr};
            registeredImage=reservedRecoveryDevice=&reserved;guardDrive=true;intrinsicReadOnly=true;
            GuardFatProfile candidate=profile;if(mode==1)++candidate.dataStart;
            if(mode==3)failRead=true;
            if(mode==4)failReadSector=secondSector;
            const auto writesBefore=diskWrites;uint8_t records[2][128];std::memset(records,0xaa,sizeof(records));
            const int result=FatFsGuardInspectReadOnly(1,&reserved,candidate,records);failRead=false;failReadSector=~uint64_t{0};
            assert((result==0)==(mode==0));assert(diskWrites==writesBefore);
            if(mode==0)assert(records[0][0]==50 && records[1][0]==51);
            else for(const auto& record:records)for(uint8_t byte:record)assert(!byte);
            std::memset(records,0xaa,sizeof(records));assert(FatFsGuardInspectReadOnly(1,&reserved,profile,records)==-22);
            for(const auto& record:records)for(uint8_t byte:record)assert(!byte);
            _exit(0);
        }
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    for(unsigned mode=0;mode<9;++mode) {
        const pid_t privateChild=fork();assert(privateChild>=0);
        if(!privateChild) {
            Device dummy{};FatPrivateMount* privateGuard=nullptr;
            Device reserved{};reservedRecoveryDevice=&reserved;
            assert(!FatFsMountPrivateRecovery(0,&reserved,&privateGuard) && !exclusiveBound && !driveAcquired);
            if(mode==2) {
                void* temporary=nullptr;assert(FatFsMount(0,&temporary));
                assert(!FatFsUnlink(temporary,0,"/RECOVERY/GUARD1.BIN"));assert(!FatFsUnmount(temporary));
            }
            if(mode==3 || mode==4) {
                void* temporary=nullptr;assert(FatFsMount(0,&temporary));
                auto* tempMount=static_cast<FatFsMountData*>(temporary);
                Vnode* first=FatFsOpen(temporary,0,"/RECOVERY/GUARD0.BIN",VFS_O_STREAM);assert(first);
                Vnode* other=FatFsOpen(temporary,0,mode==3?"/RECOVERY/GUARD1.BIN":"/RECOVERY/THIRD.BIN",VFS_O_STREAM);assert(other);
                const auto cluster=static_cast<FatOpenFile*>(first->priv)->fil.obj.sclust;
                const auto* otherFile=static_cast<FatOpenFile*>(other->priv);
                const auto sector=otherFile->fil.dir_sect;const auto offset=otherFile->fil.dir_ptr-tempMount->fs.win;
                assert(offset>=0 && offset+32<=512 && uint64_t(sector)*512+offset+32<=disk.size());
                closeNode(first);closeNode(other);assert(!FatFsUnmount(temporary));
                auto* entry=disk.data()+uint64_t(sector)*512+offset;
                entry[20]=uint8_t(cluster>>16);entry[21]=uint8_t(cluster>>24);
                entry[26]=uint8_t(cluster);entry[27]=uint8_t(cluster>>8);
            }
            if(mode==5) {
                void* temporary=nullptr;assert(FatFsMount(0,&temporary));
                Vnode* file=FatFsOpen(temporary,0,"/RECOVERY/GUARD1.BIN",VFS_O_WRITE|VFS_O_TRUNC);assert(file);
                uint8_t bytes[127]{};uint64_t position=0;
                assert(file->ops->write(file,bytes,sizeof(bytes),&position)==127);closeNode(file);assert(!FatFsUnmount(temporary));
            }
            const auto before=disk;const auto writesBefore=diskWrites;
            if(mode==1)failRead=true;
            const bool ok=FatFsMountPrivateRecovery(0,&dummy,&privateGuard);failRead=false;
            assert(ok!=(mode==1) && (privateGuard!=nullptr)==ok);
            assert(exclusiveBound==&dummy && driveAcquired);
            if(ok) {
                uint8_t a[128]{},b[128]{};
                assert(FatFsPrivateRecoveryInspect(privateGuard,a,a)==-22);
                if(mode>=6)failDriveFlushAt=driveFlushes+int(mode)-5;
                const int result=FatFsPrivateRecoveryInspect(privateGuard,a,b);
                failDriveFlushAt=-1;
                assert(result==(mode>=6?-5:mode==2?-2:mode==3 || mode==5?-22:0));
                if(mode==0 || mode==4)assert(a[0]==50 && b[0]==51);
                assert(FatFsPrivateRecoveryInspect(privateGuard,a,b)==-22);
                assert(privateGuard->nodes[0]);
                if(mode!=6)assert(privateGuard->leases[0] && !FatFsOpen(privateGuard->mount,0,"/RECOVERY/GUARD0.BIN",VFS_O_STREAM));
                if(mode==7 || mode==8)assert(FatFsImageFlush(privateGuard->leases[0])==-5);
            }
            assert(!FatFsMountPrivateRecovery(0,&dummy,&privateGuard) && !privateGuard);
            void* ordinary=nullptr;assert(!FatFsMount(0,&ordinary) && !ordinary);
            assert(disk==before && diskWrites==writesBefore);_exit(0);
        }
        int privateStatus=0;assert(waitpid(privateChild,&privateStatus,0)==privateChild && WIFEXITED(privateStatus) && !WEXITSTATUS(privateStatus));
    }
    std::ofstream output(argv[1],std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<char*>(disk.data()),disk.size());assert(output);output.close();
    std::puts("FAT_OBJECTS_PASS production-backend directory-pins/file-slot-pins/rename/independent-opens/fsync/rollback/final-release");
}
