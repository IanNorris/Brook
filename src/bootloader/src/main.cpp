#include <Uefi.h>
#include "console.h"
#include "boot_splash.h"
#include "brook_boot_logo_data.h"
#include "config.h"
#include "graphics.h"
#include "memory.h"
#include "firmware_runtime_cache.h"
#include "acpi.h"
#include "elf_loader.h"
#include "fs.h"
#include "update_slots.h"
#include "paging.h"
#include "boot_protocol/boot_protocol.h"
#if defined(BROOK_EFI_BOOT_INVENTORY)
#include "boot_variable_inventory.h"
#endif
#if defined(BROOK_GUARD_CHECK_DATA)
#include "recovery_firmware_discovery.h"
static bool ConfiguredDataIsClean(EFI_HANDLE image,EFI_BOOT_SERVICES* services) {
    brook::bootloader::RecoveryDiskIdentity id{};
    const char* input=BROOK_GUARD_IDENTITY;uint8_t* fields[]={id.diskGuid,id.dataGuid,id.fsUuid};
    for(unsigned field=0;field<3;++field)for(unsigned i=0;i<16;++i) {
        unsigned value=0;
        for(unsigned j=0;j<2;++j) {const char c=*input++;value=value*16+(c<='9'?c-'0':c-'a'+10);}
        fields[field][i]=static_cast<uint8_t>(value);
    }
    id.dataFirst=BROOK_GUARD_DATA_FIRST;id.dataSectors=BROOK_GUARD_DATA_SECTORS;
    return brook::bootloader::RecoveryInspectFirmwareDisk(image,services,id)==brook::bootloader::RecoveryDiskState::Clean;
}
#endif

static inline uint64_t ReadTsc()
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// Decimal seconds with up to two fractional digits, e.g. "0.5", "2", "1.25".
static void PrintSeconds(uint32_t ms)
{
    using namespace brook::bootloader;
    ConsolePrintDec(ms / 1000);
    const uint32_t centis = (ms % 1000) / 10;
    if (!centis) return;
    char16_t frac[4] = {u'.', static_cast<char16_t>(u'0' + centis / 10),
                        static_cast<char16_t>(u'0' + centis % 10), 0};
    if (frac[2] == u'0') frac[2] = 0;
    ConsolePrint(frac);
}

