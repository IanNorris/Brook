#include "ssd_install_qemu.h"
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
#include "ssd_install_preflight.h"
#include "ssd_install_engine.h"
#include "ssd_install_gate.h"
#include "ssd_install_segmented_source.h"
#include "ssd_install_compact_source.h"
#include "ssd_install_digest.h"
#include "ssd_install_source_identity.h"
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
FatImageLease* retainedLease[4]{};
struct Source {
    FatImageLease** leases; SsdInstallSegmentedSource map; const SsdInstallCompactSource* compact;
    int readSegment(uint32_t index,uint64_t offset,void* p,uint32_t n) {
        return FatFsImageRead(leases[index],offset,p,n);
    }
    int readPacked(uint64_t offset,void* p,uint32_t n) {return map.Read(*this,offset,p,n);}
    int read(uint64_t offset,void* p,uint32_t n) {return compact ? compact->Read(*this,offset,p,n) : readPacked(offset,p,n);}
};
struct Io {
    Source* source;Device* target;
    int readSource(uint64_t lba,void* p,uint32_t n){return source->read(lba*512,p,n);}
    int readTarget(uint64_t lba,void* p,uint32_t n){return target->ops->read(target,lba*512,p,n);}
    int writeTarget(uint64_t lba,const void* p,uint32_t n){return DeviceWrite(target,lba*512,p,n);}
    int flushTarget(){return DeviceFlush(target);}
};
int Run() {
    Device* target=AhciDisposableQemuInstallDevice();
    if(!target || DeviceBlockSize(target)!=512)return -22;
    const uint64_t sectors=DeviceBlockCount(target);
    if(sectors!=131072 && sectors!=12587008 && sectors!=500118192)return -22;
    auto* marker=VfsOpen("/media/usb/INSTCMP.OK");const bool compact=marker!=nullptr;
    if(marker)VfsClose(marker);
    auto* desktopMarker=VfsOpen("/media/usb/INSTDSK.OK");const bool desktop=desktopMarker!=nullptr;
    if(desktopMarker)VfsClose(desktopMarker);
    // Compact mode accepts only two recognized QEMU geometries until a
    // production source manifest and physical identity gate exist.
    if(desktop && (!compact || sectors!=500118192))return -22;
    const SsdInstallGeometry compactGeometry{sectors,2048,desktop?12582912ull:126976ull};
    const SsdInstallCompactSource compactMap(compactGeometry,(34+compactGeometry.payloadSectors+33)*512);
    if((compact && sectors!=131072 && sectors!=500118192) || (!compact && sectors==500118192))return -22;
    const uint64_t sourceBytes=compact?compactMap.ExpectedSize():sectors*512;
    const uint64_t segment=(compact && !desktop) || sectors==131072?32ull*1024*1024:2ull*1024*1024*1024;
    const uint32_t count=uint32_t((sourceBytes-1)/segment+1);
    if(count>4)return -22;
    Source source{retainedLease,SsdInstallSegmentedSource(sourceBytes,segment,count),compact?&compactMap:nullptr};
    static const char* paths[]={"/media/usb/INST0000.IMG","/media/usb/INST0001.IMG",
                                "/media/usb/INST0002.IMG","/media/usb/INST0003.IMG"};
    int result=0;Device* firstBacking=nullptr;
    for(uint32_t i=0;i<count;++i) {
        auto* node=VfsOpen(paths[i],VFS_O_WRITE|VFS_O_READWRITE);
        if(!node){result=-2;break;}
        VnodeStat st{};result=VfsStat(node,&st);
        if(!result && (st.isDir || st.isSymlink || st.size!=source.map.ExpectedSize(i)))result=-22;
        if(!result)result=FatFsAcquireImageLease(node,st.size,&retainedLease[i]);
        VfsRelease(node);if(result)break;
        Device* backing=FatFsImageLeaseDevice(retainedLease[i]);
        // Test fixture only: both segments must belong to the same USB device.
        if(!SsdInstallUsbSource(backing,target) || (i && backing!=firstBacking)) {result=-22;break;}
        firstBacking=backing;
    }
    auto* meta=static_cast<uint8_t*>(kmalloc(34304));
    auto* a=static_cast<uint8_t*>(kmalloc(65536));auto* b=static_cast<uint8_t*>(kmalloc(65536));
    if(!meta || !a || !b)result=-12;
    if(!result && compact) {
        auto* record=VfsOpen("/media/usb/INSTHASH.BIN",VFS_O_STREAM);
        uint8_t data[SsdDigestRecordBytes]{};VnodeStat st{};uint64_t off=0;
        if(!record)result=-2;
        else {
            if(VfsStat(record,&st) || st.isDir || st.isSymlink || st.size!=sizeof(data) ||
               VfsRead(record,data,sizeof(data),&off)!=int(sizeof(data)))result=-22;
            VfsRelease(record);
        }
        auto* expected=VfsOpen("/etc/ssd-install-expected",VFS_O_STREAM);
        if(!result && expected) {
            uint8_t trusted[SsdDigestRecordBytes]{};VnodeStat expectedStat{};uint64_t expectedOffset=0;
            if(VfsStat(expected,&expectedStat) || expectedStat.isDir || expectedStat.isSymlink ||
               expectedStat.size!=sizeof(trusted) || VfsRead(expected,trusted,sizeof(trusted),&expectedOffset)!=int(sizeof(trusted)) ||
               !SsdDigestVerifyExpectedPinned(source,data,sizeof(data),trusted,sizeof(trusted),compactGeometry,sourceBytes,a,65536))result=-5;
        } else if(!result && !SsdDigestVerifyPinned(source,data,sizeof(data),compactGeometry,sourceBytes,a,65536))result=-5;
        if(expected)VfsRelease(expected);
    }
    if(!result) {
        uint8_t* mbr=meta;uint8_t* primary=meta+512;uint8_t* backup=meta+1024;
        uint8_t* entries=meta+1536;uint8_t* copy=meta+17920;
        if(source.read(0,mbr,512)!=512 || source.read(512,primary,512)!=512 ||
           source.read((sectors-1)*512,backup,512)!=512 ||
           source.read(2*512,entries,16384)!=16384 ||
           source.read((sectors-33)*512,copy,16384)!=16384)result=-5;
        uint64_t first=0,last=0;
        if(!result)for(unsigned i=0;i<128;++i){const uint8_t* e=entries+i*128;
            if(!StrictAllZero(e,16)){first=StrictLe64(e+32);last=StrictLe64(e+40);break;}}
        const SsdInstallGeometry geometry{sectors,first,last>=first?last-first+1:0};
        // Bind the compact map to the GPT before even the invalidation phase.
        // A valid GPT with a different payload must never drive this map.
        if(!result && compact && (geometry.diskSectors!=compactGeometry.diskSectors ||
            geometry.payloadFirst!=compactGeometry.payloadFirst ||
            geometry.payloadSectors!=compactGeometry.payloadSectors))result=-22;
        if(!result && !SsdInstallPreflight(geometry,sectors,DeviceBlockCount(target),DeviceBlockSize(target),
            mbr,primary,backup,entries,copy,16384))result=-22;
        if(!result) {
            Io io{&source,target};SsdInstallEngine engine(geometry,true);SsdInstallStepResult step;
            auto* pauseMarker=VfsOpen("/etc/ahci-qemu-install-pause");const bool pauseTest=pauseMarker!=nullptr;
            if(pauseMarker)VfsClose(pauseMarker);
            auto* payloadMarker=VfsOpen("/etc/ahci-qemu-pause-payload");const bool pausePayload=payloadMarker!=nullptr;
            if(payloadMarker)VfsClose(payloadMarker);
            bool didPause=false;
            do {
                const auto before=engine.Stage();
                __atomic_store_n(&stage,uint32_t(before),__ATOMIC_RELEASE);
                step=engine.Step(io,a,b);__atomic_add_fetch(&operations,1ull,__ATOMIC_RELAXED);
                if(!didPause && step==SsdInstallStepResult::Progress &&
                   ((pauseTest && __atomic_load_n(&operations,__ATOMIC_RELAXED)==1) ||
                    (pausePayload && before==SsdInstallStage::Payload))) {
                    didPause=true;
                    __atomic_store_n(&paused,1u,__ATOMIC_RELEASE);
                    while(__atomic_load_n(&paused,__ATOMIC_ACQUIRE))SchedulerSleepMs(10);
                }
            } while(step==SsdInstallStepResult::Progress);
            if(step!=SsdInstallStepResult::Complete)result=-5;
            __atomic_store_n(&stage,uint32_t(engine.Stage()),__ATOMIC_RELEASE);
        }
    }
    if(meta)kfree(meta);if(a)kfree(a);if(b)kfree(b);
    for(uint32_t i=0;i<count;++i)if(retainedLease[i]) {
        const int release=FatFsReleaseImageLease(retainedLease[i]);
        if(!release)retainedLease[i]=nullptr;
        if(!result && release)result=release;
    }
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
