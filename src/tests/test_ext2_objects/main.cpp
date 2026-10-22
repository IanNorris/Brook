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
#include "../../kernel/src/ext2_vfs.cpp"

namespace {
std::map<brook::KRwLock*,std::unique_ptr<std::shared_mutex>> fsLocks;
std::unordered_set<void*> allocations,frames;
std::mutex trackingLock;
thread_local long failAllocation=-1;
unsigned requiredAccess=0;
brook::Process process{};
std::vector<unsigned char> disk;
int readDisk(brook::Device*,uint64_t offset,void* out,uint64_t bytes){
    if(offset>disk.size() || bytes>disk.size()-offset)return -5;
    std::memcpy(out,disk.data()+offset,bytes);return bytes;
}
int writeDisk(brook::Device*,uint64_t offset,const void* in,uint64_t bytes){
    if(offset>disk.size() || bytes>disk.size()-offset)return -5;
    std::memcpy(disk.data()+offset,in,bytes);return bytes;
}
int ioctlDisk(brook::Device*,uint32_t,void*){return 0;}
const brook::DeviceOps diskOps={readDisk,writeDisk,ioctlDisk,nullptr};
void closeNode(brook::Vnode* node){assert(node);node->ops->close(node);brook::kfree(node);}
}
namespace brook {
extern "C" void* kmalloc(uint64_t size){
    if(failAllocation==0){failAllocation=-1;return nullptr;}
    if(failAllocation>0)--failAllocation;
    void* p=std::malloc(size);if(p){std::lock_guard<std::mutex> lock(trackingLock);allocations.insert(p);std::memset(p,0xcc,size);}return p;
}
extern "C" void kfree(void* p){if(p){std::lock_guard<std::mutex> lock(trackingLock);assert(allocations.erase(p)==1);std::free(p);}}
PhysicalAddress PmmAllocPage(MemTag,uint16_t){
    void* p=nullptr;assert(posix_memalign(&p,4096,4096)==0);{std::lock_guard<std::mutex> lock(trackingLock);frames.insert(p);}return PhysicalAddress(reinterpret_cast<uint64_t>(p));
}
void PmmFreePage(PhysicalAddress p){void* memory=reinterpret_cast<void*>(p.raw());{std::lock_guard<std::mutex> lock(trackingLock);assert(frames.erase(memory)==1);}std::free(memory);}
void KRwLockInit(KRwLock* lock){*lock={};fsLocks[lock]=std::make_unique<std::shared_mutex>();}
void KRwLockReadLock(KRwLock* lock){fsLocks.at(lock)->lock_shared();}
void KRwLockReadUnlock(KRwLock* lock){fsLocks.at(lock)->unlock_shared();}
void KRwLockWriteLock(KRwLock* lock){fsLocks.at(lock)->lock();}
void KRwLockWriteUnlock(KRwLock* lock){fsLocks.at(lock)->unlock();}
void FileObjectInit(FileObject* object){*object={};}
int FileObjectTruncateUnmapped(FileObject*,Vnode*,uint64_t){assert(false);return -95;}
void FileObjectDispose(FileObject* object){assert(!object->mappings && !object->mappingRegistrations);for(auto* p:object->pages)assert(!p);}
Process* ProcessCurrent(){return &process;}
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
uint64_t RtcNow(){return 1792627200;}
int DeviceFlush(Device*){return 0;}
extern "C" int DeviceWrite(Device* device,uint64_t offset,const void* bytes,uint64_t size){return device->ops->write(device,offset,bytes,size);}
extern "C" void SerialPrintf(const char*,...){}
void SerialPuts(const char*){}
}
// Production walker combined with the production Ext2 object callbacks. This
// provider represents one retained attachment; cross-mount topology is covered
// separately by the retained-walker identity graph tests.
struct BackendProvider {
    void Retain(brook::VfsObject* o){brook::VfsObjectRetain(o);}
    void Release(brook::VfsObject* o){brook::VfsObjectRelease(o);}
    bool Same(brook::VfsObject* a,brook::VfsObject* b){
        return a->identity==b->identity &&
            static_cast<brook::Ext2OpenInode*>(a->priv)->mnt==static_cast<brook::Ext2OpenInode*>(b->priv)->mnt;
    }
    int Stat(brook::VfsObject* o,brook::RetainedNodeInfo& info){
        brook::VnodeStat stat{};int e=o->ops->stat(o,&stat);info={stat.isDir,stat.isSymlink};return e;
    }
    int Search(brook::VfsObject* o){return o->ops->access(o,1,false);}
    int Lookup(brook::VfsObject* o,const char* n,brook::VfsObject*& out){return o->ops->lookup(o,n,&out);}
    int Parent(brook::VfsObject* o,brook::VfsObject*,brook::VfsObject*& out){return o->ops->parent(o,&out);}
    int Readlink(brook::VfsObject* o,char* out,uint32_t cap){return o->ops->readlink(o,out,cap);}
};
int main(int argc,char** argv){
    assert(argc==2);std::ifstream input(argv[1],std::ios::binary);assert(input);
    disk.assign(std::istreambuf_iterator<char>(input),{});input.close();
    using namespace brook;
    process.uid=process.euid=process.suid=1000;process.gid=process.egid=process.sgid=1000;process.umask=022;
    Device device{&diskOps,"host-ext2-object-fixture",DeviceType::Block,nullptr};
    g_ext2Devices[0]=&device;void* mount=nullptr;assert(Ext2FsMount(0,&mount));
    auto* m=static_cast<Ext2Mount*>(mount);
    assert(m->writeError==0 && m->deferredSyncError==0);
    // Make ownership independent of the host account that generated fixtures.
    m->skipPermChecks=true;
    VfsObject *root=nullptr,*held=nullptr,*marker=nullptr,*link=nullptr,*parent=nullptr;
    assert(!Ext2RootObject(m,0,&root));assert(!Ext2ObjectChild(root,"dir",&held));
    assert(!Ext2ObjectChild(held,"marker",&marker));
    VnodeStat original{};assert(!Ext2ObjectStat(marker,&original));
    int error=0;Vnode* first=Ext2ObjectOpen(marker,VFS_O_READ,&error);assert(first && !error);
    Vnode* second=Ext2ObjectOpen(marker,VFS_O_READ,&error);assert(second && !error && first!=second && first->priv!=second->priv);
    char bytes[32]{};uint64_t offset=0;assert(first->ops->read(first,bytes,8,&offset)==8 && !std::memcmp(bytes,"original",8));
    VfsObject* from=nullptr;assert(!Ext2ObjectFromVnode(first,&from));assert(from->identity==marker->identity);VfsObjectRelease(from);
    Vnode* directoryA=Ext2ObjectOpen(held,VFS_O_READ,&error);
    Vnode* directoryB=Ext2ObjectOpen(held,VFS_O_READ,&error);assert(directoryA && directoryB && directoryA->priv!=directoryB->priv);
    DirEntry entryA{},entryB{};uint32_t cookieA=0,cookieB=0;
    assert(directoryA->ops->readdir(directoryA,&entryA,&cookieA)==1);
    char firstEntry[256];std::strcpy(firstEntry,entryA.name);
    assert(directoryA->ops->readdir(directoryA,&entryA,&cookieA)==1);
    assert(directoryB->ops->readdir(directoryB,&entryB,&cookieB)==1 && !std::strcmp(firstEntry,entryB.name));
    assert(!Ext2FsRename(m,0,"/dir","/moved"));assert(!Ext2FsMkdir(m,0,"/dir",0755));
    Vnode* poison=Ext2FsOpenModeChecked(m,0,"/dir/marker",VFS_O_CREATE|VFS_O_WRITE,0644,&error);assert(poison);closeNode(poison);
    VfsObject* after=nullptr;assert(!Ext2ObjectChild(held,"marker",&after));assert(after->identity==marker->identity);VfsObjectRelease(after);
    assert(!Ext2ObjectParent(held,&parent));assert(parent->identity==root->identity);VfsObjectRelease(parent);
    assert(!Ext2ObjectChild(held,"relative",&link));char target[32];assert(Ext2ObjectReadlink(link,target,sizeof(target))==6 && !std::memcmp(target,"marker",6));
    assert(!Ext2ObjectOpen(link,VFS_O_READ,&error) && error==-40);
    Vnode* metadata=Ext2ObjectOpen(link,VFS_O_METADATA,&error);assert(metadata && !error && !metadata->ops->read);closeNode(metadata);VfsObjectRelease(link);
    BackendProvider provider;
    auto walk=[&](const char* text,VfsObject* expected){
        RetainedWalkResult<VfsObject*> result{};
        assert(!WalkRetainedPath(root,held,text,&result,provider));
        assert(provider.Same(result.object,expected));
        VfsObjectRelease(result.object);VfsObjectRelease(result.parent);
    };
    walk("relative",marker);walk("../moved/marker",marker);
    VfsObject* replacement=nullptr;assert(!Ext2ObjectChild(root,"dir",&replacement));walk("../dir",replacement);VfsObjectRelease(replacement);
    // Concurrent ancestor renames cannot redirect held-object child lookup.
    std::atomic<bool> begin{false};
    std::thread writer([&]{
        while(!begin.load(std::memory_order_acquire))std::this_thread::yield();
        for(unsigned i=0;i<100;++i){assert(!Ext2FsRename(m,0,"/moved","/alternate"));assert(!Ext2FsRename(m,0,"/alternate","/moved"));}
    });
    begin.store(true,std::memory_order_release);
    for(unsigned i=0;i<200;++i)walk("relative",marker);
    writer.join();
    // Real object-open authorization path: inspect exact required bits through
    // the host's single-identity credential shim, with on-disk mode changes.
    assert(!Ext2FsChmod(m,0,"/moved/marker",0444));m->skipPermChecks=false;
    assert(!Ext2ObjectOpen(marker,VFS_O_READWRITE,&error) && error==-22);
    assert(!Ext2ObjectOpen(held,VFS_O_READWRITE,&error) && error==-22);
    assert(!Ext2ObjectOpen(marker,VFS_O_WRITE|VFS_O_READWRITE,&error) && error==-13 && requiredAccess==6);
    assert(!Ext2ObjectOpen(marker,VFS_O_WRITE|VFS_O_EXEC,&error) && error==-22);
    assert(!Ext2ObjectOpen(marker,VFS_O_TRUNC,&error) && error==-22);
    assert(!Ext2ObjectOpen(held,VFS_O_WRITE,&error) && error==-21);
    m->skipPermChecks=true;assert(!Ext2FsChmod(m,0,"/moved/marker",0644));
    // Warm caches before forcing each object-factory allocation failure. No
    // failed acquisition may leave an inode holder or wrapper behind.
    assert(!Ext2ObjectChild(held,"marker",&after));VfsObjectRelease(after);
    const auto before=allocations.size();
    for(long failure=0;failure<2;++failure){
        failAllocation=failure;after=reinterpret_cast<VfsObject*>(1);
        assert(Ext2ObjectChild(held,"marker",&after)==-12 && !after);
        failAllocation=-1;assert(allocations.size()==before);
    }
    for(auto* object : {held,marker})for(long failure=0;failure<3;++failure){
        const auto live=allocations.size();failAllocation=failure;
        assert(!Ext2ObjectOpen(object,VFS_O_READ,&error) && error==-12);
        failAllocation=-1;assert(allocations.size()==live);
        VnodeStat intact{};assert(!Ext2ObjectStat(object,&intact));
    }
    closeNode(directoryB);closeNode(directoryA);closeNode(second);closeNode(first);
    assert(!Ext2FsRemovePath(m,0,"/moved/marker",false));
    Vnode* reopened=Ext2ObjectOpen(marker,VFS_O_READ,&error);assert(reopened && !error);
    offset=0;assert(reopened->ops->read(reopened,bytes,8,&offset)==8 && !std::memcmp(bytes,"original",8));closeNode(reopened);
    VnodeStat orphan{};assert(!Ext2ObjectStat(marker,&orphan) && !orphan.nlink && orphan.ino==original.ino);
    VfsObjectRelease(marker);assert(!Ext2FsRemovePath(m,0,"/moved/relative",false));
    assert(!Ext2FsRemovePath(m,0,"/moved",true));
    assert(!Ext2ObjectStat(held,&orphan) && !orphan.nlink && orphan.isDir);
    assert(!Ext2FsMkdir(m,0,"/fresh",0755));
    VfsObject* fresh=nullptr;assert(!Ext2ObjectChild(root,"fresh",&fresh));assert(fresh->identity!=held->identity);
    after=reinterpret_cast<VfsObject*>(1);assert(Ext2ObjectChild(held,"marker",&after)==-2 && !after);
    VfsObjectRelease(fresh);assert(!Ext2FsRemovePath(m,0,"/fresh",true));
    VfsObjectRelease(held);assert(!Ext2FsCanUnmount(m)); // root still held
    VfsObjectRelease(root);assert(Ext2FsCanUnmount(m));assert(!Ext2FsUnmount(m));
    assert(!g_ext2OpenInodes && allocations.empty() && frames.empty());
    // Compose the production backend and production namespace, using distinct
    // locks rather than a shared host shim that would mask lock-order errors.
    VfsInit();assert(VfsRegisterFs("ext2",&g_ext2FsOps));assert(VfsMount("/","ext2",0));
    process.uid=process.euid=process.suid=0;process.gid=process.egid=process.sgid=0;
    assert(!VfsMkdir("/context-dir",0755));
    Vnode* created=VfsOpen("/context-dir/value",VFS_O_CREATE|VFS_O_WRITE);assert(created);VfsClose(created);
    VfsPath fsRoot{},cwd{},value{},resolved{};assert(!VfsPathRoot(&fsRoot));
    assert(!VfsPathResolve(fsRoot,fsRoot,"/context-dir",&cwd));
    assert(!VfsPathResolve(fsRoot,cwd,"value",&value));
    Vnode* legacyDirectory=VfsOpen("/context-dir");assert(legacyDirectory);
    VfsPath captured{};assert(!VfsPathFromVnode(legacyDirectory,&captured));assert(VfsPathSame(captured,cwd));
    VfsPathRelease(captured);VfsClose(legacyDirectory);
    assert(!VfsRename("/context-dir","/context-moved"));assert(!VfsMkdir("/context-dir",0755));
    assert(!VfsPathResolve(fsRoot,cwd,"value",&resolved)&&VfsPathSame(resolved,value));VfsPathRelease(resolved);
    // A second attachment on a separate memory-device number covers the held
    // cwd. No writes are made while the same backing bytes are mounted twice.
    g_ext2Devices[1]=&device;assert(VfsMount("/context-moved","ext2",1));
    assert(!VfsPathResolve(fsRoot,fsRoot,"/context-moved",&resolved));assert(resolved.attachment!=cwd.attachment);
    VfsPathRelease(resolved);
    assert(!VfsPathResolve(fsRoot,cwd,"value",&resolved)&&VfsPathSame(resolved,value));VfsPathRelease(resolved);
    assert(VfsUnmount("/context-moved"));
    Vnode* exact=VfsPathOpen(value,VFS_O_READ,&error);assert(exact&&!error);
    VfsPathRelease(value);VfsPathRelease(cwd);VfsPathRelease(fsRoot);assert(!VfsUnmount("/"));
    VfsClose(exact);assert(VfsUnmount("/"));
    assert(!g_ext2OpenInodes && allocations.empty() && frames.empty());
    std::ofstream output(argv[1],std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<char*>(disk.data()),disk.size());assert(output);output.close();
    std::puts("EXT2_OBJECTS_PASS production-backend rename/orphan/independent-opens/permission-modes/allocation-unwind/teardown");
}
