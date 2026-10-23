#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include "process.h"
#include "process_fs.h"
#include "mount_info_format.h"
#include "../../kernel/src/vfs.cpp"

namespace {
std::shared_mutex namespaceLock;
brook::VfsPath magicTarget{};
std::mutex allocationLock;
std::unordered_set<void*> allocations;
long failAllocation=-1;
unsigned schedulerLockDepth=0;
struct Mount;
struct Node { Mount* mount;uint64_t id;bool dir;Node* parent;std::map<std::string,Node*> children;std::string target;uint16_t mode=0;uint32_t nlink=1; };
struct Mount { uint64_t identity=0;bool alive=false;std::atomic<unsigned> live{0};unsigned closed=0;Node root{}; };
Mount mounts[8];
unsigned mountCalls[8]{},unmountCalls[8]{},syncCalls[8]{};
bool rejectRoot[8]{},rejectUnmount[8]{};
uint64_t rejectDirectoryReport=0;
bool rejectProvider=false;unsigned providerCalls=0;
unsigned deviceActivations=0;
extern const brook::VfsObjectOps objectOps;
Node& node(brook::VfsObject* object){return *static_cast<Node*>(object->priv);}
int makeObject(Node* source,brook::VfsObject** out){
    *out=nullptr;assert(source->mount->alive);
    auto* object=static_cast<brook::VfsObject*>(brook::kmalloc(sizeof(brook::VfsObject)));
    if(!object)return -12;
    ++source->mount->live;*object={&objectOps,source,source->id,1};*out=object;return 0;
}
void destroyObject(brook::VfsObject* object){auto& m=*node(object).mount;assert(m.alive && m.live);--m.live;brook::kfree(object);}
int objectStat(brook::VfsObject* object,brook::VnodeStat* stat){
    auto& n=node(object);assert(n.mount->alive);*stat={};stat->isDir=n.dir;stat->isSymlink=!n.target.empty();stat->ino=n.id;stat->dev=n.mount->identity;stat->mode=n.mode?n.mode:n.dir?0040755:0100644;stat->nlink=n.nlink;return 0;
}
int objectAccess(brook::VfsObject*,uint8_t,bool){return 0;}
int objectChild(brook::VfsObject* parent,const char* name,brook::VfsObject** out,bool=false){
    auto& n=node(parent);assert(n.mount->alive);*out=nullptr;auto it=n.children.find(name);return it==n.children.end()?-2:makeObject(it->second,out);
}
int objectParent(brook::VfsObject* object,brook::VfsObject** out,bool=false){if(!node(object).dir)return -20;return makeObject(node(object).parent,out);}
int objectReadlink(brook::VfsObject* object,char* out,uint32_t size){auto& t=node(object).target;if(t.size()>size)return -36;std::memcpy(out,t.data(),t.size());return t.size();}
void nodeClose(brook::Vnode* vn){assert(node(vn->backendObject).mount->alive);++node(vn->backendObject).mount->closed;}
int nodeStat(brook::Vnode* vn,brook::VnodeStat* out){return objectStat(vn->backendObject,out);}
int nodeTruncate(brook::Vnode*,uint64_t){return -5;}
int nodeAccess(brook::Vnode* vn,uint8_t required,bool realIds){return objectAccess(vn->backendObject,required,realIds);}
int nodeReaddir(brook::Vnode* vn,brook::DirEntry* out,uint32_t* cookie) {
    auto& directory=node(vn->backendObject);assert(directory.dir);
    auto it=directory.children.begin();for(uint32_t i=0;i<*cookie && it!=directory.children.end();++i)++it;
    if(it==directory.children.end())return 0;
    *out={};std::strncpy(out->name,it->first.c_str(),sizeof(out->name)-1);out->isDir=it->second->dir;++*cookie;return 1;
}
const brook::VnodeOps nodeOps={.open=nullptr,.read=nullptr,.write=nullptr,.readdir=nodeReaddir,.close=nodeClose,.stat=nodeStat,.fsync=nullptr,.truncate=nodeTruncate,.access=nodeAccess};
bool omitMetadataReference=false;
brook::Vnode* objectOpen(brook::VfsObject* object,int flags,int* error){
    if((node(object).mode&0170000)==0020000 && !(flags&brook::VFS_O_METADATA))++deviceActivations;
    *error=0;auto* vn=static_cast<brook::Vnode*>(brook::kmalloc(sizeof(brook::Vnode)));
    if(!vn){*error=-12;return nullptr;}
    *vn={};vn->ops=&nodeOps;vn->type=node(object).dir?brook::VnodeType::Dir:brook::VnodeType::File;vn->refCount=1;
    if(!omitMetadataReference || !(flags&brook::VFS_O_METADATA)){vn->backendObject=object;brook::VfsObjectRetain(object);}return vn;
}
int objectFollow(const brook::VfsPath& path,brook::VfsPath* out) {
    auto* object=path.object;
    *out={};if(node(object).target!="magic-reporting-only")return 0;
    *out=magicTarget;brook::VfsPathRetain(*out);return 1;
}
int objectDirectoryPath(brook::VfsObject* object,char* out,uint32_t capacity) {
    if(node(object).id==rejectDirectoryReport)return -95;
    auto* current=&node(object);std::string text;
    for(unsigned depth=0;current!=&current->mount->root;++depth) {
        if(depth>=256)return -40;
        std::string name;for(const auto& child:current->parent->children)
            if(child.second==current) {name=child.first;break;}
        if(name.empty())return -2;
        text="/"+name+text;current=current->parent;
    }
    if(text.empty())text="/";
    if(text.size()>=capacity)return -36;
    std::memcpy(out,text.c_str(),text.size()+1);return 0;
}
int objectRename(brook::VfsObject* oldParent,const char* oldName,brook::VfsObject* newParent,const char* newName) {
    auto& old=node(oldParent);auto& next=node(newParent);
    auto found=old.children.find(oldName);if(found==old.children.end())return -2;
    auto* moved=found->second;old.children.erase(found);next.children[newName]=moved;moved->parent=&next;return 0;
}
int objectRemove(brook::VfsObject* parent,const char* name,bool directory) {
    auto& p=node(parent);auto i=p.children.find(name);if(i==p.children.end())return -2;
    auto* child=i->second;if(child->dir!=directory)return directory?-20:-21;
    if(directory&&!child->children.empty())return -39;
    p.children.erase(i);child->nlink=0;return 0;
}
const brook::VfsObjectOps objectOps={.destroy=destroyObject,.stat=objectStat,.access=objectAccess,
    .lookup=objectChild,.parent=objectParent,.readlink=objectReadlink,.open=objectOpen,.remove=objectRemove,.rename=objectRename,.directory_path=objectDirectoryPath,.follow_link=objectFollow};
bool mountBackend(uint8_t drive,void** out){assert(drive<8 && !mounts[drive].alive);auto& m=mounts[drive];++mountCalls[drive];m.alive=true;m.identity=drive+1;m.root={&m,1,true,&m.root,{}, {}};*out=&m;return true;}
int unmountBackend(void* priv){auto* m=static_cast<Mount*>(priv);assert(m->alive && !m->live);auto index=m-mounts;++unmountCalls[index];if(rejectUnmount[index])return -5;m->alive=false;return 0;}
unsigned freshUnmounts=0,freshUnmountAttempts=0;
bool freshRejectUnmount=false;
int freshSync(void* priv){assert(static_cast<Mount*>(priv)->alive);return 0;}
bool freshRejectRoot=false,freshFailAfterRoot=false;
int freshUnmount(void* priv){auto* m=static_cast<Mount*>(priv);assert(m->alive && !m->live);++freshUnmountAttempts;if(freshRejectUnmount)return -5;m->alive=false;++freshUnmounts;return 0;}
int freshRoot(void* priv,uint8_t,brook::VfsObject** out){*out=nullptr;if(freshRejectRoot)return -5;int error=makeObject(&static_cast<Mount*>(priv)->root,out);return !error && freshFailAfterRoot?-5:error;}
int rootObject(void* priv,uint8_t drive,brook::VfsObject** out){if(rejectRoot[drive]){*out=nullptr;return -5;}return makeObject(&static_cast<Mount*>(priv)->root,out);}
int vnodeObject(brook::Vnode* vn,brook::VfsObject** out){*out=vn->backendObject;brook::VfsObjectRetain(*out);return 0;}
Node* legacyLookup(Mount* mount,const char* path){
    Node* current=&mount->root;std::string text(path);std::size_t start=0;
    while(start<text.size()){
        if(text[start]=='/'){++start;continue;}
        auto end=text.find('/',start);if(end==std::string::npos)end=text.size();
        auto found=current->children.find(text.substr(start,end-start));
        if(found==current->children.end())return nullptr;
        current=found->second;start=end;
    }
    return current;
}
int legacyStat(void* priv,uint8_t,const char* path,brook::VnodeStat* stat){
    Node* n=legacyLookup(static_cast<Mount*>(priv),path);if(!n)return -2;
    brook::VfsObject borrowed{&objectOps,n,n->id,1};return objectStat(&borrowed,stat);
}
brook::Vnode* legacyOpen(void* priv,uint8_t,const char* path,int flags){
    Node* n=legacyLookup(static_cast<Mount*>(priv),path);if(!n)return nullptr;
    brook::VfsObject* object=nullptr;if(makeObject(n,&object))return nullptr;
    int error=0;auto* opened=objectOpen(object,flags,&error);brook::VfsObjectRelease(object);return opened;
}
int legacyRename(void* priv,uint8_t,const char* oldPath,const char* newPath){
    auto* mount=static_cast<Mount*>(priv);
    std::string oldText(oldPath),newText(newPath);
    auto oldSlash=oldText.rfind('/'),newSlash=newText.rfind('/');
    auto* oldParent=legacyLookup(mount,oldSlash==std::string::npos?"/":oldText.substr(0,oldSlash).c_str());
    auto* newParent=legacyLookup(mount,newSlash==std::string::npos?"/":newText.substr(0,newSlash).c_str());
    if(!oldParent || !newParent)return -2;
    brook::VfsObject oldObject{&objectOps,oldParent,oldParent->id,1},newObject{&objectOps,newParent,newParent->id,1};
    return objectRename(&oldObject,oldText.substr(oldSlash==std::string::npos?0:oldSlash+1).c_str(),
                        &newObject,newText.substr(newSlash==std::string::npos?0:newSlash+1).c_str());
}
int syncBackend(void* priv){auto* m=static_cast<Mount*>(priv);assert(m->alive);++syncCalls[m-mounts];return 0;}
const brook::VfsFsOps fsOps={.mount=mountBackend,.unmount=unmountBackend,.open=legacyOpen,.stat_path=legacyStat,.lstat_path=legacyStat,.unlink=nullptr,.mkdir=nullptr,.rename=legacyRename,.symlink=nullptr,.readlink=nullptr,.sync=syncBackend,.root_object=rootObject,.vnode_object=vnodeObject};
const brook::VfsFsOps freshOps={.mount=mountBackend,.unmount=freshUnmount,.open=legacyOpen,.stat_path=legacyStat,.lstat_path=legacyStat,.unlink=nullptr,.mkdir=nullptr,.rename=legacyRename,.symlink=nullptr,.readlink=nullptr,.sync=freshSync,.root_object=freshRoot,.vnode_object=vnodeObject};
const brook::VfsFsOps unsupportedOps={.mount=mountBackend,.unmount=unmountBackend,.open=nullptr,.stat_path=nullptr,.lstat_path=nullptr,.unlink=nullptr,.mkdir=nullptr,.rename=nullptr,.symlink=nullptr,.readlink=nullptr,.sync=nullptr};
brook::VfsPath resolve(const brook::VfsPath& root,const brook::VfsPath& start,const char* text){brook::VfsPath out{};int error=brook::VfsPathResolve(root,start,text,&out);if(error)std::fprintf(stderr,"resolve %s: %d\n",text,error);assert(!error);return out;}
void expect(const brook::VfsPath& root,const brook::VfsPath& start,const char* text,const brook::VfsPath& expected){auto p=resolve(root,start,text);assert(brook::VfsPathSame(p,expected));brook::VfsPathRelease(p);}
std::map<std::string,unsigned> directoryEntries(const brook::VfsPath& path) {
    int error=0;auto* vn=brook::VfsPathOpen(path,brook::VFS_O_READ,&error);assert(vn&&!error);
    std::map<std::string,unsigned> result;uint32_t cookie=0;brook::DirEntry entry{};int status;
    while((status=brook::VfsReaddir(vn,&entry,&cookie))>0) {assert(cookie<1000);++result[entry.name];}
    assert(!status);brook::VfsClose(vn);return result;
}
}
namespace brook {
bool ModuleRetainAddresses(const uintptr_t* addresses,uint32_t count){assert(addresses&&count);++providerCalls;return !rejectProvider;}
extern "C" void* kmalloc(uint64_t size){assert(!schedulerLockDepth);if(failAllocation==0){failAllocation=-1;return nullptr;}if(failAllocation>0)--failAllocation;void* p=std::malloc(size);if(p){std::memset(p,0xcc,size);std::lock_guard<std::mutex> lock(allocationLock);allocations.insert(p);}return p;}
extern "C" void kfree(void* p){assert(!schedulerLockDepth);if(p){std::lock_guard<std::mutex> lock(allocationLock);assert(allocations.erase(p)==1);std::free(p);}}
void WakeVfsCleanupWorker(){assert(!schedulerLockDepth);}
void KRwLockInit(KRwLock* lock){*lock={};}
void KRwLockReadLock(KRwLock*){namespaceLock.lock_shared();}
void KRwLockReadUnlock(KRwLock*){namespaceLock.unlock_shared();}
void KRwLockWriteLock(KRwLock*){namespaceLock.lock();}
void KRwLockWriteUnlock(KRwLock*){namespaceLock.unlock();}
extern "C" void SerialPrintf(const char*,...){}
void SerialPuts(const char*){}
int FileObjectWritebackAll(){return 0;}
int FileObjectWrite(FileObject*,Vnode*,const void*,uint64_t,uint64_t*){assert(false);return -5;}
int FileObjectAppend(FileObject*,Vnode*,const void*,uint64_t,uint64_t*){assert(false);return -5;}
int FileObjectWriteback(FileObject*,Vnode*,uint64_t,uint64_t){assert(false);return -5;}
int FileObjectTruncateUnmapped(FileObject*,Vnode*,uint64_t){assert(false);return -5;}
int FileObjectGrowMapped(FileObject*,Vnode*,uint64_t){assert(false);return -5;}
}
#include "scheduler_membership_fixture.inc"
// Actual Process publication helpers and FsContext core. These exercise owned
// context handoff/failure/races; native acceptance covers actual clone/exec.
static void testFreshMounts(brook::MountNamespace* ns,const brook::VfsPath& root,
    const brook::VfsPath& target,const brook::VfsPath& second) {
    using namespace brook;
    assert(VfsRegisterFs("fresh",&freshOps));
    const auto before=allocations.size();const auto records=g_mounts.count;
    Mount first{},other{},rollback{};
    auto prepare=[](Mount& m,uint64_t id) {m.alive=true;m.identity=id;m.root={&m,1,true,&m.root,{},{}};};
    prepare(first,1001);prepare(other,1002);prepare(rollback,1003);
    assert(VfsMountNamespaceMountFresh(ns,root,"fresh",&freshOps,&rollback)==-95);
    assert(VfsMountNamespaceMountFresh(ns,target,"missing",&freshOps,&rollback)==-95);
    assert(VfsMountNamespaceMountFresh(ns,target,"objects",&freshOps,&rollback)==-95);
    freshRejectRoot=true;
    assert(VfsMountNamespaceMountFresh(ns,target,"fresh",&freshOps,&rollback)==-5);
    freshRejectRoot=false;assert(rollback.alive && !rollback.live && !freshUnmounts);
    freshFailAfterRoot=true;
    assert(VfsMountNamespaceMountFresh(ns,target,"fresh",&freshOps,&rollback)==-5);
    freshFailAfterRoot=false;assert(rollback.alive && !rollback.live && !freshUnmounts);
    rollback.root.dir=false;
    assert(VfsMountNamespaceMountFresh(ns,target,"fresh",&freshOps,&rollback)==-20);
    rollback.root.dir=true;assert(rollback.alive && !rollback.live && !freshUnmounts);
    bool published=false;
    for(long failure=0;failure<64;++failure) {
        failAllocation=failure;
        int result=VfsMountNamespaceMountFresh(ns,target,"fresh",&freshOps,&rollback);
        failAllocation=-1;
        if(!result) {
            auto path=resolve(root,root,"/ns-target");
            assert(!VfsMountNamespaceDetach(ns,path));VfsPathRelease(path);VfsDrainMountCleanup();
            assert(!rollback.alive);published=true;
        } else {assert(result==-12 && rollback.alive && !rollback.live);}
        assert(allocations.size()==before && g_mounts.count==records);
        if(published)break;
    }
    assert(published && freshUnmounts==1);
    assert(!VfsMountNamespaceMountFresh(ns,target,"fresh",&freshOps,&first,0));
    assert(!VfsMountNamespaceMountFresh(ns,second,"fresh",&freshOps,&other,0));
    assert(!VfsUnmount("/ns-target")); // retained fresh root requires namespace detach
    auto held=resolve(root,root,"/ns-target"),otherHeld=resolve(root,root,"/ns-a");
    VnodeStat a{},b{};assert(!VfsPathStat(held,&a) && !VfsPathStat(otherHeld,&b));
    assert(a.dev==1001 && b.dev==1002 && a.ino==b.ino);
    // The old driver/device API must not discover/coalesce a fresh instance.
    auto oldMountCalls=mountCalls[0];
    assert(VfsMount("/fresh-legacy","fresh",0));assert(mountCalls[0]==oldMountCalls+1);
    assert(VfsUnmount("/fresh-legacy"));assert(freshUnmounts==2);
    int openError=0;auto* vn=VfsPathOpen(held,VFS_O_METADATA,&openError);
    assert(vn);
    MountNamespace* copy=nullptr;VfsPath copyRoot{},copyCwd{};
    assert(!VfsMountNamespaceCopy(ns,root,root,&copy,&copyRoot,&copyCwd));
    assert(VfsMountNamespaceMountFresh(copy,copyRoot,"fresh",&freshOps,&rollback)==-3);
    assert(VfsMountNamespaceActivate(copy));
    assert(VfsMountNamespaceMountFresh(copy,target,"fresh",&freshOps,&rollback)==-22);
    auto copied=resolve(copyRoot,copyRoot,"/ns-target");
    VnodeStat c{};assert(!VfsPathStat(copied,&c) && c.dev==a.dev && c.ino==a.ino);
    assert(!VfsMountNamespaceDetach(ns,held));assert(!VfsMountNamespaceDetach(ns,otherHeld));
    VfsDrainMountCleanup();assert(first.alive && other.alive && freshUnmounts==2);
    assert(!VfsPathStat(copied,&c) && c.dev==1001);
    VfsPathRelease(copied);VfsMountNamespaceLeave(copy);VfsDrainMountCleanup();
    VfsPathRelease(copyRoot);VfsPathRelease(copyCwd);VfsMountNamespaceRelease(copy);VfsDrainMountCleanup();
    assert(first.alive && other.alive && freshUnmounts==2);
    VfsPathRelease(otherHeld);VfsDrainMountCleanup();assert(!other.alive && freshUnmounts==3);
    VfsPathRelease(held);VfsDrainMountCleanup();assert(first.alive);
    VfsClose(vn);VfsDrainMountCleanup();assert(!first.alive && freshUnmounts==4);
    assert(allocations.size()==before && g_mounts.count==records);
    Mount retirement{};prepare(retirement,1004);
    assert(!VfsMountNamespaceMountFresh(ns,target,"fresh",&freshOps,&retirement));
    auto retiring=resolve(root,root,"/ns-target");
    assert(!VfsMountNamespaceDetach(ns,retiring));VfsPathRelease(retiring);
    auto attempts=freshUnmountAttempts;freshRejectUnmount=true;VfsDrainMountCleanup();
    assert(retirement.alive && freshUnmountAttempts==attempts+1 && freshUnmounts==4);
    bool pending=false;
    for(auto* backing=g_backings;backing;backing=backing->next)
        if(backing->mountPriv==&retirement) {assert(backing->independent && !backing->references);pending=true;}
    assert(pending && VfsSync(nullptr)==-5 && freshUnmountAttempts==attempts+2);
    freshRejectUnmount=false;assert(!VfsSync(nullptr));
    assert(!retirement.alive && freshUnmounts==5 && freshUnmountAttempts==attempts+3);
    assert(allocations.size()==before && g_mounts.count==records);
}
static void testProcessContexts(const brook::VfsPath& root,const brook::VfsPath& cwd) {
    using namespace brook;
    Process parent{},copy{},shared{},privateThread{};
    int error=0;auto* context=FsContextCreate(root,cwd,022,&error);assert(context&&!error);
    assert(!ProcessFsInitialize(&parent,"/"));
    ProcessFsReplaceOwned(&parent,context);
    scheduler_membership_fixture::test(parent);
    assert(!ProcessFsInherit(&copy,&parent,false));
    assert(!ProcessFsInherit(&shared,&parent,true));
    assert(!ProcessFsInherit(&privateThread,&parent,false));
    assert(ProcessFsExchangeMask(&shared,077)==022);
    assert(ProcessFsMask(&parent)==077 && ProcessFsMask(&copy)==022 && ProcessFsMask(&privateThread)==022);
    assert(!ProcessFsSetCwd(&shared,root));
    FsSnapshot snapshot{};assert(!ProcessFsSnapshot(&parent,&snapshot));
    assert(VfsPathSame(snapshot.cwd,root));FsSnapshotRelease(&snapshot);
    assert(!ProcessFsSnapshot(&copy,&snapshot));assert(VfsPathSame(snapshot.cwd,cwd));FsSnapshotRelease(&snapshot);
    // Failed independent copy cannot publish a borrowed reference or mutate
    // the source. Subsequent unpublished-child cleanup is safe.
    Process failed{};failAllocation=0;assert(ProcessFsInherit(&failed,&parent,false)==-12);failAllocation=-1;
    assert(!failed.fsContext && ProcessFsMask(&parent)==077);ProcessFsClear(&failed);
    // Exercise explicit context replacement, including preparation OOM.
    failAllocation=0;assert(!ProcessFsCopy(&parent,&error)&&error==-12);failAllocation=-1;
    auto* prepared=ProcessFsCopy(&parent,&error);assert(prepared&&!error);
    ProcessFsReplaceOwned(&parent,prepared);
    assert(ProcessFsExchangeMask(&parent,027)==077 && ProcessFsMask(&shared)==077);
    std::atomic<bool> start{false};
    std::thread reader([&] {
        while(!start.load(std::memory_order_acquire))std::this_thread::yield();
        for(unsigned i=0;i<4000;++i) {
            auto* held=ProcessFsAcquire(&parent);assert(held);
            FsSnapshot fs{};assert(!FsContextSnapshot(held,&fs));
            assert(VfsPathSame(fs.root,root));
            FsSnapshotRelease(&fs);FsContextRelease(held);
        }
    });
    start.store(true,std::memory_order_release);
    for(unsigned i=0;i<1000;++i) {
        auto* replacement=ProcessFsCopy(&parent,&error);assert(replacement&&!error);
        ProcessFsReplaceOwned(&parent,replacement);
    }
    reader.join();
    // Real process-context integration: failure never replaces inherited
    // state; copied/shared contexts own storage while each task owns a member.
    Process isolated{},sibling{};assert(!ProcessFsInherit(&isolated,&copy,false));
    const auto originalMember=isolated.mountNamespaceMember;
    bool isolatedSuccessfully=false;
    for(long failure=0;failure<128;++failure) {
        const auto before=allocations.size();const auto records=g_mounts.count;
        failAllocation=failure;int result=ProcessFsIsolateMounts(&isolated);failAllocation=-1;
        if(!result) {isolatedSuccessfully=true;break;}
        assert(result==-12 && isolated.mountNamespaceMember==originalMember);
        VfsDrainMountCleanup();assert(allocations.size()==before && records==g_mounts.count);
    }
    assert(isolatedSuccessfully && isolated.mountNamespaceMember!=originalMember);
    FsSnapshot isolatedFs{};assert(!ProcessFsSnapshot(&isolated,&isolatedFs));
    assert(isolatedFs.mountNamespace==isolated.mountNamespaceMember && isolatedFs.root.attachment!=root.attachment);
    assert(isolatedFs.mask==022 && !ProcessFsInherit(&sibling,&isolated,true));
    auto isolatedTarget=resolve(isolatedFs.root,isolatedFs.root,"/namespace-target");
    assert(!VfsMountNamespaceBind(isolatedFs.mountNamespace,isolatedFs.cwd,isolatedTarget));VfsPathRelease(isolatedTarget);
    auto heldBind=resolve(isolatedFs.root,isolatedFs.root,"/namespace-target");
    auto* retired=ProcessFsRetireMounts(&isolated);assert(retired && !isolated.mountNamespaceMember);
    assert(!ProcessFsRetireMounts(&isolated));ProcessFsFinishMountRetirement(retired);VfsDrainMountCleanup();
    auto stillBound=resolve(isolatedFs.root,isolatedFs.root,"/namespace-target");assert(VfsPathSame(stillBound,heldBind));VfsPathRelease(stillBound);
    ProcessFsClear(&sibling); // last member: publication ends despite snapshot/context holds
    auto uncovered=resolve(isolatedFs.root,isolatedFs.root,"/namespace-target");
    assert(!VfsPathSame(uncovered,heldBind));VfsPathRelease(uncovered);
    Process refused{};assert(ProcessFsInherit(&refused,&isolated,false)==-3 && !refused.fsContext && !refused.mountNamespaceMember);
    VfsDrainMountCleanup();expect(isolatedFs.root,heldBind,"..",heldBind);
    ProcessFsClear(&refused);ProcessFsClear(&isolated);
    VfsPathRelease(heldBind);FsSnapshotRelease(&isolatedFs);VfsDrainMountCleanup();
    ProcessFsClear(&parent);ProcessFsClear(&copy);ProcessFsClear(&shared);ProcessFsClear(&privateThread);
    assert(!parent.fsContext && !copy.fsContext && !shared.fsContext && !privateThread.fsContext);
}
static std::vector<brook::VfsMountInfo> infoRows(brook::MountNamespace* ns,const brook::VfsPath& root) {
    brook::VfsMountInfo* rows=nullptr;uint32_t count=0;
    int error=-11;
    for(unsigned attempt=0;error==-11 && attempt<64;++attempt) {error=brook::VfsMountNamespaceInfo(ns,root,&rows,&count);if(error==-11)std::this_thread::yield();}
    assert(!error);
    std::vector<brook::VfsMountInfo> result(rows,rows+count);brook::VfsMountInfoFree(rows);
    assert(!result.empty()&&result.front().parent==0);
    for(size_t i=0;i<result.size();++i) {
        assert(result[i].id && result[i].id<=0x7ffffffe);
        if(!result[i].parent)continue;
        bool found=false;for(size_t j=0;j<i;++j)found|=result[j].id==result[i].parent;
        assert(found);
    }
    return result;
}
static void testMountInfo(brook::MountNamespace* initial,const brook::VfsPath& stableRoot) {
    using namespace brook;
    auto before=allocations.size();auto records=g_mounts.count;
    const char* sourceName="info source\t\\\n";const char* targetName="info target\t\\\n";
    Node sourceNode{&mounts[6],610,true,&mounts[6].root,{},{}},targetNode{&mounts[6],611,true,&mounts[6].root,{},{}};
    mounts[6].root.children[sourceName]=&sourceNode;mounts[6].root.children[targetName]=&targetNode;
    MountNamespace* ns=nullptr;VfsPath root{},cwd{};assert(!VfsMountNamespaceCopy(initial,stableRoot,stableRoot,&ns,&root,&cwd));assert(VfsMountNamespaceActivate(ns));
    auto source=resolve(root,root,sourceName),target=resolve(root,root,targetName),b=resolve(root,root,"ns-b");
    assert(!VfsMountNamespaceBind(ns,source,target));auto lower=resolve(root,root,targetName);
    // Reusing a covered target fd must stack on the existing top attachment.
    assert(!VfsMountNamespaceBind(ns,b,target));auto top=resolve(root,root,targetName);
    auto* lowerMount=static_cast<MountEntry*>(lower.attachment);auto* topMount=static_cast<MountEntry*>(top.attachment);
    assert(topMount->covered.attachment==lowerMount);
    assert(!VfsRenameAt(root,root,sourceName,root,"info renamed\t\\\n"));
    auto rows=infoRows(ns,root);bool foundLower=false,foundTop=false;
    for(const auto& row:rows) {
        if(row.id==lowerMount->publication) {foundLower=true;assert(!std::strcmp(row.root,"/info renamed\t\\\n"));assert(row.device==7);}
        if(row.id==topMount->publication) {foundTop=true;assert(row.parent==lowerMount->publication&&!std::strcmp(row.root,"/ns-b"));}
    }
    assert(foundLower&&foundTop);
    std::vector<char> text(rows.size()*2600+1);uint32_t written=0;
    assert(!FormatMountInfo(rows.data(),rows.size(),text.data(),text.size(),&written)&&written);
    assert(std::strstr(text.data(),"\\040")&&std::strstr(text.data(),"\\011")&&std::strstr(text.data(),"\\012")&&std::strstr(text.data(),"\\134"));
    std::printf("MOUNTINFO_FIXTURE_BEGIN\n%sMOUNTINFO_FIXTURE_END\n",text.data());
    char small[8];assert(FormatMountInfo(rows.data(),rows.size(),small,sizeof(small),&written)==-75&&!written);
    bool complete=false;
    for(long failure=0;failure<8;++failure) {
        auto allocated=allocations.size();VfsMountInfo* failed=nullptr;uint32_t count=99;
        failAllocation=failure;int error=VfsMountNamespaceInfo(ns,root,&failed,&count);failAllocation=-1;
        if(!error) {VfsMountInfoFree(failed);complete=true;}else assert(error==-12&&!failed&&!count);
        assert(allocations.size()==allocated);if(complete)break;
    }
    assert(complete);
    uint64_t saved=topMount->publication;
    {FileMutationGuard transaction;MountViewGuard view;topMount->publication=0x7fffffffULL;}
    VfsMountInfo* failed=nullptr;uint32_t count=0;assert(VfsMountNamespaceInfo(ns,root,&failed,&count)==-75&&!failed&&!count);
    {FileMutationGuard transaction;MountViewGuard view;topMount->publication=saved;}
    assert(!VfsRenameAt(root,root,"info renamed\t\\\n",root,sourceName));
    assert(!VfsMountNamespaceDetach(ns,top));assert(!VfsMountNamespaceDetach(ns,lower));
    Node childNode{&mounts[6],612,true,&sourceNode,{},{}};sourceNode.children["child"]=&childNode;
    std::atomic<bool> done{false};std::atomic<unsigned> snapshots{0};
    std::thread observer([&] {
        while(!done.load(std::memory_order_acquire)) {auto copy=infoRows(ns,root);assert(!copy.empty());++snapshots;}
    });
    while(!snapshots.load())std::this_thread::yield();
    for(unsigned i=0;i<100;++i) {
        assert(!VfsMountNamespaceBind(ns,source,target));
        auto parent=resolve(root,root,targetName);
        auto child=resolve(root,parent,"child");
        assert(!VfsMountNamespaceBind(ns,b,child));
        assert(!VfsMountNamespaceDetach(ns,parent));
        VfsPathRelease(child);VfsPathRelease(parent);
    }
    done.store(true,std::memory_order_release);observer.join();
    Mount fresh{};fresh.alive=true;fresh.identity=1100;fresh.root={&fresh,1,true,&fresh.root,{},{}};
    Node inside{&fresh,2,true,&fresh.root,{},{}},destination{&fresh,3,true,&fresh.root,{},{}};
    fresh.root.children={{"src",&inside},{"dst",&destination}};
    assert(!VfsMountNamespaceMountFresh(ns,target,"fresh",&freshOps,&fresh));
    auto freshRoot=resolve(root,root,targetName);
    auto insidePath=resolve(root,freshRoot,"src"),destinationPath=resolve(root,freshRoot,"dst");
    assert(!VfsMountNamespaceBind(ns,insidePath,destinationPath));
    // A legacy case-insensitive ancestor can report another spelling of the
    // same covered identity. An unrelated backing rename must preserve the
    // published fresh mount's lowercase anchor.
    mounts[6].root.children["INFO target\t\\\n"]=&targetNode;
    assert(!VfsRenameAt(root,freshRoot,"src",freshRoot,"renamed"));
    rows=infoRows(ns,root);bool anchored=false;
    for(const auto& row:rows)if(!std::strcmp(row.root,"/renamed")) {
        anchored=true;assert(std::string(row.mountPoint)==std::string("/")+targetName+"/dst");
    }
    assert(anchored);mounts[6].root.children.erase("INFO target\t\\\n");
    VfsPathRelease(insidePath);VfsPathRelease(destinationPath);
    assert(!VfsMountNamespaceDetach(ns,freshRoot));VfsPathRelease(freshRoot);
    VfsPathRelease(top);VfsPathRelease(lower);VfsPathRelease(source);VfsPathRelease(target);VfsPathRelease(b);
    VfsMountNamespaceLeave(ns);VfsDrainMountCleanup();VfsPathRelease(root);VfsPathRelease(cwd);VfsMountNamespaceRelease(ns);VfsDrainMountCleanup();
    mounts[6].root.children.erase(sourceName);mounts[6].root.children.erase(targetName);
    assert(allocations.size()==before&&g_mounts.count==records);
}
static void testMountRestrictions(brook::MountNamespace* initial,const brook::VfsPath& stableRoot) {
    using namespace brook;
    auto allocated=allocations.size();auto records=g_mounts.count;
    MountNamespace* ns=nullptr;VfsPath root{},cwd{};
    assert(!VfsMountNamespaceCopy(initial,stableRoot,stableRoot,&ns,&root,&cwd));assert(VfsMountNamespaceActivate(ns));
    auto source=resolve(root,root,"ns-a"),target=resolve(root,root,"ns-b"),second=resolve(root,root,"ns-target");
    Node device{&mounts[6],700,false,&node(source.object),{},{},0020666};node(source.object).children["device"]=&device;
    assert(!VfsMountNamespaceBind(ns,source,target));auto bind=resolve(root,root,"ns-b"),dev=resolve(root,bind,"device");
    int error=0;auto* held=VfsPathOpen(dev,VFS_O_READ,&error);assert(held&&!error);
    auto activations=deviceActivations;
    assert(VfsMountNamespaceRemount(initial,stableRoot,6)==-95);
    for(uint64_t attributes:{0ULL,14ULL,2097158ULL})assert(VfsMountNamespaceRemount(ns,bind,attributes)==-22);
    assert(static_cast<MountEntry*>(bind.attachment)->attributes==0);
    assert(VfsMountNamespaceRemount(ns,dev,6)==-22);
    assert(!VfsMountNamespaceRemount(ns,bind,2));
    auto* deviceAllowed=VfsPathOpen(dev,VFS_O_READ,&error);assert(deviceAllowed&&!error);VfsRelease(deviceAllowed);activations=deviceActivations;
    assert(!VfsMountNamespaceRemount(ns,bind,6));assert(VfsMountNamespaceRemount(ns,bind,2)==-1);
    assert(!VfsPathOpen(dev,VFS_O_READ,&error)&&error==-13&&deviceActivations==activations);
    auto* metadata=VfsPathOpen(dev,VFS_O_METADATA,&error);assert(metadata&&!error&&deviceActivations==activations);VfsRelease(metadata);
    VnodeStat stat{};assert(!VfsStat(held,&stat)&&stat.mode==0020666);VfsRelease(held);
    auto original=resolve(root,source,"device");held=VfsPathOpen(original,VFS_O_READ,&error);assert(held&&!error);VfsRelease(held);VfsPathRelease(original);
    assert(!VfsMountNamespaceBind(ns,bind,second));auto alias=resolve(root,root,"ns-target/device");assert(!VfsPathOpen(alias,VFS_O_READ,&error)&&error==-13);VfsPathRelease(alias);
    assert(!VfsMountNamespaceRemount(ns,root,6));
    assert(VfsPathDeviceActivation(root,"/dev/null")==-13);
    // An independent child mount has its own flags, rather than inheriting
    // NODEV from the namespace root. Generic broker eligibility is unchanged.
    assert(!VfsPathDeviceActivation(root,"/synthetic/deep/mount/missing"));
    bool allowed=false;assert(!VfsPathBrokerAllowed(root,"/dev/shm/test",&allowed)&&allowed);
    MountNamespace* copied=nullptr;VfsPath copyRoot{},copyCwd{};
    assert(!VfsMountNamespaceCopy(ns,root,root,&copied,&copyRoot,&copyCwd));assert(VfsMountNamespaceActivate(copied));
    auto copiedDev=resolve(copyRoot,copyRoot,"ns-b/device");assert(!VfsPathOpen(copiedDev,VFS_O_READ,&error)&&error==-13);
    VfsPathRelease(copiedDev);VfsMountNamespaceLeave(copied);VfsPathRelease(copyRoot);VfsPathRelease(copyCwd);VfsMountNamespaceRelease(copied);VfsDrainMountCleanup();
    // Legacy initial-namespace opens use the same pre-activation check.
    auto loose=resolve(stableRoot,stableRoot,"loose-file");node(loose.object).mode=0020666;
    auto* initialMount=static_cast<MountEntry*>(stableRoot.attachment);
    {FileMutationGuard transaction;MountViewGuard view;initialMount->attributes=6;}
    assert(!VfsOpenModeChecked("/loose-file",VFS_O_READ,0666,&error)&&error==-13);
    Vnode* executable=nullptr;char name[512];assert(VfsOpenExecutable("/loose-file",&executable,name,sizeof(name))==-13&&!executable);
    {FileMutationGuard transaction;MountViewGuard view;initialMount->attributes=0;}
    node(loose.object).mode=0;VfsPathRelease(loose);
    VfsPathRelease(bind);VfsPathRelease(dev);VfsPathRelease(source);VfsPathRelease(target);VfsPathRelease(second);
    VfsMountNamespaceLeave(ns);VfsPathRelease(root);VfsPathRelease(cwd);VfsMountNamespaceRelease(ns);VfsDrainMountCleanup();
    mounts[6].root.children["ns-a"]->children.erase("device");
    assert(allocations.size()==allocated&&g_mounts.count==records);
}
static void testReadOnlyMount(brook::MountNamespace* initial,const brook::VfsPath& stableRoot) {
    using namespace brook;
    auto allocated=allocations.size();auto records=g_mounts.count;
    MountNamespace* ns=nullptr;VfsPath root{},cwd{};
    assert(!VfsMountNamespaceCopy(initial,stableRoot,stableRoot,&ns,&root,&cwd));assert(VfsMountNamespaceActivate(ns));
    auto source=resolve(root,root,"ns-a"),target=resolve(root,root,"ns-b");
    Node file{&mounts[6],701,false,&node(source.object),{},{}};node(source.object).children["ro-file"]=&file;
    assert(VfsMountNamespaceRemount(ns,root,7)==-95);assert(VfsMountAttributes(static_cast<MountEntry*>(root.attachment))==0);
    assert(!VfsMountNamespaceBind(ns,source,target));auto bind=resolve(root,root,"ns-b"),path=resolve(root,bind,"ro-file");
    assert(!VfsMountNamespaceRemount(ns,bind,2));
    int error=0;auto* metadata=VfsPathOpen(path,VFS_O_METADATA,&error);assert(metadata&&!error&&!metadata->mountDataHold);
    auto* directory=VfsPathOpen(bind,VFS_O_READ,&error);assert(directory&&!error&&!directory->mountDataHold);
    auto* reader=VfsPathOpen(path,VFS_O_READ,&error);assert(reader&&!error&&reader->mountDataHold);
    assert(VfsMountNamespaceRemount(ns,bind,3)==-16);
    // A mapping/writeback owner retaining the vnode must keep admission busy
    // after the descriptor owner retires; final close releases the data hold.
    VfsRetain(reader);VfsRelease(reader);assert(VfsMountNamespaceRemount(ns,bind,3)==-16);VfsRelease(reader);
    assert(!VfsMountNamespaceRemount(ns,bind,3));assert(VfsMountReadOnly(metadata)&&VfsMountReadOnly(directory));
    assert(VfsMountNamespaceRemount(ns,bind,2)==-1);
    assert(!VfsPathOpen(path,VFS_O_WRITE,&error)&&error==-30);
    reader=VfsPathOpen(path,VFS_O_READ,&error);assert(reader&&!error);assert(!VfsMountNamespaceRemount(ns,bind,7));assert(VfsMountNamespaceRemount(ns,bind,3)==-1);
    uint64_t offset=0;VnodeTime times[2]{};
    assert(VfsWrite(reader,"x",1,&offset)==-30&&VfsAppend(reader,"x",1,&offset)==-30&&VfsTruncate(reader,0)==-30);
    assert(VfsChmod(directory,0700)==-30&&VfsChown(directory,0,0)==-30&&VfsSetTimes(directory,times)==-30);
    assert(VfsAccess(reader,2,false)==-30&&VfsPathAccess(path,2)==-30&&VfsAccessAt(root,bind,"ro-file",2)==-30);
    assert(!VfsOpenAt(root,bind,"new",VFS_O_CREATE|VFS_O_WRITE,0600,0,&error)&&error==-30);
    assert(VfsMkdirAt(root,bind,"new",0700,0)==-30&&VfsRemoveAt(root,bind,"ro-file",false)==-30);
    assert(VfsSymlinkAt(root,bind,"ro-file","new")==-30&&VfsRenameAt(root,bind,"ro-file",bind,"other")==-30);
    assert(VfsChmodAt(root,bind,"ro-file",0600)==-30&&VfsChownAt(root,bind,"ro-file",0,0)==-30&&VfsSetTimesAt(root,bind,"ro-file",times)==-30);
    assert(!VfsCreateSocketAt(root,bind,"sock",0600,0,&error)&&error==-30);
    auto alias=resolve(root,source,"ro-file");auto* writer=VfsPathOpen(alias,VFS_O_WRITE,&error);assert(writer&&!error&&!VfsMountReadOnly(writer));VfsRelease(writer);VfsPathRelease(alias);
    auto rows=infoRows(ns,root);std::string text(16384,0);uint32_t written=0;
    assert(!FormatMountInfo(rows.data(),rows.size(),text.data(),text.size(),&written));assert(text.find(" ro,nosuid,nodev - objects objects rw\n")!=std::string::npos);
    assert(!VfsMountNamespaceDetach(ns,bind));assert(!VfsOpenAt(root,bind,"new",VFS_O_CREATE|VFS_O_WRITE,0600,0,&error)&&error==-30);
    VfsRelease(metadata);VfsRelease(directory);VfsRelease(reader);VfsPathRelease(path);VfsPathRelease(bind);
    VfsPathRelease(source);VfsPathRelease(target);VfsMountNamespaceLeave(ns);VfsPathRelease(root);VfsPathRelease(cwd);VfsMountNamespaceRelease(ns);VfsDrainMountCleanup();
    mounts[6].root.children["ns-a"]->children.erase("ro-file");assert(allocations.size()==allocated&&g_mounts.count==records);
}
static void testFileBinds(brook::MountNamespace* initial,const brook::VfsPath& stableRoot) {
    using namespace brook;
    const auto allocated=allocations.size();const auto records=g_mounts.count;
    MountNamespace* ns=nullptr;VfsPath root{},cwd{};
    assert(!VfsMountNamespaceCopy(initial,stableRoot,stableRoot,&ns,&root,&cwd));assert(VfsMountNamespaceActivate(ns));
    auto a=resolve(root,root,"ns-a"),b=resolve(root,root,"ns-b"),dest=resolve(root,root,"ns-target");
    auto* originalSource=node(a.object).children["value"];auto* originalTarget=node(b.object).children["value"];
    originalTarget->nlink=2;node(b.object).children["alias"]=originalTarget;
    assert(VfsMountNamespaceBindAt(ns,root,root,"ns-a/value","ns-b/value")==-95);
    auto alias=resolve(root,root,"ns-b/alias");assert(alias.object->identity==201);VfsPathRelease(alias);
    node(b.object).children.erase("alias");originalTarget->nlink=1;
    originalSource->nlink=2;assert(VfsMountNamespaceBindAt(ns,root,root,"ns-a/value","ns-b/value")==-95);originalSource->nlink=1;
    assert(VfsMountNamespaceBindAt(ns,root,root,"ns-a/value","ns-b")==-20);
    assert(VfsMountNamespaceBindAt(ns,root,root,"ns-a","ns-b/value")==-20);
    bool succeeded=false;
    for(long failure=0;failure<128;++failure) {
        auto before=allocations.size();auto count=g_mounts.count;failAllocation=failure;
        int error=VfsMountNamespaceBindAt(ns,root,root,"ns-a/value","ns-b/value");failAllocation=-1;
        if(error) {assert(error==-12);VfsDrainMountCleanup();assert(allocations.size()==before && g_mounts.count==count);continue;}
        succeeded=true;break;
    }
    assert(succeeded);auto bound=resolve(root,root,"ns-b/value");assert(bound.object->identity==101);
    omitMetadataReference=true;int metadataError=0;auto* metadata=VfsPathOpen(bound,VFS_O_METADATA,&metadataError);omitMetadataReference=false;
    assert(metadata && !metadataError && metadata->backendObject==bound.object && metadata->mountOwner==bound.attachment);VfsClose(metadata);
    char fileReport[512];assert(!VfsPathObjectPathFromCallback(bound,fileReport,sizeof(fileReport)) && !std::strcmp(fileReport,"/ns-b/value"));
    assert(VfsRemoveAt(root,b,"value",false)==-16);
    assert(VfsRenameAt(root,root,"loose-file",b,"value")==-16);
    assert(!VfsRenameAt(root,a,"value",a,"renamed"));
    assert(std::strcmp(static_cast<MountEntry*>(bound.attachment)->physicalRoot,"/ns-a/renamed")==0);
    Node replacement{&mounts[6],901,false,&node(a.object),{},{}};node(a.object).children["value"]=&replacement;
    Node overwrite{&mounts[6],902,false,&node(a.object),{},{}};node(a.object).children["overwrite"]=&overwrite;
    assert(!VfsRenameAt(root,a,"overwrite",a,"renamed"));originalSource->nlink=0;
    assert(static_cast<MountEntry*>(bound.attachment)->rootFileUnlinked);
    assert(!VfsRemoveAt(root,a,"renamed",false));
    assert(!VfsRenameAt(root,root,"ns-a",root,"source-parent-moved"));
    assert(!std::strcmp(static_cast<MountEntry*>(bound.attachment)->physicalRoot,"/ns-a/renamed"));
    assert(!VfsRenameAt(root,root,"source-parent-moved",root,"ns-a"));
    auto retained=resolve(root,root,"ns-b/value");assert(retained.object->identity==101);VfsPathRelease(retained);
    // Stacking remains unambiguous even after the old bind source is unlinked.
    assert(!VfsMountNamespaceBindAt(ns,root,root,"loose-file","ns-b/value"));
    auto stack=resolve(root,root,"ns-b/value");assert(stack.object->identity==500);
    assert(!VfsRenameAt(root,root,"ns-b",root,"file-target-moved"));
    auto moved=resolve(root,root,"file-target-moved/value");assert(moved.object->identity==500);VfsPathRelease(moved);
    assert(!std::strcmp(static_cast<MountEntry*>(bound.attachment)->mountPoint,"/file-target-moved/value"));
    assert(!VfsPathObjectPathFromCallback(bound,fileReport,sizeof(fileReport)) && !std::strcmp(fileReport,"/file-target-moved/value"));
    assert(!VfsRenameAt(root,root,"file-target-moved",root,"ns-b"));
    assert(!VfsMountNamespaceDetach(ns,stack));VfsPathRelease(stack);
    VfsDrainMountCleanup();assert(!VfsMountNamespaceRemount(ns,bound,7));int error=0;assert(!VfsPathOpen(bound,VFS_O_WRITE,&error)&&error==-30);
    succeeded=false;
    for(long failure=0;failure<512;++failure) {
        auto before=allocations.size();auto count=g_mounts.count;failAllocation=failure;
        int result=VfsMountNamespaceBind(ns,b,dest,true);failAllocation=-1;
        if(result) {assert(result==-12);VfsDrainMountCleanup();assert(allocations.size()==before && g_mounts.count==count);continue;}
        succeeded=true;break;
    }
    assert(succeeded);auto graft=resolve(root,root,"ns-target"),copiedFile=resolve(root,root,"ns-target/value");assert(copiedFile.object->identity==101);
    assert(VfsMountAttributes(static_cast<MountEntry*>(copiedFile.attachment))==7);
    assert(static_cast<MountEntry*>(copiedFile.attachment)->rootFileUnlinked);
    MountNamespace* copy=nullptr;VfsPath cr{},cc{};
    assert(!VfsMountNamespaceCopy(ns,root,root,&copy,&cr,&cc));assert(VfsMountNamespaceActivate(copy));
    auto childFile=resolve(cr,cr,"ns-b/value");assert(!VfsMountNamespaceDetach(copy,childFile));VfsPathRelease(childFile);
    childFile=resolve(cr,cr,"ns-b/value");assert(childFile.object->identity==201);VfsPathRelease(childFile);
    childFile=resolve(cr,cr,"ns-target/value");assert(childFile.object->identity==101);assert(static_cast<MountEntry*>(childFile.attachment)->rootFileUnlinked);VfsPathRelease(childFile);
    VfsMountNamespaceLeave(copy);VfsPathRelease(cr);VfsPathRelease(cc);VfsMountNamespaceRelease(copy);VfsDrainMountCleanup();
    assert(!VfsMountNamespaceDetach(ns,bound));auto uncovered=resolve(root,root,"ns-b/value");assert(uncovered.object->identity==201);VfsPathRelease(uncovered);
    VnodeStat st{};assert(!VfsPathStat(bound,&st)&&st.ino==101);assert(!VfsPathStat(copiedFile,&st)&&st.ino==101);
    assert(!VfsMountNamespaceDetach(ns,graft));VfsPathRelease(graft);VfsPathRelease(copiedFile);VfsPathRelease(bound);
    assert(!VfsMountNamespaceBindAt(ns,root,root,"ns-a/value","ns-b/value"));
    auto legacyBound=resolve(root,root,"ns-b/value");
    assert(!VfsRename("/ns-a/value","/ns-a/renamed"));
    assert(!std::strcmp(static_cast<MountEntry*>(legacyBound.attachment)->physicalRoot,"/ns-a/renamed"));
    Node legacyOverwrite{&mounts[6],903,false,&node(a.object),{},{}};node(a.object).children["overwrite"]=&legacyOverwrite;
    assert(!VfsRename("/ns-a/overwrite","/ns-a/renamed"));
    assert(static_cast<MountEntry*>(legacyBound.attachment)->rootFileUnlinked);
    assert(!VfsRemoveAt(root,a,"renamed",false));
    assert(!VfsMountNamespaceDetach(ns,legacyBound));VfsPathRelease(legacyBound);
    VfsPathRelease(a);VfsPathRelease(b);VfsPathRelease(dest);VfsMountNamespaceLeave(ns);VfsPathRelease(root);VfsPathRelease(cwd);VfsMountNamespaceRelease(ns);VfsDrainMountCleanup();
    originalSource->nlink=1;originalSource->parent=mounts[6].root.children["ns-a"];originalSource->parent->children["value"]=originalSource;
    assert(allocations.size()==allocated && g_mounts.count==records);
}
static void testSocketBinds(brook::MountNamespace* initial,const brook::VfsPath& stableRoot) {
    using namespace brook;
    const auto allocated=allocations.size();const auto records=g_mounts.count;
    MountNamespace* ns=nullptr;VfsPath root{},cwd{};
    assert(!VfsMountNamespaceCopy(initial,stableRoot,stableRoot,&ns,&root,&cwd));assert(VfsMountNamespaceActivate(ns));
    auto a=resolve(root,root,"ns-a");Node socket{&mounts[6],950,false,&node(a.object),{},{}};socket.mode=0140777;node(a.object).children["socket"]=&socket;
    assert(!VfsMountNamespaceBindAt(ns,root,root,"ns-a/socket","ns-b/value"));auto bound=resolve(root,root,"ns-b/value");VnodeStat stat{};assert(!VfsPathStat(bound,&stat) && (stat.mode&0170000)==0140000);
    assert(!VfsRenameAt(root,a,"socket",a,"socket-renamed"));assert(!std::strcmp(static_cast<MountEntry*>(bound.attachment)->physicalRoot,"/ns-a/socket-renamed"));
    assert(!VfsMountNamespaceRemount(ns,bound,7));assert(!VfsPathAccess(bound,2));assert(!VfsAccessAt(root,root,"ns-b/value",2,false,true));int error=0;auto* metadata=VfsPathOpen(bound,VFS_O_METADATA,&error);assert(metadata && !error && !VfsAccess(metadata,2,false));VfsClose(metadata);
    assert(!VfsRename("/ns-a/socket-renamed","/ns-a/socket-final"));assert(!std::strcmp(static_cast<MountEntry*>(bound.attachment)->physicalRoot,"/ns-a/socket-final"));
    assert(!VfsRemoveAt(root,a,"socket-final",false));assert(static_cast<MountEntry*>(bound.attachment)->rootFileUnlinked);
    assert(!VfsMountNamespaceDetach(ns,bound));VfsPathRelease(bound);VfsPathRelease(a);VfsMountNamespaceLeave(ns);VfsPathRelease(root);VfsPathRelease(cwd);VfsMountNamespaceRelease(ns);VfsDrainMountCleanup();
    assert(allocations.size()==allocated && g_mounts.count==records);
}
static void testLongMountPaths(brook::MountNamespace* initial,const brook::VfsPath& stableRoot) {
    using namespace brook;
    const auto allocated=allocations.size();const auto records=g_mounts.count;
    MountNamespace* ns=nullptr;VfsPath root{},cwd{};assert(!VfsMountNamespaceCopy(initial,stableRoot,stableRoot,&ns,&root,&cwd));assert(VfsMountNamespaceActivate(ns));
    std::string leaf(200,'x'),renamed(180,'n');
    Node parent{&mounts[6],960,true,&mounts[6].root,{},{}},target{&mounts[6],961,true,&parent,{},{}};
    mounts[6].root.children["long-parent"]=&parent;parent.children[leaf]=&target;
    std::string path="long-parent/"+leaf;
    assert(!VfsMountNamespaceBindAt(ns,root,root,"ns-a",path.c_str()));
    auto bound=resolve(root,root,path.c_str());auto rows=infoRows(ns,root);bool found=false;
    for(auto& row:rows)if(row.mountPoint=="/"+path)found=true;assert(found);
    found=false;for(uint32_t i=0;i<VfsMountNamespaceCount(ns);++i){auto snap=VfsMountNamespaceSnapshotAt(ns,i);if(snap.mountPoint=="/"+path)found=true;}assert(found);
    assert(!VfsRenameAt(root,root,"long-parent",root,renamed.c_str()));path=renamed+"/"+leaf;
    assert(!std::strcmp(static_cast<MountEntry*>(bound.attachment)->mountPoint,("/"+path).c_str()));
    auto moved=resolve(root,root,path.c_str());assert(VfsPathSame(moved,bound));VfsPathRelease(moved);
    auto source=resolve(root,root,renamed.c_str()),dest=resolve(root,root,"ns-target");assert(!VfsMountNamespaceBind(ns,source,dest,true));
    auto graft=resolve(root,root,"ns-target"),value=resolve(root,graft,(leaf+"/value").c_str());assert(value.object->identity==101);VfsPathRelease(value);
    MountNamespace* copy=nullptr;VfsPath cr{},cc{};assert(!VfsMountNamespaceCopy(ns,root,root,&copy,&cr,&cc));assert(VfsMountNamespaceActivate(copy));
    auto child=resolve(cr,cr,path.c_str());assert(!VfsMountNamespaceDetach(copy,child));VfsPathRelease(child);
    child=resolve(cr,cr,path.c_str());assert(child.object->identity==961);VfsPathRelease(child);
    VfsMountNamespaceLeave(copy);VfsPathRelease(cr);VfsPathRelease(cc);VfsMountNamespaceRelease(copy);VfsDrainMountCleanup();
    rows=infoRows(ns,root);std::string text(rows.size()*4600+1,'\0');uint32_t written=0;assert(!FormatMountInfo(rows.data(),rows.size(),text.data(),text.size(),&written));assert(text.find("/"+path+" ")!=std::string::npos);
    assert(!VfsMountNamespaceDetach(ns,graft));VfsPathRelease(graft);assert(!VfsMountNamespaceDetach(ns,bound));VfsPathRelease(bound);VfsPathRelease(source);VfsPathRelease(dest);
    VfsMountNamespaceLeave(ns);VfsPathRelease(root);VfsPathRelease(cwd);VfsMountNamespaceRelease(ns);VfsDrainMountCleanup();mounts[6].root.children.erase(renamed);
    {MountEntry dummy{};VfsRenameLabels labels;std::string boundary="/"+std::string(510,'z');auto before=allocations.size();assert(labels.Add(&dummy,boundary.c_str(),"x","/")==-36 && allocations.size()==before);assert(labels.Add(&dummy,(boundary+"x").c_str(),"","/")==-36 && allocations.size()==before);assert(!labels.Add(&dummy,boundary.c_str(),"","/"));labels.Apply();assert(!std::strcmp(dummy.mountPoint,boundary.c_str()));}
    assert(allocations.size()==allocated && g_mounts.count==records);
}
static void testNullDeviceBridge(brook::MountNamespace* initial,const brook::VfsPath& stableRoot) {
    using namespace brook;
    const auto allocated=allocations.size();const auto records=g_mounts.count;
    Node dev{&mounts[6],980,true,&mounts[6].root,{},{}};mounts[6].root.children["dev"]=&dev;
    Node dri{&mounts[6],19799,true,&dev,{},{}};dev.children["dri"]=&dri;
    assert(!mounts[6].root.children.count("sys"));
    Node sysRoot{&mounts[6],19800,true,&mounts[6].root,{},{}};
    Node sysDev{&mounts[6],19801,true,&sysRoot,{},{}};
    Node sysChar{&mounts[6],19802,true,&sysDev,{},{}};
    mounts[6].root.children["sys"]=&sysRoot;sysRoot.children["dev"]=&sysDev;sysDev.children["char"]=&sysChar;
    assert(!VfsCanonicalDeviceAnchorInitialize());assert(VfsCanonicalDeviceAnchorInitialize()==-16);
    assert(!VfsCanonicalDrmAnchorInitialize());assert(VfsCanonicalDrmAnchorInitialize()==-16);
    MountNamespace* ns=nullptr;VfsPath root{},cwd{};assert(!VfsMountNamespaceCopy(initial,stableRoot,stableRoot,&ns,&root,&cwd));assert(VfsMountNamespaceActivate(ns));
    auto original=resolve(root,root,"dev"),unrelated=resolve(root,root,"ns-a");bool allowed=false;
    assert(!VfsCanonicalDeviceAllowed(root,root,"/dev/null",&allowed) && allowed);
    assert(!VfsCanonicalDeviceAllowed(root,root,"/dev/urandom",&allowed,true,"urandom") && allowed);
    assert(VfsCanonicalDeviceAllowed(root,root,"/dev/random",&allowed,true,"random")==-22 && !allowed);
    assert(!VfsMountNamespaceBind(ns,original,original));auto bound=resolve(root,root,"dev");assert(!VfsMountNamespaceRemount(ns,bound,2));
    assert(!VfsCanonicalDeviceAllowed(root,root,"/dev/null",&allowed) && allowed);assert(!VfsCanonicalDeviceAllowed(root,bound,"null",&allowed) && allowed);
    assert(!VfsCanonicalDrmBrokerAllowed(root,false,&allowed) && allowed);
    assert(!VfsPathDeviceActivation(root,"/dev/dri/renderD128"));
    Node fakeDri{&mounts[6],19803,true,&dev,{},{}};dev.children["dri"]=&fakeDri;
    assert(!VfsCanonicalDrmBrokerAllowed(root,false,&allowed) && !allowed);dev.children["dri"]=&dri;
    Node fakeRender{&mounts[6],19805,false,&dri,{},{}};dri.children["renderD128"]=&fakeRender;
    assert(!VfsCanonicalDrmBrokerAllowed(root,false,&allowed) && !allowed);dri.children.erase("renderD128");
    auto sys=resolve(root,root,"sys");assert(!VfsMountNamespaceBind(ns,sys,sys));
    auto sysBound=resolve(root,root,"sys");assert(!VfsMountNamespaceRemount(ns,sysBound,3));
    assert(!VfsCanonicalDrmBrokerAllowed(root,true,&allowed) && allowed);
    Node fakeGpu{&mounts[6],19804,true,&sysChar,{},{}};sysChar.children["226:128"]=&fakeGpu;
    assert(!VfsCanonicalDrmBrokerAllowed(root,true,&allowed) && !allowed);sysChar.children.erase("226:128");
    auto chars=resolve(root,root,"/sys/dev/char");assert(!VfsMountNamespaceBind(ns,unrelated,chars));
    auto fakeChars=resolve(root,root,"/sys/dev/char");assert(!VfsCanonicalDrmBrokerAllowed(root,true,&allowed) && !allowed);
    assert(!VfsMountNamespaceDetach(ns,fakeChars));VfsPathRelease(fakeChars);VfsPathRelease(chars);
    assert(!VfsCanonicalDrmBrokerAllowed(root,true,&allowed) && allowed);
    bool general=true;assert(!VfsPathBrokerAllowed(root,"/dev/shm/isolated",&general) && !general);
    Node realNull{&mounts[6],981,false,&dev,{},{}};dev.children["null"]=&realNull;assert(!VfsCanonicalDeviceAllowed(root,root,"/dev/null",&allowed) && !allowed);dev.children.erase("null");
    Node alias{&mounts[6],982,false,&node(unrelated.object),{},"/dev/null"};node(unrelated.object).children["null"]=&alias;
    assert(!VfsCanonicalDeviceAllowed(root,unrelated,"null",&allowed,false) && !allowed);
    assert(!VfsCanonicalDeviceAllowed(root,unrelated,"null",&allowed,true) && allowed);
    node(unrelated.object).children.erase("null");
    MountNamespace* copy=nullptr;VfsPath cr{},cc{};assert(!VfsMountNamespaceCopy(ns,root,root,&copy,&cr,&cc));assert(VfsMountNamespaceActivate(copy));
    assert(!VfsCanonicalDeviceAllowed(cr,cr,"/dev/null",&allowed) && allowed);
    assert(!VfsCanonicalDrmBrokerAllowed(cr,false,&allowed) && allowed);
    assert(!VfsCanonicalDrmBrokerAllowed(cr,true,&allowed) && allowed);
    VfsMountNamespaceLeave(copy);VfsPathRelease(cr);VfsPathRelease(cc);VfsMountNamespaceRelease(copy);VfsDrainMountCleanup();
    std::atomic<bool> stop{false},strict{false};std::atomic<unsigned> observations{0},denials{0};
    std::thread reader([&]{while(!stop.load(std::memory_order_acquire)){bool requireDenial=strict.load(std::memory_order_acquire),ok=false;int e=VfsCanonicalDeviceAllowed(root,root,"/dev/null",&ok);assert((!e)||(e==-13&&!ok));if(requireDenial){assert(e==-13&&!ok);denials.fetch_add(1);}observations.fetch_add(1);}});
    while(!observations.load())std::this_thread::yield();
    for(unsigned i=0;i<100;++i){assert(!VfsMountNamespaceBind(ns,unrelated,bound));auto shadow=resolve(root,root,"dev");assert(!VfsCanonicalDeviceAllowed(root,root,"/dev/null",&allowed) && !allowed);assert(!VfsCanonicalDrmBrokerAllowed(root,false,&allowed) && !allowed);assert(!VfsMountNamespaceDetach(ns,shadow));VfsPathRelease(shadow);}
    assert(!VfsMountNamespaceRemount(ns,bound,6));strict.store(true,std::memory_order_release);while(!denials.load())std::this_thread::yield();stop.store(true,std::memory_order_release);reader.join();
    assert(VfsCanonicalDeviceAllowed(root,root,"/dev/null",&allowed)==-13 && !allowed);
    assert(VfsCanonicalDeviceAllowed(root,root,"/dev/urandom",&allowed,true,"urandom")==-13 && !allowed);
    assert(!VfsCanonicalDrmBrokerAllowed(root,false,&allowed) && allowed); // metadata only
    assert(VfsPathDeviceActivation(root,"/dev/dri/renderD128")==-13);
    assert(!VfsMountNamespaceDetach(ns,sysBound));VfsPathRelease(sysBound);VfsPathRelease(sys);
    assert(!VfsMountNamespaceDetach(ns,bound));VfsPathRelease(bound);VfsPathRelease(original);VfsPathRelease(unrelated);VfsMountNamespaceLeave(ns);VfsPathRelease(root);VfsPathRelease(cwd);VfsMountNamespaceRelease(ns);VfsDrainMountCleanup();
    {FileMutationGuard transaction;VfsPathRelease(g_canonicalDeviceAnchor);g_canonicalDeviceAnchor={};VfsPathRelease(g_canonicalDrmDeviceAnchor);g_canonicalDrmDeviceAnchor={};VfsPathRelease(g_canonicalDrmSysAnchor);g_canonicalDrmSysAnchor={};}
    mounts[6].root.children.erase("dev");mounts[6].root.children.erase("sys");VfsDrainMountCleanup();assert(allocations.size()==allocated && g_mounts.count==records);
}
static void testPivotRoots(brook::MountNamespace* initial,const brook::VfsPath& stableRoot) {
    using namespace brook;
    auto allocated=allocations.size();auto records=g_mounts.count;
    MountNamespace* ns=nullptr;VfsPath root{},cwd{};
    assert(!VfsMountNamespaceCopy(initial,stableRoot,stableRoot,&ns,&root,&cwd));assert(VfsMountNamespaceActivate(ns));
    auto a=resolve(root,root,"/ns-a"),b=resolve(root,root,"/ns-b"),target=resolve(root,root,"/ns-target");
    assert(!VfsMountNamespaceBind(ns,a,target));auto newRoot=resolve(root,root,"/ns-target");
    auto putOld=resolve(root,newRoot,"sub");
    int error=0;auto* exact=FsContextCreate(root,root,022,&error,ns);assert(exact&&!error);
    auto* descendant=FsContextCreate(root,a,077,&error,ns);assert(descendant&&!error);
    FsContextRetain(exact); // shared CLONE_FS references remain one context
    FsSnapshot stale{};assert(!FsContextSnapshot(exact,&stale));
    bool succeeded=false;
    for(long failure=0;failure<256;++failure) {
        auto before=allocations.size();failAllocation=failure;
        int result=VfsMountNamespacePivotRoot(ns,root,newRoot,putOld);failAllocation=-1;
        if(!result) {succeeded=true;break;}
        assert(result==-12);assert(allocations.size()==before);
        FsSnapshot snap{};assert(!FsContextSnapshot(exact,&snap));assert(VfsPathSame(snap.root,root)&&VfsPathSame(snap.cwd,root)&&snap.revision==stale.revision);FsSnapshotRelease(&snap);
        auto still=resolve(root,root,"/ns-target");assert(VfsPathSame(still,newRoot));VfsPathRelease(still);
    }
    assert(succeeded);
    FsSnapshot snap{};assert(!FsContextSnapshot(exact,&snap));assert(VfsPathSame(snap.root,newRoot)&&VfsPathSame(snap.cwd,newRoot)&&snap.revision>stale.revision);FsSnapshotRelease(&snap);
    assert(!FsContextSnapshot(descendant,&snap));assert(VfsPathSame(snap.root,newRoot)&&VfsPathSame(snap.cwd,a));FsSnapshotRelease(&snap);
    assert(FsContextSetResolvedCwd(exact,root,stale)==-11);FsSnapshotRelease(&stale);
    auto* obsolete=FsContextCreate(root,root,022,&error,ns);assert(!obsolete&&error==-11);
    auto* copied=FsContextCopy(exact,&error);assert(copied&&!error);
    auto oldLookup=resolve(newRoot,newRoot,"/sub");assert(VfsPathSame(oldLookup,root));VfsPathRelease(oldLookup);
    expect(newRoot,newRoot,"..",newRoot);
    auto marker=resolve(newRoot,newRoot,"/value");VnodeStat stat{};assert(!VfsPathStat(marker,&stat)&&stat.ino==101);VfsPathRelease(marker);
    marker=resolve(newRoot,newRoot,"/sub/ns-target/value");assert(!VfsPathStat(marker,&stat)&&stat.ino==301);VfsPathRelease(marker);
    char label[64];assert(!VfsPathDirectoryPath(root,label,sizeof(label))&&!std::strcmp(label,"/sub"));
    // A pivoted root with no covered path still needs physical-root cache
    // updates when another retained view renames its backing directory.
    assert(!VfsRenameAt(stableRoot,stableRoot,"ns-a",stableRoot,"info-root-renamed"));
    auto rootRows=infoRows(ns,newRoot);assert(!std::strcmp(rootRows.front().root,"/info-root-renamed")&&!std::strcmp(rootRows.front().mountPoint,"/"));
    assert(!VfsRenameAt(stableRoot,stableRoot,"info-root-renamed",stableRoot,"ns-a"));
    // Retained fchdir is intentional and is never epoch-translated.
    assert(!FsContextSetCwd(exact,root));assert(!FsContextSnapshot(exact,&snap));assert(VfsPathSame(snap.cwd,root)&&VfsPathSame(snap.root,newRoot));FsSnapshotRelease(&snap);
    assert(!FsContextSetCwd(exact,newRoot));
    // Alternative pivot('.', '.') stacks the old root on the new mount root.
    auto onOldRoot=resolve(newRoot,newRoot,"/sub");assert(!VfsMountNamespaceBind(ns,b,onOldRoot));VfsPathRelease(onOldRoot);
    auto second=resolve(newRoot,newRoot,"/sub");
    std::vector<FsContext*> concurrentCopies;
    std::atomic<unsigned> copiesStarted{0};std::atomic<bool> moveDone{false};
    std::thread copier([&] {
        for(unsigned i=0;i<256 && (!moveDone.load(std::memory_order_acquire)||i<32);++i) {
            int copyError=0;auto* context=FsContextCopy(exact,&copyError);assert(context&&!copyError);
            if(i%2)FsContextRelease(context);else concurrentCopies.push_back(context);
            copiesStarted.fetch_add(1,std::memory_order_release);
            std::this_thread::yield();
        }
    });
    while(!copiesStarted.load(std::memory_order_acquire))std::this_thread::yield();
    FsSnapshot beforeMove{};assert(!FsContextSnapshot(copied,&beforeMove));
    std::atomic<bool> setReady{false};int setResult=0;
    std::thread setter([&] {setReady.store(true,std::memory_order_release);setResult=FsContextSetResolvedCwd(copied,a,beforeMove);});
    while(!setReady.load(std::memory_order_acquire))std::this_thread::yield();
    assert(!VfsMountNamespacePivotRoot(ns,newRoot,second,second));
    moveDone.store(true,std::memory_order_release);copier.join();setter.join();
    assert(setResult==0 || setResult==-11);FsSnapshotRelease(&beforeMove);
    for(auto* context:concurrentCopies) {
        FsSnapshot state{};assert(!FsContextSnapshot(context,&state));
        assert(VfsPathSame(state.root,second));FsSnapshotRelease(&state);FsContextRelease(context);
    }
    assert(!FsContextSnapshot(copied,&snap));assert(VfsPathSame(snap.root,second));
    assert(VfsPathSame(snap.cwd,setResult==0?a:second));FsSnapshotRelease(&snap);
    assert(!FsContextSnapshot(exact,&snap));assert(VfsPathSame(snap.root,second)&&VfsPathSame(snap.cwd,second));FsSnapshotRelease(&snap);
    marker=resolve(second,second,"/value");assert(!VfsPathStat(marker,&stat)&&stat.ino==201);VfsPathRelease(marker);
    auto dot=resolve(second,second,".");assert(VfsPathSame(dot,second));
    assert(!VfsMountNamespaceDetach(ns,dot));VfsPathRelease(dot);
    marker=resolve(second,second,"/value");assert(!VfsPathStat(marker,&stat)&&stat.ino==201);VfsPathRelease(marker);
    FsContextRelease(copied);FsContextRelease(descendant);FsContextRelease(exact);FsContextRelease(exact);
    VfsPathRelease(second);VfsPathRelease(putOld);VfsPathRelease(newRoot);VfsPathRelease(target);VfsPathRelease(a);VfsPathRelease(b);
    VfsMountNamespaceLeave(ns);VfsDrainMountCleanup();VfsPathRelease(root);VfsPathRelease(cwd);VfsMountNamespaceRelease(ns);VfsDrainMountCleanup();
    assert(allocations.size()==allocated && g_mounts.count==records);
}
static void testRecursiveBinds(brook::MountNamespace* initial,const brook::VfsPath& stableRoot) {
    using namespace brook;
    const auto initialAllocations=allocations.size();const auto initialRecords=g_mounts.count;
    MountNamespace* ns=nullptr;VfsPath root{},cwd{};
    assert(!VfsMountNamespaceCopy(initial,stableRoot,stableRoot,&ns,&root,&cwd));
    assert(VfsMountNamespaceActivate(ns));
    auto target=resolve(root,root,"/ns-target");
    auto originalSynthetic=resolve(root,root,"/synthetic/deep/mount");
    const auto before=VfsMountNamespaceCount(ns);
    std::atomic<bool> stopReader{false};std::atomic<unsigned> observations{0};
    std::thread reader([&] {
        while(!stopReader.load(std::memory_order_acquire)) {
            auto count=VfsMountNamespaceCount(ns);assert(count==before || count==2*before);
            observations.fetch_add(1,std::memory_order_release);
        }
    });
    while(!observations.load(std::memory_order_acquire))std::this_thread::yield();
    bool succeeded=false;
    // Root into its own descendant freezes the old topology, including virtual
    // boot ancestors, rather than discovering the just-created graft forever.
    for(long failure=0;failure<512;++failure) {
        auto allocated=allocations.size();auto records=g_mounts.count;
        failAllocation=failure;int error=VfsMountNamespaceBind(ns,root,target,true);failAllocation=-1;
        if(error) {
            if(error!=-12)std::fprintf(stderr,"rbind failure=%ld error=%d\n",failure,error);
            assert(error==-12);VfsDrainMountCleanup();
            assert(allocations.size()==allocated && g_mounts.count==records);
            assert(VfsMountNamespaceCount(ns)==before);
            continue;
        }
        assert(VfsMountNamespaceCount(ns)==2*before);
        stopReader.store(true,std::memory_order_release);reader.join();assert(observations.load());
        auto graft=resolve(root,root,"/ns-target");
        auto synthetic=resolve(root,graft,"synthetic/deep/mount");
        assert(synthetic.attachment!=originalSynthetic.attachment);
        assert(synthetic.object->identity==originalSynthetic.object->identity);
        auto up=resolve(root,graft,"synthetic/deep/../..");assert(VfsPathSame(up,graft));VfsPathRelease(up);
        VfsPathRelease(synthetic);
        assert(!VfsMountNamespaceDetach(ns,graft));VfsPathRelease(graft);VfsDrainMountCleanup();
        assert(allocations.size()==allocated && g_mounts.count==records);succeeded=true;break;
    }
    if(reader.joinable()) {stopReader.store(true,std::memory_order_release);reader.join();}
    assert(succeeded);
    // Self-bind, nested and stacked mounts; partial sources must not copy
    // unrelated siblings. Retained covered dirfds keep their original view.
    auto a=resolve(root,root,"/ns-a"),b=resolve(root,root,"/ns-b");
    auto sub=resolve(root,a,"sub");assert(!VfsMountNamespaceBind(ns,b,sub));
    auto stack=resolve(root,a,"sub");assert(!VfsMountNamespaceBind(ns,a,stack));VfsPathRelease(stack);
    auto nested=resolve(root,a,"sub/sub");assert(!VfsMountNamespaceBind(ns,b,nested));VfsPathRelease(nested);
    const auto nestedCount=VfsMountNamespaceCount(ns);
    // Source on a different attachment from the namespace root: the root has
    // no covered edge and must be excluded without dereferencing an empty path.
    auto mountedSource=resolve(root,a,"sub");
    assert(!VfsMountNamespaceBind(ns,mountedSource,target,true));
    assert(VfsMountNamespaceCount(ns)==nestedCount+2);
    auto mountedGraft=resolve(root,root,"/ns-target");
    auto mountedValue=resolve(root,mountedGraft,"sub/value");VnodeStat mountedStat{};
    assert(!VfsPathStat(mountedValue,&mountedStat)&&mountedStat.ino==201);VfsPathRelease(mountedValue);
    assert(!VfsMountNamespaceDetach(ns,mountedGraft));VfsPathRelease(mountedGraft);VfsPathRelease(mountedSource);
    assert(VfsMountNamespaceCount(ns)==nestedCount);
    assert(!VfsMountNamespaceBind(ns,a,target,true));assert(VfsMountNamespaceCount(ns)==nestedCount+4);
    auto graft=resolve(root,root,"/ns-target");
    auto value=resolve(root,graft,"sub/sub/value");VnodeStat st{};assert(!VfsPathStat(value,&st)&&st.ino==201);VfsPathRelease(value);
    value=resolve(root,target,"value");assert(!VfsPathStat(value,&st)&&st.ino==301);VfsPathRelease(value);
    char label[64];value=resolve(root,graft,"sub/sub");assert(!VfsPathDirectoryPath(value,label,sizeof(label)));
    assert(!std::strcmp(label,"/ns-target/sub/sub"));VfsPathRelease(value);
    assert(!VfsRenameAt(root,root,"ns-a",root,"rbind-renamed"));
    value=resolve(root,graft,"sub/sub/value");assert(!VfsPathStat(value,&st)&&st.ino==201);VfsPathRelease(value);
    assert(!VfsRenameAt(root,root,"rbind-renamed",root,"ns-a"));
    assert(!VfsMountNamespaceDetach(ns,graft));VfsPathRelease(graft);
    assert(VfsMountNamespaceCount(ns)==nestedCount);
    assert(!VfsMountNamespaceBind(ns,b,b,true));
    graft=resolve(root,root,"/ns-b");assert(!VfsMountNamespaceDetach(ns,graft));VfsPathRelease(graft);
    // A less-privileged namespace copy locks inherited descendant mounts.
    auto* owner=static_cast<UserNamespace*>(kmalloc(sizeof(UserNamespace)));*owner={};owner->owner=1000;
    MountNamespace* locked=nullptr;VfsPath lockedRoot{},lockedCwd{};
    assert(!VfsMountNamespaceCopy(ns,root,root,&locked,&lockedRoot,&lockedCwd));
    assert(!VfsMountNamespaceSetUserOwnerPrepared(locked,owner));assert(VfsMountNamespaceActivate(locked));
    auto lockedA=resolve(lockedRoot,lockedRoot,"/ns-a"),lockedTarget=resolve(lockedRoot,lockedRoot,"/ns-target");
    assert(!VfsMountNamespaceBind(locked,lockedA,lockedTarget,true));
    auto lockedSub=resolve(lockedRoot,lockedRoot,"/ns-target/sub");
    assert(VfsMountNamespaceDetach(locked,lockedSub)==-22);VfsPathRelease(lockedSub);
    auto fresh=resolve(lockedRoot,lockedRoot,"/ns-target");assert(!VfsMountNamespaceDetach(locked,fresh));VfsPathRelease(fresh);
    VfsPathRelease(lockedA);VfsPathRelease(lockedTarget);VfsMountNamespaceLeave(locked);VfsDrainMountCleanup();
    VfsPathRelease(lockedRoot);VfsPathRelease(lockedCwd);VfsMountNamespaceRelease(locked);VfsDrainMountCleanup();UserNamespaceRelease(owner);
    VfsPathRelease(a);VfsPathRelease(b);VfsPathRelease(sub);VfsPathRelease(target);VfsPathRelease(originalSynthetic);
    VfsMountNamespaceLeave(ns);VfsDrainMountCleanup();VfsPathRelease(root);VfsPathRelease(cwd);VfsMountNamespaceRelease(ns);VfsDrainMountCleanup();
    assert(allocations.size()==initialAllocations && g_mounts.count==initialRecords);
}
int main(){
    using namespace brook;
    VfsInit();rejectProvider=true;assert(!VfsRegisterFs("rejected",&fsOps));rejectProvider=false;
    assert(VfsRegisterFs("objects",&fsOps));assert(providerCalls==3);assert(VfsRegisterFs("unsupported",&unsupportedOps));
    assert(VfsMount("/","objects",0));assert(!VfsMount("/","objects",1));
    Node directory{&mounts[0],2,true,&mounts[0].root,{},{}}, marker{&mounts[0],3,false,&directory,{},{}}, poison{&mounts[0],4,true,&mounts[0].root,{},{}}, file{&mounts[0],5,false,&mounts[0].root,{},{}}, link{&mounts[0],6,false,&directory,{},"/dir/marker"}, namespaceTarget{&mounts[0],900,true,&mounts[0].root,{},{}};
    mounts[0].root.children={{"dir",&directory},{"file",&file},{"namespace-target",&namespaceTarget}};directory.children={{"marker",&marker},{"absolute",&link}};
    VfsPath root{},held{};assert(!VfsPathRoot(&root));held=resolve(root,root,"/dir");
    testProcessContexts(root,held);
    auto original=resolve(root,held,"marker");expect(root,held,"absolute",original);
    // The actual namespace lock stabilizes lookup against a hostile rename.
    {
        FileMutationGuard transaction;mounts[0].root.children.erase("dir");mounts[0].root.children["moved"]=&directory;mounts[0].root.children["dir"]=&poison;
    }
    expect(root,held,"marker",original);expect(root,root,"moved/marker",original);
    std::atomic<bool> begin{false};
    std::thread writer([&]{
        while(!begin.load(std::memory_order_acquire))std::this_thread::yield();
        for(unsigned i=0;i<100;++i){
            FileMutationGuard transaction;
            mounts[0].root.children.erase("moved");mounts[0].root.children["temporary"]=&directory;
            mounts[0].root.children.erase("temporary");mounts[0].root.children["moved"]=&directory;
        }
    });
    begin.store(true,std::memory_order_release);
    for(unsigned i=0;i<200;++i)expect(root,held,"marker",original);
    writer.join();
    assert(VfsMount("/moved","objects",1));
    auto mounted=resolve(root,root,"/moved");assert(mounted.attachment!=held.attachment);
    expect(root,held,"marker",original);expect(root,mounted,"..",root);
    Node magic{&mounts[0],11,false,&mounts[0].root,{},"magic-reporting-only"};
    mounts[0].root.children["magic"]=&magic;magicTarget=held;
    // The reported text is intentionally invalid. Following must use the
    // retained lower cwd even after it is covered by another mount.
    expect(root,root,"/magic/marker",original);
    auto magicDirectory=resolve(root,root,"/magic");assert(VfsPathSame(magicDirectory,held));VfsPathRelease(magicDirectory);
    char reported[64]{};assert(VfsReadlinkAt(root,root,"/magic",reported,sizeof(reported))==20);
    assert(!std::strcmp(reported,"magic-reporting-only"));magicTarget={};
    mounts[0].root.children.erase("magic");
    assert(VfsRemovePath("/moved",true)==-16);
    assert(VfsRename("/moved","/bad")==-16);
    assert(VfsMount("/moved/inside","objects",3));
    Node lowerDir{&mounts[1],2,true,&mounts[1].root,{},{}}, lowerPoint{&mounts[1],3,true,&lowerDir,{}, {}};
    lowerDir.children["m"]=&lowerPoint;mounts[1].root.children["d"]=&lowerDir;
    assert(VfsMount("/moved/d/m","objects",4));
    Node lowerP{&mounts[1],4,true,&mounts[1].root,{}, {}};mounts[1].root.children["p"]=&lowerP;
    assert(VfsMount("/moved/p/missing/probe","objects",5));
    // Stacked mount roots must cross all covered edges before one parent step.
    assert(VfsMount("/moved","objects",2));
    auto stacked=resolve(root,root,"/moved");assert(stacked.attachment!=mounted.attachment);
    expect(root,stacked,"..",root);expect(mounted,stacked,"..",mounted);
    Node upperDir{&mounts[2],2,true,&mounts[2].root,{}, {}};mounts[2].root.children["d"]=&upperDir;
    assert(!VfsRename("/moved/d","/moved/e"));
    Node upperP{&mounts[2],3,true,&mounts[2].root,{},{}}, upperSource{&mounts[2],4,true,&mounts[2].root,{}, {}};
    mounts[2].root.children["p"]=&upperP;mounts[2].root.children["source"]=&upperSource;
    { FileMutationGuard transaction;assert(!VfsReservedMutationLocked("/moved/p",true)); }
    assert(!VfsRename("/moved/source","/moved/p"));
    VfsPath hidden{};assert(VfsPathResolve(root,root,"/moved/inside",&hidden)==-2 && !hidden.object);
    assert(!VfsOpen("/moved/inside"));
    auto* legacyTop=VfsOpen("/moved");assert(legacyTop && legacyTop->mountOwner==stacked.attachment);VfsClose(legacyTop);
    assert(!VfsUnmount("/moved"));VfsPathRelease(stacked);assert(VfsUnmount("/moved"));
    auto inside=resolve(root,root,"/moved/inside");
    auto* legacyInside=VfsOpen("/moved/inside");assert(legacyInside && legacyInside->mountOwner==inside.attachment);VfsClose(legacyInside);
    VfsPathRelease(inside);assert(VfsUnmount("/moved/inside"));
    auto exposed=resolve(root,root,"/moved/d/m");
    auto* legacyExposed=VfsOpen("/moved/d/m");assert(legacyExposed && legacyExposed->mountOwner==exposed.attachment);VfsClose(legacyExposed);
    assert(!VfsOpen("/moved/e/m"));VfsPathRelease(exposed);assert(VfsUnmount("/moved/d/m"));
    assert(VfsUnmount("/moved/p/missing/probe"));
    // Nested missing ancestors have real retained identities, with no disk mkdir.
    assert(VfsMount("/mnt/probe","objects",2));
    assert(VfsMkdir("/mnt",0755)==-16);assert(VfsSymlink("/dir","/mnt")==-16);
    assert(VfsRename("/dir","/mnt")==-16);assert(VfsRemovePath("/mnt",true)==-16);
    auto synthetic=resolve(root,root,"/mnt");auto probe=resolve(root,root,"/mnt/probe");expect(root,probe,"..",synthetic);expect(root,probe,"../..",root);
    // Unmount removes the name link; a held synthetic directory remains alive.
    VfsPathRelease(probe);assert(VfsUnmount("/mnt/probe"));
    VfsPath missing{};assert(VfsPathResolve(root,root,"/mnt",&missing)==-2 && !missing.object);
    expect(root,synthetic,"..",root);
    int error=0;Vnode* virtualNode=VfsPathOpen(synthetic,VFS_O_READ,&error);assert(virtualNode&&!error);
    VfsPath virtualFrom{};assert(!VfsPathFromVnode(virtualNode,&virtualFrom)&&VfsPathSame(virtualFrom,synthetic));
    VfsPathRelease(virtualFrom);VfsClose(virtualNode);VfsPathRelease(synthetic);
    assert(!VfsMount("/file/nested","objects",2));assert(!mounts[2].alive);
    bool mountedAfterFailures=false;
    for(long failure=0;failure<30;++failure){
        const auto before=allocations.size();failAllocation=failure;
        bool mounted=VfsMount("/new/child","objects",2);failAllocation=-1;
        if(mounted){assert(VfsUnmount("/new/child"));mountedAfterFailures=true;}
        assert(!mounts[2].alive && allocations.size()==before);
        if(mounted)break;
    }
    assert(mountedAfterFailures);
    assert(VfsMount("/opaque","unsupported",2));
    assert(VfsPathResolve(root,root,"/opaque",&missing)==-95 && !missing.object);
    assert(VfsUnmount("/opaque"));
    // A mounted descendant follows an ancestor rename in both APIs. Labels
    // remain reporting/legacy dispatch data; the retained edge owns identity.
    Node tree{&mounts[0],7,true,&mounts[0].root,{},{}}, branch{&mounts[0],8,true,&tree,{}, {}};
    tree.children["branch"]=&branch;mounts[0].root.children["tree"]=&tree;
    assert(VfsMount("/tree/branch","objects",2));assert(!VfsRename("/tree","/renamed"));
    auto renamed=resolve(root,root,"/renamed/branch");
    auto* legacyRenamed=VfsOpen("/renamed/branch");assert(legacyRenamed && legacyRenamed->mountOwner==renamed.attachment);VfsClose(legacyRenamed);
    assert(!VfsOpen("/tree/branch"));VfsPathRelease(renamed);assert(VfsUnmount("/renamed/branch"));
    // A physically empty directory with synthetic children is nonempty in
    // this namespace. Moving the source ancestor remains allowed.
    Node physical{&mounts[0],9,true,&mounts[0].root,{},{}}, source{&mounts[0],10,true,&mounts[0].root,{}, {}};
    mounts[0].root.children["p"]=&physical;mounts[0].root.children["source"]=&source;
    assert(VfsMount("/p/missing/probe","objects",2));
    assert(VfsRemovePath("/p",true)==-39);assert(VfsRename("/source","/p")==-39);
    assert(!VfsRename("/p","/p"));assert(!VfsRename("/p","/q"));
    auto movedVirtual=resolve(root,root,"/q/missing/probe");
    auto* legacyMovedVirtual=VfsOpen("/q/missing/probe");assert(legacyMovedVirtual && legacyMovedVirtual->mountOwner==movedVirtual.attachment);VfsClose(legacyMovedVirtual);
    VfsPathRelease(movedVirtual);assert(VfsUnmount("/q/missing/probe"));
    // Open transfers a backing hold before releasing either retained path.
    Vnode* opened=VfsPathOpen(mounted,VFS_O_READ,&error);assert(opened&&!error);
    VfsPath from{};assert(!VfsPathFromVnode(opened,&from)&&VfsPathSame(from,mounted));
    VfsPathRelease(from);VfsPathRelease(mounted);assert(!VfsUnmount("/moved"));
    VfsClose(opened);assert(VfsUnmount("/moved") && mounts[1].closed==1);
    failAllocation=0;assert(!VfsPathOpen(original,VFS_O_READ,&error)&&error==-12);failAllocation=-1;
    const auto before=allocations.size();assert(!VfsPathOpen(original,VFS_O_TRUNC,&error)&&error==-5);assert(allocations.size()==before);
    assert(!VfsUnmount("/"));VfsPathRelease(original);VfsPathRelease(held);VfsPathRelease(root);
    assert(VfsUnmount("/"));for(const auto& mount:mounts)assert(!mount.alive && !mount.live);
    // A copied attachment pins one real backing without remounting its driver.
    assert(!g_backings);assert(VfsMount("/","objects",6));
    MountEntry alias{};
    {
        FileMutationGuard transaction;
        alias=*g_rootMount;alias.next=nullptr;alias.pathHolds=0;alias.vnodeHolds=0;
        alias.backing->RetainLocked();
    }
    VfsPath aliasRoot{};
    { FileMutationGuard transaction;assert(!VfsMountRootLocked(&alias,&aliasRoot)); }
    assert(mountCalls[6]==1 && alias.backing->references==2);
    assert(!VfsSync(nullptr) && syncCalls[6]==1); // one sync per backing, not per attachment
    assert(VfsUnmount("/") && mounts[6].alive && !unmountCalls[6]);
    VnodeStat aliasStat{};assert(!VfsPathStat(aliasRoot,&aliasStat) && aliasStat.isDir);
    VfsPathRelease(aliasRoot);
    {
        FileMutationGuard transaction;
        rejectUnmount[6]=true;
        assert(VfsBackingDropLocked(alias.backing)==-5 && alias.backing->references==1);
        rejectUnmount[6]=false;
        assert(!VfsBackingDropLocked(alias.backing));alias.backing=nullptr;alias.used=false;
    }
    assert(!mounts[6].alive && unmountCalls[6]==2 && !g_backings);
    // Failed private construction must retain failed teardown for sync/retry.
    rejectRoot[6]=true;rejectUnmount[6]=true;
    assert(!VfsMount("/","objects",6));
    assert(!g_rootMount && g_backings && !g_backings->references && mounts[6].alive);
    const auto callsBeforeRemount=mountCalls[6];
    assert(!VfsMount("/","objects",6) && mountCalls[6]==callsBeforeRemount);
    assert(VfsSync(nullptr)==-5 && mounts[6].alive);
    rejectUnmount[6]=false;assert(!VfsSync(nullptr));rejectRoot[6]=false;
    assert(!g_backings && !mounts[6].alive);
    assert(VfsMount("/","objects",6) && mountCalls[6]==callsBeforeRemount+1);
    const auto callsBeforeAlias=mountCalls[6];
    assert(VfsMount("/same","objects",6) && mountCalls[6]==callsBeforeAlias);
    auto* sameBacking=g_rootMount->backing;assert(sameBacking->references==2);
    const auto syncBeforeAlias=syncCalls[6];assert(!VfsSync(nullptr));
    assert(syncCalls[6]==syncBeforeAlias+1);
    assert(VfsUnmount("/same") && sameBacking->references==1);
    VfsPath stableRoot{};assert(!VfsPathRoot(&stableRoot));
    const auto dynamicMountCalls=mountCalls[6];
    for(unsigned i=0;i<64;++i) {
        char point[32];std::snprintf(point,sizeof(point),"/dynamic%u",i);
        assert(VfsMount(point,"objects",6));
        auto path=resolve(stableRoot,stableRoot,point);
        expect(stableRoot,path,"..",stableRoot);VfsPathRelease(path);
    }
    assert(VfsMountMaxSlots()==65 && VfsRootMountCount()==64);
    assert(mountCalls[6]==dynamicMountCalls && sameBacking->references==65);
    Node enumeratedParent{&mounts[6],42,true,&mounts[6].root,{}, {}};
    mounts[6].root.children["enum-parent"]=&enumeratedParent;
    std::atomic<bool> stopEnumeration{false};std::atomic<unsigned> observations{0};
    std::thread enumerator([&] {
        while(!stopEnumeration.load(std::memory_order_acquire)) {
            const auto count=VfsMountMaxSlots();assert(count>=65);
            auto snap=VfsMountSnapshotAt(observations.load()%count);
            if(snap.used) {
                assert(snap.mountPoint[0]=='/' && std::memchr(snap.mountPoint,0,sizeof(snap.mountPoint)));
                assert(!std::strcmp(snap.fsType,"objects"));
            }
            char label[64]{};
            if(VfsRootMountNameAt(0,label,sizeof(label)))assert(label[0] && std::memchr(label,0,sizeof(label)));
            assert(VfsRootMountCount()>=64);
            observations.fetch_add(1,std::memory_order_release);
        }
    });
    while(!observations.load(std::memory_order_acquire))std::this_thread::yield();
    for(unsigned i=0;i<200;++i) {
        assert(VfsMount("/enum-parent/alias","objects",6));
        assert(!VfsRename("/enum-parent","/enum-other"));
        assert(VfsUnmount("/enum-other/alias"));
        assert(!VfsRename("/enum-other","/enum-parent"));
    }
    stopEnumeration.store(true,std::memory_order_release);enumerator.join();
    mounts[6].root.children.erase("enum-parent");

    auto retainedDynamic=resolve(stableRoot,stableRoot,"/dynamic0");
    for(unsigned i=0;i<64;++i) {
        char point[32];std::snprintf(point,sizeof(point),"/dynamic%u",i);
        if(!i) {assert(!VfsUnmount(point));VfsPathRelease(retainedDynamic);}
        assert(VfsUnmount(point));
    }
    for(unsigned i=0;i<32;++i)assert(VfsMount("/stacked-dynamic","objects",6));
    assert(VfsMountMaxSlots()==33);
    auto stackedDynamic=resolve(stableRoot,stableRoot,"/stacked-dynamic");
    expect(stableRoot,stackedDynamic,"..",stableRoot);VfsPathRelease(stackedDynamic);
    for(unsigned i=0;i<32;++i)assert(VfsUnmount("/stacked-dynamic"));
    Node a{&mounts[6],100,true,&mounts[6].root,{},{}},av{&mounts[6],101,false,&a,{},{}},sub{&mounts[6],102,true,&a,{},{}},
         b{&mounts[6],200,true,&mounts[6].root,{},{}},bv{&mounts[6],201,false,&b,{},{}},
         target{&mounts[6],300,true,&mounts[6].root,{},{}},tv{&mounts[6],301,false,&target,{},{}},
         renameTree{&mounts[6],400,true,&mounts[6].root,{},{}},renameSlot{&mounts[6],401,true,&renameTree,{},{}},
         looseFile{&mounts[6],500,false,&mounts[6].root,{},{}};
    a.children={{"value",&av},{"sub",&sub}};b.children={{"value",&bv}};target.children={{"value",&tv}};
    renameTree.children={{"slot",&renameSlot}};
    mounts[6].root.children={{"ns-a",&a},{"ns-b",&b},{"ns-target",&target},{"rename-tree",&renameTree},{"loose-file",&looseFile}};
    // Clone synthetic boot ancestors too, retaining their distinct parent links.
    assert(VfsMount("/synthetic/deep/mount","objects",6));
    assert(VfsMount("/rename-tree/slot","objects",6));
    auto originalSynthetic=resolve(stableRoot,stableRoot,"/synthetic/deep");
    auto originalTarget=resolve(stableRoot,stableRoot,"/ns-target");
    auto aPath=resolve(stableRoot,stableRoot,"/ns-a"),bPath=resolve(stableRoot,stableRoot,"/ns-b");
    auto* initial=VfsMountNamespaceInitial();
    testFreshMounts(initial,stableRoot,originalTarget,aPath);
    testRecursiveBinds(initial,stableRoot);
    testPivotRoots(initial,stableRoot);
    testMountInfo(initial,stableRoot);
    testMountRestrictions(initial,stableRoot);
    testReadOnlyMount(initial,stableRoot);
    testFileBinds(initial,stableRoot);
    testSocketBinds(initial,stableRoot);
    testLongMountPaths(initial,stableRoot);
    testNullDeviceBridge(initial,stableRoot);
    MountNamespace* parentNs=nullptr;VfsPath parentRoot{},parentCwd{};
    assert(!VfsMountNamespaceCopy(initial,stableRoot,originalSynthetic,&parentNs,&parentRoot,&parentCwd));
    assert(VfsMountNamespaceBind(parentNs,aPath,parentCwd)==-3);
    assert(VfsMountNamespaceActivate(parentNs));
    assert(VfsMountNamespaceCount(parentNs)==VfsMountNamespaceCount(initial));
    assert(directoryEntries(parentRoot)["synthetic"]==1);
    // Backend names are shared: rename through a copied attachment must also
    // update initial legacy dispatch, and an initial rename must update copies.
    assert(!VfsRenameAt(parentRoot,parentRoot,"rename-tree",parentRoot,"renamed-tree"));
    auto* crossNamespaceNode=VfsOpen("/renamed-tree/slot");assert(crossNamespaceNode);VfsClose(crossNamespaceNode);
    assert(!VfsOpen("/rename-tree/slot"));
    auto copiedRenamed=resolve(parentRoot,parentRoot,"/renamed-tree/slot");
    char copiedName[512];assert(!VfsPathDirectoryPath(copiedRenamed,copiedName,sizeof(copiedName)));
    assert(!std::strcmp(copiedName,"/renamed-tree/slot"));
    assert(!VfsRename("/renamed-tree","/rename-tree"));
    assert(!VfsPathDirectoryPath(copiedRenamed,copiedName,sizeof(copiedName)));
    assert(!std::strcmp(copiedName,"/rename-tree/slot"));VfsPathRelease(copiedRenamed);
    assert(parentRoot.attachment!=stableRoot.attachment && parentCwd.attachment!=originalSynthetic.attachment);
    auto parentSynthetic=resolve(parentRoot,parentRoot,"/synthetic/deep");assert(VfsPathSame(parentCwd,parentSynthetic));
    auto parentSyntheticUp=resolve(parentRoot,parentCwd,"..");
    assert(parentSyntheticUp.attachment==parentRoot.attachment);VfsPathRelease(parentSyntheticUp);VfsPathRelease(parentSynthetic);
    auto parentTarget=resolve(parentRoot,parentRoot,"/ns-target");
    assert(!VfsMountNamespaceBind(parentNs,aPath,parentTarget));
    bool brokerAllowed=false;
    assert(!VfsPathBrokerAllowed(parentRoot,"/ns-target/missing-synthetic-tail",&brokerAllowed)&&!brokerAllowed);
    assert(!VfsPathBrokerAllowed(parentRoot,"/ns-b/missing-synthetic-tail",&brokerAllowed)&&brokerAllowed);
    assert(!VfsPathBrokerAllowed(stableRoot,"/ns-target/missing-synthetic-tail",&brokerAllowed)&&brokerAllowed);
    assert(VfsMountNamespaceCount(parentNs)==VfsMountNamespaceCount(initial)+1);
    assert(directoryEntries(parentRoot)["ns-target"]==1); // no duplicate physical mountpoint
    auto inode=[&](const VfsPath& rootPath,const VfsPath& start,const char* name,uint64_t expected) {
        auto path=resolve(rootPath,start,name);VnodeStat st{};assert(!VfsPathStat(path,&st)&&st.ino==expected);VfsPathRelease(path);
    };
    inode(parentRoot,parentRoot,"/ns-target/value",101);inode(stableRoot,stableRoot,"/ns-target/value",301);
    inode(parentRoot,parentTarget,"value",301);
    // Complete private copy rollback, including synthetic-parent memoization.
    bool copiedAfterFailures=false;
    for(long failure=0;failure<256;++failure) {
        auto before=allocations.size();const auto beforeRecords=g_mounts.count;
        MountNamespace* failedNs=nullptr;VfsPath failedRoot{},failedCwd{};
        failAllocation=failure;
        int copyResult=VfsMountNamespaceCopy(parentNs,parentRoot,parentCwd,&failedNs,&failedRoot,&failedCwd);
        failAllocation=-1;
        if(!copyResult) {
            assert(VfsMountNamespaceAbandon(failedNs));
            VfsPathRelease(failedRoot);VfsPathRelease(failedCwd);
            VfsDrainMountCleanup();VfsMountNamespaceRelease(failedNs);copiedAfterFailures=true;
        } else {
            assert(copyResult==-12 && !failedNs && !failedRoot.object && !failedCwd.object);
            VfsDrainMountCleanup();
        }
        assert(allocations.size()==before && g_mounts.count==beforeRecords);
        inode(parentRoot,parentRoot,"/ns-target/value",101);
        if(copiedAfterFailures)break;
    }
    assert(copiedAfterFailures);
    // A namespace FD is an active topology lease, unlike a directory path.
    // The last task can leave while two independent namespace handles keep
    // nested mounts searchable through a foreign retained directory.
    MountNamespace* heldNs=nullptr;VfsPath heldRoot{},heldCwd{};
    assert(!VfsMountNamespaceCopy(initial,stableRoot,aPath,&heldNs,&heldRoot,&heldCwd));
    assert(!VfsMountNamespaceHandleAcquire(heldNs)); // never revive prepared state
    assert(VfsMountNamespacePrivatePropagation(heldNs,heldRoot)==-3);
    assert(VfsMountNamespaceActivate(heldNs));
    assert(!VfsMountNamespacePrivatePropagation(heldNs,heldRoot));
    assert(VfsMountNamespacePrivatePropagation(heldNs,stableRoot)==-22);
    assert(VfsMountNamespacePrivatePropagation(heldNs,heldCwd)==-22);
    assert(VfsMountNamespaceIdentity(heldNs)!=VfsMountNamespaceIdentity(initial));
    auto heldSub=resolve(heldRoot,heldCwd,"sub");
    assert(!VfsMountNamespaceBind(heldNs,bPath,heldSub));VfsPathRelease(heldSub);
    assert(VfsMountNamespaceHandleAcquire(heldNs));assert(VfsMountNamespaceHandleAcquire(heldNs));
    VfsMountNamespaceLeave(heldNs);VfsDrainMountCleanup();
    inode(heldRoot,heldCwd,"sub/value",201);
    VfsMountNamespaceHandleRelease(heldNs);VfsDrainMountCleanup();
    inode(heldRoot,heldCwd,"sub/value",201);
    VfsMountNamespaceHandleRelease(heldNs);
    assert(!VfsMountNamespaceHandleAcquire(heldNs)); // storage cannot revive retirement
    assert(VfsMountNamespacePrivatePropagation(heldNs,heldRoot)==-3);
    VfsDrainMountCleanup();
    VfsPath disappeared{};assert(VfsPathResolve(heldRoot,heldCwd,"sub/value",&disappeared)==-2);
    VfsPathRelease(heldRoot);VfsPathRelease(heldCwd);VfsMountNamespaceRelease(heldNs);VfsDrainMountCleanup();
    // Less-privileged user owner pins its identity and locks inherited
    // attachments; stacking a fresh bind does not unlock the covered mount.
    auto* userOwner=static_cast<UserNamespace*>(kmalloc(sizeof(UserNamespace)));*userOwner={};userOwner->owner=1000;
    MountNamespace* lockedNs=nullptr;VfsPath lockedRoot{},lockedCwd{};
    assert(!VfsMountNamespaceCopy(parentNs,parentRoot,parentTarget,&lockedNs,&lockedRoot,&lockedCwd));
    assert(!VfsMountNamespaceSetUserOwnerPrepared(lockedNs,userOwner));assert(userOwner->references==2);
    auto* ownerLease=VfsMountNamespaceUserOwnerAcquire(lockedNs);assert(ownerLease==userOwner);UserNamespaceRelease(ownerLease);
    assert(VfsMountNamespaceActivate(lockedNs));
    assert(VfsMountNamespaceSetUserOwnerPrepared(lockedNs,nullptr)==-22);
    auto lockedBind=resolve(lockedRoot,lockedRoot,"/ns-target");
    assert(!VfsMountNamespacePrivatePropagation(lockedNs,lockedBind));
    assert(VfsMountNamespaceDetach(lockedNs,lockedBind)==-22);
    assert(!VfsMountNamespaceBind(lockedNs,bPath,lockedBind));
    auto freshBind=resolve(lockedRoot,lockedRoot,"/ns-target");
    assert(!VfsMountNamespaceDetach(lockedNs,freshBind));VfsPathRelease(freshBind);
    assert(VfsMountNamespaceDetach(lockedNs,lockedBind)==-22);VfsPathRelease(lockedBind);
    VfsMountNamespaceLeave(lockedNs);VfsDrainMountCleanup();
    VfsPathRelease(lockedRoot);VfsPathRelease(lockedCwd);VfsMountNamespaceRelease(lockedNs);VfsDrainMountCleanup();
    assert(userOwner->references==1);UserNamespaceRelease(userOwner);
    MountNamespace* childNs=nullptr;VfsPath childRoot{},childCwd{};
    assert(!VfsMountNamespaceCopy(parentNs,parentRoot,parentTarget,&childNs,&childRoot,&childCwd));
    assert(VfsMountNamespaceActivate(childNs));
    assert(VfsMountNamespaceCount(childNs)==VfsMountNamespaceCount(parentNs));
    auto childSynthetic=resolve(childRoot,childRoot,"/synthetic");
    auto childSyntheticLeaf=resolve(childRoot,childRoot,"/synthetic/deep/mount");
    assert(directoryEntries(childSynthetic)["deep"]==1);
    assert(!VfsMountNamespaceDetach(childNs,childSyntheticLeaf));VfsPathRelease(childSyntheticLeaf);
    assert(!directoryEntries(childRoot).count("synthetic"));
    assert(directoryEntries(stableRoot)["synthetic"]==1 && directoryEntries(parentRoot)["synthetic"]==1);
    assert(directoryEntries(childSynthetic).empty());VfsPathRelease(childSynthetic);
    assert(VfsMountNamespaceCount(childNs)+1==VfsMountNamespaceCount(parentNs));
    auto parentBound=resolve(parentRoot,parentRoot,"/ns-target");
    assert(!VfsMountNamespaceBind(parentNs,bPath,parentBound));VfsPathRelease(parentBound);
    inode(childRoot,childRoot,"/ns-target/value",101);inode(parentRoot,parentRoot,"/ns-target/value",201);
    inode(childRoot,parentRoot,"ns-target/value",201); // foreign original namespace capability
    inode(childRoot,childCwd,"value",301); // cwd remapped without re-entering overmount
    auto bound=resolve(childRoot,childRoot,"/ns-target");expect(childRoot,bound,"..",childRoot);
    auto nestedTarget=resolve(childRoot,bound,"sub");assert(!VfsMountNamespaceBind(childNs,bPath,nestedTarget));VfsPathRelease(nestedTarget);
    auto nested=resolve(childRoot,bound,"sub");inode(childRoot,nested,"value",201);
    auto propagationAllocations=allocations.size();
    assert(!VfsMountNamespacePrivatePropagation(childNs,bound));
    assert(!VfsMountNamespacePrivatePropagation(childNs,nested));
    assert(allocations.size()==propagationAllocations);
    // Renaming the physical source of a bind must preserve its namespace
    // label, and the nested bind label beneath it.
    assert(!VfsRenameAt(parentRoot,parentRoot,"ns-a",parentRoot,"renamed-a"));
    assert(!VfsPathDirectoryPath(nested,copiedName,sizeof(copiedName)));
    assert(!std::strcmp(copiedName,"/ns-target/sub"));
    assert(!VfsRename("/renamed-a","/ns-a"));
    // File/no-op renames do not depend on mount reporting. Directory rename
    // currently fails honestly before mutation if a related backing's bind
    // ancestry cannot report its physical name.
    rejectDirectoryReport=100;
    assert(!VfsRename("/loose-file","/moved-file"));
    assert(!VfsRename("/rename-tree","/rename-tree"));
    assert(VfsRename("/rename-tree","/blocked-tree")==-95);
    assert(mounts[6].root.children.count("rename-tree") && !mounts[6].root.children.count("blocked-tree"));
    rejectDirectoryReport=0;
    assert(!VfsMountNamespaceDetach(childNs,bound));
    assert(VfsMountNamespacePrivatePropagation(childNs,bound)==-22);
    inode(childRoot,childRoot,"/ns-target/value",301);inode(childRoot,bound,"value",101);
    VfsPath absent{};assert(VfsPathResolve(childRoot,bound,"sub/value",&absent)==-2);
    expect(childRoot,bound,"..",bound);inode(childRoot,nested,"value",201);
    VfsPathRelease(nested);VfsPathRelease(bound);
    // Storage snapshots and foreign dirfds must not keep namespace publication.
    VfsMountNamespaceRetain(parentNs);
    VfsMountNamespaceLeave(parentNs);inode(childRoot,parentRoot,"ns-target/value",301);
    VfsDrainMountCleanup();inode(childRoot,parentRoot,"ns-target/value",301);
    VfsMountNamespaceRelease(parentNs);VfsMountNamespaceRelease(parentNs);
    VfsPathRelease(parentTarget);VfsPathRelease(parentCwd);VfsPathRelease(parentRoot);
    VfsMountNamespaceLeave(childNs);VfsDrainMountCleanup();VfsMountNamespaceRelease(childNs);
    VfsPathRelease(childCwd);VfsPathRelease(childRoot);VfsDrainMountCleanup();
    // Initial namespace binds must preserve legacy kernel/driver dispatch:
    // visible suffixes are relative to the arbitrary bind root, not FS root.
    assert(!VfsMountNamespaceBind(initial,aPath,originalTarget));
    auto* legacyBind=VfsOpen("/ns-target/value");assert(legacyBind);
    VnodeStat legacyBoundStat{};assert(!VfsStat(legacyBind,&legacyBoundStat)&&legacyBoundStat.ino==101);VfsClose(legacyBind);
    assert(!VfsStatPath("/ns-target/value",&legacyBoundStat)&&legacyBoundStat.ino==101);
    auto initialBound=resolve(stableRoot,stableRoot,"/ns-target");
    auto initialSub=resolve(stableRoot,initialBound,"sub");
    assert(!VfsMountNamespaceBind(initial,bPath,initialSub));VfsPathRelease(initialSub);
    legacyBind=VfsOpen("/ns-target/sub/value");assert(legacyBind);
    assert(!VfsStat(legacyBind,&legacyBoundStat)&&legacyBoundStat.ino==201);VfsClose(legacyBind);
    assert(!VfsMountNamespaceDetach(initial,initialBound));
    assert(!VfsStatPath("/ns-target/value",&legacyBoundStat)&&legacyBoundStat.ino==301);
    VfsPathRelease(initialBound);VfsDrainMountCleanup();
    VfsPathRelease(originalSynthetic);VfsPathRelease(originalTarget);VfsPathRelease(aPath);VfsPathRelease(bPath);
    assert(VfsUnmount("/synthetic/deep/mount"));VfsMountNamespaceRelease(initial);
    assert(VfsUnmount("/rename-tree/slot"));
    assert(!g_retiredMounts);
    VfsPathRelease(stableRoot);
    // Model the final-release window after the diagnostic count is cleared but
    // before its combined attachment lease is dropped. Backing must stay live.
    auto* releasingRoot=g_rootMount;
    VfsPathHoldMount(releasingRoot);
    __atomic_fetch_sub(&releasingRoot->pathHolds,uint64_t{1},__ATOMIC_RELEASE);
    assert(!VfsUnmount("/") && g_rootMount==releasingRoot && g_backings);
    VfsMountDrop(releasingRoot);
    assert(VfsUnmount("/"));
    assert(!g_backings && !g_mounts.count);
    assert(allocations.empty());std::puts("VFS_PATHS_PASS rename/overmount/stacked-dotdot/root-clamp/virtual/rollback/unsupported/open-transfer");
}
