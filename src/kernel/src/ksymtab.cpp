#include "ksymtab.h"
#include "serial.h"

// The linker script places all .ksymtab entries between these two symbols.
// Declared as arrays so &__start_ksymtab gives the address of the first entry.
extern "C" brook::KernelSymbol __start_ksymtab[];
extern "C" brook::KernelSymbol __stop_ksymtab[];

// ---- Exported kernel symbols ----
// Include the headers that declare each symbol we want to export.

#include "heap.h"
#include "kprintf.h"
#include "vmm.h"
#include "pmm.h"
#include "device.h"
#include "vfs.h"
#include "fatfs_glue.h"
#include "serial.h"
#include "panic.h"
#include "pci.h"
#include "virtio_blk.h"
#include "keyboard.h"
#include "input.h"

// Bring all brook:: names into scope so EXPORT_SYMBOL(fn) resolves correctly.
using namespace brook;

// Memory
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kfree);

// Formatted output
EXPORT_SYMBOL(KPrintf);
EXPORT_SYMBOL(KPuts);
EXPORT_SYMBOL(SerialPrintf);
EXPORT_SYMBOL(SerialPuts);

// Physical memory
EXPORT_SYMBOL(PmmAllocPage);
EXPORT_SYMBOL(PmmAllocPages);
EXPORT_SYMBOL(PmmFreePage);

// Virtual memory
EXPORT_SYMBOL(VmmAllocPages);
EXPORT_SYMBOL(VmmFreePages);
EXPORT_SYMBOL(VmmVirtToPhys);

// Device registry
EXPORT_SYMBOL(DeviceRegister);
EXPORT_SYMBOL(DeviceFind);
EXPORT_SYMBOL(DeviceIterate);

// VFS
EXPORT_SYMBOL(VfsMount);
EXPORT_SYMBOL(VfsUnmount);
EXPORT_SYMBOL(VfsOpen);
EXPORT_SYMBOL(VfsRead);
EXPORT_SYMBOL(VfsWrite);
EXPORT_SYMBOL(VfsClose);
EXPORT_SYMBOL(VfsReaddir);

// FatFS glue
EXPORT_SYMBOL(FatFsBindDrive);

// PCI
EXPORT_SYMBOL(PciFindDevice);
EXPORT_SYMBOL(PciFindNextDevice);
EXPORT_SYMBOL(PciEnumerate);
EXPORT_SYMBOL(PciEnableBusMaster);

// virtio-blk
EXPORT_SYMBOL(VirtioBlkInitAll);

// Keyboard
EXPORT_SYMBOL(KbdInit);
EXPORT_SYMBOL(KbdGetChar);
EXPORT_SYMBOL(KbdPeekChar);
EXPORT_SYMBOL(KbdIsAvailable);

// Input subsystem
EXPORT_SYMBOL(InputInit);
EXPORT_SYMBOL(InputRegister);
EXPORT_SYMBOL(InputPollEvent);
EXPORT_SYMBOL(InputWaitEvent);
EXPORT_SYMBOL(InputHasEvents);

// Panic
EXPORT_SYMBOL(KernelPanic);

namespace brook {

// ---- KsymLookup ----

static bool StrEq(const char* a, const char* b)
{
    while (*a && *b) { if (*a++ != *b++) return false; }
    return *a == *b;
}

const void* KsymLookup(const char* name)
{
    if (!name) return nullptr;
    for (KernelSymbol* sym = __start_ksymtab; sym < __stop_ksymtab; ++sym)
    {
        if (StrEq(sym->name, name)) return sym->addr;
    }
    return nullptr;
}

void KsymDump()
{
    uint32_t count = static_cast<uint32_t>(__stop_ksymtab - __start_ksymtab);
    SerialPrintf("ksymtab: %u exported symbols\n", count);
    for (KernelSymbol* sym = __start_ksymtab; sym < __stop_ksymtab; ++sym)
        SerialPrintf("  %-32s  0x%016lx\n", sym->name,
                     reinterpret_cast<uint64_t>(sym->addr));
}

} // namespace brook
