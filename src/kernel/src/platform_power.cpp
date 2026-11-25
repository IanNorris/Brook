#include "platform_power.h"
#include "apic.h"
#if !defined(BROOK_HOST_TEST)
#include "intel_bdw_scanout_target.h"
#endif
#include "acpi.h"
#include "fw_cfg.h"
#include "portio.h"
#include "serial.h"
#if !defined(BROOK_HOST_TEST)
#include "kprintf.h"
#endif
#include <stddef.h>
#if !defined(BROOK_HOST_TEST)
#include "efi_runtime_mapping.h"
#include "boot_protocol/efi_runtime_cache.h"
#include "boot_protocol/efi_runtime_mtrr.h"
#include "memory/virtual_memory.h"
#include "cpu.h"
#endif
#ifdef BROOK_HOST_TEST
#include <cstring>
#else
#include "string.h"
#endif
namespace brook {
namespace {
PlatformPowerPlan plan;
PowerQuiesceList quiesce;
#if !defined(BROOK_HOST_TEST)
uint64_t efiTestCandidate=0;
MemoryDescriptor efiTestMap[4096]{};
BootProtocol efiTestBoot{};
bool EfiTestOff(void*,uint64_t entry) {
    uint64_t cr0=0;__asm__ volatile("mov %%cr0,%0":"=r"(cr0));
    if(cr0&((1ull<<2)|(1ull<<3)))return false;
    uint64_t flags=0,cr3=0;
    __asm__ volatile("pushfq; popq %0; cli; cld; mov %%cr3,%1":"=r"(flags),"=r"(cr3)::"memory");
    const auto inventory=InspectEfiRuntime(&efiTestBoot);
    auto probe=[](uint64_t address,KernelAliasMapping& identity) {
        KernelAliasMapping direct{};return VmmInspectKernelAliases(address,identity,direct);
    };
    const auto mapping=InspectEfiRuntimeMappings(&efiTestBoot,inventory,probe);
    bool cacheMatches=true;
    if(efiTestBoot.version>=4){
        auto cacheProbe=[&](uint64_t address,uint64_t& physical,uint8_t& pat){KernelAliasMapping m{};
            if(!probe(address,m) || !m.present)return false;physical=m.physical;pat=m.patIndex;return true;};
        const auto current=FingerprintRuntimeCache(efiTestBoot.memoryMap,efiTestBoot.memoryMapCount,cacheProbe);
        uint32_t low=0,high=0;__asm__ volatile("rdmsr":"=a"(low),"=d"(high):"c"(0x277));
        cacheMatches=efiTestBoot.efiRuntimeCacheValid==1 && current.valid &&
            current.hash==efiTestBoot.efiRuntimeCacheFingerprint && current.slots==efiTestBoot.efiRuntimePatSlots &&
            RuntimePatSlotsMatch(efiTestBoot.efiRuntimePat,uint64_t(low)|(uint64_t(high)<<32),current.slots);
    }
    if(efiTestBoot.version>=5){
        CpuCacheSnapshot snapshot{};const bool available=CpuCaptureCacheSnapshot(snapshot);
        const auto& m=snapshot.mtrrs;
        const uint64_t fingerprint=available?RuntimeMtrrFingerprint(m.physicalBits,m.capability,m.defaults,m.base,m.mask,m.fixed,m.count):0;
        cacheMatches=cacheMatches && efiTestBoot.efiRuntimeMtrrValid==1 && fingerprint &&
            fingerprint==efiTestBoot.efiRuntimeMtrrFingerprint && !snapshot.cacheDisabled && !snapshot.notWriteThrough;
    }
    if((cr3&0x000ffffffffff000ull)!=VmmKernelCR3().pml4.raw() || entry!=inventory.resetSystem ||
       !mapping.complete || mapping.unavailable || mapping.wrongAddress || mapping.nonExecutable || mapping.nonWritable || !cacheMatches) {
        __asm__ volatile("pushq %0; popfq"::"r"(flags):"memory","cc");
        SerialPuts("efi-runtime: shutdown recheck failed\n");return false;
    }
    using ResetFn=void (__attribute__((ms_abi)) *)(uint32_t,uint64_t,uint64_t,void*);
    const auto reset=reinterpret_cast<ResetFn>(entry);
    uint16_t oldCw=0;uint32_t oldMxcsr=0;const uint16_t cw=0x037f;const uint32_t mxcsr=0x1f80;
    SerialPuts("efi-runtime: shutdown call\n");
    __asm__ volatile("fnstcw %0; stmxcsr %1; fldcw %2; ldmxcsr %3"
        :"=m"(oldCw),"=m"(oldMxcsr):"m"(cw),"m"(mxcsr):"memory");
    reset(2,0,0,nullptr);
    __asm__ volatile("fldcw %0; ldmxcsr %1; pushq %2; popfq"::"m"(oldCw),"m"(oldMxcsr),"r"(flags):"memory","cc");
    SerialPuts("efi-runtime: shutdown returned\n");return false;
}
#endif
bool Write8(void*,uint16_t port,uint8_t value){outb(port,value);return true;}
bool Write16(void*,uint16_t port,uint16_t value){outw(port,value);return true;}
bool Read16(void*,uint16_t port,uint16_t& value){value=inw(port);return true;}
}
void PlatformPowerConfigure() {
    plan={};plan.firmwareReset=AcpiGetResetInfo();
    char mode[24]{};const auto bytes=FwCfgReadFile("opt/brook/power",mode,sizeof(mode)-1);
    if(bytes==9 && !memcmp(mode,"q35-reset",9)){plan.q35Off=true;plan.q35Reset=true;}
    else if(bytes==8 && !memcmp(mode,"q35-stop",8)){plan.q35Off=true;plan.resetDisabledByLaunch=true;}
#if defined(BROOK_EFI_SHUTDOWN_TEST)
    else if(bytes==8 && !memcmp(mode,"efi-test",8))plan.efiEntry=efiTestCandidate;
#endif
    if(!plan.q35Off && !plan.efiEntry) {
        plan.acpiS5=AcpiGetS5Info();
        if(plan.acpiS5.Available()) {
            uint16_t control=0;
            if(!Read16(nullptr,plan.acpiS5.controls.pm1a,control) || control==0xffff || !(control&1)) {
                plan.acpiS5={};
                plan.acpiS5.reason="Firmware ACPI mode is unavailable";
            }
        }
    }
    SerialPrintf("power: reset=%s poweroff=%s reason=%s\n",plan.ResetMethod(),plan.PowerOffMethod(),plan.PowerOffReason());
#if !defined(BROOK_HOST_TEST)
    if(plan.firmwareReset.available)
        KPrintf("power: FADT reset register io 0x%x value 0x%x\n",plan.firmwareReset.port,plan.firmwareReset.value);
    else KPrintf("power: reset=%s (%s)\n",plan.ResetMethod(),plan.ResetReason());
#endif
}
const PlatformPowerPlan& PlatformPowerGetPlan(){return plan;}
bool PlatformPowerAddQuiesce(PowerQuiesceFn fn){return quiesce.Add(fn);}
// Each hook is logged with its address and duration, so a reset that never
// lands shows (on serial / the bridge log) which device quiesce stopped in.
unsigned PlatformPowerRunQuiesce(){
    const unsigned n=__atomic_load_n(&quiesce.count,__ATOMIC_ACQUIRE);
    for(unsigned i=n;i-->0;){
        const uint64_t start=ApicTickCount();
        SerialPrintf("power: quiesce %u/%u at %p\n",n-i,n,reinterpret_cast<void*>(quiesce.hooks[i]));
        quiesce.hooks[i]();
        SerialPrintf("power: quiesce %u/%u done in %lu ms\n",n-i,n,static_cast<unsigned long>(ApicTickCount()-start));
    }
    return n;
}
// Reset watchdog: armed before the quiesce hooks run; if the reset has not
// been issued by the deadline (a hook hung), the timer tick forces it.
static volatile uint64_t resetDeadline=0;
extern "C" void PlatformPowerResetWatchdogTick(uint64_t now){
    const uint64_t deadline=__atomic_load_n(&resetDeadline,__ATOMIC_ACQUIRE);
    if(!deadline || now<deadline)return;
    if(!__atomic_compare_exchange_n(&resetDeadline,const_cast<uint64_t*>(&deadline),0,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
    SerialPuts("power: quiesce watchdog expired; forcing reset\n");
    (void)plan.Issue(true,{nullptr,Write8,Write16,nullptr,Read16});
}
// A hook with a known long, bounded step (the GPU reset's GFW-boot waits) pushes an
// armed deadline out to now + ms; never shortens it, never arms one.
extern "C" void PlatformPowerExtendResetWatchdog(uint32_t ms){
    for(;;){
        uint64_t d=__atomic_load_n(&resetDeadline,__ATOMIC_ACQUIRE);
        const uint64_t want=ApicTickCount()+ms;
        if(!d||d>=want)return;
        if(__atomic_compare_exchange_n(&resetDeadline,&d,want,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
            SerialPrintf("power: reset watchdog extended by %u ms\n",ms);return;}
    }
}
bool PlatformPowerIssue(bool restart){
    // Stop device DMA first: a reset that lands while xHCI/NIC bus masters
    // are running has been seen to hang the desktop firmware in POST.
    if(restart?plan.CanReset():plan.CanPowerOff()){
        if(restart && plan.CanReset())__atomic_store_n(&resetDeadline,ApicTickCount()+5000,__ATOMIC_RELEASE);
        const unsigned hooks=PlatformPowerRunQuiesce();
        SerialPrintf("power: %u quiesce hook%s ran before %s\n",hooks,hooks==1?"":"s",restart?"reset":"power-off");
    }
#if !defined(BROOK_HOST_TEST)
    // Leave plane A on the firmware GOP surface for firmware and the next
    // (possibly recovery) kernel; a no-op unless a flip path was activated.
    BdwScanoutPanicRestore();
#endif
    return plan.Issue(restart,{nullptr,Write8,Write16,
#if !defined(BROOK_HOST_TEST)
    EfiTestOff,
#else
    nullptr,
#endif
    Read16});}
#if !defined(BROOK_HOST_TEST)
void PlatformPowerSetEfiCandidate(const BootProtocol* boot){
    const auto inventory=InspectEfiRuntime(boot);
    if(!PlatformPowerEfiHandoffAccepted(boot,inventory.valid))return;
    // Copy only what every version-5 loader provides; an older loader's
    // handoff ends before the appended timing extension.
    __builtin_memcpy(&efiTestBoot,boot,BootProtocolV5Size);
    for(uint32_t i=0;i<boot->memoryMapCount;++i)efiTestMap[i]=boot->memoryMap[i];
    efiTestBoot.memoryMap=efiTestMap;efiTestCandidate=inventory.resetSystem;
}
#else
void PlatformPowerSetEfiCandidate(const BootProtocol*){}
#endif
bool PlatformPowerEnableEfi(){
#if !defined(BROOK_HOST_TEST)
    if(!efiTestCandidate)return false;
    plan.efiEntry=efiTestCandidate;return true;
#else
    return false;
#endif
}
}
