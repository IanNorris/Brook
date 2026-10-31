#include <cassert>
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
#include "diskio.h"
#include <unistd.h>
#include <sys/wait.h>
static std::vector<unsigned char> disk;
static bool failRead=false, failWrite=false, failSync=false, intrinsicReadOnly=false, driveAcquired=false;
static unsigned diskReads=0;
extern "C" DSTATUS disk_initialize(BYTE drive){return drive?STA_NOINIT:0;}
extern "C" DSTATUS disk_status(BYTE drive){return drive?STA_NOINIT:0;}
extern "C" DRESULT disk_read(BYTE drive,BYTE* out,LBA_t sector,UINT count){
    ++diskReads;
    uint64_t offset=uint64_t(sector)*512,bytes=uint64_t(count)*512;
    if(drive || failRead || offset>disk.size() || bytes>disk.size()-offset)return RES_ERROR;
    std::memcpy(out,disk.data()+offset,bytes);return RES_OK;
}
extern "C" DRESULT disk_write(BYTE drive,const BYTE* in,LBA_t sector,UINT count){
    uint64_t offset=uint64_t(sector)*512,bytes=uint64_t(count)*512;
    if(drive || failWrite || offset>disk.size() || bytes>disk.size()-offset)return RES_ERROR;
    std::memcpy(disk.data()+offset,in,bytes);return RES_OK;
}
extern "C" DRESULT disk_ioctl(BYTE drive,BYTE command,void* out){
    if(drive)return RES_PARERR;
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
int FatFsFlushDrive(uint8_t){return failSync?-5:0;}
void VfsRelease(Vnode* node){if(__atomic_fetch_sub(&node->refCount,1,__ATOMIC_ACQ_REL)==1)::closeNode(node);}
bool FatFsAcquireDrive(uint8_t drive,bool* ro){if(drive || driveAcquired)return false;driveAcquired=true;*ro=intrinsicReadOnly;return true;}
void FatFsReleaseDrive(uint8_t){assert(driveAcquired);driveAcquired=false;}
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
    assert(!FatFsUnlink(mount,0,"/parent/moved/new retained.txt"));
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
    auto* other=FatObjectOpen(file,VFS_O_STREAM,&error);assert(other && FatFsFileSourceReadOnly(other));closeNode(other);
    VfsObjectRelease(file);VfsObjectRelease(root);
    assert(FatFsUnmount(mount)==-16);closeNode(stream);
    assert(!FatFsUnmount(mount) && !driveAcquired);
    assert(disk==diskBefore && allocations.empty() && pages.empty());
    intrinsicReadOnly=false;
    // Ordinary readers can still populate a cache, but streaming must bypass it.
    assert(FatFsMount(0,&privateMount));mount=static_cast<FatFsMountData*>(privateMount);
    auto* cached=FatFsOpen(mount,0,"/image.brk",VFS_O_READ);assert(cached && cached->ops==&g_cachedFileOps);
    stream=FatFsOpen(mount,0,"/image.brk",VFS_O_STREAM);assert(stream && stream->ops==&g_fatFileOps && !FatFsFileSourceReadOnly(stream));
    closeNode(stream);closeNode(cached);assert(!FatFsUnmount(mount));
    assert(allocations.empty() && pages.empty() && !driveAcquired);
    // A writable fixed-size image must have one exclusive, uncached owner.
    assert(FatFsMount(0,&privateMount));mount=static_cast<FatFsMountData*>(privateMount);
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
    for(unsigned fault=0;fault<5;++fault){
        const pid_t child=fork();assert(child>=0);
        if(!child){
            if(fault==0){failRead=true;assert(FatFsImageRead(lease,512*500,readback.data(),512)==-5);failRead=false;}
            if(fault==1){failWrite=true;assert(FatFsImageWrite(lease,512*500,sector.data(),512)==-5);failWrite=false;}
            if(fault==2){failSync=true;assert(FatFsImageFlush(lease)==-5);failSync=false;}
            if(fault==3){failSync=true;assert(FatFsSync(mount)==-5);failSync=false;}
            if(fault==4){failSync=true;assert(owner->ops->fsync(owner)==-5);failSync=false;}
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
    assert(!FatFsUnmount(mount));assert(allocations.empty() && pages.empty() && !driveAcquired);
    std::ofstream output(argv[1],std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<char*>(disk.data()),disk.size());assert(output);output.close();
    std::puts("FAT_OBJECTS_PASS production-backend directory-pins/file-slot-pins/rename/independent-opens/fsync/rollback/final-release");
}
