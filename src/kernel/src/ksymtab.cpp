#include "ksymtab.h"
#include "serial.h"

// The linker script places all .ksymtab entries between these two symbols.
// Declared as arrays so &__start_ksymtab gives the address of the first entry.
extern "C" brook::KernelSymbol __start_ksymtab[];
extern "C" brook::KernelSymbol __stop_ksymtab[];

// ---- Exported kernel symbols ----
// Include the headers that declare each symbol we want to export.

#include "memory/heap.h"
#include "kprintf.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "device.h"
#include "vfs.h"
#include "fatfs_glue.h"
#include "ext2_vfs.h"
#include "serial.h"
#include "panic.h"
#include "pci.h"
#include "virtio_blk.h"
#include "keyboard.h"
#include "input.h"
#include "scheduler.h"
#include "display.h"
#include "tty.h"
#include "compositor.h"
#include "mouse.h"
#include "idt.h"
#include "apic.h"
#include "net.h"
#include "audio.h"

// Bring all brook:: names into scope so EXPORT_SYMBOL(fn) resolves correctly.
using namespace brook;

// Non-inline wrapper for PhysToVirt (inlined in header, but modules need a callable symbol).
extern "C" uint64_t KsymPhysToVirt(uint64_t physAddr)
{
    return PhysToVirt(PhysicalAddress(physAddr)).raw();
}

// Memory
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kfree);
EXPORT_SYMBOL(krealloc);

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
EXPORT_SYMBOL(KsymPhysToVirt);
// Also export with mangled names for C++ modules that include the header.
EXPORT_SYMBOL_NAMED(VmmAllocPages, "_ZN5brook13VmmAllocPagesEmmNS_6MemTagEt");
EXPORT_SYMBOL_NAMED(VmmVirtToPhys, "_ZN5brook13VmmVirtToPhysENS_9PageTableENS_14VirtualAddressE");
EXPORT_SYMBOL_NAMED(VmmFreePages,  "_ZN5brook12VmmFreePagesENS_14VirtualAddressEm");
EXPORT_SYMBOL_NAMED(VmmMapPage,    "_ZN5brook10VmmMapPageENS_9PageTableENS_14VirtualAddressENS_15PhysicalAddressEmNS_6MemTagEt");

// Device registry
EXPORT_SYMBOL(DeviceRegister);
EXPORT_SYMBOL(DeviceUnregister);
EXPORT_SYMBOL(DeviceFind);
EXPORT_SYMBOL(DeviceIterate);

// VFS
EXPORT_SYMBOL(VfsMount);
EXPORT_SYMBOL(VfsUnmount);
EXPORT_SYMBOL(VfsRegisterFs);
EXPORT_SYMBOL(VfsUnregisterFs);
EXPORT_SYMBOL(VfsOpen);
EXPORT_SYMBOL(VfsRead);
EXPORT_SYMBOL(VfsWrite);
EXPORT_SYMBOL(VfsClose);
EXPORT_SYMBOL(VfsReaddir);

// FatFS glue
EXPORT_SYMBOL(FatFsBindDrive);

// Ext2
EXPORT_SYMBOL(Ext2BindDevice);

// PCI
EXPORT_SYMBOL(PciFindDevice);
EXPORT_SYMBOL(PciFindNextDevice);
EXPORT_SYMBOL(PciEnumerate);
EXPORT_SYMBOL(PciEnableBusMaster);
EXPORT_SYMBOL(PciEnableMemSpace);
EXPORT_SYMBOL(PciConfigRead32);
EXPORT_SYMBOL(PciConfigRead16);
EXPORT_SYMBOL(PciConfigRead8);
EXPORT_SYMBOL(PciConfigWrite16);

// Display
EXPORT_SYMBOL(DisplayRegister);

// TTY
EXPORT_SYMBOL(TtyGetFramebuffer);
EXPORT_SYMBOL(TtyGetFramebufferPhys);
EXPORT_SYMBOL(TtyRemap);

// Compositor
EXPORT_SYMBOL(CompositorRemap);
EXPORT_SYMBOL(CompositorGetPhysDims);
EXPORT_SYMBOL(CompositorWake);

// Mouse
EXPORT_SYMBOL(MouseInit);
EXPORT_SYMBOL(MouseIsAvailable);
EXPORT_SYMBOL(MouseGetPosition);
EXPORT_SYMBOL(MouseSetBounds);
EXPORT_SYMBOL(MouseGetButtons);
EXPORT_SYMBOL(MouseSetPosition);
EXPORT_SYMBOL(MouseSetButtons);
EXPORT_SYMBOL(MouseSetAvailable);

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
EXPORT_SYMBOL(InputWakeWaiters);

// Scheduler — for dynamic policy modules
EXPORT_SYMBOL(SchedulerRegisterPolicy);
EXPORT_SYMBOL(SchedulerBlock);
EXPORT_SYMBOL(SchedulerUnblock);
EXPORT_SYMBOL(SchedulerYield);
EXPORT_SYMBOL(SchedulerSleepMs);

// Process
EXPORT_SYMBOL(ProcessCurrent);
EXPORT_SYMBOL(ProcessFindByPid);
EXPORT_SYMBOL(ProcessSendSignal);

// Panic
EXPORT_SYMBOL(KernelPanic);

// IDT / APIC — for driver IRQ setup
EXPORT_SYMBOL(IdtInstallHandler);
EXPORT_SYMBOL(IoApicUnmaskIrq);
EXPORT_SYMBOL(IoApicRegisterHandler);
EXPORT_SYMBOL(ApicSendEoi);

// Network
EXPORT_SYMBOL(NetRegisterIf);
EXPORT_SYMBOL(NetReceive);
EXPORT_SYMBOL_NAMED(NetRegisterIf, "_ZN5brook13NetRegisterIfEPNS_5NetIfE");
EXPORT_SYMBOL_NAMED(NetReceive,    "_ZN5brook10NetReceiveEPNS_5NetIfEPKvj");

// Audio
EXPORT_SYMBOL(AudioRegister);

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
