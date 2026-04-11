#include "boot_protocol/boot_protocol.h"
#include "serial.h"
#include "kprintf.h"
#include "panic.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "memory/heap.h"
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
#include "scheduler.h"
#include "compositor.h"
#include "smp.h"
#include "fat_test_image.h"

// All kernel initialization and runtime — called by KernelMain after stack switch.
__attribute__((noreturn)) static void KernelMainBody(brook::BootProtocol* bootProtocol);

// Global kernel CPU environment (needed to set syscall table after init).
// Non-static so the scheduler can update syscallStack on context switch.
KernelCpuEnv* g_kernelEnv = nullptr;

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
        brook::VirtualAddress base = brook::VmmAllocPages(TOTAL_PAGES, brook::VMM_WRITABLE,
                                             brook::MemTag::KernelData, brook::KernelPid);
        if (base)
        {
            brook::VmmUnmapPage(brook::KernelPageTable, base);

            void* newTop = reinterpret_cast<void*>(base.raw() + TOTAL_PAGES * 0x1000 - 16);
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
            constexpr uint64_t SYSCALL_STACK_PAGES = 16;  // 64 KB
            constexpr uint64_t GUARD_PAGES = 1;
            brook::VirtualAddress scBase = brook::VmmAllocPages(
                SYSCALL_STACK_PAGES + GUARD_PAGES,
                brook::VMM_WRITABLE, brook::MemTag::KernelData, brook::KernelPid);
            if (scBase)
            {
                brook::VmmUnmapPage(brook::KernelPageTable, scBase); // guard page
                uint64_t scTop = scBase.raw() + (SYSCALL_STACK_PAGES + GUARD_PAGES) * 0x1000 - 16;
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

    // ---- SMP: boot Application Processors ----
    if (acpiOk)
    {
        uint32_t cpuCount = brook::SmpInit();
        brook::KPrintf("SMP          %u CPU(s) online\n", cpuCount);
    }

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

    // Register BSP's KernelCpuEnv with the scheduler.
    brook::SchedulerSetCpuEnv(0, g_kernelEnv);

    // ---- User-mode ELF binaries ----
    // Create all processes and hand them to the scheduler.
    {
        const char* envp[] = { "HOME=/", nullptr };
        const char* argv_doom[] = { "doom", "-iwad", "/boot/DOOM1.WAD", nullptr };

        // Initialise the scheduler (creates idle process) and compositor.
        brook::SchedulerInit();
        brook::CompositorInit();

        uint64_t fbPhys;
        uint32_t fbW, fbH, fbStride;
        brook::TtyGetFramebufferPhys(&fbPhys, &fbW, &fbH, &fbStride);

        // ---- Grid layout for 32 DOOM instances ----
        // DOOM renders at 640×400 (DOOMGENERIC_RESX/Y). Each VFB is 640×400.
        // Compositor downscales by scale factor when blitting to physical FB.
        // With scale=3: each cell ≈ 213×133 pixels on screen.
        // 8 columns × 4 rows = 32 instances.
        constexpr uint32_t DOOM_VFB_W = 640;
        constexpr uint32_t DOOM_VFB_H = 400;
        constexpr uint8_t  DOOM_SCALE = 3;
        constexpr int GRID_COLS = 8;
        constexpr int GRID_ROWS = 4;
        constexpr int NUM_DOOMS = GRID_COLS * GRID_ROWS;

        uint32_t cellW = DOOM_VFB_W / DOOM_SCALE; // 213
        uint32_t cellH = DOOM_VFB_H / DOOM_SCALE; // 133

        // Center the grid on the physical framebuffer.
        int16_t gridX0 = static_cast<int16_t>((fbW - cellW * GRID_COLS) / 2);
        int16_t gridY0 = static_cast<int16_t>((fbH - cellH * GRID_ROWS) / 2);

        // Load DOOM ELF into memory once, create 4 processes from it.
        brook::Vnode* doomVn = brook::VfsOpen("/boot/DOOM", 0);
        if (!doomVn) doomVn = brook::VfsOpen("/boot/doom", 0);

        uint8_t* elfBuf = nullptr;
        uint64_t elfSize = 0;
        constexpr uint64_t MAX_ELF_SIZE = 2 * 1024 * 1024;
        constexpr uint64_t ELF_BUF_PAGES = MAX_ELF_SIZE / 4096;
        brook::VirtualAddress elfBufAddr{};

        if (doomVn)
        {
            elfBufAddr = brook::VmmAllocPages(ELF_BUF_PAGES,
                brook::VMM_WRITABLE, brook::MemTag::Heap, brook::KernelPid);
            if (elfBufAddr)
            {
                elfBuf = reinterpret_cast<uint8_t*>(elfBufAddr.raw());
                uint64_t offset = 0;
                while (elfSize < MAX_ELF_SIZE)
                {
                    int ret = brook::VfsRead(doomVn, elfBuf + elfSize, 4096, &offset);
                    if (ret <= 0) break;
                    elfSize += static_cast<uint64_t>(ret);
                }
            }
            brook::VfsClose(doomVn);
            brook::SerialPrintf("USER: loaded DOOM (%lu bytes)\n", elfSize);
        }

        // Disable interrupts during bulk process creation to avoid timer ISR
        // interference with page table/heap operations.
        __asm__ volatile("cli");

        // Create 32 DOOM processes in an 8×4 grid.
        for (int d = 0; d < NUM_DOOMS && elfBuf; ++d)
        {
            auto* proc = brook::ProcessCreate(elfBuf, elfSize,
                                               3, argv_doom,
                                               1, envp);
            if (!proc)
            {
                brook::SerialPrintf("USER: ProcessCreate failed for doom #%d\n", d);
                continue;
            }

            // Set process name: doom00..doom31.
            proc->name[0] = 'd'; proc->name[1] = 'o';
            proc->name[2] = 'o'; proc->name[3] = 'm';
            proc->name[4] = static_cast<char>('0' + (d / 10));
            proc->name[5] = static_cast<char>('0' + (d % 10));
            proc->name[6] = '\0';

            int col = d % GRID_COLS;
            int row = d / GRID_COLS;
            int16_t destX = gridX0 + static_cast<int16_t>(col * cellW);
            int16_t destY = gridY0 + static_cast<int16_t>(row * cellH);

            brook::CompositorSetupProcess(proc, destX, destY,
                                           DOOM_VFB_W, DOOM_VFB_H, DOOM_SCALE);

            brook::SchedulerAddProcess(proc);
            brook::SerialPrintf("USER: doom #%d → grid(%d,%d) dest=(%d,%d)\n",
                                 d, col, row, destX, destY);
        }

        // Re-enable interrupts.
        __asm__ volatile("sti");

        if (elfBufAddr)
            brook::VmmFreePages(elfBufAddr, ELF_BUF_PAGES);

        brook::SerialPrintf("SCHED: all processes created, starting scheduler...\n");

        // Initialise keyboard before entering user mode (DOOM needs it).
        if (acpiOk && !brook::KbdIsAvailable())
        {
            brook::KPuts("KBD: ps2_kbd module not loaded — falling back to direct init\n");
            brook::KbdInit();
        }

        // Activate APs — they will set up per-CPU state and enter the scheduler.
        brook::SmpActivateAPs();

        brook::SchedulerStart();
        // SchedulerStart never returns.
    }

    // Unreachable — SchedulerStart is [[noreturn]].
    for (;;) { __asm__ volatile("hlt"); }
}
