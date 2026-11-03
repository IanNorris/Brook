#include <Uefi.h>
#include "recovery_firmware_discovery.h"

// Only built into BROOKPROBE.efi, for disposable QEMU fixtures. QEMU debugcon
// is not part of the production boot path and this image intentionally halts.
static void Report(const char* text) {
    while(*text) {unsigned char c=static_cast<unsigned char>(*text++);__asm__ volatile("outb %0, $0xe9" : : "a"(c));}
}
extern "C" EFI_STATUS EFIAPI EfiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE* system) {
    brook::bootloader::RecoveryDiskIdentity id{};
    const char* input=BROOK_RECOVERY_PROBE_IDENTITY;
    uint8_t* fields[]={id.diskGuid,id.dataGuid,id.fsUuid};
    for(unsigned field=0;field<3;++field)for(unsigned i=0;i<16;++i) {
        unsigned value=0;
        for(unsigned j=0;j<2;++j) {char c=*input++;value=value*16+(c<='9'?c-'0':c-'a'+10);}
        fields[field][i]=static_cast<uint8_t>(value);
    }
    id.dataFirst=BROOK_RECOVERY_PROBE_FIRST;id.dataSectors=BROOK_RECOVERY_PROBE_SECTORS;
    brook::bootloader::RecoveryFirmwareDisk disk{};
    Report(brook::bootloader::RecoveryFindFirmwareDisk(image,system->BootServices,disk)
        ?"BROOK_PROBE_PARENT_FOUND\n":"BROOK_PROBE_PARENT_NOT_FOUND\n");
    const auto state=brook::bootloader::RecoveryInspectFirmwareDisk(image,system->BootServices,id);
    using brook::bootloader::RecoveryDiskState;
    Report(state==RecoveryDiskState::Clean?"BROOK_PROBE_CLEAN\n":
           state==RecoveryDiskState::NeedsCheck?"BROOK_PROBE_DIRTY\n":"BROOK_PROBE_INVALID\n");
    for(;;)__asm__ volatile("hlt");
}
