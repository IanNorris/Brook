// Fault-inject boot publication/ownership around the production admission code.
// Header data comes from a real mkfs.ext2 file; USB/FAT/ext2 byte I/O itself has
// separate production-backend integration and private QEMU acceptance gates.
#include "writable_desktop.h"
#include "device.h"
#include "vfs.h"
#include "vfs_path.h"
#include "mount_namespace.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
namespace {
std::string scenario,record;
std::vector<uint8_t> disk;
brook::Vnode config{},marker{},source{};
unsigned opens=0,roots=0,leases=0,writes=0,flushes=0,cleans=0,unmounts=0,syncs=0;
bool registered=false,bound=false,mounted=false,homeBound=false,owner=false,target=false;
int Read(brook::Device*,uint64_t offset,void* p,uint64_t bytes){assert(offset+bytes<=disk.size());std::memcpy(p,disk.data()+offset,bytes);return int(bytes);}
int Write(brook::Device*,uint64_t offset,const void* p,uint64_t bytes){
    assert(offset==1082 && bytes==2);++writes;const bool clean=*static_cast<const uint8_t*>(p)==1;if(clean)++cleans;
    if((!clean&&scenario=="dirty-write")||(clean&&scenario=="clean-write"))return -5;
    std::memcpy(disk.data()+offset,p,bytes);return int(bytes);
}
int Control(brook::Device*,uint32_t,void*){return 0;}
brook::BlockDeviceOps ops{Read,Write,Control,nullptr,nullptr,nullptr};
brook::Device image{reinterpret_cast<const brook::DeviceOps*>(&ops),"desktop-image",brook::DeviceType::Block,nullptr};
bool Is(const char* value){return scenario==value;}
}
namespace brook {
extern "C" Vnode* VfsOpen(const char* path,int flags){
    Vnode* node=nullptr;
    if(!std::strcmp(path,"/etc/writable-desktop")){if(Is("absent")||Is("system-absent"))return nullptr;assert(flags==VFS_O_STREAM);node=&config;}
    else if(!std::strcmp(path,"/media/usb/STORE.OK") || !std::strcmp(path,"/media/system/STORE.OK")){
        assert((scenario.rfind("system-",0)==0)==(!std::strcmp(path,"/media/system/STORE.OK")));
        if(Is("no-marker")||Is("system-no-marker"))return nullptr;assert(flags==VFS_O_STREAM);node=&marker;}
    else {assert(!std::strcmp(path,scenario.rfind("system-",0)==0?"/media/system/STORE.IMG":"/media/usb/STORE.IMG"));assert(flags==(VFS_O_WRITE|VFS_O_READWRITE));node=&source;}
    ++opens;++node->refCount;return node;
}
extern "C" void VfsClose(Vnode* node){assert(opens&&node->refCount<=1);--opens;node->refCount=0;}
void VfsRelease(Vnode* node){assert(opens&&node->refCount);--opens;--node->refCount;}
int VfsStat(Vnode* node,VnodeStat* out){assert(node==&config||node==&marker);*out={};out->size=record.size();return 0;}
extern "C" int VfsRead(Vnode* node,void* out,uint64_t bytes,uint64_t* offset){
    assert(node==&config||node==&marker);assert(!*offset&&bytes==record.size());std::memcpy(out,record.data(),bytes);
    if((node==&config&&Is("bad-config"))||(node==&marker&&Is("bad-marker")))static_cast<char*>(out)[0]='X';
    *offset+=bytes;return int(bytes);
}
Device* FatImageDeviceCreate(Vnode* node,uint64_t bytes,const char*,int* error){
    assert(node==&source && node->refCount==1 && bytes==disk.size());
    if(Is("lease")){*error=-16;return nullptr;}
    ++leases;++source.refCount;return &image;
}
int FatImageDeviceDestroyUnregistered(Device* d){
    assert(d==&image&&!registered&&!bound);
    if(Is("dirty-write")||Is("dirty-flush"))return -5;
    assert(leases==1&&source.refCount==1);--leases;--source.refCount;return 0;
}
extern "C" bool DeviceRegister(Device* d){assert(d==&image&&leases);if(Is("register"))return false;registered=true;return true;}
extern "C" bool DeviceUnregister(Device* d){assert(d==&image&&registered);registered=false;return true;}
int DeviceWrite(Device* d,uint64_t at,const void* p,uint64_t bytes){return Write(d,at,p,bytes);}
int DeviceFlush(Device*){
    ++flushes;if((!cleans&&Is("dirty-flush"))||(cleans&&Is("clean-flush")))return -5;return 0;
}
bool DeviceFinalSyncOwner(){return owner;}
bool Ext2DriveAvailable(uint8_t drive){assert(drive==3);return !Is("slot");}
extern "C" bool Ext2BindDevice(uint8_t drive,Device* d){assert(drive==3&&d==&image&&registered);if(Is("bind"))return false;bound=true;return true;}
MountNamespace* VfsMountNamespaceInitial(){return reinterpret_cast<MountNamespace*>(1);}
uint32_t VfsMountNamespaceCount(MountNamespace*){return Is("occupied")?2:1;}
VfsMountSnapshot VfsMountNamespaceSnapshotAt(MountNamespace*,uint32_t i){
    VfsMountSnapshot row{};row.used=true;std::strcpy(row.mountPoint,i?"/nix":"/DATA");std::strcpy(row.fsType,i?"ext2":"tmpfs");return row;
}
int VfsPathRoot(VfsPath* out){++roots;*out={reinterpret_cast<void*>(1),reinterpret_cast<VfsObject*>(1)};return 0;}
void VfsPathRelease(VfsPath path){if(path.object){assert(roots);--roots;}}
int VfsStatAt(const VfsPath&,const VfsPath&,const char* path,VnodeStat* out,bool){
    assert(roots&&!std::strcmp(path,"/data/home"));if(!target)return -2;*out={};out->isDir=true;return 0;
}
int VfsMkdirAt(const VfsPath&,const VfsPath&,const char*,uint16_t,uint16_t){assert(roots);if(Is("target"))return -13;target=true;return 0;}
bool VfsMountConfigured(const char* path,const char* type,uint8_t drive,uint64_t attributes){
    assert(bound&&drive==3&&attributes==6&&!std::strcmp(path,"/nix")&&!std::strcmp(type,"ext2"));
    assert(StoreGet16(disk.data()+1082)==0 && flushes);if(Is("mount")||Is("mount-cleanup"))return false;
    mounted=true;return true;
}
extern "C" bool VfsUnmount(const char* path){assert(mounted&&!homeBound&&!std::strcmp(path,"/nix"));++unmounts;if(Is("unmount"))return false;mounted=false;return true;}
extern "C" int VfsLstatPath(const char*,VnodeStat* out){assert(mounted);*out={};out->isDir=true;out->uid=out->gid=1000;out->mode=0040755;
    return Is("payload")||Is("unmount")?-2:0;
}
extern "C" int VfsStatPath(const char*,VnodeStat* out){assert(mounted);*out={};out->size=1;out->mode=0100755;return 0;}
int VfsMountNamespaceBindAt(MountNamespace*,const VfsPath&,const VfsPath&,const char* from,const char* to,bool){
    assert(roots&&mounted&&target&&!std::strcmp(from,"/nix/home/brook")&&!std::strcmp(to,"/data/home"));
    if(Is("home"))return -12;homeBound=true;return 0;
}
void VfsDrainMountCleanup(){}
bool VfsMountCleanupPending(){return false;}
int VfsSync(Vnode*){++syncs;return Is("mount-cleanup")?-5:0;}
extern "C" void KPrintf(const char*,...){}
}
int main(int argc,char** argv){
    assert(argc==3);scenario=argv[2];std::ifstream file(argv[1],std::ios::binary);
    disk.assign(std::istreambuf_iterator<char>(file),{});assert(disk.size()==16*1024*1024);
    record="BRKSTORE1 16777216 cf16f1ae-68c5-5fac-89e9-382b8af5cd5c\n";
    if(Is("header"))disk[1080]=0;
    if(Is("unclean"))disk[1082]=0;
    uint8_t drive=3;const auto sourceVolume=scenario.rfind("system-",0)==0?brook::WritableDesktopSource::System:brook::WritableDesktopSource::Usb;
    const auto result=brook::WritableDesktopBootMount(&drive,sourceVolume);auto info=brook::WritableDesktopRead();
    assert(!opens&&!roots);
    const bool ready=Is("ready")||Is("system-ready")||Is("clean-write")||Is("clean-flush");
    if(ready){
        assert(result==brook::WritableDesktopBoot::Ready&&info.ready&&info.pinned&&info.dirty&&drive==4);
        assert(mounted&&homeBound&&bound&&leases==1&&source.refCount==1);
        assert(brook::WritableDesktopFinalSync()==-1&&!cleans);owner=true;
        assert(!brook::VfsSync(nullptr));const int error=brook::WritableDesktopFinalSync();info=brook::WritableDesktopRead();
        if(Is("ready")||Is("system-ready")){assert(!error&&info.clean&&!info.dirty&&brook::StoreGet16(disk.data()+1082)==1);}
        else assert(error==-5&&!info.clean&&info.finalError==-5&&info.stateUncertain);
    } else {
        if(Is("absent"))assert(result==brook::WritableDesktopBoot::Absent&&!info.configured);
        else if(Is("occupied")||Is("unmount")||Is("mount-cleanup")||Is("system-no-marker")||Is("system-absent"))assert(result==brook::WritableDesktopBoot::Maintenance);
        else assert(result==brook::WritableDesktopBoot::Fallback);
        assert(!info.ready);const auto before=writes;owner=true;assert(!brook::WritableDesktopFinalSync()&&writes==before&&!cleans);
        if(Is("payload")||Is("home"))assert(unmounts==1&&!mounted&&!homeBound&&info.dirty);
        if(Is("header")||Is("unclean"))assert(!leases&&!registered&&!writes);
        if(Is("bind")||Is("register"))assert(!leases&&!registered&&!bound);
        if(Is("dirty-write")||Is("dirty-flush"))assert(info.stateUncertain);
    }
}
