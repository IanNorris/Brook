#include "ssd_install_qemu.h"
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
#include "ssd_install_preflight.h"
#include "ssd_install_engine.h"
#include "ssd_install_gate.h"
#include "ahci_readonly.h"
#include "fatfs_vfs.h"
#include "device.h"
#include "memory/heap.h"
#include "process.h"
#include "scheduler.h"
#include "shutdown.h"
#include "vfs.h"

namespace brook {
namespace {
// Test-kernel-only, one-shot worker. No generic disk-offset interface exists.
SsdInstallGate gate;uint32_t state=0,stage=0,paused=0;uint64_t operations=0;int error=0;
FatImageLease* retainedLease=nullptr;
struct Io {
    FatImageLease* source;Device* target;
    int readSource(uint64_t lba,void* p,uint32_t n){return FatFsImageRead(source,lba*512,p,n);}
    int readTarget(uint64_t lba,void* p,uint32_t n){return target->ops->read(target,lba*512,p,n);}
    int writeTarget(uint64_t lba,const void* p,uint32_t n){return DeviceWrite(target,lba*512,p,n);}
    int flushTarget(){return DeviceFlush(target);}
};
int Run() {
    Device* target=AhciDisposableQemuInstallDevice();
    if(!target || DeviceBlockCount(target)!=131072 || DeviceBlockSize(target)!=512)return -22;
    auto* node=VfsOpen("/media/usb/INSTALL.IMG",VFS_O_WRITE|VFS_O_READWRITE);if(!node)return -2;
    VnodeStat st{};FatImageLease* lease=nullptr;
    int result=VfsStat(node,&st);
    if(!result && (st.isDir || st.isSymlink || st.size!=131072ull*512))result=-22;
    if(!result)result=FatFsAcquireImageLease(node,st.size,&lease);
    VfsRelease(node);if(result)return result;
    retainedLease=lease;Device* backing=FatFsImageLeaseDevice(lease);
    // The rehearsal's source is the private USB device, never the SATA disk.
    if(!backing || backing==target || !backing->name || backing->name[0]!='u' ||
       backing->name[1]!='s' || backing->name[2]!='b')result=-22;
    auto* meta=static_cast<uint8_t*>(kmalloc(34304));
    auto* a=static_cast<uint8_t*>(kmalloc(65536));auto* b=static_cast<uint8_t*>(kmalloc(65536));
    if(!meta || !a || !b)result=-12;
    if(!result) {
        uint8_t* mbr=meta;uint8_t* primary=meta+512;uint8_t* backup=meta+1024;
        uint8_t* entries=meta+1536;uint8_t* copy=meta+17920;
        constexpr uint64_t sectors=131072;
        if(FatFsImageRead(lease,0,mbr,512)!=512 || FatFsImageRead(lease,512,primary,512)!=512 ||
           FatFsImageRead(lease,(sectors-1)*512,backup,512)!=512 ||
           FatFsImageRead(lease,2*512,entries,16384)!=16384 ||
           FatFsImageRead(lease,(sectors-33)*512,copy,16384)!=16384)result=-5;
        uint64_t first=0,last=0;
        if(!result)for(unsigned i=0;i<128;++i){const uint8_t* e=entries+i*128;
            if(!StrictAllZero(e,16)){first=StrictLe64(e+32);last=StrictLe64(e+40);break;}}
        const SsdInstallGeometry geometry{sectors,first,last>=first?last-first+1:0};
        if(!result && !SsdInstallPreflight(geometry,sectors,DeviceBlockCount(target),DeviceBlockSize(target),
            mbr,primary,backup,entries,copy,16384))result=-22;
        if(!result) {
            Io io{lease,target};SsdInstallEngine engine(geometry,true);SsdInstallStepResult step;
            auto* pauseMarker=VfsOpen("/etc/ahci-qemu-install-pause");const bool pauseTest=pauseMarker!=nullptr;
            if(pauseMarker)VfsClose(pauseMarker);
            do {
                __atomic_store_n(&stage,uint32_t(engine.Stage()),__ATOMIC_RELEASE);
                step=engine.Step(io,a,b);__atomic_add_fetch(&operations,1ull,__ATOMIC_RELAXED);
                if(pauseTest && __atomic_load_n(&operations,__ATOMIC_RELAXED)==1 && step==SsdInstallStepResult::Progress) {
                    __atomic_store_n(&paused,1u,__ATOMIC_RELEASE);
                    while(__atomic_load_n(&paused,__ATOMIC_ACQUIRE))SchedulerSleepMs(10);
                }
            } while(step==SsdInstallStepResult::Progress);
            if(step!=SsdInstallStepResult::Complete)result=-5;
            __atomic_store_n(&stage,uint32_t(engine.Stage()),__ATOMIC_RELEASE);
        }
    }
    if(meta)kfree(meta);if(a)kfree(a);if(b)kfree(b);
    const int release=FatFsReleaseImageLease(lease);
    if(!release)retainedLease=nullptr;
    if(!result && release)result=release;
    return result;
}
void Worker(void*) {
    __atomic_store_n(&state,2u,__ATOMIC_RELEASE);const int result=Run();
    __atomic_store_n(&error,result,__ATOMIC_RELEASE);
    __atomic_store_n(&state,result?4u:3u,__ATOMIC_RELEASE);
    gate.Finish();
}
}
bool SsdQemuInstallBegin() {
    if(!AhciDisposableQemuInstallDevice() || !ShutdownLaunchAllowed())return false;
    uint32_t initial=0;if(!__atomic_compare_exchange_n(&state,&initial,1u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    if(!gate.Begin()) {
        __atomic_store_n(&state,4u,__ATOMIC_RELEASE);return false;
    }
    if(!ShutdownLaunchAllowed()){__atomic_store_n(&state,4u,__ATOMIC_RELEASE);gate.Finish();return false;}
    auto* worker=KernelThreadCreate("ssd_install_test",Worker,nullptr,3);
    if(!worker){__atomic_store_n(&state,4u,__ATOMIC_RELEASE);gate.Finish();return false;}
    SchedulerAddProcess(worker);return true;
}
SsdQemuStatus SsdQemuInstallStatus() {
    return {__atomic_load_n(&state,__ATOMIC_ACQUIRE),__atomic_load_n(&stage,__ATOMIC_ACQUIRE),
            __atomic_load_n(&operations,__ATOMIC_ACQUIRE),__atomic_load_n(&error,__ATOMIC_ACQUIRE),
            __atomic_load_n(&paused,__ATOMIC_ACQUIRE)!=0};
}
bool SsdQemuInstallResume() {return __atomic_exchange_n(&paused,0u,__ATOMIC_ACQ_REL)!=0;}
bool SsdQemuInstallCloseReady() {return gate.CloseReady();}
}
#endif
