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
#include "diskio.h"
static std::vector<unsigned char> disk;
static bool failRead=false;
extern "C" DSTATUS disk_initialize(BYTE drive){return drive?STA_NOINIT:0;}
extern "C" DSTATUS disk_status(BYTE drive){return drive?STA_NOINIT:0;}
extern "C" DRESULT disk_read(BYTE drive,BYTE* out,LBA_t sector,UINT count){
    uint64_t offset=uint64_t(sector)*512,bytes=uint64_t(count)*512;
    if(drive || failRead || offset>disk.size() || bytes>disk.size()-offset)return RES_ERROR;
    std::memcpy(out,disk.data()+offset,bytes);return RES_OK;
}
extern "C" DRESULT disk_write(BYTE drive,const BYTE* in,LBA_t sector,UINT count){
    uint64_t offset=uint64_t(sector)*512,bytes=uint64_t(count)*512;
    if(drive || offset>disk.size() || bytes>disk.size()-offset)return RES_ERROR;
    std::memcpy(disk.data()+offset,in,bytes);return RES_OK;
}
extern "C" DRESULT disk_ioctl(BYTE drive,BYTE command,void* out){
    if(drive)return RES_PARERR;
    if(command==CTRL_SYNC)return RES_OK;
    if(command==GET_SECTOR_COUNT){*static_cast<LBA_t*>(out)=disk.size()/512;return RES_OK;}
    if(command==GET_SECTOR_SIZE){*static_cast<WORD*>(out)=512;return RES_OK;}
    if(command==GET_BLOCK_SIZE){*static_cast<DWORD*>(out)=1;return RES_OK;}
    return RES_PARERR;
}
extern "C" DWORD get_fattime(){return (DWORD(46)<<25)|(DWORD(10)<<21)|(DWORD(22)<<16);}

namespace {
std::mutex fatMutex,allocationMutex;
std::unordered_set<void*> allocations,pages;
thread_local long failAllocation=-1;
}
namespace brook {
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
int FatFsFlushDrive(uint8_t){return 0;}
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
    assert(allocations.empty() && pages.empty());
    std::ofstream output(argv[1],std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<char*>(disk.data()),disk.size());assert(output);output.close();
    std::puts("FAT_OBJECTS_PASS production-backend directory-pins/file-slot-pins/rename/independent-opens/fsync/rollback/final-release");
}
