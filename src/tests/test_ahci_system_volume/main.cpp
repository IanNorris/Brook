#include "ahci_system_volume.h"
#include "ahci_readonly.h"
#include "device.h"
#include "vfs.h"
#include "mount_namespace.h"
#include "strict_gpt_inventory.h"
#include "block_write_leases.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace brook;
namespace {
std::string scenario,record="BRKESP1\n01000000000000000000000000000000\n02000000000000000000000000000000\n2048,2048\n";
std::string dataRecord="BRKDATA1\n01000000000000000000000000000000\n03000000000000000000000000000000\n8192,2048\n04000000000000000000000000000000\n",lastLog;
uint8_t mbr[512]{},primary[512]{},backup[512]{},entries[16384]{},copy[16384]{};
uint8_t superblock[1024]{},gdt[32]{};
Vnode config{},dataConfig{};Device* registered=nullptr;Device* dataDevice=nullptr;unsigned binds=0,mounts=0,reads=0,writes=0,allocations=0,frees=0,failAllocation=0,leaseCalls=0;
BlockWriteLeases guardLedger;
bool is(const char* s){return scenario==s;}
bool probe(){return scenario.rfind("probe-",0)==0;}
bool guard(){return scenario.rfind("guard-",0)==0;}
bool direct(){return scenario.rfind("direct-",0)==0 || guard();}
void put16(uint8_t* p,uint16_t v){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
void put32(uint8_t* p,uint32_t v){for(int i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
void put64(uint8_t* p,uint64_t v){put32(p,uint32_t(v));put32(p+4,uint32_t(v>>32));}
void hdr(uint8_t* h,bool back,uint32_t crc){std::memcpy(h,"EFI PART",8);put32(h+8,0x10000);put32(h+12,92);
    put64(h+24,back?99999:1);put64(h+32,back?1:99999);put64(h+40,34);put64(h+48,99966);h[56]=1;
    put64(h+72,back?99967:2);put32(h+80,128);put32(h+84,128);put32(h+88,crc);put32(h+16,StrictCrc32(h,92,true));}
int DiskRead(Device*,uint64_t off,void* dst,uint64_t n){++reads;if(is("short-read") && off==512)return 0;
    if(guard() && off==2048ull*512+3) {if(is("guard-read-error"))return -5;std::memset(dst,0x42,n);return int(n);}
    if((is("probe-readfail") || is("guard-super-readfail")) && off==8192ull*512+1024)return -5;
    if(is("guard-super-short") && off==8192ull*512+1024)return int(n)-1;
    if(off==8192ull*512+1024){assert(n==sizeof(superblock));std::memcpy(dst,superblock,n);return int(n);}
    if(off==8192ull*512+4096){assert(n==sizeof(gdt));std::memcpy(dst,gdt,n);return int(n);}
    const uint8_t* src=off==0?mbr:off==512?primary:off==99999ull*512?backup:off==2*512?entries:off==99967ull*512?copy:nullptr;
    if(src){std::memcpy(dst,src,n);return int(n);}return -5;}
int DiskWrite(Device*,uint64_t,const void*,uint64_t n){++writes;return int(n);}
int DiskIoctl(Device*,uint32_t request,void*){return request==DEVICE_IOCTL_READ_ONLY?0:request==DEVICE_IOCTL_FLUSH?(is("guard-flush-error")?-5:0):-95;}
uint64_t DiskBlocks(Device*){return 100000;}uint32_t DiskSize(Device*){return 512;}
BlockDeviceOps diskOps{DiskRead,DiskWrite,DiskIoctl,nullptr,DiskBlocks,DiskSize};
Device disk{reinterpret_cast<const DeviceOps*>(&diskOps),"ahci0",DeviceType::Block,nullptr};
}
namespace brook {
extern "C" void* kmalloc(uint64_t n){++allocations;return is("alloc") || allocations==failAllocation?nullptr:std::malloc(n);}
extern "C" void kfree(void* p){++frees;std::free(p);}
Device* AhciWriteAdmittedDevice(){return is("no-disk") || is("guard-no-disk")?nullptr:&disk;}
bool DeviceInstallBootWriteLeases(const BlockWriteLease* leases,uint32_t n){
    if(guard() && !is("guard-after-normal")) {
        assert(n==4 && registered && !binds && !mounts && !writes);
        uint8_t byte=0;assert(registered->ops->write(registered,0,&byte,1)==-19);
        assert(registered->ops->read(registered,0,&byte,1)==-19 && !writes);
        assert(!leases[1].owner && leases[1].start==8192*512ULL);
        assert(leases[3].owner==reinterpret_cast<uintptr_t>(registered) && leases[3].start==2048*512ULL && leases[3].length==2048*512ULL);
        ++leaseCalls;return !is("guard-lease-fail") && guardLedger.Install(leases,n,true);
    }
    assert(direct() && n==3 && registered && dataDevice && !binds && !mounts);
    assert(leases[0].start==0 && leases[0].length==34*512ULL && !leases[0].owner);
    assert(leases[1].start==8192*512ULL && leases[1].length==2048*512ULL && leases[1].owner==reinterpret_cast<uintptr_t>(dataDevice));
    assert(leases[2].start==99967*512ULL && leases[2].length==33*512ULL && !leases[2].owner);
    ++leaseCalls;return !is("direct-lease-fail");}
int DeviceWriteLeased(Device* parent,Device* owner,uint64_t off,const void* p,uint64_t n){
    if(!guard() || !guardLedger.AllowOwner(reinterpret_cast<uintptr_t>(parent),reinterpret_cast<uintptr_t>(owner),off,n))return -30;
    return parent->ops->write(parent,off,p,n);
}
extern "C" int VfsLstatPath(const char* path,VnodeStat* out){*out={};
    if(!std::strcmp(path,"/etc/ahci-data-volume")){if(!direct())return -2;out->size=dataRecord.size();return 0;}
    if(!std::strcmp(path,"/etc/ahci-data-inspect")){if(!probe() || is("probe-absent"))return -2;out->size=dataRecord.size();out->isSymlink=is("probe-link");return 0;}
    if(!std::strcmp(path,"/etc/ahci-boot-volume")){if(is("absent") || is("guard-absent"))return -2;if(is("stat-error") || is("guard-stat-error"))return -5;out->size=record.size();out->isSymlink=is("link") || is("guard-link");return 0;}
    assert(!std::strcmp(path,"/media/system"));if(is("missing-dir"))return -2;out->isDir=true;out->isSymlink=is("system-link");return 0;}
extern "C" Vnode* VfsOpen(const char* path,int flags){assert(flags==VFS_O_STREAM);
    if(!std::strcmp(path,"/etc/ahci-data-volume"))return &dataConfig;
    if(!std::strcmp(path,"/etc/ahci-data-inspect"))return is("probe-open")?nullptr:&dataConfig;
    assert(!std::strcmp(path,"/etc/ahci-boot-volume"));return is("open") || is("guard-open")?nullptr:&config;}
extern "C" int VfsRead(Vnode* n,void* out,uint64_t size,uint64_t* offset){assert((n==&config || n==&dataConfig) && !*offset);if(is("selector-short") || is("guard-selector-short"))return 0;
    const auto& s=n==&dataConfig?dataRecord:record;std::memcpy(out,s.data(),size);return int(size);}
void VfsRelease(Vnode* n){assert(n==&config || n==&dataConfig);}
MountNamespace* VfsMountNamespaceInitial(){return reinterpret_cast<MountNamespace*>(1);}
uint32_t VfsMountNamespaceCount(MountNamespace*){return is("occupied")?1:0;}
VfsMountSnapshot VfsMountNamespaceSnapshotAt(MountNamespace*,uint32_t){VfsMountSnapshot s{};s.used=true;std::strcpy(s.mountPoint,"/MEDIA/SYSTEM/child");return s;}
extern "C" Device* DeviceFind(const char* name){if(!std::strcmp(name,"ahci-data"))return nullptr;if(!std::strcmp(name,"ahci-guard-esp"))return is("guard-duplicate")?&disk:nullptr;assert(!std::strcmp(name,"ahci-system"));return is("duplicate")?&disk:nullptr;}
extern "C" bool DeviceRegister(Device* d){if(is("register") || is("guard-register"))return false;if(!registered)registered=d;else dataDevice=d;return true;}
extern "C" int DeviceWrite(Device* d,uint64_t off,const void* p,uint64_t n){if(guard() && !guardLedger.AllowOrdinary(reinterpret_cast<uintptr_t>(d),off,n))return -30;return d->ops->write(d,off,p,n);}
extern "C" int DeviceFlush(Device* d){return d->ops->ioctl(d,DEVICE_IOCTL_FLUSH,nullptr);}
bool FatFsBindDrive(uint8_t drive,Device* d){assert(drive==1 && d==registered);++binds;return !is("bind");}
bool VfsMountConfiguredWithPublication(const char* path,const char* type,uint8_t drive,uint64_t flags,uint64_t* publication){assert(!std::strcmp(path,"/media/system")&&!std::strcmp(type,"fatfs")&&drive==1&&!flags);++mounts;*publication=is("mount")?0:42;return !is("mount");}
bool VfsMountConfigured(const char* path,const char* type,uint8_t drive,uint64_t flags){uint64_t unused;return VfsMountConfiguredWithPublication(path,type,drive,flags,&unused);}
extern "C" void KPrintf(const char* format,...){char buffer[256];va_list args;va_start(args,format);std::vsnprintf(buffer,sizeof(buffer),format,args);va_end(args);lastLog=buffer;}
}
int main(int argc,char** argv){assert(argc==2);scenario=argv[1];
    mbr[510]=0x55;mbr[511]=0xaa;mbr[450]=0xee;put32(mbr+454,1);put32(mbr+458,99999);
    const uint8_t esp[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b};
    std::memcpy(entries,esp,16);entries[16]=2;put64(entries+32,2048);put64(entries+40,4095);
    if(probe() || direct()){
        const uint8_t linuxFs[16]={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
        std::memcpy(entries+128,linuxFs,16);entries[144]=3;put64(entries+160,8192);put64(entries+168,10239);
        put32(superblock,1024);put32(superblock+4,256);put32(superblock+12,200);put32(superblock+16,1000);
        put32(superblock+24,2);put32(superblock+28,2);put32(superblock+32,32768);put32(superblock+36,32768);put32(superblock+40,1024);
        put16(superblock+56,0xef53);put16(superblock+58,(is("probe-dirty") || is("guard-super-dirty"))?0:1);put32(superblock+76,1);put32(superblock+84,11);
        put16(superblock+88,128);put32(superblock+96,2);put32(superblock+100,3);superblock[104]=4;
        if(is("guard-super-uuid"))superblock[104]^=1;
        if(is("guard-super-magic"))superblock[56]=0;
        if(is("guard-super-geometry"))put32(superblock+24,0xffffffffu);
        put32(gdt,2);put32(gdt+4,3);put32(gdt+8,4);
        if(is("probe-malformed") || is("direct-malformed") || is("guard-malformed"))dataRecord[0]='X';
    }
    std::memcpy(copy,entries,sizeof(entries));
    const auto crc=StrictCrc32(entries,sizeof(entries));hdr(primary,false,crc);hdr(backup,true,crc);
    if(is("bad-gpt") || is("direct-bad-gpt") || is("guard-bad-gpt"))backup[0]=0;
    if(is("wrong-guid") || is("direct-wrong-guid") || is("guard-wrong-guid"))record[8]='f';if(is("bad-record"))record[0]='x';
    if(guard()) {
        if(is("guard-after-normal")) {
            uint8_t next=1;assert(AhciSystemVolumeMount(&next)==AhciSystemBoot::Ready);
            assert(!AhciSystemGuardPrepare() && leaseCalls==1 && binds==1 && mounts==1 && !writes);return 0;
        }
        Device* esp=AhciSystemGuardPrepare();
        assert(!AhciSystemGuardPrepare());
        uint8_t next=1;assert(AhciSystemVolumeMount(&next)==AhciSystemBoot::Maintenance);
        assert(!binds && !mounts && !writes && next==1);
        const bool ready=is("guard-ready") || is("guard-super-dirty") || is("guard-super-geometry") || is("guard-read-error") || is("guard-flush-error");
        if(ready) {
            assert(esp && esp==registered && leaseCalls==1);
            uint8_t bytes[2]{};
            assert(DeviceWrite(&disk,2048ull*512,bytes,1)==-30);
            assert(DeviceWrite(&disk,8192ull*512,bytes,1)==-30);
            assert(DeviceWrite(&disk,0,bytes,1)==-30);
            assert(DeviceWrite(&disk,99999ull*512,bytes,1)==-30 && !writes);
            auto* block=reinterpret_cast<const BlockDeviceOps*>(esp->ops);
            assert(block->block_count(esp)==2048 && block->block_size(esp)==512);
            assert(esp->ops->ioctl(esp,DEVICE_IOCTL_READ_ONLY,nullptr)==1);
            assert(AhciSystemGuardIsReservedDevice(esp) && !AhciSystemGuardIsReservedDevice(&disk));
            const int read=esp->ops->read(esp,3,bytes,2);
            assert(read==(is("guard-read-error")?-5:2));if(read==2)assert(bytes[0]==0x42);
            assert(esp->ops->read(esp,2048ull*512-1,bytes,2)==-22);
            assert(esp->ops->ioctl(esp,DEVICE_IOCTL_FLUSH,nullptr)==(is("guard-flush-error")?-5:0));
            assert(esp->ops->write(esp,2048ull*512-1,bytes,2)==-22 && !writes);
            const int result=esp->ops->write(esp,3,bytes,2);
            assert(result==-30 && !writes);
        } else {
            assert(!esp && !writes);
            if(is("guard-lease-fail")) {
                assert(registered && leaseCalls==1);uint8_t byte=0;
                assert(registered->ops->write(registered,0,&byte,1)==-19 && !writes);
            } else assert(!leaseCalls);
        }
        return 0;
    }
    uint8_t next=1;const auto result=AhciSystemVolumeMount(&next);
    assert(AhciSystemVolumePublication()==(result==AhciSystemBoot::Ready?42u:0u));
    if(direct()) {
        if(is("direct-ready"))assert(result==AhciSystemBoot::Ready && registered && dataDevice && leaseCalls==1 && binds==1 && mounts==1 && next==2 && !writes);
        else if(is("direct-lease-fail"))assert(result==AhciSystemBoot::Maintenance && registered && dataDevice && leaseCalls==1 && !binds && !mounts && !writes);
        else assert(result==AhciSystemBoot::Maintenance && !leaseCalls && !binds && !mounts && !writes);
        return 0;
    }
    if(probe()){
        assert(result==AhciSystemBoot::Ready && registered && binds==1 && mounts==1 && next==2 && !writes);
        const unsigned before=reads, freesBefore=frees;
        if(is("probe-alloc1"))failAllocation=allocations+1;
        if(is("probe-alloc2"))failAllocation=allocations+2;
        if(is("probe-alloc3"))failAllocation=allocations+3;
        AhciSystemDataInspect();
        assert(!writes && binds==1 && mounts==1 && next==2);
        if(is("probe-absent"))assert(reads==before);
        else if(is("probe-clean"))assert(lastLog.find("format eligible entry=2")!=std::string::npos && reads-before==7);
        else if(is("probe-dirty"))assert(lastLog.find("offline filesystem check")!=std::string::npos && reads-before==6);
        else if(is("probe-readfail"))assert(lastLog.find("rejected")!=std::string::npos && reads-before==6);
        else if(is("probe-alloc1")||is("probe-alloc2")||is("probe-alloc3"))assert(lastLog.find("scratch allocation failed")!=std::string::npos && reads==before && frees-freesBefore==2);
        else assert(reads==before);
        return 0;
    }
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
