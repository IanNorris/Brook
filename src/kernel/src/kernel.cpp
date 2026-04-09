#include "boot_protocol/boot_protocol.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "acpi.h"
#include "apic.h"
#include "tty.h"

// Kernel entry point. Called by the bootloader via SysV ABI (argument in RDI).
// At this stage: UEFI page tables still active, running at physical address.
extern "C" __attribute__((sysv_abi)) void KernelMain(brook::BootProtocol* bootProtocol)
{
    brook::SerialInit();
    brook::SerialPrintf("Brook kernel starting...\n");

    // Validate boot protocol
    if (bootProtocol == nullptr ||
        bootProtocol->magic != brook::BootProtocolMagic)
    {
        for (;;) { __asm__ volatile("hlt"); }
    }

    GdtInit();
    brook::SerialPuts("GDT loaded\n");

    IdtInit(&bootProtocol->framebuffer);
    brook::SerialPuts("IDT loaded\n");

    brook::PmmInit(bootProtocol);
    brook::SerialPrintf("PMM ready: %u free pages (%u MB)\n",
                        static_cast<uint32_t>(brook::PmmGetFreePageCount()),
                        static_cast<uint32_t>((brook::PmmGetFreePageCount() * 4096) / (1024*1024)));

    brook::VmmInit();
    brook::HeapInit();
    brook::PmmEnableTracking();

    // ACPI: parse tables to get LAPIC/IOAPIC addresses.
    bool acpiOk = brook::AcpiInit(bootProtocol->acpi.rsdpPhysical);
    if (acpiOk)
    {
        // APIC: disable legacy PIC, enable LAPIC, calibrate + start timer.
        const brook::MadtInfo& madt = brook::AcpiGetMadt();
        brook::ApicInit(madt.localApicPhysical);
    }
    else
    {
        brook::SerialPuts("WARNING: ACPI init failed — running without LAPIC\n");
    }

    // TTY: map framebuffer into virtual space and initialise the text display.
    const brook::Framebuffer& fb = bootProtocol->framebuffer;
    bool ttyOk = brook::TtyInit(fb);

    if (ttyOk)
    {
        // Banner
        brook::TtyPuts("Brook OS\n");
        brook::TtyPuts("--------\n");
        brook::TtyPrintf("Framebuffer  %ux%u\n", fb.width, fb.height);
        brook::TtyPrintf("PMM          %u MB free / %u MB total\n",
                         static_cast<uint32_t>((brook::PmmGetFreePageCount() * 4096) / (1024*1024)),
                         static_cast<uint32_t>((brook::PmmGetTotalPageCount() * 4096) / (1024*1024)));

        if (acpiOk)
        {
            const brook::MadtInfo& madt = brook::AcpiGetMadt();
            brook::TtyPrintf("LAPIC        @ 0x%p  (%u processor(s))\n",
                             reinterpret_cast<void*>(madt.localApicPhysical),
                             madt.processorCount);
            brook::TtyPrintf("Timer        %u ticks/ms\n",
                             brook::ApicGetTimerTicksPerMs());
        }

        brook::TtyPuts("\nKernel running.\n");
    }
    else
    {
        brook::SerialPuts("TTY init failed — display output unavailable\n");
    }

    brook::SerialPuts("Kernel running — waiting for interrupts\n");

    // Enable interrupts and halt
    __asm__ volatile("sti");
    for (;;) { __asm__ volatile("hlt"); }
}
