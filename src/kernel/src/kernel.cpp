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
#include "device.h"
#include "pci.h"
#include "ramdisk.h"
#include "fatfs_glue.h"
#include "vfs.h"
#include "virtio_blk.h"
#include "fat_test_image.h"

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

    // ---- Device / VFS layer ----
    brook::VfsInit();

    // Mount the embedded FAT16 test image as a ramdisk.
    // The image is read-only from the kernel's perspective (const data section).
    // We cast away const for the ramdisk: writes go to a separate copy if needed;
    // for now read-only access is sufficient.
    brook::Device* rd = brook::RamdiskCreate(
        const_cast<void*>(static_cast<const void*>(g_fatTestImage)),
        g_fatTestImageSize, 512, "ramdisk0");
    if (rd && brook::DeviceRegister(rd))
    {
        brook::FatFsBindDrive(0, rd);
        if (brook::VfsMount("/", "fatfs", 0))
        {
            // Smoke-test: open and read BROOK.CFG
            brook::Vnode* cfg = brook::VfsOpen("/BROOK.CFG");
            if (cfg)
            {
                char buf[128] = {};
                uint64_t off = 0;
                int n = brook::VfsRead(cfg, buf, sizeof(buf) - 1, &off);
                brook::VfsClose(cfg);
                if (n > 0)
                    brook::KPrintf("VFS: /BROOK.CFG (%d bytes): %s", n, buf);
                else
                    brook::KPuts("VFS: /BROOK.CFG open OK but read returned 0\n");
            }
            else
            {
                brook::KPuts("VFS: could not open /BROOK.CFG\n");
            }
        }
    }
    else
    {
        brook::KPuts("VFS: ramdisk init failed\n");
    }

    // ---- virtio-blk: enumerate all PCI virtio drives ----
    // Each virtio drive is probed for a BROOK.MNT file at its root.
    // BROOK.MNT contains a single line: the VFS mount path for that volume.
    // If found, the drive is mounted at that path automatically.
    // Drives without BROOK.MNT are registered but not auto-mounted.
    brook::PciScanPrint();
    uint32_t vioCount = brook::VirtioBlkInitAll();
    if (vioCount > 0)
    {
        brook::KPrintf("virtio: found %u device(s)\n", vioCount);

        // Probe each virtio drive: bind to FatFS, look for BROOK.MNT.
        // FatFS physical drives: 0 = ramdisk, 1..N = virtio0..N-1
        for (uint32_t i = 0; i < vioCount; ++i)
        {
            char name[16] = "virtio";
            // Simple uint-to-string for drive index.
            name[6] = static_cast<char>('0' + i);
            name[7] = '\0';
            brook::Device* vd = brook::DeviceFind(name);
            if (!vd) continue;

            uint8_t pdrv = static_cast<uint8_t>(i + 1); // pdrv 1+ for virtio
            brook::FatFsBindDrive(pdrv, vd);

            // Try mounting — FatFS will return FR_OK only if it sees a valid FAT volume.
            if (!brook::VfsMount("/mnt/probe", "fatfs", pdrv))
            {
                brook::SerialPrintf("virtio: %s — not a FAT volume or mount failed\n", name);
                continue;
            }

            // Read BROOK.MNT to discover the intended mount path.
            brook::Vnode* mnt = brook::VfsOpen("/mnt/probe/BROOK.MNT");
            if (!mnt)
            {
                brook::SerialPrintf("virtio: %s mounted at /mnt/probe (no BROOK.MNT)\n", name);
                continue;
            }

            char mntPath[64] = {};
            uint64_t off = 0;
            int n = brook::VfsRead(mnt, mntPath, sizeof(mntPath) - 1, &off);
            brook::VfsClose(mnt);

            if (n <= 0)
            {
                brook::SerialPrintf("virtio: %s — BROOK.MNT empty\n", name);
                continue;
            }

            // Strip trailing whitespace/newline from the path.
            for (int j = n - 1; j >= 0; --j)
            {
                if (mntPath[j] == '\n' || mntPath[j] == '\r' || mntPath[j] == ' ')
                    mntPath[j] = '\0';
                else
                    break;
            }

            // Remount at the target path.
            brook::VfsUnmount("/mnt/probe");
            if (brook::VfsMount(mntPath, "fatfs", pdrv))
            {
                brook::KPrintf("virtio: %s mounted at %s\n", name, mntPath);

                // Read BROOK.CFG from the mounted volume as a sanity check.
                char cfgPath[80] = {};
                // Build "<mntPath>/BROOK.CFG"
                uint32_t plen = 0;
                while (mntPath[plen]) cfgPath[plen] = mntPath[plen++];
                const char* suffix = "/BROOK.CFG";
                for (uint32_t j = 0; suffix[j]; ++j) cfgPath[plen++] = suffix[j];

                brook::Vnode* cfg = brook::VfsOpen(cfgPath);
                if (cfg)
                {
                    char buf[256] = {};
                    uint64_t coff = 0;
                    int nr = brook::VfsRead(cfg, buf, sizeof(buf) - 1, &coff);
                    brook::VfsClose(cfg);
                    if (nr > 0)
                        brook::KPrintf("virtio: %s (%d bytes): %s", cfgPath, nr, buf);
                }
            }
            else
            {
                brook::KPrintf("virtio: %s — remount at %s failed\n", name, mntPath);
            }
        }
    }

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
