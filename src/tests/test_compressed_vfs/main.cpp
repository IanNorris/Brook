#include <array>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include "fs/compressed_vfs.h"
#include "fs/compressed_image.h"
#include "vfs.h"
#include "vfs_object.h"
#include "device.h"
#include "process.h"
#include "sync/kmutex.h"
#include "memory/file_page_cache.h"
using namespace brook;
namespace { std::atomic<int> allocations{0};int failAfter=-1;uint32_t actor=1000;std::mutex io;Device* registered=nullptr;
Vnode sourceNode{};Device* fileDevice=nullptr;bool sourceTrusted=true,sourceOpen=false;
unsigned sourceCloses=0;void* fileMount=nullptr;bool mountAccepted=true;int shortRead=0; }
namespace brook {
extern "C" void* kmalloc(uint64_t size){if(failAfter==0){failAfter=-1;return nullptr;}if(failAfter>0)--failAfter;auto* p=malloc(size);if(p)++allocations;return p;}
extern "C" void kfree(void* p){if(p){--allocations;free(p);}}
Process* ProcessCurrent(){return nullptr;}
int ProcessCheckFileAccess(Process*,uint32_t owner,uint32_t,uint16_t mode,uint8_t required,bool){unsigned bits=(mode>>(owner==actor?6:0))&7;return (bits&required)==required?0:-13;}
void KMutexInit(KMutex*){}
void KMutexLock(KMutex*){io.lock();}
void KMutexUnlock(KMutex*){io.unlock();}
extern "C" bool DeviceIsRegistered(Device* d){return d==registered;}
extern "C" void SerialPrintf(const char*,...){}
Vnode* VfsOpen(const char*,int flags){assert(flags==VFS_O_STREAM && !sourceOpen);sourceOpen=true;return &sourceNode;}
void VfsClose(Vnode* node){assert(node==&sourceNode && sourceOpen);sourceOpen=false;++sourceCloses;}
bool FatFsFileSourceReadOnly(Vnode* node){return node==&sourceNode && sourceOpen && sourceTrusted;}
int VfsStat(Vnode*,VnodeStat* out){*out={};out->size=DeviceBlockCount(fileDevice)*512;return 0;}
int VfsRead(Vnode* node,void* out,uint64_t bytes,uint64_t* at){assert(node==&sourceNode && sourceOpen);if(shortRead)return shortRead;int n=fileDevice->ops->read(fileDevice,*at,out,bytes);if(n>0)*at+=n;return n;}
bool VfsMountConfigured(const char*,const char*,uint8_t slot,uint64_t attributes){assert(attributes==7);return mountAccepted && CompressedVfsOperations()->mount(slot,&fileMount);}
}
struct Disk {
    std::vector<uint8_t> bytes;int error=0;unsigned reads=0;
    static int Read(Device* d,uint64_t at,void* out,uint64_t n){auto& s=*static_cast<Disk*>(d->priv);++s.reads;if(s.error)return s.error;assert(at<=s.bytes.size() && n<=s.bytes.size()-at);memcpy(out,s.bytes.data()+at,n);return n;}
    static uint64_t Count(Device* d){return static_cast<Disk*>(d->priv)->bytes.size()/512;}
    static uint32_t Size(Device*){return 512;}
};
static void hash(Disk& d){auto& h=*reinterpret_cast<CompressedImageHeader*>(d.bytes.data());h.metadataChecksum=CompressedImageChecksum(d.bytes.data()+160,h.dataOffset-160);h.headerChecksum=CompressedImageChecksum(&h,136);}
static Disk fixture() {
    Disk d;d.bytes.resize(4096+65536+4096);auto& h=*reinterpret_cast<CompressedImageHeader*>(d.bytes.data());
    h={};h.magic=COMPRESSED_IMAGE_MAGIC;h.version=1;h.headerBytes=160;h.imageBytes=d.bytes.size();h.blockBytes=65536;h.identity[0]=1;
    h.inodeOffset=160;h.inodeCount=5;h.direntOffset=480;h.direntCount=5;h.blockOffset=560;h.blockCount=2;h.stringOffset=624;h.stringBytes=20;h.dataOffset=4096;
    memcpy(d.bytes.data()+624,"dirfilehardlinkempty",20);
    auto* n=reinterpret_cast<CompressedImageInode*>(d.bytes.data()+160);
    n[0]={CompressedImageKind::Directory,0040755,1000,1000,0,0,0,0,0,0,0};
    n[1]={CompressedImageKind::File,0100644,1000,1000,65553,0,2,0,0,0,0};
    n[2]={CompressedImageKind::Symlink,0120777,1000,1000,4,0,0,3,4,0,0};
    n[3]={CompressedImageKind::Directory,0040700,1000,1000,0,0,0,0,0,0,0};
    n[4]={CompressedImageKind::File,0100644,1000,1000,0,2,0,0,0,0,0};
    auto* e=reinterpret_cast<CompressedImageDirent*>(d.bytes.data()+480);
    e[0]={0,3,0,3,0};e[1]={0,1,3,4,0};e[2]={0,1,7,4,0};e[3]={0,2,11,4,0};e[4]={3,4,15,5,0};
    for(unsigned i=0;i<65553;++i)d.bytes[4096+i]=i*73+(i>>8);
    auto* b=reinterpret_cast<CompressedImageBlock*>(d.bytes.data()+560);
    b[0]={4096,65536,65536,0,0,CompressedImageChecksum(d.bytes.data()+4096,65536)};
    b[1]={4096+65536,17,17,0,0,CompressedImageChecksum(d.bytes.data()+4096+65536,17)};
    hash(d);return d;
}
static void close(Vnode* vn){auto* object=vn->backendObject;vn->ops->close(vn);kfree(vn);VfsObjectRelease(object);}
int main() {
    auto disk=fixture();BlockDeviceOps deviceOps{Disk::Read,nullptr,nullptr,nullptr,Disk::Count,Disk::Size};
    Device device{reinterpret_cast<const DeviceOps*>(&deviceOps),"image",DeviceType::Block,&disk};registered=&device;
    auto* fs=CompressedVfsOperations();assert(CompressedVfsBindDevice(0,&device));
    // Every fallible allocation in mount preparation must unwind completely.
    for(int fail=0;fail<6;++fail){failAfter=fail;void* p=nullptr;assert(!fs->mount(0,&p));assert(!allocations);failAfter=-1;}
    disk.error=-5;void* failed=nullptr;assert(!fs->mount(0,&failed));disk.error=0;assert(!allocations);
    void* mount=nullptr;assert(fs->mount(0,&mount));assert(!CompressedVfsBindDevice(0,&device));
    VfsObject* root=nullptr;assert(fs->root_object(mount,0,&root)==0);
    VfsObject *file=nullptr,*hard=nullptr,*dir=nullptr,*link=nullptr;
    assert(root->ops->lookup(root,"file",&file,false)==0 && root->ops->lookup(root,"hard",&hard,false)==0 && file->identity==hard->identity);
    assert(root->ops->lookup(root,"dir",&dir,false)==0 && root->ops->lookup(root,"link",&link,false)==0);
    VnodeStat st{};assert(file->ops->stat(file,&st)==0 && st.nlink==2 && st.size==65553);assert(root->ops->stat(root,&st)==0 && st.nlink==3);
    char name[256],path[4096];VfsObject* parent=nullptr;assert(hard->ops->entry_parent(hard,&parent,name,sizeof(name))==0 && !strcmp(name,"hard") && parent->identity==root->identity);VfsObjectRelease(parent);
    assert(hard->ops->object_path(hard,path,sizeof(path))==0 && !strcmp(path,"/hard"));
    assert(dir->ops->directory_path(dir,path,sizeof(path))==0 && !strcmp(path,"/dir"));
    assert(link->ops->readlink(link,name,sizeof(name))==4 && !memcmp(name,"file",4));
    actor=2000;VfsObject* denied=nullptr;assert(dir->ops->lookup(dir,"empty",&denied,false)==-13 && !denied);
    assert(dir->ops->stat(dir,&st)==0 && fs->check_access(mount,&st,1)==-13 && fs->check_access_real(mount,&st,1)==-13);actor=1000;
    int error=0;assert(!file->ops->open(file,VFS_O_WRITE,&error) && error==-30);assert(!file->ops->open(file,VFS_O_EXEC,&error) && error==-13);
    assert(file->ops->chmod(file,0777)==-30 && file->ops->chown(file,0,0)==-30);
    assert(!root->ops->create(root,"new",0,0644,&error) && error==-30);
    assert(fs->mkdir(mount,0,"new",0755)==-30 && fs->remove_path(mount,0,"dir",true)==-30);
    auto* vn=file->ops->open(file,0,&error);assert(vn && !error && vn->cacheId==file->identity);
    VfsObject* converted=nullptr;assert(fs->vnode_object(vn,&converted)==0 && converted->identity==file->identity);VfsObjectRelease(converted);
    std::array<uint8_t,32> bytes{};uint64_t at=65530;assert(vn->ops->read(vn,bytes.data(),32,&at)==23 && at==65553);
    for(unsigned i=0;i<23;++i)assert(bytes[i]==uint8_t((65530+i)*73+((65530+i)>>8)));
    at=0;assert(vn->ops->read(vn,bytes.data(),32,&at)==32);auto reads=disk.reads;assert(vn->ops->read(vn,bytes.data(),32,&at)==32 && reads==disk.reads);
    std::thread workers[2];
    for(unsigned worker=0;worker<2;++worker)workers[worker]=std::thread([&,worker]{
        for(unsigned turn=0;turn<200;++turn) {
            uint64_t pos=worker?65536:0;std::array<uint8_t,32> buffer{};
            int expected=worker?17:32;assert(vn->ops->read(vn,buffer.data(),32,&pos)==expected);
            for(int i=0;i<expected;++i){unsigned offset=(worker?65536:0)+i;assert(buffer[i]==uint8_t(offset*73+(offset>>8)));}
        }
    });
    for(auto& worker:workers)worker.join();
    VfsSpace space{};assert(vn->ops->space(vn,&space)==0 && space.flags==1);
    auto* directory=root->ops->open(root,0,&error);assert(directory);uint32_t cookie=0;DirEntry entry{};unsigned count=0;
    while(directory->ops->readdir(directory,&entry,&cookie)>0)++count;assert(count==6);close(directory);
    // Unmount retires the pathname backing reference, not retained handles.
    auto identity=file->identity;assert(fs->unmount(mount)==0);assert(!CompressedVfsBindDevice(0,&device));
    at=1;assert(vn->ops->read(vn,bytes.data(),32,&at)==32);
    VfsObjectRelease(root);VfsObjectRelease(file);VfsObjectRelease(hard);VfsObjectRelease(dir);VfsObjectRelease(link);close(vn);assert(!allocations);
    assert(CompressedVfsBindDevice(0,&device));assert(fs->mount(0,&mount));assert(fs->root_object(mount,0,&root)==0);
    assert(root->ops->lookup(root,"file",&file,false)==0 && file->identity!=identity);VfsObjectRelease(file);VfsObjectRelease(root);assert(fs->unmount(mount)==0 && !allocations);
    // Production file-backed path: early rejection, failed-mount ownership,
    // retained handles after detach, and deferred close outside namespace locks.
    fileDevice=&device;
    sourceTrusted=false;unsigned closes=sourceCloses;
    assert(!CompressedVfsMountFile("/source","/nix") && !sourceOpen && sourceCloses==closes+1);
    sourceTrusted=true;mountAccepted=false;
    assert(!CompressedVfsMountFile("/source","/nix") && !sourceOpen);mountAccepted=true;
    for(int fail=0;fail<6;++fail){failAfter=fail;closes=sourceCloses;
        assert(!CompressedVfsMountFile("/source","/nix") && !sourceOpen && !allocations && sourceCloses==closes+1);failAfter=-1;}
    for(int failure : {-5,1}){shortRead=failure;assert(!CompressedVfsMountFile("/source","/nix") && !sourceOpen && !allocations);}shortRead=0;
    assert(CompressedVfsMountFile("/source","/nix") && sourceOpen);
    assert(fs->root_object(fileMount,0,&root)==0 && root->ops->lookup(root,"file",&file,false)==0);
    vn=file->ops->open(file,0,&error);assert(vn);VfsObjectRelease(file);VfsObjectRelease(root);
    closes=sourceCloses;assert(!fs->unmount(fileMount) && sourceOpen);
    CompressedVfsCollectRetired();assert(sourceOpen && sourceCloses==closes);
    at=0;assert(vn->ops->read(vn,bytes.data(),32,&at)==32);
    close(vn);assert(CompressedVfsCleanupPending() && sourceOpen);
    CompressedVfsCollectRetired();assert(!sourceOpen && sourceCloses==closes+1 && !allocations && !CompressedVfsCleanupPending());
    puts("compressed VFS: OOM cleanup/permissions/readonly/hardlinks/cache/retained-unmount/remount identities pass");
}
