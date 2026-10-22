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
#include "process.h"
#include "process_fs.h"
#include "../../kernel/src/vfs.cpp"

namespace {
std::shared_mutex namespaceLock;
brook::VfsPath magicTarget{};
std::mutex allocationLock;
std::unordered_set<void*> allocations;
long failAllocation=-1;
unsigned schedulerLockDepth=0;
struct Mount;
struct Node { Mount* mount;uint64_t id;bool dir;Node* parent;std::map<std::string,Node*> children;std::string target; };
struct Mount { bool alive=false;unsigned live=0;unsigned closed=0;Node root{}; };
Mount mounts[8];
unsigned mountCalls[8]{},unmountCalls[8]{},syncCalls[8]{};
bool rejectRoot[8]{},rejectUnmount[8]{};
uint64_t rejectDirectoryReport=0;
bool rejectProvider=false;unsigned providerCalls=0;
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
    auto& n=node(object);assert(n.mount->alive);*stat={};stat->isDir=n.dir;stat->isSymlink=!n.target.empty();stat->ino=n.id;stat->dev=n.mount-mounts+1;stat->mode=n.dir?0040755:0100644;return 0;
}
int objectAccess(brook::VfsObject*,uint8_t,bool){return 0;}
int objectChild(brook::VfsObject* parent,const char* name,brook::VfsObject** out,bool=false){
    auto& n=node(parent);assert(n.mount->alive);*out=nullptr;auto it=n.children.find(name);return it==n.children.end()?-2:makeObject(it->second,out);
}
int objectParent(brook::VfsObject* object,brook::VfsObject** out,bool=false){return makeObject(node(object).parent,out);}
int objectReadlink(brook::VfsObject* object,char* out,uint32_t size){auto& t=node(object).target;if(t.size()>size)return -36;std::memcpy(out,t.data(),t.size());return t.size();}
void nodeClose(brook::Vnode* vn){assert(node(vn->backendObject).mount->alive);++node(vn->backendObject).mount->closed;}
int nodeStat(brook::Vnode* vn,brook::VnodeStat* out){return objectStat(vn->backendObject,out);}
int nodeTruncate(brook::Vnode*,uint64_t){return -5;}
const brook::VnodeOps nodeOps={.open=nullptr,.read=nullptr,.write=nullptr,.readdir=nullptr,.close=nodeClose,.stat=nodeStat,.fsync=nullptr,.truncate=nodeTruncate};
brook::Vnode* objectOpen(brook::VfsObject* object,int,int* error){
    *error=0;auto* vn=static_cast<brook::Vnode*>(brook::kmalloc(sizeof(brook::Vnode)));
    if(!vn){*error=-12;return nullptr;}
    *vn={};vn->ops=&nodeOps;vn->type=node(object).dir?brook::VnodeType::Dir:brook::VnodeType::File;vn->refCount=1;
    vn->backendObject=object;brook::VfsObjectRetain(object);return vn;
}
int objectFollow(brook::VfsObject* object,brook::VfsPath* out) {
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
const brook::VfsObjectOps objectOps={.destroy=destroyObject,.stat=objectStat,.access=objectAccess,
    .lookup=objectChild,.parent=objectParent,.readlink=objectReadlink,.open=objectOpen,.rename=objectRename,.directory_path=objectDirectoryPath,.follow_link=objectFollow};
bool mountBackend(uint8_t drive,void** out){assert(drive<8 && !mounts[drive].alive);auto& m=mounts[drive];++mountCalls[drive];m.alive=true;m.root={&m,1,true,&m.root,{}, {}};*out=&m;return true;}
int unmountBackend(void* priv){auto* m=static_cast<Mount*>(priv);assert(m->alive && !m->live);auto index=m-mounts;++unmountCalls[index];if(rejectUnmount[index])return -5;m->alive=false;return 0;}
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
    // This fixture needs root-level ancestor renames only.
    std::string oldName(oldPath),newName(newPath);
    if(oldName[0]=='/')oldName.erase(0,1);if(newName[0]=='/')newName.erase(0,1);
    assert(oldName.find('/')==std::string::npos && newName.find('/')==std::string::npos);
    auto& children=static_cast<Mount*>(priv)->root.children;auto it=children.find(oldName);
    if(it==children.end())return -2;
    Node* original=it->second;children.erase(it);children[newName]=original;return 0;
}
int syncBackend(void* priv){auto* m=static_cast<Mount*>(priv);assert(m->alive);++syncCalls[m-mounts];return 0;}
const brook::VfsFsOps fsOps={.mount=mountBackend,.unmount=unmountBackend,.open=legacyOpen,.stat_path=legacyStat,.lstat_path=legacyStat,.unlink=nullptr,.mkdir=nullptr,.rename=legacyRename,.symlink=nullptr,.readlink=nullptr,.sync=syncBackend,.root_object=rootObject,.vnode_object=vnodeObject};
const brook::VfsFsOps unsupportedOps={.mount=mountBackend,.unmount=unmountBackend,.open=nullptr,.stat_path=nullptr,.lstat_path=nullptr,.unlink=nullptr,.mkdir=nullptr,.rename=nullptr,.symlink=nullptr,.readlink=nullptr,.sync=nullptr};
brook::VfsPath resolve(const brook::VfsPath& root,const brook::VfsPath& start,const char* text){brook::VfsPath out{};int error=brook::VfsPathResolve(root,start,text,&out);if(error)std::fprintf(stderr,"resolve %s: %d\n",text,error);assert(!error);return out;}
void expect(const brook::VfsPath& root,const brook::VfsPath& start,const char* text,const brook::VfsPath& expected){auto p=resolve(root,start,text);assert(brook::VfsPathSame(p,expected));brook::VfsPathRelease(p);}
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
int FileObjectWriteback(FileObject*,Vnode*,uint64_t,uint64_t){assert(false);return -5;}
int FileObjectTruncateUnmapped(FileObject*,Vnode*,uint64_t){assert(false);return -5;}
}
#include "scheduler_membership_fixture.inc"
// Actual Process publication helpers and FsContext core. These exercise owned
// context handoff/failure/races; native acceptance covers actual clone/exec.
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
    MountNamespace* parentNs=nullptr;VfsPath parentRoot{},parentCwd{};
    assert(!VfsMountNamespaceCopy(initial,stableRoot,originalSynthetic,&parentNs,&parentRoot,&parentCwd));
    assert(VfsMountNamespaceBind(parentNs,aPath,parentCwd)==-3);
    assert(VfsMountNamespaceActivate(parentNs));
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
    MountNamespace* childNs=nullptr;VfsPath childRoot{},childCwd{};
    assert(!VfsMountNamespaceCopy(parentNs,parentRoot,parentTarget,&childNs,&childRoot,&childCwd));
    assert(VfsMountNamespaceActivate(childNs));
    auto parentBound=resolve(parentRoot,parentRoot,"/ns-target");
    assert(!VfsMountNamespaceBind(parentNs,bPath,parentBound));VfsPathRelease(parentBound);
    inode(childRoot,childRoot,"/ns-target/value",101);inode(parentRoot,parentRoot,"/ns-target/value",201);
    inode(childRoot,parentRoot,"ns-target/value",201); // foreign original namespace capability
    inode(childRoot,childCwd,"value",301); // cwd remapped without re-entering overmount
    auto bound=resolve(childRoot,childRoot,"/ns-target");expect(childRoot,bound,"..",childRoot);
    auto nestedTarget=resolve(childRoot,bound,"sub");assert(!VfsMountNamespaceBind(childNs,bPath,nestedTarget));VfsPathRelease(nestedTarget);
    auto nested=resolve(childRoot,bound,"sub");inode(childRoot,nested,"value",201);
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
