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
#include "../../kernel/src/vfs.cpp"

namespace {
std::shared_mutex namespaceLock;
std::mutex allocationLock;
std::unordered_set<void*> allocations;
long failAllocation=-1;
struct Mount;
struct Node { Mount* mount;uint64_t id;bool dir;Node* parent;std::map<std::string,Node*> children;std::string target; };
struct Mount { bool alive=false;unsigned live=0;unsigned closed=0;Node root{}; };
Mount mounts[8];
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
int objectChild(brook::VfsObject* parent,const char* name,brook::VfsObject** out){
    auto& n=node(parent);assert(n.mount->alive);*out=nullptr;auto it=n.children.find(name);return it==n.children.end()?-2:makeObject(it->second,out);
}
int objectParent(brook::VfsObject* object,brook::VfsObject** out){return makeObject(node(object).parent,out);}
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
const brook::VfsObjectOps objectOps={destroyObject,objectStat,objectAccess,objectChild,objectParent,objectReadlink,objectOpen};
bool mountBackend(uint8_t drive,void** out){assert(drive<8 && !mounts[drive].alive);auto& m=mounts[drive];m.alive=true;m.root={&m,1,true,&m.root,{}, {}};*out=&m;return true;}
int unmountBackend(void* priv){auto* m=static_cast<Mount*>(priv);assert(m->alive && !m->live);m->alive=false;return 0;}
int rootObject(void* priv,uint8_t,brook::VfsObject** out){return makeObject(&static_cast<Mount*>(priv)->root,out);}
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
const brook::VfsFsOps fsOps={.mount=mountBackend,.unmount=unmountBackend,.open=legacyOpen,.stat_path=legacyStat,.lstat_path=legacyStat,.unlink=nullptr,.mkdir=nullptr,.rename=legacyRename,.symlink=nullptr,.readlink=nullptr,.sync=nullptr,.root_object=rootObject,.vnode_object=vnodeObject};
const brook::VfsFsOps unsupportedOps={.mount=mountBackend,.unmount=unmountBackend,.open=nullptr,.stat_path=nullptr,.lstat_path=nullptr,.unlink=nullptr,.mkdir=nullptr,.rename=nullptr,.symlink=nullptr,.readlink=nullptr,.sync=nullptr};
brook::VfsPath resolve(const brook::VfsPath& root,const brook::VfsPath& start,const char* text){brook::VfsPath out{};int error=brook::VfsPathResolve(root,start,text,&out);if(error)std::fprintf(stderr,"resolve %s: %d\n",text,error);assert(!error);return out;}
void expect(const brook::VfsPath& root,const brook::VfsPath& start,const char* text,const brook::VfsPath& expected){auto p=resolve(root,start,text);assert(brook::VfsPathSame(p,expected));brook::VfsPathRelease(p);}
}
namespace brook {
extern "C" void* kmalloc(uint64_t size){if(failAllocation==0){failAllocation=-1;return nullptr;}if(failAllocation>0)--failAllocation;void* p=std::malloc(size);if(p){std::memset(p,0xcc,size);std::lock_guard<std::mutex> lock(allocationLock);allocations.insert(p);}return p;}
extern "C" void kfree(void* p){if(p){std::lock_guard<std::mutex> lock(allocationLock);assert(allocations.erase(p)==1);std::free(p);}}
void KRwLockInit(KRwLock* lock){*lock={};}
void KRwLockReadLock(KRwLock*){namespaceLock.lock_shared();}
void KRwLockReadUnlock(KRwLock*){namespaceLock.unlock_shared();}
void KRwLockWriteLock(KRwLock*){namespaceLock.lock();}
void KRwLockWriteUnlock(KRwLock*){namespaceLock.unlock();}
extern "C" void SerialPrintf(const char*,...){}
void SerialPuts(const char*){}
int FileObjectTruncateUnmapped(FileObject*,Vnode*,uint64_t){assert(false);return -5;}
}
int main(){
    using namespace brook;
    VfsInit();assert(VfsRegisterFs("objects",&fsOps));assert(VfsRegisterFs("unsupported",&unsupportedOps));
    assert(VfsMount("/","objects",0));assert(!VfsMount("/","objects",1));
    Node directory{&mounts[0],2,true,&mounts[0].root,{},{}}, marker{&mounts[0],3,false,&directory,{},{}}, poison{&mounts[0],4,true,&mounts[0].root,{},{}}, file{&mounts[0],5,false,&mounts[0].root,{},{}}, link{&mounts[0],6,false,&directory,{},"/dir/marker"};
    mounts[0].root.children={{"dir",&directory},{"file",&file}};directory.children={{"marker",&marker},{"absolute",&link}};
    VfsPath root{},held{};assert(!VfsPathRoot(&root));held=resolve(root,root,"/dir");
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
    for(long failure=0;failure<4;++failure){
        const auto before=allocations.size();failAllocation=failure;
        assert(!VfsMount("/new/child","objects",2));failAllocation=-1;
        assert(!mounts[2].alive && allocations.size()==before);
    }
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
    assert(allocations.empty());std::puts("VFS_PATHS_PASS rename/overmount/stacked-dotdot/root-clamp/virtual/rollback/unsupported/open-transfer");
}
