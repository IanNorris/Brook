#include "platform_power.h"
#include "acpi.h"
#include "fw_cfg.h"
#include "portio.h"
#include "serial.h"
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
}
void PlatformPowerConfigure() {
    plan={};plan.firmwareReset=AcpiGetResetInfo();
    char mode[24]{};const auto bytes=FwCfgReadFile("opt/brook/power",mode,sizeof(mode)-1);
    if(bytes==9 && !memcmp(mode,"q35-reset",9)){plan.q35Off=true;plan.q35Reset=true;}
    else if(bytes==8 && !memcmp(mode,"q35-stop",8)){plan.q35Off=true;plan.resetDisabledByLaunch=true;}
#if defined(BROOK_EFI_SHUTDOWN_TEST)
    else if(bytes==8 && !memcmp(mode,"efi-test",8))plan.efiEntry=efiTestCandidate;
#endif
    SerialPrintf("power: reset=%s poweroff=%s reason=%s\n",plan.ResetMethod(),plan.PowerOffMethod(),plan.PowerOffReason());
}
const PlatformPowerPlan& PlatformPowerGetPlan(){return plan;}
bool PlatformPowerIssue(bool restart){return plan.Issue(restart,{nullptr,Write8,Write16
#if !defined(BROOK_HOST_TEST)
    ,EfiTestOff
#endif
});}
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
