#include "hda_observe.h"
#include "hda_dell_inventory.h"
#include "hda_dell_tone_ram.h"
#include "hda_cache_mapping.h"
#include "pci.h"
#include "vfs.h"
#include "serial.h"
#include "cpu.h"
#include "smp.h"
#include "apic.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "../../drivers/intel_hda/hda_dell_tone_adapter.h"
#include "../../drivers/intel_hda/hda_dell_tone_controller.h"
#include "../../drivers/intel_hda/hda_dma_publish.h"
#include "../../drivers/intel_hda/hda_dell_stream_owner.h"
#include "../../drivers/intel_hda/hda_dell_stream_queue.h"
#include "../../drivers/intel_hda/hda_dell_audio_bridge.h"
#include "native_tsc_observe.h"
#include "native_tsc_policy.h"
#include "native_tsc_audio_clock.h"
#include "audio.h"
#include "sync/kmutex.h"
#include <stddef.h>
#ifdef BROOK_HOST_TEST
#include <cstring>
#include <new>
#else
#include "string.h"
inline void* operator new(size_t,void* storage) noexcept {return storage;}
#endif

namespace brook {
#ifdef BROOK_HOST_TEST
uint8_t HdaObserveTestRead8(uint32_t);uint16_t HdaObserveTestRead16(uint32_t);uint32_t HdaObserveTestRead32(uint32_t);
void HdaObserveTestWrite8(uint32_t,uint8_t);void HdaObserveTestWrite16(uint32_t,uint16_t);void HdaObserveTestWrite32(uint32_t,uint32_t);
bool HdaObserveTestFlushInfo(uint32_t&);void HdaObserveTestFlushLine(void*);
uint64_t HdaObserveTestNowNs();
void HdaObserveTestBeforeStreamIteration();
#endif
namespace {
HdaObservation observation{};
CpuCacheSnapshot cacheSnapshots[MAX_CPUS]{};
volatile uint8_t* registers=nullptr; // borrowed page; intentionally boot-lifetime
constexpr uint64_t Aperture=16384,Page=4096;

bool SafeBootRange(const BootProtocol* boot,uint64_t base) {
    if(!boot || (boot->memoryMapCount && !boot->memoryMap) || boot->memoryMapCount>4096 ||
       !base || (base&(Aperture-1)) || base>~uint64_t{0}-Aperture)return false;
    for(uint32_t i=0;i<boot->memoryMapCount;++i) {
        const auto& d=boot->memoryMap[i];
        if(d.pageCount>(~uint64_t{0}-d.physicalStart)/Page)return false;
        const uint64_t end=d.physicalStart+d.pageCount*Page;
        if(d.physicalStart>=base+Aperture || end<=base)continue;
        if(d.type!=MemoryType::Mmio && d.type!=MemoryType::Reserved)return false;
    }
    return true;
}
bool OtherBarStartsClear(uint64_t base) {
    if(PciBootInventorySeen()!=PciBootInventoryCount())return false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry e{};if(!PciBootInventoryRead(i,&e))return false;
        const auto& d=e.device;
        if(d.bus==0 && d.dev==0x1b && d.fn==0)continue;
        const uint8_t header=e.headerType&0x7f;
        // Type 1 bridge BARs are real resources too; the remaining header
        // registers describe bridge windows, not BARs. Reject unknown types.
        const unsigned bars=header==0?6:header==1?2:header==2?1:0;
        if(!bars)return false;
        for(unsigned n=0;n<bars;++n) {
            const uint32_t bar=d.bar[n];
            if(bar==0xffffffff)return false;
            if(!bar || (bar&1))continue;
            if((bar&6)==6)return false;
            uint64_t address=bar&~uint32_t(15);
            if((bar&6)==4) {
                if(n+1>=bars || d.bar[n+1]==0xffffffff)return false;
                address|=uint64_t(d.bar[++n])<<32;
            }
            if(address>=base && address<base+Aperture)return false;
        }
    }
    // BAR sizes of other devices are not available here; this is only a
    // known-base conflict check, not a complete PCI resource allocator.
    return true;
}
bool CacheCompatible(uint64_t base) {
    if(SmpCurrentCpuIndex()!=0)return false;
    for(unsigned i=0;i<4;++i) {
        const uint64_t page=base+i*Page;
        KernelAliasMapping identity{},direct{};
        if(!VmmInspectKernelAliases(page,identity,direct) || !identity.present || !direct.present ||
           identity.physical!=page || direct.physical!=page || identity.patIndex!=0 || direct.patIndex!=0)return false;
        CpuMemoryTypeObservation type{};
        if(!CpuObserveMemoryTypes(page,page,type) || type.cpu!=0 || type.patSlot0!=6 ||
           type.cacheDisabled || type.notWriteThrough || type.page0.type!=0)return false;
    }
    return true;
}
bool PowerD0(const PciDevice& d) {
    if(!(PciConfigRead16(d.bus,d.dev,d.fn,6)&0x10))return false;
    uint8_t cap=PciConfigRead8(d.bus,d.dev,d.fn,0x34);uint64_t seen=0;
    for(unsigned n=0;n<48 && cap;++n) {
        if(cap<0x40 || (cap&3) || cap>0xf8)return false;
        const uint64_t bit=uint64_t(1)<<(cap/4);if(seen&bit)return false;seen|=bit;
        const uint8_t id=PciConfigRead8(d.bus,d.dev,d.fn,cap);
        if(id==1)return (PciConfigRead16(d.bus,d.dev,d.fn,cap+4)&3)==0;
        cap=PciConfigRead8(d.bus,d.dev,d.fn,cap+1);
    }
    return false;
}
volatile uint8_t* MapBorrowed(uint64_t physical) {
    const auto address=VmmAllocPages(1,VMM_WRITABLE|VMM_NO_EXEC,MemTag::Device,KernelPid);
    if(!address)return nullptr;
    const uint64_t leaf=VmmDetachPage(KernelPageTable,address);
    if(!leaf){VmmFreePages(address,1);return nullptr;}
    PmmFreePage(VmmLeafPhysical(leaf)); // fresh, unpublished RAM leaf only
    if(!VmmMapPage(KernelPageTable,address,PhysicalAddress(physical),
                   VMM_WRITABLE|VMM_NO_EXEC|VMM_CACHE_DISABLE|VMM_WRITE_THRU,MemTag::Device,KernelPid)) {
        VmmFreePages(address,1);return nullptr;
    }
    // Never free this VA with VmmFreePages: its leaf is borrowed device MMIO.
    return reinterpret_cast<volatile uint8_t*>(address.raw());
}
struct Io {
    const BootProtocol* boot;PciBootEntry entry{};uint64_t base=0;
    bool liveIdentity() {
        const auto& d=entry.device;
        const uint32_t classRevision=PciConfigRead32(d.bus,d.dev,d.fn,8);
        if(PciConfigRead32(d.bus,d.dev,d.fn,0)!=0x9ca08086 ||
           PciConfigRead32(d.bus,d.dev,d.fn,0x2c)!=0x06651028 ||
           (classRevision>>16)!=0x0403 ||
           (PciConfigRead8(d.bus,d.dev,d.fn,0x0e)&0x7f)!=0 ||
           PciConfigRead32(d.bus,d.dev,d.fn,0x10)!=d.bar[0] ||
           PciConfigRead32(d.bus,d.dev,d.fn,0x14)!=d.bar[1])return false;
        return PowerD0(d);
    }
    bool prepare() {
        if(SmpCurrentCpuIndex()!=0){observation.reason="not boot CPU";return false;}
        bool found=false;
        for(uint32_t i=0;i<PciBootInventoryCount();++i){PciBootEntry e{};if(!PciBootInventoryRead(i,&e))continue;
            const auto& d=e.device;HdaProbePci p{d.vendorId,d.deviceId,e.subsystemVendor,e.subsystemDevice,
                d.bus,d.dev,d.fn,d.classCode,d.subclass,d.bar[0],d.bar[1]};uint64_t b=0;
            if((e.headerType&0x7f)==0 && HdaProbePciCheck(p,b)==HdaProbeResult::Ready){entry=e;base=b;found=true;break;}}
        if(!found){observation.reason="target absent";return false;}
        observation.base=base;
        if(!SafeBootRange(boot,base) || !OtherBarStartsClear(base)){observation.reason="unsafe aperture";return false;}
        if(!liveIdentity() || command()!=0){observation.reason="PCI identity, power or command changed";return false;}
        if(!CacheCompatible(base)){observation.reason="cache aliases not verified";return false;}
        registers=MapBorrowed(base);if(!registers){observation.reason="mapping failed";return false;}
        observation.mapped=true;return true;
    }
    bool valid(){return SmpCurrentCpuIndex()==0 && liveIdentity() && CacheCompatible(base);}
    uint16_t command(){return PciConfigRead16(entry.device.bus,entry.device.dev,entry.device.fn,4);}
    void setCommand(uint16_t value){PciConfigWrite32(entry.device.bus,entry.device.dev,entry.device.fn,4,value);}
    uint8_t read8(uint32_t offset){
#ifdef BROOK_HOST_TEST
        return HdaObserveTestRead8(offset);
#else
        return *reinterpret_cast<volatile uint8_t*>(registers+offset);
#endif
    }
    uint16_t read16(uint32_t offset){
#ifdef BROOK_HOST_TEST
        return HdaObserveTestRead16(offset);
#else
        return *reinterpret_cast<volatile uint16_t*>(registers+offset);
#endif
    }
    uint32_t read32(uint32_t offset){
#ifdef BROOK_HOST_TEST
        return HdaObserveTestRead32(offset);
#else
        return *reinterpret_cast<volatile uint32_t*>(registers+offset);
#endif
    }
    void write8(uint32_t offset,uint8_t value){
#ifdef BROOK_HOST_TEST
        HdaObserveTestWrite8(offset,value);
#else
        *reinterpret_cast<volatile uint8_t*>(registers+offset)=value;
#endif
    }
    void write16(uint32_t offset,uint16_t value){
#ifdef BROOK_HOST_TEST
        HdaObserveTestWrite16(offset,value);
#else
        *reinterpret_cast<volatile uint16_t*>(registers+offset)=value;
#endif
    }
    void write32(uint32_t offset,uint32_t value){
#ifdef BROOK_HOST_TEST
        HdaObserveTestWrite32(offset,value);
#else
        *reinterpret_cast<volatile uint32_t*>(registers+offset)=value;
#endif
    }
    uint64_t ticks(){return ApicTickCount();}
    void pause(){__asm__ volatile("pause");}
};
// BSP-only, opt-in boot diagnostic. Quarantine retains this platform, the
// adapter, the borrowed MMIO mapping and all owned RAM pages until reboot.
struct TonePlatform {
    Io& io;const HdaSessionReport& snapshot;
    void* dmaPages[17]{};unsigned expectedPages=4;bool streamMode=false,runtimeMode=false;uint64_t tscOrigin=0;NativeTscAudioClock runtimeClock{};
    bool flushInfo(uint32_t& line){
#ifdef BROOK_HOST_TEST
        const bool available=HdaObserveTestFlushInfo(line);
#else
        uint32_t a=1,b=0,c=0,d=0;__asm__ volatile("cpuid":"+a"(a),"=b"(b),"=c"(c),"=d"(d));
        line=((b>>8)&255)*8;const bool available=(d&(1u<<19))!=0;
#endif
        return available && line>=16 && line<=4096 && !(line&(line-1)) && !(4096%line);
    }
    bool identityStable(){
        const bool inventory=HdaDellToneInventoryMatches(),valid=io.valid(),range=SafeBootRange(io.boot,io.base),bars=OtherBarStartsClear(io.base);
        observation.toneAdmission=(observation.toneAdmission&16)|(inventory?1u:0u)|(valid?2u:0u)|(range?4u:0u)|(bars?8u:0u);
        return inventory && valid && range && bars;}
    uint16_t command(){return io.command();}void setCommand(uint16_t v){io.setCommand(v);}
    bool controllerIdle(){return HdaDellToneControllerIdle(*this,snapshot);}
    bool allocatePage(uint64_t& physical,void*& address){
        const auto p=PmmAllocPage(MemTag::KernelData,KernelPid);
        if(!p){observation.tonePage=0;observation.tonePageStatus=4;return false;}
        physical=p.raw();address=reinterpret_cast<void*>(PhysToVirt(p).raw());observation.tonePage=physical;
        for(auto& slot:dmaPages)if(!slot){slot=address;return true;}
        PmmFreePage(p);physical=0;address=nullptr;return false;}
    bool pageAllowed(uint64_t physical,void* address){
        if(!physical || physical>0xfffff000ULL || (physical&4095) || !address ||
           (reinterpret_cast<uintptr_t>(address)&4095)){observation.tonePageStatus=1;return false;}
        KernelAliasMapping identity{},direct{};
        if(!VmmInspectKernelAliases(physical,identity,direct) ||
           !HdaDellToneRamMappingMatches(physical,reinterpret_cast<uint64_t>(address),DIRECT_MAP_BASE,direct)){
            observation.tonePageStatus=2;return false;}
        // Validate the pages actually owned by the runtime, before touching
        // them or building a descriptor. Temporary probe pages prove nothing
        // about subsequent PMM allocations.
        if(runtimeMode && (!observation.cacheMatch || observation.cacheCpuCount!=SmpGetCpuCount() ||
           !HdaCacheDmaMappings(physical,identity,direct) || !CpuCacheHdaDmaPageType(cacheSnapshots[0],physical))){
            observation.tonePageStatus=5;return false;}
        if(runtimeMode)++observation.runtimePagesValidated;
        observation.tonePageStatus=3;return true;}
    void clearPage(void* p){memset(p,0,4096);}void freePage(uint64_t p){
        void* address=reinterpret_cast<void*>(PhysToVirt(PhysicalAddress(p)).raw());for(auto& slot:dmaPages)if(slot==address)slot=nullptr;
        PmmFreePage(PhysicalAddress(p));}
    uint8_t read8(uint32_t o){return io.read8(o);}uint16_t read16(uint32_t o){return io.read16(o);}
    uint32_t read32(uint32_t o){return io.read32(o);}void write8(uint32_t o,uint8_t v){io.write8(o,v);}
    void write16(uint32_t o,uint16_t v){io.write16(o,v);}void write32(uint32_t o,uint32_t v){io.write32(o,v);}
    uint64_t ticks(){return io.ticks();}void pause(){io.pause();}
    uint64_t nowNs(){
#ifdef BROOK_HOST_TEST
        return HdaObserveTestNowNs();
#else
        const auto& clock=NativeTscObserveRead();if(!clock.consistent || !clock.khz)return 0;
        // Keep the CPU index and TSC sample on the same CPU, including against
        // an A->B->A migration. Restore the caller's original interrupt state.
        uint64_t flags;__asm__ volatile("pushfq; popq %0; cli":"=r"(flags)::"memory");
        const auto cpu=SmpCurrentCpuIndex();uint32_t lo,hi,aux;
        __asm__ volatile("rdtscp; lfence":"=a"(lo),"=d"(hi),"=c"(aux)::"memory");
        __asm__ volatile("pushq %0; popfq"::"r"(flags):"memory","cc");
        if(!runtimeMode && cpu!=0)return 0;const uint64_t ticks=(uint64_t(hi)<<32)|lo;
        uint64_t ns=0;if(runtimeMode){if(SmpGetCpuCount()!=clock.coreExpected+1 || !runtimeClock.read(cpu,ticks,ns))return 0;return ns;}
        if(!tscOrigin || !NativeTscElapsedNs(tscOrigin,ticks,clock.khz,ns) || ns==~uint64_t(0))return 0;
        return ns+1;
#endif
    }
    bool delayUs(unsigned us){
        if(us>1000)return false;uint64_t flags=0;__asm__ volatile("pushfq; popq %0":"=r"(flags));
        if(!(flags&0x200))return false;const auto start=ticks();uint64_t anchor=0;bool haveBoundary=false;
        // Discard the first delivered IRQ (which could already be pending),
        // then require two further boundaries on the BSP millisecond clock.
        for(unsigned i=0;i<10000000;++i){const auto now=ticks();
            if(!haveBoundary && now>start){anchor=now;haveBoundary=true;}
            else if(haveBoundary && now>=anchor && now-anchor>=2)return true;pause();}
        return false;
    }
    void line(void* address){
#ifdef BROOK_HOST_TEST
        HdaObserveTestFlushLine(address);
#else
        __asm__ volatile("clflush (%0)"::"r"(address):"memory");
#endif
    }
    bool fence(){__atomic_thread_fence(__ATOMIC_SEQ_CST);return true;}
    bool publishDma(){
        uint32_t line=0;if(!flushInfo(line))return false;
        for(unsigned i=0;i<expectedPages;++i)if(!HdaDmaFlushRange(*this,dmaPages[i],4096,0,4096,line))return false;
        return fence();
    }
    void quarantine(){if(runtimeMode)__atomic_store_n(&observation.runtimeQuarantined,true,__ATOMIC_RELEASE);else if(streamMode)observation.streamQuarantined=true;else observation.toneQuarantined=true;}
};
bool Marker(const char* path){auto* node=VfsOpen(path);if(!node)return false;VfsClose(node);return true;}
// Read-only mapping/type observation of newly owned RAM pages. No BDL is
// published, BME is not enabled, and none of the pages are dereferenced here.
void ObserveCachePages(Io& io){
    observation.cachePagesRequested=true;
    if(!observation.cacheCaptured || !observation.cacheMatch || !observation.mapped ||
       observation.session.result!=HdaSessionResult::Complete || io.command()!=0)return;
    KernelAliasMapping mmio{};
    observation.cacheMmioMapping=VmmInspectKernelAddress(reinterpret_cast<uint64_t>(registers),mmio) &&
        HdaCacheMmioMapping(io.base,mmio);
    if(!observation.cacheMmioMapping)return;
    uint64_t pages[17]{};unsigned acquired=0;bool valid=true;
    for(unsigned i=0;i<17;++i){const auto p=PmmAllocPage(MemTag::KernelData,KernelPid);if(!p){valid=false;break;}
        pages[acquired++]=p.raw();for(unsigned j=0;j<i;++j)if(pages[j]==p.raw())valid=false;
        KernelAliasMapping identity{},direct{};
        if(!valid || !VmmInspectKernelAliases(p.raw(),identity,direct) || !HdaCacheDmaMappings(p.raw(),identity,direct)){
            valid=false;break;}}
    observation.cachePages=acquired;observation.cacheDmaMappings=valid && acquired==17;
    if(observation.cacheDmaMappings)observation.cachePageTypes=CpuCacheHdaPageTypes(cacheSnapshots[0],io.base,pages,17);
    // Defensive duplicate handling: a broken allocator must not double-free.
    while(acquired){const uint64_t p=pages[--acquired];bool duplicate=false;
        for(unsigned i=0;i<acquired;++i)if(pages[i]==p)duplicate=true;if(!duplicate)PmmFreePage(PhysicalAddress(p));}
}
struct RuntimeLock {KMutex mutex{};void init(){KMutexInit(&mutex);}void lock(){KMutexLock(&mutex);}
    bool tryLock(){return KMutexTryLock(&mutex);}void unlock(){KMutexUnlock(&mutex);}};
using RuntimeOwner=HdaDellStreamOwner<TonePlatform>;using RuntimeBridge=HdaDellAudioBridge<RuntimeOwner,RuntimeLock>;
RuntimeOwner* runtimeOwner=nullptr;RuntimeBridge* runtimeBridge=nullptr;
int RuntimePlay(const void* p,uint32_t n,uint32_t hz,uint8_t ch,uint8_t bits,bool){return runtimeBridge?runtimeBridge->play(p,n,hz,ch,bits):-19;}
void RuntimeStop(){if(runtimeBridge)runtimeBridge->stop();}
bool RuntimePlaying(){return runtimeBridge && runtimeBridge->isPlaying();}
uint32_t RuntimePosition(){AudioDeviceProgress p{};return runtimeBridge && runtimeBridge->progress(&p)?uint32_t(p.completed):0;}
void RuntimeService(){if(runtimeBridge)runtimeBridge->service();}
AudioSubmitReceipt RuntimeSubmit(const void* p,uint32_t n){return runtimeBridge?runtimeBridge->submit(p,n):AudioSubmitReceipt{0,0,0,-19};}
bool RuntimeProgress(AudioDeviceProgress* p){return runtimeBridge && runtimeBridge->progress(p);}
bool RuntimeRestart(uint64_t epoch){return runtimeBridge && runtimeBridge->restart(epoch);}
void PrepareRuntime(Io& io){
    // Admission is boot-bound. Hotplug, suspend and cache reprogramming must
    // invalidate it before any of those features can be used with this driver.
    KernelAliasMapping mmio{};
    if(!observation.cacheCaptured || !observation.cacheMatch || !observation.cacheCpuCount ||
       observation.cacheCpuCount!=SmpGetCpuCount() ||
       !CpuCacheHdaApertureType(cacheSnapshots[0],io.base) ||
       !VmmInspectKernelAddress(reinterpret_cast<uint64_t>(registers),mmio) || !HdaCacheMmioMapping(io.base,mmio)){
        observation.runtimeReason="all-CPU cache admission unavailable";return;}
    const auto& clock=NativeTscObserveRead();if(!clock.consistent || !Marker("/etc/audio-muted")){observation.runtimeReason="native clock or boot mute unavailable";return;}
    alignas(TonePlatform) static unsigned char platformStorage[sizeof(TonePlatform)];
    auto* platform=::new(platformStorage) TonePlatform{io,observation.session};platform->expectedPages=17;platform->runtimeMode=true;
#ifdef BROOK_HOST_TEST
    const uint64_t origin=1;
#else
    uint32_t lo,hi,aux;__asm__ volatile("rdtscp; lfence":"=a"(lo),"=d"(hi),"=c"(aux)::"memory");const uint64_t origin=(uint64_t(hi)<<32)|lo;
#endif
    if(!platform->runtimeClock.init(clock,SmpGetCpuCount(),origin)){observation.runtimeReason="native clock admission failed";return;}
    uint32_t line=0;if(!platform->flushInfo(line) || line!=cacheSnapshots[0].clflushLine || !platform->delayUs(3)){observation.runtimeReason="cache or timer unavailable";return;}
    alignas(RuntimeOwner) static unsigned char ownerStorage[sizeof(RuntimeOwner)];
    runtimeOwner=::new(ownerStorage) RuntimeOwner(*platform,observation.session,line);
    if(!runtimeOwner->prepare()){observation.runtimeReason="owner preparation failed";return;}
    static RuntimeLock lock;lock.init();alignas(RuntimeBridge) static unsigned char bridgeStorage[sizeof(RuntimeBridge)];
    // Two admitted CPUs may differ by up to 20 us. This covers the observed
    // offset bound, not systematic PIT calibration error or future drift.
    runtimeBridge=::new(bridgeStorage) RuntimeBridge(*runtimeOwner,lock,line,4096,1024,20000);
    observation.runtimePrepared=true;observation.runtimeReason="prepared muted";
}
void RunStreamProbe(Io& io){
    observation.streamAttempted=true;
    const auto& tsc=NativeTscObserveRead();
    if(!tsc.capable || !tsc.consistent || !tsc.khz){observation.streamReason="native clock unavailable";return;}
    alignas(TonePlatform) static unsigned char platformStorage[sizeof(TonePlatform)];
    auto* platform=::new(platformStorage) TonePlatform{io,observation.session};platform->expectedPages=17;platform->streamMode=true;
#ifndef BROOK_HOST_TEST
    uint32_t lo,hi,aux;__asm__ volatile("rdtscp; lfence":"=a"(lo),"=d"(hi),"=c"(aux)::"memory");
    platform->tscOrigin=(uint64_t(hi)<<32)|lo;if(!platform->tscOrigin){observation.streamReason="native clock unavailable";return;}
#endif
    uint32_t line=0;if(!platform->flushInfo(line) || !platform->delayUs(3)){observation.streamReason="cache or timer unavailable";return;}
    using Owner=HdaDellStreamOwner<TonePlatform>;using Queue=HdaDellStreamQueue<Owner>;
    alignas(Owner) static unsigned char ownerStorage[sizeof(Owner)];alignas(Queue) static unsigned char queueStorage[sizeof(Queue)];
    auto* owner=::new(ownerStorage) Owner(*platform,observation.session,line);
    if(!owner->prepare()){observation.streamCommand=platform->command();observation.streamReason="owner preparation failed";return;}
    auto* queue=::new(queueStorage) Queue(*owner,line,4096,1024);
    if(!queue->start()){observation.streamCommand=platform->command();observation.streamReason="queue start failed";return;}
    static uint8_t samples[1764];for(unsigned i=0;i<sizeof(samples);++i)samples[i]=uint8_t(i*37);
    const uint64_t start=platform->nowNs();uint64_t next=start;bool ok=start!=0,complete=false;
    for(unsigned i=0;ok && i<10000000;++i){
#ifdef BROOK_HOST_TEST
        HdaObserveTestBeforeStreamIteration();
#endif
        const uint64_t now=platform->nowNs();if(!now || now<start){ok=false;break;}
        observation.streamElapsedNs=now-start;if(observation.streamElapsedNs>=2000000000ULL){
            if(!queue->service())ok=false;else complete=true;break;}
        if(now>=next){const auto receipt=queue->submit(samples,sizeof(samples));
            if(receipt.error){ok=false;break;}if(receipt.accepted){++observation.streamSubmissions;observation.streamAccepted+=receipt.accepted;}
            if(now>~uint64_t(0)-10000000ULL){ok=false;break;}next=now+10000000ULL;
        }else if(!queue->service()){ok=false;break;}
        platform->pause();
    }
    observation.streamCompleted=queue->progress().RawCompleted();
    if(!queue->failed() && !queue->shutdown())ok=false;
    if(!queue->failed() && !owner->retire())ok=false;
    observation.streamCommand=platform->command();
    observation.streamFinished=ok && complete && observation.streamAccepted && observation.streamCompleted>65536 && !observation.streamQuarantined && observation.streamCommand==0;
    observation.streamReason=observation.streamFinished?"complete":"stream test failed";
}
}
void HdaObserveConfigure(const BootProtocol* boot) {
    if(observation.attempted)return;
    auto* marker=VfsOpen("/etc/dell-hda-observe");if(!marker)return;VfsClose(marker);
    observation.requested=true;observation.attempted=true;observation.reason="prepared";
    static Io io{nullptr};io.boot=boot;observation.session=HdaProbeDellSession(io);
    if(observation.session.result!=HdaSessionResult::Unprepared)observation.reason="transaction attempted";
    SerialPrintf("HDA_OBSERVE result=%u probe=%u before=0x%x enabled=0x%x after=0x%x reason=%s\n",
        unsigned(observation.session.result),unsigned(observation.session.probe),unsigned(observation.session.before),
        unsigned(observation.session.enabled),unsigned(observation.session.after),observation.reason);
    const bool muted=Marker("/etc/dell-hda-muted-dma"),audible=Marker("/etc/dell-hda-quiet-tone");
    const bool mixerProbe=Marker("/etc/dell-hda-mixer-format-probe");
    const bool runtimeRequested=Marker("/etc/dell-hda-audio");
    if(Marker("/etc/dell-hda-cache-observe") || runtimeRequested){
        observation.cacheRequested=true;const auto count=SmpGetCpuCount();
        if(count && count<=MAX_CPUS && SmpObserveCacheSnapshots(cacheSnapshots,MAX_CPUS)){
            observation.cacheCaptured=true;observation.cacheCpuCount=count;observation.cachePat=cacheSnapshots[0].pat;
            observation.cacheMatch=true;for(uint32_t i=0;i<count;++i)
                if(!CpuCacheSnapshotsMatch(cacheSnapshots[0],cacheSnapshots[i]))observation.cacheMatch=false;
        }
        SerialPrintf("HDA_CACHE_OBSERVE captured=%u cpus=%u match=%u pat=0x%lx\n",unsigned(observation.cacheCaptured),
                     observation.cacheCpuCount,unsigned(observation.cacheMatch),observation.cachePat);
    }
    if(Marker("/etc/dell-hda-dma-cache-observe"))ObserveCachePages(io);
    if(runtimeRequested){
        observation.runtimeRequested=true;
        if(muted || audible || mixerProbe || Marker("/etc/dell-hda-stream-probe")){observation.runtimeReason="conflicting markers";return;}
        if(observation.session.result!=HdaSessionResult::Complete || observation.session.after!=0 || !observation.mapped){observation.runtimeReason="observation incomplete";return;}
        PrepareRuntime(io);return;
    }
    if(Marker("/etc/dell-hda-stream-probe")){
        observation.streamRequested=true;
        if(muted || audible || mixerProbe){observation.streamReason="conflicting markers";return;}
        if(observation.session.result!=HdaSessionResult::Complete || observation.session.after!=0 || !observation.mapped){observation.streamReason="observation incomplete";return;}
        RunStreamProbe(io);return;
    }
    if(!muted && !audible)return;
    observation.toneRequested=true;observation.toneAudible=audible;
    if(muted && audible){observation.toneReason="conflicting markers";return;}
    if(mixerProbe && (!muted || audible)){observation.toneReason="mixer format probe requires mute";return;}
    if(observation.session.result!=HdaSessionResult::Complete || observation.session.after!=0 || !observation.mapped){
        observation.toneReason="observation incomplete";return;}
    // Explicit placement storage avoids destructing quarantined ownership.
    alignas(TonePlatform) static unsigned char platformStorage[sizeof(TonePlatform)];
    using ToneAdapter=HdaDellToneAdapter<TonePlatform>;
    alignas(ToneAdapter) static unsigned char adapterStorage[sizeof(ToneAdapter)];
    auto* platform=::new(platformStorage) TonePlatform{io,observation.session};
    auto* adapter=::new(adapterStorage) ToneAdapter(*platform,observation.session,mixerProbe);
    observation.toneAttempted=true;
    HdaDellTonePlan checkPlan{};if(HdaDellMakeTonePlan(observation.session,checkPlan))observation.toneAdmission|=16;
    uint32_t flushLine=0;if(!platform->flushInfo(flushLine)){observation.toneReason="cache flush unavailable";return;}
    if(!platform->delayUs(3)){observation.toneReason="clock unavailable";return;}
    if(!adapter->allocate()){observation.toneReason="allocation or admission failed";return;}
    if(!adapter->begin()){observation.toneReason="controller admission failed";return;}
    const auto result=adapter->run(audible);observation.toneResult=uint8_t(result);
    observation.tonePrepareStage=adapter->prepareStage();observation.tonePrepareValue=adapter->prepareValue();
    observation.toneStreamStage=adapter->streamStage();observation.toneStreamDetail=adapter->streamDetail();observation.toneCommand=platform->command();observation.toneIciFailed=adapter->iciFailed();
    observation.toneFifoSize=adapter->streamFifoSize();observation.toneFifoSampled=adapter->streamFifoSampled();
    observation.tonePlayReason=adapter->playReason();observation.tonePlayLoops=adapter->playLoops();observation.tonePlayProgress=adapter->playProgress();observation.tonePlayElapsed=adapter->playElapsed();
    observation.toneProgressReason=adapter->progressReason();observation.toneProgressStatus=adapter->progressStatus();observation.toneProgressControl=adapter->progressControl();
    observation.toneProgressFirst=adapter->progressFirst();observation.toneProgressLast=adapter->progressLast();
    if(result!=HdaToneResult::Complete){observation.toneReason="transaction failed";return;}
    observation.toneFinished=adapter->finish();
    observation.toneCommand=platform->command();
    observation.toneReason=observation.toneFinished?"complete":"decode restore failed";
    SerialPrintf("HDA_TONE mode=%s result=%u finished=%u quarantine=%u reason=%s\n",
        audible?"quiet":"muted",unsigned(observation.toneResult),unsigned(observation.toneFinished),
        unsigned(observation.toneQuarantined),observation.toneReason);
}
const HdaObservation& HdaObserveRead(){return observation;}
void HdaRuntimeRegister(){
    static bool attempted=false;
    if(attempted || !observation.runtimePrepared || !runtimeOwner || !runtimeBridge)return;
    attempted=true;
    static const AudioDriver driver{"dell_hda",RuntimePlay,RuntimeStop,RuntimePlaying,RuntimePosition,RuntimeService,RuntimeSubmit,RuntimeProgress,RuntimeRestart};
    if(!AudioRegister(&driver)){
        const bool muted=runtimeOwner->mute(),stopped=runtimeOwner->stop();
        if(!muted || !stopped || !runtimeOwner->retire())runtimeOwner->quarantine();
        observation.runtimePrepared=false;observation.runtimeReason="audio registration rejected";
        SerialPrintf("HDA_RUNTIME registration rejected quarantined=%u\n",unsigned(__atomic_load_n(&observation.runtimeQuarantined,__ATOMIC_ACQUIRE)));return;
    }
    observation.runtimeRegistered=true;observation.runtimeReason="registered muted (experimental)";
    SerialPrintf("HDA_RUNTIME registered muted experimental\n");
}
}
