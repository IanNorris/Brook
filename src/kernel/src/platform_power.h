#pragma once
#include "acpi_tables.h"
#include "boot_protocol/boot_protocol.h"
namespace brook {
struct PowerIo {
    void* context;
    bool (*write8)(void*,uint16_t,uint8_t);
    bool (*write16)(void*,uint16_t,uint16_t);
    bool (*efiOff)(void*,uint64_t)=nullptr;
    bool (*read16)(void*,uint16_t,uint16_t&)=nullptr;
    bool (*efiWarmReset)(void*,uint64_t)=nullptr;
};
struct PlatformPowerPlan {
    AcpiResetInfo firmwareReset{};
    bool q35Off=false,q35Reset=false,resetDisabledByLaunch=false;
    uint64_t efiEntry=0;
    AcpiS5Info acpiS5{};
    bool CanReset() const {return !resetDisabledByLaunch && (firmwareReset.available || q35Reset);}
    bool CanForceEfiRestart() const { return !resetDisabledByLaunch && efiEntry != 0; }
    bool CanPowerOff() const {return q35Off || efiEntry || acpiS5.Available();}
    const char* ResetMethod() const {return resetDisabledByLaunch?"disabled":firmwareReset.available?"acpi-io8":q35Reset?"q35-io8":"unavailable";}
    const char* PowerOffMethod() const {return efiEntry?"efi-runtime":q35Off?"q35-io16":acpiS5.Available()?"acpi-s5":"manual";}
    const char* ResetReason() const {return resetDisabledByLaunch?"Reset disabled by stop-only launch policy":CanReset()?ResetMethod():firmwareReset.reason;}
    const char* PowerOffReason() const {return efiEntry?"Validated firmware runtime opt-in":q35Off?"Explicit Q35 power-off opt-in":acpiS5.reason;}
    bool IssueForcedRestart(const PowerIo& io) const {
        // The firmware callback returns true if it issued ResetSystem.
        // If its runtime recheck fails before the call, use the platform reset.
        if (CanForceEfiRestart() && io.efiWarmReset && io.efiWarmReset(io.context, efiEntry)) return true;
        return Issue(true, io);
    }
    // Select once. In particular, failure of the advertised register never
    // triggers a speculative alternative reset port or register width.
    bool Issue(bool restart,const PowerIo& io) const {
        if(restart) {
            if(resetDisabledByLaunch)return false;
            if(firmwareReset.available)
                return io.write8 && io.write8(io.context,firmwareReset.port,firmwareReset.value);
            if(q35Reset)return io.write8 && io.write8(io.context,0xcf9,6);
        } else if(efiEntry)return io.efiOff && io.efiOff(io.context,efiEntry);
        else if(q35Off)return io.write16 && io.write16(io.context,0x604,0x2000);
        else if(acpiS5.Available()) {
            constexpr uint16_t sleepEnable=1u<<13,sleepType=7u<<10;
            const auto a=acpiS5.controls.pm1a,b=acpiS5.controls.pm1b;
            uint16_t oldA=0,oldB=0;
            if(!io.read16 || !io.write16 || acpiS5.typeA>7 || acpiS5.typeB>7 ||
               !io.read16(io.context,a,oldA) || !(oldA&1) || oldA==0xffff ||
               (b && (!io.read16(io.context,b,oldB) || oldB==0xffff)))return false;
            // Preserve unrelated writable fields. Stage both types before
            // enabling sleep, in case the first enable immediately stops us.
            const uint16_t typeA=(oldA&~(sleepEnable|sleepType)) | uint16_t(acpiS5.typeA)<<10;
            const uint16_t typeB=(oldB&~(sleepEnable|sleepType)) | uint16_t(acpiS5.typeB)<<10;
            if(!io.write16(io.context,a,typeA) || (b && !io.write16(io.context,b,typeB)))return false;
            return io.write16(io.context,a,typeA|sleepEnable) &&
                   (!b || io.write16(io.context,b,typeB|sleepEnable));
        }
        return false;
    }
};
// Best-effort device quiesce hooks run just before the selected reset or
// power-off write (bus-master DMA stopped, controllers halted). Each hook must
// be bounded; none can veto the transition. Registration is append-only.
using PowerQuiesceFn = void (*)();
struct PowerQuiesceList {
    static constexpr unsigned Capacity = 8;
    PowerQuiesceFn hooks[Capacity]{};
    unsigned count = 0;
    bool Add(PowerQuiesceFn fn) {
        if (!fn) return false;
        for (unsigned i = 0; i < count; ++i) if (hooks[i] == fn) return true;
        if (count >= Capacity) return false;
        hooks[count] = fn;
        __atomic_store_n(&count, count + 1, __ATOMIC_RELEASE);
        return true;
    }
    // Reverse registration order, as device shutdown runs children first.
    unsigned Run() const {
        const unsigned n = __atomic_load_n(&count, __ATOMIC_ACQUIRE);
        for (unsigned i = n; i-- > 0;) hooks[i]();
        return n;
    }
};
extern "C" bool PlatformPowerAddQuiesce(PowerQuiesceFn fn);
extern "C" void PlatformPowerExtendResetWatchdog(uint32_t ms);
unsigned PlatformPowerRunQuiesce();
void PlatformPowerConfigure(); // boot-time discovery and explicit emulator opt-in
const PlatformPowerPlan& PlatformPowerGetPlan();
// True means the selected write was issued, not proof that hardware reset/off
// succeeded. The coordinator stays quiescent and reports a returned transition.
bool PlatformPowerIssue(bool restart, bool forceWarm = false);
struct BootProtocol;
// EFI runtime power-off needs at least a version-5 handoff (cache and MTRR
// fingerprints). Later loaders keep version 5 and append extensions, so this
// must never require an exact version.
inline bool PlatformPowerEfiHandoffAccepted(const BootProtocol* boot, bool inventoryValid)
{
    return boot && inventoryValid && boot->version >= 5 && boot->efiRuntimeCacheValid == 1 &&
           boot->efiRuntimeMtrrValid == 1 && boot->memoryMapCount <= 4096;
}
void PlatformPowerSetEfiCandidate(const BootProtocol* boot);
bool PlatformPowerEnableEfi();
}
