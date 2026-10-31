#include "ahci_system_volume.h"
#include "ahci_readonly.h"
#include "device.h"
#include "vfs.h"
#include "mount_namespace.h"
#include "strict_gpt_inventory.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace brook;
namespace {
std::string scenario,record="BRKESP1\n01000000000000000000000000000000\n02000000000000000000000000000000\n2048,2048\n";
uint8_t mbr[512]{},primary[512]{},backup[512]{},entries[16384]{},copy[16384]{};
Vnode config{};Device* registered=nullptr;unsigned binds=0,mounts=0,reads=0,writes=0,allocations=0;
bool is(const char* s){return scenario==s;}
void put32(uint8_t* p,uint32_t v){for(int i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
void put64(uint8_t* p,uint64_t v){put32(p,uint32_t(v));put32(p+4,uint32_t(v>>32));}
void hdr(uint8_t* h,bool back,uint32_t crc){std::memcpy(h,"EFI PART",8);put32(h+8,0x10000);put32(h+12,92);
    put64(h+24,back?99999:1);put64(h+32,back?1:99999);put64(h+40,34);put64(h+48,99966);h[56]=1;
    put64(h+72,back?99967:2);put32(h+80,128);put32(h+84,128);put32(h+88,crc);put32(h+16,StrictCrc32(h,92,true));}
int DiskRead(Device*,uint64_t off,void* dst,uint64_t n){++reads;if(is("short-read") && off==512)return 0;
    const uint8_t* src=off==0?mbr:off==512?primary:off==99999ull*512?backup:off==2*512?entries:off==99967ull*512?copy:nullptr;
    if(src){std::memcpy(dst,src,n);return int(n);}std::memset(dst,0,n);return int(n);}
int DiskWrite(Device*,uint64_t,const void*,uint64_t n){++writes;return int(n);}
int DiskIoctl(Device*,uint32_t request,void*){return request==DEVICE_IOCTL_READ_ONLY?0:request==DEVICE_IOCTL_FLUSH?0:-95;}
uint64_t DiskBlocks(Device*){return 100000;}uint32_t DiskSize(Device*){return 512;}
BlockDeviceOps diskOps{DiskRead,DiskWrite,DiskIoctl,nullptr,DiskBlocks,DiskSize};
Device disk{reinterpret_cast<const DeviceOps*>(&diskOps),"ahci0",DeviceType::Block,nullptr};
}
namespace brook {
extern "C" void* kmalloc(uint64_t n){++allocations;return is("alloc")?nullptr:std::malloc(n);}
extern "C" void kfree(void* p){std::free(p);}
Device* AhciWriteAdmittedDevice(){return is("no-disk")?nullptr:&disk;}
extern "C" int VfsLstatPath(const char* path,VnodeStat* out){*out={};
    if(!std::strcmp(path,"/etc/ahci-boot-volume")){if(is("absent"))return -2;if(is("stat-error"))return -5;out->size=record.size();out->isSymlink=is("link");return 0;}
    assert(!std::strcmp(path,"/media/system"));if(is("missing-dir"))return -2;out->isDir=true;out->isSymlink=is("system-link");return 0;}
extern "C" Vnode* VfsOpen(const char* path,int flags){assert(!std::strcmp(path,"/etc/ahci-boot-volume")&&flags==VFS_O_STREAM);return is("open")?nullptr:&config;}
extern "C" int VfsRead(Vnode* n,void* out,uint64_t size,uint64_t* offset){assert(n==&config && !*offset);if(is("selector-short"))return 0;std::memcpy(out,record.data(),size);return int(size);}
void VfsRelease(Vnode* n){assert(n==&config);}
MountNamespace* VfsMountNamespaceInitial(){return reinterpret_cast<MountNamespace*>(1);}
uint32_t VfsMountNamespaceCount(MountNamespace*){return is("occupied")?1:0;}
VfsMountSnapshot VfsMountNamespaceSnapshotAt(MountNamespace*,uint32_t){VfsMountSnapshot s{};s.used=true;std::strcpy(s.mountPoint,"/MEDIA/SYSTEM/child");return s;}
extern "C" Device* DeviceFind(const char* name){assert(!std::strcmp(name,"ahci-system"));return is("duplicate")?&disk:nullptr;}
extern "C" bool DeviceRegister(Device* d){if(is("register"))return false;registered=d;return true;}
extern "C" int DeviceWrite(Device* d,uint64_t off,const void* p,uint64_t n){return d->ops->write(d,off,p,n);}
extern "C" int DeviceFlush(Device* d){return d->ops->ioctl(d,DEVICE_IOCTL_FLUSH,nullptr);}
bool FatFsBindDrive(uint8_t drive,Device* d){assert(drive==1 && d==registered);++binds;return !is("bind");}
bool VfsMountConfigured(const char* path,const char* type,uint8_t drive,uint64_t flags){assert(!std::strcmp(path,"/media/system")&&!std::strcmp(type,"fatfs")&&drive==1&&!flags);++mounts;return !is("mount");}
extern "C" void KPrintf(const char*,...){}
}
int main(int argc,char** argv){assert(argc==2);scenario=argv[1];
    mbr[510]=0x55;mbr[511]=0xaa;mbr[450]=0xee;put32(mbr+454,1);put32(mbr+458,99999);
    const uint8_t esp[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b};
    std::memcpy(entries,esp,16);entries[16]=2;put64(entries+32,2048);put64(entries+40,4095);std::memcpy(copy,entries,sizeof(entries));
    const auto crc=StrictCrc32(entries,sizeof(entries));hdr(primary,false,crc);hdr(backup,true,crc);
    if(is("bad-gpt"))backup[0]=0;if(is("wrong-guid"))record[8]='f';if(is("bad-record"))record[0]='x';
    uint8_t next=1;const auto result=AhciSystemVolumeMount(&next);
    if(is("absent")){assert(result==AhciSystemBoot::Absent && !reads && !writes);return 0;}
    if(is("ready")){
        assert(result==AhciSystemBoot::Ready && registered && binds==1 && mounts==1 && next==2 && !writes);
        uint8_t b[2]{};assert(registered->ops->write(registered,2048ull*512-1,b,2)==-22 && !writes);
        assert(registered->ops->write(registered,2048ull*512-2,b,2)==2 && writes==1);
    } else {
        assert(result==AhciSystemBoot::Maintenance && !writes);
        if(is("mount"))assert(registered && binds==1 && mounts==1 && next==2);
        else if(is("bind"))assert(registered && binds==1 && !mounts && next==1);
        else assert(!mounts && next==1);
    }
}
