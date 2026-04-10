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
#include "ksymtab.h"
#include "module.h"
#include "tty.h"
#include "device.h"
#include "pci.h"
#include "ramdisk.h"
#include "fatfs_glue.h"
#include "vfs.h"
#include "virtio_blk.h"
#include "syscall.h"
#include "process.h"
#include "fat_test_image.h"

// All kernel initialization and runtime — called by KernelMain after stack switch.
__attribute__((noreturn)) static void KernelMainBody(brook::BootProtocol* bootProtocol);

// Global kernel CPU environment (needed to set syscall table after init).
static KernelCpuEnv* g_kernelEnv = nullptr;

// Kernel entry point. Called by the bootloader via SysV ABI (argument in RDI).
// Immediately switches to a dedicated kernel stack, then tail-calls KernelMainBody.
extern "C" __attribute__((sysv_abi, noreturn)) void KernelMain(brook::BootProtocol* bootProtocol)
{
    // g_kernelStackTop: allocated in BSS (always mapped from kernel start).
    // Switch RSP before the compiler can spill bootProtocol anywhere on the old stack.
    // bootProtocol lives in RDI throughout; "noreturn" prevents the compiler generating
    // any callee-save/restore code that would reference the old stack after this.
    extern void* g_kernelStackTop;
    void* newSp = g_kernelStackTop;
    __asm__ volatile("movq %0, %%rsp\n\t" :: "r"(newSp) : "memory");
    KernelMainBody(bootProtocol);
    __builtin_unreachable();
}

