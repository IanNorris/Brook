#include "boot_protocol/boot_protocol.h"
#include "serial.h"
#include "kprintf.h"
#include "panic.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "acpi.h"
#include "apic.h"
#include "keyboard.h"
#include "tty.h"

// Kernel entry point. Called by the bootloader via SysV ABI (argument in RDI).
// At this stage: UEFI page tables still active, running at physical address.
extern "C" __attribute__((sysv_abi)) void KernelMain(brook::BootProtocol* bootProtocol)
{
    brook::SerialInit();
    brook::KPrintfInit();
    brook::KPuts("Brook kernel starting...\n");

    // Validate boot protocol
    if (bootProtocol == nullptr ||
        bootProtocol->magic != brook::BootProtocolMagic)
    {
        KernelPanic("Invalid boot protocol (ptr=%p magic=%lx expected=%lx)\n",
                    reinterpret_cast<void*>(bootProtocol),
                    bootProtocol ? static_cast<unsigned long>(bootProtocol->magic) : 0UL,
                    static_cast<unsigned long>(brook::BootProtocolMagic));
    }

    GdtInit();
    CpuInitFpu();
    brook::KPuts("GDT+FPU loaded\n");

    IdtInit(&bootProtocol->framebuffer);
    brook::KPuts("IDT loaded\n");

    brook::PmmInit(bootProtocol);
    brook::KPrintf("PMM ready: %u free pages (%u MB)\n",
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
        brook::IoApicInit(madt.ioApicPhysical, madt.ioApicGsiBase);
    }
    else
    {
        brook::KPuts("WARNING: ACPI init failed — running without LAPIC\n");
    }

    // TTY: map framebuffer into virtual space and initialise the text display.
    const brook::Framebuffer& fb = bootProtocol->framebuffer;
    bool ttyOk = brook::TtyInit(fb);

    if (!ttyOk)
    {
        brook::KPuts("TTY init failed — display output unavailable\n");
    }

    // From here on KPrintf fans to both serial and TTY automatically.
    brook::KPuts("Brook OS\n");
    brook::KPuts("--------\n");
    brook::KPrintf("Framebuffer  %ux%u\n", fb.width, fb.height);
    brook::KPrintf("PMM          %u MB free / %u MB total\n",
                   static_cast<uint32_t>((brook::PmmGetFreePageCount() * 4096) / (1024*1024)),
                   static_cast<uint32_t>((brook::PmmGetTotalPageCount() * 4096) / (1024*1024)));

    if (acpiOk)
    {
        const brook::MadtInfo& madt = brook::AcpiGetMadt();
        brook::KPrintf("LAPIC        @ 0x%p  (%u processor(s))\n",
                       reinterpret_cast<void*>(madt.localApicPhysical),
                       madt.processorCount);
        brook::KPrintf("Timer        %u ticks/ms\n",
                       brook::ApicGetTimerTicksPerMs());
    }

    brook::KPuts("\nKernel running.\n");

    // Keyboard init (after I/O APIC is set up).
    if (acpiOk) brook::KbdInit();

    // Enable interrupts.
    __asm__ volatile("sti");

    // Simple echo loop — type to see output on TTY and serial.
    brook::KPuts("\nType something (keyboard echo):\n> ");
    for (;;)
    {
        char c = brook::KbdGetChar();
        if (c == '\b')
        {
            // Rudimentary backspace: overwrite with space.
            brook::KPuts("\b \b");
        }
        else if (c == '\n')
        {
            brook::KPuts("\n> ");
        }
        else
        {
            char buf[2] = { c, '\0' };
            brook::KPuts(buf);
        }
    }
}
