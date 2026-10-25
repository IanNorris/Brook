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

namespace {
std::map<brook::KRwLock*,std::unique_ptr<std::shared_mutex>> fsLocks;
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
int readDisk(brook::Device* device,uint64_t offset,void* out,uint64_t bytes){
    auto& storage=device->priv?*static_cast<std::vector<unsigned char>*>(device->priv):disk;
    if(offset>storage.size() || bytes>storage.size()-offset)return -5;
    std::memcpy(out,storage.data()+offset,bytes);return bytes;
}
int writeDisk(brook::Device* device,uint64_t offset,const void* in,uint64_t bytes){
    if(offset==failWriteOffset){failWriteOffset=nextFailWriteOffset;nextFailWriteOffset=~uint64_t{0};writeFailureInjected=true;++injectedWriteFailures;return -5;}
    auto& storage=device->priv?*static_cast<std::vector<unsigned char>*>(device->priv):disk;
    if(offset>storage.size() || bytes>storage.size()-offset)return -5;
    std::memcpy(storage.data()+offset,in,bytes);return bytes;
}
int ioctlDisk(brook::Device*,uint32_t,void*){return 0;}
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
void PmmFreePage(PhysicalAddress p){void* memory=reinterpret_cast<void*>(p.raw());{std::lock_guard<std::mutex> lock(trackingLock);assert(frames.erase(memory)==1);}std::free(memory);}
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
    int Lookup(brook::VfsObject* o,const char* n,brook::VfsObject*& out){return o->ops->lookup(o,n,&out,false);}
    int Parent(brook::VfsObject* o,brook::VfsObject*,brook::VfsObject*& out){return o->ops->parent(o,&out,false);}
    int Readlink(brook::VfsObject* o,char* out,uint32_t cap){return o->ops->readlink(o,out,cap);}
};
int main(int argc,char** argv){
    assert(argc==2);std::ifstream input(argv[1],std::ios::binary);assert(input);
    disk.assign(std::istreambuf_iterator<char>(input),{});input.close();
    using namespace brook;
    process.uid=process.euid=process.suid=1000;process.gid=process.egid=process.sgid=1000;legacyTestMask=022;
    Device device{&diskOps,"host-ext2-object-fixture",DeviceType::Block,nullptr};
    g_ext2Devices[0]=&device;void* mount=nullptr;assert(Ext2FsMount(0,&mount));
    auto* m=static_cast<Ext2Mount*>(mount);
    assert(m->writeError==0 && m->deferredSyncError==0);
    // Make ownership independent of the host account that generated fixtures.
    m->skipPermChecks=true;
    VfsObject *root=nullptr,*held=nullptr,*marker=nullptr,*link=nullptr,*parent=nullptr;
    assert(!Ext2RootObject(m,0,&root));
    // Exact entry reporting, independently of shared inode identity. These
    // production callbacks back ordinary and O_PATH proc-fd readlink alike.
    {
        assert(!root->ops->mkdir(root,"report",0700));
        assert(!root->ops->mkdir(root,"report-other",0700));
        VfsObject *directory=nullptr,*other=nullptr;
        assert(!Ext2ObjectChild(root,"report",&directory));
        assert(!Ext2ObjectChild(root,"report-other",&other));
        auto pathIs=[](VfsObject* object,const char* expected) {
            char text[512];assert(object->ops->object_path);
            assert(!object->ops->object_path(object,text,sizeof(text)));
            assert(!std::strcmp(text,expected));
            VfsObject* parent=nullptr;char leaf[256];
            int error=object->ops->entry_parent(object,&parent,leaf,sizeof(leaf));
            if(strstr(expected," (deleted)"))assert(error==-2 && !parent);
            else {
                assert(!error && parent);char directory[512];
                assert(!parent->ops->directory_path(parent,directory,sizeof(directory)));
                std::string joined=std::string(directory)+(strcmp(directory,"/")?"/":"")+leaf;
                assert(joined==expected);VfsObjectRelease(parent);
            }
        };
        auto create=[&](const char* name) {
            int error=0;auto* node=Ext2ObjectCreate(directory,name,VFS_O_WRITE,0600,&error);
            assert(node&&!error&&node->backendObject);
            VfsObject* object=nullptr;assert(!Ext2ObjectFromVnode(node,&object));
            assert(object==node->backendObject);closeNode(node);return object;
        };
        auto* source=create("source");auto* victim=create("victim");
        pathIs(source,"/report/source");pathIs(victim,"/report/victim");
        char tiny[4];assert(source->ops->object_path(source,tiny,sizeof(tiny))==-36);
        assert(!directory->ops->rename(directory,"source",directory,"victim"));
        pathIs(source,"/report/victim");pathIs(victim,"/report/victim (deleted)");
        assert(!root->ops->rename(root,"report",root,"report-moved"));
        pathIs(source,"/report-moved/victim");pathIs(victim,"/report-moved/victim (deleted)");
        assert(!directory->ops->rename(directory,"victim",other,"moved"));
        pathIs(source,"/report-other/moved");
        assert(!other->ops->remove(other,"moved",false));
        pathIs(source,"/report-other/moved (deleted)");
        int error=0;auto* replacement=Ext2ObjectCreate(other,"moved",VFS_O_WRITE,0600,&error);
        assert(replacement&&!error);pathIs(replacement->backendObject,"/report-other/moved");
        pathIs(source,"/report-other/moved (deleted)");closeNode(replacement);
        assert(!other->ops->remove(other,"moved",false));
        VfsObjectRelease(source);VfsObjectRelease(victim);

        // Model prebuilt hard links without adding a link syscall or choosing
        // an arbitrary inode alias for reporting.
        auto* left=create("left");
        KRwLockWriteLock(&g_ext2Lock);
        auto* dh=static_cast<Ext2OpenInode*>(directory->priv);
        Ext2Inode dd{},file{};assert(Ext2ReadInode(m,dh->inode,&dd));
        assert(Ext2ReadInode(m,left->identity,&file));
        assert(Ext2DirAdd(m,dh->inode,&dd,left->identity,"right",1));
        ++file.i_links_count;assert(Ext2WriteInode(m,left->identity,&file));
        KRwLockWriteUnlock(&g_ext2Lock);
        VfsObject* right=nullptr;assert(!Ext2ObjectChild(directory,"right",&right));
        pathIs(left,"/report-moved/left");pathIs(right,"/report-moved/right");
        assert(!directory->ops->rename(directory,"left",directory,"right"));
        pathIs(left,"/report-moved/left");pathIs(right,"/report-moved/right");
        assert(!directory->ops->rename(directory,"left",directory,"renamed"));
        pathIs(left,"/report-moved/renamed");pathIs(right,"/report-moved/right");
        assert(!directory->ops->remove(directory,"right",false));
        pathIs(left,"/report-moved/renamed");pathIs(right,"/report-moved/right (deleted)");

        // Report, rename and retire other references concurrently. Every
        // report must be one complete committed entry, never a mixed label.
        std::atomic<bool> go{false};
        std::thread renamer([&]{while(!go.load()){};for(unsigned i=0;i<50;++i){
            assert(!directory->ops->rename(directory,"renamed",directory,"alternate"));
            assert(!directory->ops->rename(directory,"alternate",directory,"renamed"));
        }});
        std::thread observer([&]{while(!go.load()){};for(unsigned i=0;i<100;++i){
            VfsObject* temporary=nullptr;int e=Ext2ObjectChild(directory,"renamed",&temporary);
            assert(e==0||e==-2);VfsObjectRelease(temporary);
        }});
        go=true;
        for(unsigned i=0;i<100;++i){char text[512];assert(!left->ops->object_path(left,text,sizeof(text)));
            assert(!strcmp(text,"/report-moved/renamed")||!strcmp(text,"/report-moved/alternate"));}
        renamer.join();observer.join();

        // Fail the final source-inode metadata write, after both directory
        // entries have committed. EIO must not leave reporting at the old name.
        uint32_t ino=left->identity,group=(ino-1)/m->inodesPerGroup,index=(ino-1)%m->inodesPerGroup;
        failWriteOffset=(uint64_t(m->bgdt[group].bg_inode_table)<<m->blockShift)+uint64_t(index)*m->inodeSize;
        assert(directory->ops->rename(directory,"renamed",directory,"after-error")==-5);
        assert(writeFailureInjected&&m->writeError==-5);
        pathIs(left,"/report-moved/after-error");
        // Restore only the host fault fixture for later unrelated cases. This
        // is not a production filesystem error-recovery mechanism.
        m->writeError=0;
        assert(!directory->ops->remove(directory,"after-error",false));
        VfsObjectRelease(left);VfsObjectRelease(right);

        auto inodeOffset=[&](uint32_t number){return
            (uint64_t(m->bgdt[(number-1)/m->inodesPerGroup].bg_inode_table)<<m->blockShift)+
            uint64_t((number-1)%m->inodesPerGroup)*m->inodeSize;};
        // Failed destination and source restoration must permanently reject
        // uncertain reporting, even after the same inode appears there again.
        for(bool failSource : {false,true}) {
            auto* uncertain=create("rollback-source");
            auto* parentHold=static_cast<Ext2OpenInode*>((failSource?directory:other)->priv);
            Ext2Inode parentData{};
            KRwLockReadLock(&g_ext2Lock);assert(Ext2ReadInode(m,parentHold->inode,&parentData));
            auto block=Ext2BlockMap(m,&parentData,0);KRwLockReadUnlock(&g_ext2Lock);assert(block);
            failWriteOffset=inodeOffset(parentHold->inode);
            nextFailWriteOffset=uint64_t(block)<<m->blockShift;
            auto prior=injectedWriteFailures;
            assert(directory->ops->rename(directory,"rollback-source",other,"rollback-target")==-5);
            assert(injectedWriteFailures==prior+2);
            char text[512];assert(uncertain->ops->object_path(uncertain,text,sizeof(text))==-5);
            // Repair only the memory-disk fixture's directory entry, then
            // prove the old reporting record cannot resurrect by name reuse.
            KRwLockWriteLock(&g_ext2Lock);
            if(failSource) {
                auto* dh=static_cast<Ext2OpenInode*>(directory->priv);Ext2Inode data{};
                assert(Ext2ReadInode(m,dh->inode,&data));
                assert(Ext2DirAdd(m,dh->inode,&data,uncertain->identity,"rollback-source",1));
            } else {
                auto* dh=static_cast<Ext2OpenInode*>(other->priv);Ext2Inode data{};
                assert(Ext2ReadInode(m,dh->inode,&data));
                assert(Ext2DirRemove(m,&data,dh->inode,"rollback-target")==uncertain->identity);
            }
            m->writeError=0;KRwLockWriteUnlock(&g_ext2Lock);
            assert(uncertain->ops->object_path(uncertain,text,sizeof(text))==-5);
            VfsObject* fresh=nullptr;assert(!Ext2ObjectChild(directory,"rollback-source",&fresh));
            pathIs(fresh,"/report-moved/rollback-source");VfsObjectRelease(fresh);
            assert(!directory->ops->remove(directory,"rollback-source",false));VfsObjectRelease(uncertain);
        }
        VfsObjectRelease(directory);VfsObjectRelease(other);
        assert(!root->ops->remove(root,"report-moved",true));
        assert(!root->ops->remove(root,"report-other",true));
    }
    assert(!Ext2ObjectChild(root,"dir",&held));
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
    // Mutations use the held parent inode after its old global name has been
    // replaced. Modes are already masked by the context snapshot, never by a
    // second read of the process mask inside the inode lock.
    legacyTestMask=077;
    Vnode* newlyCreated=held->ops->create(held,"created",VFS_O_WRITE,0640,&error);assert(newlyCreated&&!error);
    assert(!held->ops->create(held,"created",VFS_O_WRITE,0600,&error)&&error==-17);
    assert(!held->ops->symlink(held,"nonexistent","dangling"));
    assert(!held->ops->create(held,"dangling",VFS_O_WRITE,0600,&error)&&error==-17);
    assert(!held->ops->remove(held,"dangling",false));
    VfsObject* createdObject=nullptr;assert(!Ext2ObjectChild(held,"created",&createdObject));
    VnodeStat createdStat{};assert(!createdObject->ops->stat(createdObject,&createdStat)&&(createdStat.mode&0777)==0640);
    assert(!held->ops->mkdir(held,"nested",0750));
    VfsObject* nested=nullptr;assert(!Ext2ObjectChild(held,"nested",&nested));
    assert(!nested->ops->stat(nested,&createdStat)&&(createdStat.mode&0777)==0750);
    assert(!held->ops->rename(held,"created",nested,"renamed"));
    assert(!held->ops->symlink(held,"nested/renamed","made-link"));
    VfsObject* madeLink=nullptr;assert(!Ext2ObjectChild(held,"made-link",&madeLink));
    char madeTarget[64];assert(madeLink->ops->readlink(madeLink,madeTarget,sizeof(madeTarget))==14);
    VfsObjectRelease(madeLink);
    assert(!held->ops->remove(held,"made-link",false));
    Vnode* madeSocket=held->ops->create_socket(held,"made-socket",0660,&error);assert(madeSocket&&!error);
    assert(!madeSocket->ops->stat(madeSocket,&createdStat)&&(createdStat.mode&0777)==0660);closeNode(madeSocket);
    assert(!held->ops->remove(held,"made-socket",false));
    assert(!createdObject->ops->chmod(createdObject,0604));
    VnodeTime changedTimes[2]={{1234,0},{5678,0}};assert(!createdObject->ops->set_times(createdObject,changedTimes));
    assert(!createdObject->ops->stat(createdObject,&createdStat)&&createdStat.mtime.sec==5678&&(createdStat.mode&0777)==0604);
    assert(createdObject->ops->chown(createdObject,2000,2000)==-1);
    process.euid=0;assert(!createdObject->ops->chown(createdObject,1000,1000));process.euid=1000;
    char liveDirectory[128];assert(!held->ops->directory_path(held,liveDirectory,sizeof(liveDirectory))&&!std::strcmp(liveDirectory,"/moved"));
    assert(!held->ops->chmod(held,01777));m->skipPermChecks=false;process.uid=process.euid=2000;
    assert(nested->ops->remove(nested,"renamed",false)==-13); // Nested directory search/write denied.
    assert(held->ops->remove(held,"nested",true)==-1); // Sticky parent ownership.
    process.uid=process.euid=1000;m->skipPermChecks=true;assert(!held->ops->chmod(held,0755));
    closeNode(newlyCreated);assert(!nested->ops->remove(nested,"renamed",false));
    assert(!createdObject->ops->stat(createdObject,&createdStat)&&!createdStat.nlink);VfsObjectRelease(createdObject);
    assert(!held->ops->remove(held,"nested",true));VfsObjectRelease(nested);
    assert(held->ops->mkdir(held,"..",0700)==-22);assert(held->ops->remove(held,"a/b",false)==-22);
    assert(!held->ops->create(held,"/escape",VFS_O_WRITE,0600,&error)&&error==-22);
    legacyTestMask=022;
    assert(!held->ops->chmod(held,0600));m->skipPermChecks=false;
    assert(!held->ops->create(held,"marker",VFS_O_WRITE,0600,&error)&&error==-13);
    assert(held->ops->mkdir(held,"marker",0700)==-13);
    assert(!held->ops->create_socket(held,"marker",0600,&error)&&error==-13);
    m->skipPermChecks=true;assert(!held->ops->chmod(held,0755));
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
    bool factoryComplete=false;
    for(long failure=0;failure<32;++failure){
        failTriggered=false;failAllocation=failure;after=reinterpret_cast<VfsObject*>(1);
        int result=Ext2ObjectChild(held,"marker",&after);
        if(result)assert(result==-12&&!after);else VfsObjectRelease(after);
        failAllocation=-1;assert(allocations.size()==before);
        if(!failTriggered){assert(!result);factoryComplete=true;break;}
    }
    assert(factoryComplete);
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
    assert(!held->ops->create(held,"new",VFS_O_WRITE,0600,&error)&&error==-2);
    assert(held->ops->mkdir(held,"new",0700)==-2);
    assert(held->ops->symlink(held,"target","new")==-2);
    assert(!held->ops->create_socket(held,"new",0600,&error)&&error==-2);
    char detachedName[64];assert(held->ops->directory_path(held,detachedName,sizeof(detachedName))==-2);
    assert(!Ext2FsMkdir(m,0,"/fresh",0755));
    VfsObject* fresh=nullptr;assert(!Ext2ObjectChild(root,"fresh",&fresh));assert(fresh->identity!=held->identity);
    after=reinterpret_cast<VfsObject*>(1);assert(Ext2ObjectChild(held,"marker",&after)==-2 && !after);
    VfsObjectRelease(fresh);assert(!Ext2FsRemovePath(m,0,"/fresh",true));
    VfsObjectRelease(held);assert(!Ext2FsCanUnmount(m)); // root still held
    VfsObjectRelease(root);assert(Ext2FsCanUnmount(m));assert(!Ext2FsUnmount(m));
    assert(!g_ext2OpenInodes && allocations.empty() && frames.empty());
    // Production VFS transactions with distinct backend/namespace locks and
    // independent device images, so overmount writes never share stale caches.
    VfsInit();assert(VfsRegisterFs("ext2",&g_ext2FsOps));assert(VfsMount("/","ext2",0));
    process.uid=process.euid=process.suid=0;process.gid=process.egid=process.sgid=0;
    assert(!VfsMkdir("/container",0755));assert(!VfsMkdir("/container/context-dir",0755));
    Vnode* created=VfsOpen("/container/context-dir/value",VFS_O_CREATE|VFS_O_WRITE);assert(created);VfsClose(created);
    VfsPath fsRoot{},cwd{},value{},resolved{};assert(!VfsPathRoot(&fsRoot));
    assert(!VfsPathResolve(fsRoot,fsRoot,"/container/context-dir",&cwd));
    assert(!VfsPathResolve(fsRoot,cwd,"value",&value));
    // Mapped Ext2 backing survives unlink and replacement. Retained mutations
    // must use the same backend-specific gate as the legacy pathname API.
    auto* mappedSource=VfsOpenAt(fsRoot,cwd,"mapped-source",VFS_O_CREATE|VFS_O_WRITE,0600,0,&error);
    auto* mappedTarget=VfsOpenAt(fsRoot,cwd,"mapped-target",VFS_O_CREATE|VFS_O_WRITE,0600,0,&error);
    assert(mappedSource && mappedTarget && !VfsFileMapPin(mappedSource) && !VfsFileMapPin(mappedTarget));
    assert(!VfsRenameAt(fsRoot,cwd,"mapped-source",cwd,"mapped-target"));
    VnodeStat mappedStat{};assert(!VfsStat(mappedTarget,&mappedStat) && !mappedStat.nlink);
    assert(!VfsRemoveAt(fsRoot,cwd,"mapped-target",false));
    assert(!VfsStat(mappedSource,&mappedStat) && !mappedStat.nlink);
    VfsFileMapUnpin(mappedSource);VfsFileMapUnpin(mappedTarget);VfsClose(mappedSource);VfsClose(mappedTarget);
    FsContext* context=FsContextCreate(fsRoot,cwd,022,&error);assert(context&&!error);
    FsContext* copied=FsContextCopy(context,&error);assert(copied&&!error);
    FsContextRetain(context);FsContext* shared=context;
    assert(FsContextExchangeMask(shared,0077)==022);assert(FsContextReadMask(context)==0077&&FsContextReadMask(copied)==022);
    assert(!FsContextSetCwd(shared,fsRoot));FsSnapshot snapshot{};
    assert(!FsContextSnapshot(context,&snapshot)&&VfsPathSame(snapshot.cwd,fsRoot));FsSnapshotRelease(&snapshot);
    assert(!FsContextSnapshot(copied,&snapshot)&&VfsPathSame(snapshot.cwd,cwd));FsSnapshotRelease(&snapshot);
    assert(FsContextSetCwd(context,value)==-20);
    const auto contextAllocations=allocations.size();failAllocation=0;
    assert(!FsContextCopy(context,&error)&&error==-12);failAllocation=-1;assert(allocations.size()==contextAllocations);
    assert(!FsContextCreate(fsRoot,value,022,&error)&&error==-20);
    assert(!FsContextSetCwd(context,cwd));
    std::thread contextWriter([&]{
        for(unsigned i=0;i<100;++i){assert(!FsContextSetCwd(shared,i&1?cwd:fsRoot));FsContextExchangeMask(shared,i&1?022:077);}
    });
    for(unsigned i=0;i<200;++i){
        FsSnapshot current{};assert(!FsContextSnapshot(context,&current));
        assert(VfsPathSame(current.root,fsRoot)&&(VfsPathSame(current.cwd,cwd)||VfsPathSame(current.cwd,fsRoot)));
        assert(current.mask==022||current.mask==077);FsSnapshotRelease(&current);
    }
    contextWriter.join();FsContextRelease(shared);FsContextRelease(context);
    assert(!FsContextSnapshot(copied,&snapshot));FsContextRelease(copied);
    assert(VfsPathSame(snapshot.cwd,cwd));FsSnapshotRelease(&snapshot);
    Vnode* legacyDirectory=VfsOpen("/container/context-dir");assert(legacyDirectory);
    VfsPath captured{};assert(!VfsPathFromVnode(legacyDirectory,&captured));assert(VfsPathSame(captured,cwd));VfsPathRelease(captured);
    assert(!VfsRename("/container/context-dir","/container/context-moved"));assert(!VfsMkdir("/container/context-dir",0755));
    char cwdName[128];assert(!VfsDirectoryPath(legacyDirectory,cwdName,sizeof(cwdName))&&!std::strcmp(cwdName,"/container/context-moved"));VfsClose(legacyDirectory);
    assert(!VfsPathDirectoryPath(cwd,cwdName,sizeof(cwdName))&&!std::strcmp(cwdName,"/container/context-moved"));
    auto* exactDirectory=VfsPathOpen(cwd,VFS_O_METADATA,&error);assert(exactDirectory);
    assert(!VfsDirectoryPath(exactDirectory,cwdName,sizeof(cwdName))&&!std::strcmp(cwdName,"/container/context-moved"));VfsClose(exactDirectory);
    auto* atFile=VfsOpenAt(fsRoot,cwd,"at-created",VFS_O_CREATE|VFS_O_WRITE,0666,0027,&error);assert(atFile&&!error);
    VnodeStat atStat{};assert(!VfsStatAt(fsRoot,cwd,"at-created",&atStat)&&(atStat.mode&0777)==0640);
    assert(!VfsOpenAt(fsRoot,cwd,"at-created",VFS_O_CREATE|VFS_O_EXCL|VFS_O_WRITE,0600,0,&error)&&error==-17);
    assert(VfsMkdirAt(fsRoot,cwd,".",0755,0)==-17);assert(VfsMkdirAt(fsRoot,fsRoot,"/",0755,0)==-17);
    assert(!VfsMkdirAt(fsRoot,cwd,"nested",0777,0027));
    assert(!VfsStatAt(fsRoot,cwd,"nested",&atStat)&&(atStat.mode&0777)==0750);
    assert(!VfsSymlinkAt(fsRoot,cwd,"at-created","at-link"));
    assert(VfsReadlinkAt(fsRoot,cwd,"at-link",cwdName,sizeof(cwdName))==10);
    assert(!VfsStatAt(fsRoot,cwd,"at-link",&atStat,false)&&atStat.isSymlink);
    assert(!VfsStatAt(fsRoot,cwd,"at-link",&atStat)&&!atStat.isSymlink);
    VfsPath nestedPath{};assert(!VfsPathResolve(fsRoot,cwd,"nested",&nestedPath));
    g_ext2Mounts[0]->skipPermChecks=false;
    assert(!VfsChmodAt(fsRoot,cwd,".",0700));assert(!VfsChmodAt(fsRoot,nestedPath,".",0700));
    process.uid=0;process.euid=1000;
    assert(!VfsAccessAt(fsRoot,nestedPath,"../at-created",4,true));
    assert(VfsAccessAt(fsRoot,nestedPath,"../at-created",4,false)==-13);
    process.uid=1000;process.euid=0;
    assert(VfsAccessAt(fsRoot,nestedPath,"../at-created",4,true)==-13);
    assert(!VfsAccessAt(fsRoot,nestedPath,"../at-created",4,false));
    process.uid=process.euid=0;
    assert(!VfsChmodAt(fsRoot,cwd,".",0755));assert(!VfsChmodAt(fsRoot,nestedPath,".",0750));
    assert(!VfsRenameAt(fsRoot,cwd,"at-created",nestedPath,"renamed"));
    assert(VfsStatAt(fsRoot,cwd,"at-created",&atStat)==-2);
    assert(!VfsChmodAt(fsRoot,nestedPath,"renamed",0604));
    assert(!VfsChownAt(fsRoot,nestedPath,"renamed",0,0));
    assert(!VfsSetTimesAt(fsRoot,nestedPath,"renamed",changedTimes));
    assert(!VfsAccessAt(fsRoot,nestedPath,"renamed",4));
    auto* atSocket=VfsCreateSocketAt(fsRoot,cwd,"at-socket",0777,0077,&error);assert(atSocket&&!error);VfsClose(atSocket);
    assert(!VfsRemoveAt(fsRoot,cwd,"at-socket",false));
    assert(!VfsRemoveAt(fsRoot,cwd,"at-link",false));
    VfsClose(atFile);assert(!VfsRemoveAt(fsRoot,nestedPath,"renamed",false));
    VfsPathRelease(nestedPath);assert(!VfsRemoveAt(fsRoot,cwd,"nested",true));
    assert(!VfsPathResolve(fsRoot,cwd,"value",&resolved)&&VfsPathSame(resolved,value));VfsPathRelease(resolved);
    std::vector<unsigned char> otherDisks[4];Device otherDevices[4]{};
    auto prepareVolume=[&](unsigned number){
        assert(number>0 && number<4);assert(!Ext2FsSync(g_ext2Mounts[0]));otherDisks[number]=disk;
        otherDevices[number]={&diskOps,"private-independent-ext2",DeviceType::Block,&otherDisks[number]};g_ext2Devices[number]=&otherDevices[number];
    };
    auto parity=[&](const char* text){
        VfsPath path{};assert(!VfsPathResolve(fsRoot,fsRoot,text,&path));
        Vnode* legacy=VfsOpen(text);assert(legacy&&legacy->mountOwner==path.attachment);
        VfsClose(legacy);VfsPathRelease(path);
    };
    assert(!VfsMkdirAt(fsRoot,cwd,"d",0755,0));prepareVolume(2);
    assert(VfsMount("/container/context-moved/d/m","ext2",2));prepareVolume(1);
    assert(VfsMount("/container/context-moved","ext2",1));
    assert(!VfsPathResolve(fsRoot,fsRoot,"/container/context-moved",&resolved));assert(resolved.attachment!=cwd.attachment);
    assert(cwd.object->ops->rename(cwd.object,"value",resolved.object,"new-value")==-18);
    assert(VfsRenameAt(fsRoot,cwd,"value",resolved,"new-value")==-18);VfsPathRelease(resolved);
    assert(!VfsMkdir("/container/context-moved/d",0755));prepareVolume(3);
    assert(VfsMount("/container/context-moved/d/m","ext2",3));
    // Fail every allocation through retained rename's label preflight. No
    // partial backend rename or mount-label publication may escape failure.
    bool renamedWithLabels=false;
    for(long failure=0;failure<128;++failure) {
        failTriggered=false;failAllocation=failure;
        int renameResult=VfsRenameAt(fsRoot,cwd,"d",cwd,"e");failAllocation=-1;
        if(!renameResult) {
            if(!failTriggered) { renamedWithLabels=true;break; }
            // Backend advisory reads may retry a transient allocation failure.
            // Undo a successful operation and continue through every position.
            assert(!VfsRenameAt(fsRoot,cwd,"e",cwd,"d"));continue;
        }
        assert(renameResult<0); // Exact backend OOM errno is a separate contract.
        VnodeStat beforeRename{};
        assert(!VfsStatAt(fsRoot,cwd,"d",&beforeRename) && beforeRename.isDir);
        assert(VfsStatAt(fsRoot,cwd,"e",&beforeRename)==-2);
        parity("/container/context-moved/d/m");
    }
    assert(renamedWithLabels); // Held lower parent while covered.
    parity("/container/context-moved/d/m");
    assert(!VfsRenameAt(fsRoot,fsRoot,"container",fsRoot,"renamed-container"));
    parity("/renamed-container/context-moved/d/m");assert(!VfsOpen("/container/context-moved/d/m"));
    assert(VfsUnmount("/renamed-container/context-moved/d/m"));
    assert(VfsUnmount("/renamed-container/context-moved"));
    parity("/renamed-container/context-moved/e/m");assert(!VfsOpen("/renamed-container/context-moved/d/m"));
    assert(VfsUnmount("/renamed-container/context-moved/e/m"));assert(!VfsRemoveAt(fsRoot,cwd,"e",true));
    assert(!VfsPathResolve(fsRoot,cwd,"value",&resolved)&&VfsPathSame(resolved,value));VfsPathRelease(resolved);
    // Exercise namespace leases against actual Ext2 inode objects, including
    // a vnode surviving both detach and last-member namespace retirement.
    assert(!VfsMkdirAt(fsRoot,fsRoot,"namespace-bind",0755,0));
    auto* initialNs=VfsMountNamespaceInitial();MountNamespace* privateNs=nullptr;
    VfsPath privateRoot{},privateCwd{},bindTarget{},heldBound{},boundValue{},initialMissing{};
    assert(!VfsMountNamespaceCopy(initialNs,fsRoot,cwd,&privateNs,&privateRoot,&privateCwd));
    assert(VfsMountNamespaceActivate(privateNs));
    assert(!VfsPathResolve(privateRoot,privateRoot,"/namespace-bind",&bindTarget));
    assert(!VfsMountNamespaceBind(privateNs,privateCwd,bindTarget));VfsPathRelease(bindTarget);
    assert(!VfsPathResolve(privateRoot,privateRoot,"/namespace-bind",&heldBound));
    assert(!VfsPathResolve(privateRoot,heldBound,"value",&boundValue));
    assert(VfsPathResolve(fsRoot,fsRoot,"/namespace-bind/value",&initialMissing)==-2);
    VnodeStat boundStat{},valueStat{};assert(!VfsPathStat(boundValue,&boundStat)&&!VfsPathStat(value,&valueStat));
    assert(boundStat.ino==valueStat.ino && boundStat.dev==valueStat.dev);
    auto* ordinary=VfsPathOpen(boundValue,VFS_O_READ,&error);assert(ordinary&&!error);
    assert(VfsMountNamespaceRemount(privateNs,heldBound,7)==-16);VfsClose(ordinary);
    auto* metadataOnly=VfsPathOpen(boundValue,VFS_O_METADATA,&error);assert(metadataOnly&&!error);
    auto* sourceWriter=VfsPathOpen(value,VFS_O_WRITE,&error);assert(sourceWriter&&!error);
    uint64_t position=0;assert(VfsWrite(sourceWriter,"A",1,&position)==1);
    VnodeTime oldTimes[2]={{1,0},{2,0}};assert(!VfsSetTimes(sourceWriter,oldTimes));VfsClose(sourceWriter);
    assert(!VfsMountNamespaceRemount(privateNs,heldBound,7));assert(VfsMountReadOnly(metadataOnly));VfsClose(metadataOnly);
    auto* roReader=VfsPathOpen(boundValue,VFS_O_READ,&error);assert(roReader&&!error);
    char readByte=0;position=0;assert(VfsRead(roReader,&readByte,1,&position)==1&&readByte=='A');
    VnodeStat roStat{};assert(!VfsStat(roReader,&roStat)&&roStat.atime.sec==1); // no read-only relatime write
    VfsSpace roSpace{};assert(!VfsSpaceInfo(roReader,&roSpace)&&(roSpace.flags&7)==7);
    position=0;assert(VfsWrite(roReader,"B",1,&position)==-30&&VfsTruncate(roReader,0)==-30);
    assert(!VfsOpenAt(privateRoot,heldBound,"forbidden",VFS_O_CREATE|VFS_O_WRITE,0600,0,&error)&&error==-30);
    sourceWriter=VfsPathOpen(value,VFS_O_WRITE,&error);assert(sourceWriter&&!error&&!VfsMountReadOnly(sourceWriter));VfsClose(sourceWriter);
    VfsClose(roReader);
    auto* retainedBindNode=VfsPathOpen(boundValue,VFS_O_READ,&error);assert(retainedBindNode&&!error);
    VfsPathRelease(boundValue);assert(!VfsMountNamespaceDetach(privateNs,heldBound));
    assert(!VfsPathResolve(privateRoot,heldBound,"..",&resolved)&&VfsPathSame(resolved,heldBound));VfsPathRelease(resolved);
    assert(!VfsPathResolve(privateRoot,heldBound,"value",&boundValue));VfsPathRelease(boundValue);
    VfsMountNamespaceLeave(privateNs);VfsDrainMountCleanup();VfsMountNamespaceRelease(privateNs);
    VfsPathRelease(heldBound);VfsPathRelease(privateCwd);VfsPathRelease(privateRoot);
    VnodeStat retainedStat{};assert(!retainedBindNode->ops->stat(retainedBindNode,&retainedStat)&&retainedStat.ino==valueStat.ino);
    VfsClose(retainedBindNode);VfsDrainMountCleanup();
    assert(!VfsPathResolve(fsRoot,fsRoot,"/namespace-bind",&bindTarget));
    assert(!VfsMountNamespaceBind(initialNs,cwd,bindTarget));VfsPathRelease(bindTarget);
    assert(!VfsPathResolve(fsRoot,fsRoot,"/namespace-bind",&heldBound));
    auto* legacyBound=VfsOpen("/namespace-bind/value");assert(legacyBound);
    assert(!VfsStat(legacyBound,&retainedStat)&&retainedStat.ino==valueStat.ino);VfsClose(legacyBound);
    assert(!VfsMkdir("/namespace-bind/legacy-directory",0755));
    assert(!VfsRename("/namespace-bind/legacy-directory","/namespace-bind/legacy-renamed"));
    assert(!VfsStatAt(fsRoot,cwd,"legacy-renamed",&retainedStat)&&retainedStat.isDir);
    assert(!VfsSymlink("value","/namespace-bind/legacy-link"));
    char bindLink[32]{};assert(VfsReadlink("/namespace-bind/legacy-link",bindLink,sizeof(bindLink))==5);
    assert(!std::memcmp(bindLink,"value",5));legacyBound=VfsOpen("/namespace-bind/legacy-link");assert(legacyBound);
    assert(!VfsStat(legacyBound,&retainedStat)&&retainedStat.ino==valueStat.ino);VfsClose(legacyBound);
    assert(!VfsChmodPath("/namespace-bind/legacy-renamed",0700));
    assert(!VfsRemovePath("/namespace-bind/legacy-link",false));
    assert(!VfsRemovePath("/namespace-bind/legacy-renamed",true));
    assert(!VfsMountNamespaceDetach(initialNs,heldBound));VfsPathRelease(heldBound);VfsDrainMountCleanup();
    VfsMountNamespaceRelease(initialNs);
    assert(!VfsRemoveAt(fsRoot,fsRoot,"namespace-bind",true));
    {
        auto* fifo=VfsCreateFifoAt(fsRoot,cwd,"host-fifo",0640,0020,&error);assert(fifo && !error);
        VnodeStat stat{};assert(!VfsStat(fifo,&stat) && (stat.mode&0177777)==0010640 && fifo->type==VnodeType::Device);
        assert(!VfsCreateFifoAt(fsRoot,cwd,"host-fifo",0600,0,&error) && error==-17);
        auto* again=VfsOpenAt(fsRoot,cwd,"host-fifo",VFS_O_WRITE|VFS_O_READWRITE,0,0,&error);assert(again && !error);
        VfsPath a{},b{};assert(!VfsPathFromVnode(fifo,&a) && !VfsPathFromVnode(again,&b));assert(VfsPathSameObject(a,b));
        assert(!VfsRenameAt(fsRoot,cwd,"host-fifo",cwd,"host-fifo-moved"));
        assert(!VfsRemoveAt(fsRoot,cwd,"host-fifo-moved",false));assert(!VfsStat(again,&stat) && !stat.nlink);
        VfsPathRelease(a);VfsPathRelease(b);VfsClose(fifo);VfsClose(again);VfsDrainMountCleanup();
    }
    Vnode* exact=VfsOpenAt(fsRoot,cwd,"value",VFS_O_READ,0,0,&error);assert(exact&&!error);
    VfsPathRelease(value);VfsPathRelease(cwd);VfsPathRelease(fsRoot);assert(!VfsUnmount("/"));
    VfsClose(exact);assert(VfsUnmount("/"));assert(!g_ext2OpenInodes && allocations.empty() && frames.empty());
    for(unsigned i=1;i<4;++i){
        std::ofstream child(std::string(argv[1])+".child"+std::to_string(i),std::ios::binary|std::ios::trunc);
        child.write(reinterpret_cast<char*>(otherDisks[i].data()),otherDisks[i].size());assert(child);
    }
    std::ofstream output(argv[1],std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<char*>(disk.data()),disk.size());assert(output);output.close();
    std::puts("EXT2_OBJECTS_PASS production-backend rename/orphan/independent-opens/permission-modes/allocation-unwind/teardown");
}