__attribute__((noreturn)) static void KernelMainBody(brook::BootProtocol* bootProtocol)
{
    extern void* g_kernelStackTop;
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
    CpuInitSyscallMsrs(brook::SyscallGetEntryPoint());
    brook::KPuts("GDT+FPU+SYSCALL loaded\n");

    IdtInit(&bootProtocol->framebuffer);
    brook::KPuts("IDT loaded\n");

    brook::PmmInit(bootProtocol);
    brook::KPrintf("PMM ready: %u free pages (%u MB)\n",
                   static_cast<uint32_t>(brook::PmmGetFreePageCount()),
                   static_cast<uint32_t>((brook::PmmGetFreePageCount() * 4096) / (1024*1024)));

    brook::VmmInit();
    brook::HeapInit();
    brook::PmmEnableTracking();

    // Allocate a proper kernel stack with a guard page.  The initial BSS stack
    // is contiguous with other kernel data, so we can't safely unmap below it.
    // VmmAllocPages gives us fresh pages; we skip mapping the first page (guard).
    {
        constexpr uint64_t STACK_PAGES = 8;   // 32 KB
        constexpr uint64_t GUARD_PAGES = 1;   // 4 KB unmapped guard
        constexpr uint64_t TOTAL_PAGES = STACK_PAGES + GUARD_PAGES;

        // Reserve virtual address range, then map only the stack portion.
        uint64_t base = brook::VmmAllocPages(TOTAL_PAGES, brook::VMM_WRITABLE,
                                             brook::MemTag::KernelData, brook::KernelPid);
        if (base)
        {
            // Unmap the first page to create the guard.
            brook::VmmUnmapPage(base);

            // New stack top is end of allocation, 16-byte aligned.
            void* newTop = reinterpret_cast<void*>(base + TOTAL_PAGES * 0x1000 - 16);
            g_kernelStackTop = newTop;

            // Switch to the new stack.  We're deep enough in init that nothing
            // important lives on the old BSS stack anymore.
            __asm__ volatile("movq %0, %%rsp" :: "r"(newTop) : "memory");
        }
    }

    // Set up per-CPU kernel environment for SWAPGS.
    // Allocate a 16KB syscall stack (with guard page) and a KernelCpuEnv.
    {
        auto* env = static_cast<KernelCpuEnv*>(brook::kmalloc(sizeof(KernelCpuEnv)));
        if (env)
        {
            constexpr uint64_t SYSCALL_STACK_PAGES = 4;  // 16 KB
            constexpr uint64_t GUARD_PAGES = 1;
            uint64_t scBase = brook::VmmAllocPages(
                SYSCALL_STACK_PAGES + GUARD_PAGES,
                brook::VMM_WRITABLE, brook::MemTag::KernelData, brook::KernelPid);
            if (scBase)
            {
                brook::VmmUnmapPage(scBase); // guard page
                uint64_t scTop = scBase + (SYSCALL_STACK_PAGES + GUARD_PAGES) * 0x1000 - 16;
                env->syscallStack = scTop;
            }
            else
            {
                env->syscallStack = 0;
            }
            env->syscallTable = 0;  // set later when syscall table is ready
            env->kernelRbp    = 0;
            env->kernelRsp    = 0;
            env->currentPid   = 0;  // kernel

            CpuSetKernelGsBase(env);
            g_kernelEnv = env;

            // Set TSS.RSP0 so ring 3 → ring 0 exception transitions
            // use the syscall stack (SYSCALL itself uses LSTAR, not TSS).
            if (env->syscallStack)
                GdtSetTssRsp0(env->syscallStack);

            brook::KPrintf("CPU: kernel GS env at %p, syscall stack top 0x%016lx\n",
                           reinterpret_cast<void*>(env), env->syscallStack);
        }
    }

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
                while (mntPath[plen]) { cfgPath[plen] = mntPath[plen]; ++plen; }
                const char* suffix = "/BROOK.CFG";
                for (uint32_t j = 0; suffix[j]; ++j) cfgPath[plen++] = suffix[j];
                cfgPath[plen] = '\0';

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

    // ---- Module loader ----
    // Phase 1: load early modules from embedded ramdisk (mounted at "/").
    //   These run before virtio, so they can't access /boot yet.
    //   Keyboard is NOT here — it needs APIC which is already set up, but
    //   we prefer loading it from the virtio disk for demonstration.
    brook::KsymDump();
    brook::SerialPuts("module: Phase 1 — loading from /drivers (ramdisk)\n");
    brook::ModuleDiscoverAndLoad("/drivers");

    // Phase 2: load late modules from /boot/drivers (virtio disk).
    //   ps2_kbd.mod and virtio_blk.mod live here.
    brook::SerialPuts("module: Phase 2 — loading from /boot/drivers (virtio)\n");
    brook::ModuleDiscoverAndLoad("/boot/drivers");
    brook::SerialPuts("module: Phase 2 — done\n");

    // ---- Syscall table ----
    brook::SyscallTableInit();
    if (g_kernelEnv)
    {
        g_kernelEnv->syscallTable = reinterpret_cast<uint64_t>(brook::SyscallGetTable());
    }

    // ---- User-mode ELF test ----
    // Try to load and run a user-mode ELF binary from the virtio disk.
    {
        brook::Vnode* vn = brook::VfsOpen("/boot/bin/HELLO_MUSL", 0);
        if (!vn)
            vn = brook::VfsOpen("/boot/bin/hello_musl", 0);

        if (vn)
        {
            // Read the entire file into a kernel buffer
            // First, read in chunks (we don't have fstat yet)
            constexpr uint64_t MAX_ELF_SIZE = 128 * 1024; // 128 KB max
            auto* elfBuf = static_cast<uint8_t*>(brook::kmalloc(MAX_ELF_SIZE));
            if (elfBuf)
            {
                uint64_t totalRead = 0;
                uint64_t offset = 0;
                while (totalRead < MAX_ELF_SIZE)
                {
                    int ret = brook::VfsRead(vn, elfBuf + totalRead,
                                              4096, &offset);
                    if (ret <= 0) break;
                    totalRead += static_cast<uint64_t>(ret);
                }
                brook::VfsClose(vn);

                brook::SerialPrintf("USER: loaded ELF binary (%lu bytes)\n", totalRead);

                const char* argv[] = { "hello_musl", nullptr };
                const char* envp[] = { "HOME=/", nullptr };

                auto* proc = brook::ProcessCreate(elfBuf, totalRead,
                                                   1, argv,
                                                   1, envp);

                brook::kfree(elfBuf);

                if (proc)
                {
                    brook::SerialPuts("USER: entering ring 3...\n");

                    // Set FS base if TLS was set up
                    if (proc->fsBase)
                    {
                        uint32_t lo = static_cast<uint32_t>(proc->fsBase);
                        uint32_t hi = static_cast<uint32_t>(proc->fsBase >> 32);
                        __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi),
                                         "c"(0xC0000100U));
                    }

                    brook::SwitchToUserMode(proc->stackTop,
                                             proc->elf.entryPoint);

                    brook::SerialPuts("USER: returned from ring 3 successfully!\n");
                    brook::ProcessDestroy(proc);
                }
                else
                {
                    brook::SerialPuts("USER: ProcessCreate failed\n");
                }
            }
            else
            {
                brook::VfsClose(vn);
                brook::SerialPuts("USER: failed to allocate ELF buffer\n");
            }
        }
        else
        {
            brook::SerialPuts("USER: no test binary found at /boot/bin/HELLO_MUSL\n");
        }
    }

    // Keyboard: the ps2_kbd module calls KbdInit(). If the module wasn't
    // loaded (e.g. no /boot), fall back to initialising directly.
    if (acpiOk && !brook::KbdIsAvailable())
    {
        brook::KPuts("KBD: ps2_kbd module not loaded — falling back to direct init\n");
        brook::KbdInit();
    }

    // Enable interrupts.
    __asm__ volatile("sti");

    // Simple echo loop — type to see output on TTY and serial.
    if (brook::KbdIsAvailable())
    {
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
    else
    {
        brook::KPuts("\nNo keyboard available — halting.\n");
        for (;;) { __asm__ volatile("hlt"); }
    }
}