extern "C" EFI_STATUS EFIAPI EfiMain(EFI_HANDLE imageHandle, EFI_SYSTEM_TABLE* systemTable)
{
    using namespace brook::bootloader;
    brook::BootTiming timing{};
    timing.loaderEntryTsc = ReadTsc();

    ConsoleInit(systemTable->ConOut);
    ConsoleSetQuiet(true);
#if defined(BROOK_EFI_BOOT_INVENTORY)
    SaveBootVariableInventory(imageHandle,systemTable);
#endif

    // --- Config ---
    LoadConfig(imageHandle, systemTable->BootServices);
    ConsoleSetQuiet(!g_bootConfig.debugText);
    ConsolePrintLine(u"Brook UEFI: configuration loaded");

    // --- Graphics ---
    brook::Framebuffer framebuffer{};
    if (!GraphicsInit(systemTable->BootServices, framebuffer))
    {
        Halt(EFI_UNSUPPORTED, u"No GOP framebuffer available");
    }
    auto splash=[&](unsigned percent,bool full=false){
        brook::DrawBootSplash(reinterpret_cast<uint32_t*>(framebuffer.physicalBase),
            framebuffer.width,framebuffer.height,framebuffer.stride/4,
            g_bootLogoData,g_bootLogoWidth,g_bootLogoHeight,percent,full,
            framebuffer.format==brook::PixelFormat::Rgb8);
    };
    splash(1,true);
    ConsolePrintLine(u"Brook UEFI: framebuffer ready");

    // --- ACPI ---
    brook::AcpiInfo acpi{};
    if (!AcpiInit(systemTable, acpi))
    {
        Halt(EFI_NOT_FOUND, u"ACPI 2.0 not found");
    }
    splash(2);
    ConsolePrintLine(u"Brook UEFI: ACPI located; reading kernel");

    // --- Load kernel ---
    UINTN kernelFileSize = 0;
    uint8_t* kernelData = nullptr;
    uint8_t* initrdData = nullptr;
    UINTN initrdFileSize = 0;
    bool recoveryRequested = false;
#if defined(BROOK_RECOVERY_GUARD_REQUIRED)
    // This build variant always checks before considering any normal A/B or
    // legacy payload, regardless of BROOK.CFG. A failed check can only enter
    // the independently verified recovery pair.
    bool guardDenied = !RecoveryGuardAllowsNormal(imageHandle,systemTable->BootServices,BROOK_GUARD_IDENTITY);
#if defined(BROOK_GUARD_CHECK_DATA)
    if(!guardDenied)guardDenied=!ConfiguredDataIsClean(imageHandle,systemTable->BootServices);
#endif
    if (guardDenied) recoveryRequested = true;
#endif
    if (!recoveryRequested && g_bootConfig.recoveryMenu && systemTable->ConIn &&
        g_bootConfig.recoveryMenuMs) {
        const uint32_t waitMs = g_bootConfig.recoveryMenuMs;
        timing.waitConfiguredMs = waitMs;
        ConsoleSetQuiet(false);
        ConsolePrint(u"Press F5 for Brook recovery; normal boot continues in ");
        PrintSeconds(waitMs);
        ConsolePrintLine(u" s.");
        ConsoleSetQuiet(!g_bootConfig.debugText);
        timing.waitStartTsc = ReadTsc();
        // Poll in 10 ms steps so short waits stay close to the configured length.
        for (uint32_t elapsed = 0; elapsed < waitMs; elapsed += 10) {
            EFI_INPUT_KEY key{};
            EFI_STATUS status=systemTable->ConIn->ReadKeyStroke(systemTable->ConIn,&key);
            if (!EFI_ERROR(status)) {
                ++timing.waitKeyCount;
                timing.waitLastKey = key.ScanCode | (static_cast<uint32_t>(key.UnicodeChar) << 16);
            }
            if (!EFI_ERROR(status) && key.ScanCode==SCAN_F5) {
                recoveryRequested=true;timing.flags|=brook::BootTimingWaitKey;break;
            }
            systemTable->BootServices->Stall(10000);
        }
        timing.waitEndTsc = ReadTsc();
        timing.flags |= brook::BootTimingWaitRan;
    }
    if (recoveryRequested) {
        timing.flags |= brook::BootTimingRecovery;
        ConsoleSetQuiet(false);
#if defined(BROOK_RECOVERY_GUARD_REQUIRED)
        if (guardDenied) ConsolePrintLine(u"Recovery required; normal boot blocked.");
#endif
        ConsolePrintLine(u"Recovery selected; verifying payload.");
        ConsoleSetQuiet(!g_bootConfig.debugText);
    }
    bool updatePair = false;
#if defined(BROOK_GUARD_ALTERNATE_RECOVERY)
    if (recoveryRequested) updatePair = LoadAlternateRecoveryPair(imageHandle,
        systemTable->BootServices, kernelData, kernelFileSize, initrdData, initrdFileSize);
#endif
    if (recoveryRequested && !updatePair) updatePair = LoadRecoveryPair(imageHandle,
        systemTable->BootServices, kernelData, kernelFileSize, initrdData, initrdFileSize);
    if (updatePair) {
        ConsoleSetQuiet(false);
        ConsolePrintLine(u"Brook SSD recovery payload verified");
        ConsoleSetQuiet(!g_bootConfig.debugText);
    }
    if (recoveryRequested && !updatePair) {
        ConsoleSetQuiet(false);
#if defined(BROOK_RECOVERY_GUARD_REQUIRED)
        if (guardDenied) Halt(EFI_NOT_FOUND, u"Recovery required but payload invalid or missing.");
#endif
        Halt(EFI_NOT_FOUND, u"Recovery payload invalid or missing; restart to boot normally.");
    }
    if (!updatePair) updatePair = g_bootConfig.updateSlots && LoadUpdatePair(imageHandle,
        systemTable->BootServices, kernelData, kernelFileSize, initrdData, initrdFileSize);
    if (!updatePair) kernelData = ReadFile(imageHandle, systemTable->BootServices,
        g_bootConfig.target, &kernelFileSize);
    if (kernelData == nullptr)
    {
        Halt(EFI_NOT_FOUND, u"Kernel not found");
    }
    splash(3);

    EFI_PHYSICAL_ADDRESS kernelPhysBase = 0;
    UINTN kernelPhysPages = 0;
    KernelEntryFn kernelVirtualEntry = LoadKernelElf(
        systemTable->BootServices, kernelData, kernelFileSize, kernelPhysBase, kernelPhysPages);
    splash(4);
    ConsolePrint(u"Brook UEFI: kernel loaded; entry ");
    ConsolePrintHex(reinterpret_cast<UINT64>(kernelVirtualEntry));
    ConsolePrintLine(u"");

    // Free the temporary ELF file buffer (segments already copied out)
    FreePages(systemTable->BootServices,
        reinterpret_cast<EFI_PHYSICAL_ADDRESS>(kernelData), kernelFileSize);

    // --- Load initrd (optional) ---
    if (!updatePair) initrdData = ReadFile(imageHandle, systemTable->BootServices,
        u"INITRD.IMG", &initrdFileSize);
    if (initrdData != nullptr)
    {
        ConsolePrintLine(u"Initrd loaded");
    }
    else
    {
        ConsolePrintLine(u"No initrd found (optional)");
    }

    // --- Allocate page table memory (must be before ExitBootServices) ---
    // Pass kernelPhysPages so AllocatePageTables can size the kernel PT pool.
    // It also scans the UEFI memory map internally to size the identity map.
    PageTableAllocation pageTableAlloc = AllocatePageTables(systemTable->BootServices, kernelPhysPages);
    splash(5);

    // Remember the physical entrypoint before leaving firmware. It is only
    // handed off if the final memory map identifies it as runtime code.
    const UINT64 efiResetSystem = systemTable->RuntimeServices
        ? reinterpret_cast<UINT64>(systemTable->RuntimeServices->ResetSystem) : 0;

    // --- Memory map (must be last thing before ExitBootServices) ---
    brook::MemoryDescriptor* memoryMap = nullptr;
    UINT32 memoryMapCount              = 0;
    UINTN mapKey = BuildMemoryMap(systemTable->BootServices, &memoryMap, &memoryMapCount);
    ConsolePrintLine(u"Memory map built");

    ConsolePrintLine(u"Brook UEFI: leaving firmware and entering kernel");

    // --- Exit boot services ---
    timing.exitBootServicesTsc = ReadTsc();
    EFI_STATUS status = systemTable->BootServices->ExitBootServices(imageHandle, mapKey);
    if (EFI_ERROR(status))
    {
        mapKey = BuildMemoryMap(systemTable->BootServices, &memoryMap, &memoryMapCount);
        status = systemTable->BootServices->ExitBootServices(imageHandle, mapKey);
        if (EFI_ERROR(status))
        {
            Halt(status, u"ExitBootServices failed");
        }
    }

    // No UEFI calls after ExitBootServices; capture the actual firmware page
    // mappings while its original CR3 is still active.
    const auto firmwareRuntimeCache = CaptureFirmwareRuntimeCache(memoryMap,memoryMapCount);

    // --- Build and activate page tables ---
    // After this, virtual 0xFFFFFFFF80000000 maps to kernelPhysBase.
    // Low memory 0-4GB is identity-mapped so bootloader code continues running.
    BuildPageTables(pageTableAlloc, kernelPhysBase, kernelPhysPages);
    LoadCR3(pageTableAlloc);

    // --- Build boot protocol ---
    // This struct is on the stack (in identity-mapped low memory), accessible
    // from both the current low address and after the kernel takes over.
    brook::BootProtocol bootProtocol{};
    bootProtocol.magic          = brook::BootProtocolMagic;
    bootProtocol.version        = brook::BootProtocolVersion;
    bootProtocol.framebuffer    = framebuffer;
    bootProtocol.acpi           = acpi;
    bootProtocol.memoryMap      = memoryMap;
    bootProtocol.memoryMapCount = memoryMapCount;
    bootProtocol.kernelPhysBase  = kernelPhysBase;
    bootProtocol.kernelPhysPages = kernelPhysPages;
    bootProtocol.initrdPhysBase  = initrdData
        ? reinterpret_cast<uint64_t>(initrdData) : 0;
    bootProtocol.initrdSize      = initrdData ? initrdFileSize : 0;
    bootProtocol.efiResetSystemPhysical = brook::BootEfiRuntimeCodeContains(
        memoryMap, memoryMapCount, efiResetSystem) ? efiResetSystem : 0;
    bootProtocol.efiRuntimeCacheFingerprint = firmwareRuntimeCache.fingerprint.hash;
    bootProtocol.efiRuntimePat = firmwareRuntimeCache.pat;
    bootProtocol.efiRuntimePatSlots = firmwareRuntimeCache.fingerprint.slots;
    bootProtocol.efiRuntimeCacheValid = firmwareRuntimeCache.fingerprint.valid ? 1 : 0;
    bootProtocol.efiRuntimeMtrrFingerprint = firmwareRuntimeCache.mtrr;
    bootProtocol.efiRuntimeMtrrValid = firmwareRuntimeCache.mtrr ? 1 : 0;
    timing.flags |= brook::BootTimingValid;
    timing.kernelJumpTsc = ReadTsc();
    bootProtocol.timing = timing;
    bootProtocol.extensions = brook::BootExtensionTimingMagic;

    // --- Jump to kernel at virtual address ---
    // Use inline asm to ensure SysV calling convention:
    // first argument (bootProtocol ptr) goes in RDI, not RCX.
    {
        void* entry    = reinterpret_cast<void*>(kernelVirtualEntry);
        void* protocol = &bootProtocol;
        asm volatile(
            "mov %1, %%rdi\n\t"
            "jmp *%0\n\t"
            :
            : "r"(entry), "r"(protocol)
            : "rdi"
        );
    }

    // Should never reach here
    for (;;) { __asm__ volatile("hlt"); }
}
