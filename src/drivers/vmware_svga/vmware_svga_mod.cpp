// VMware SVGA II, legacy linear framebuffer and two-dimensional update FIFO.
// Register layout follows vmwgfx device_include/svga_reg.h. QEMU emulates the
// same indexed I/O and BAR layout in hw/display/vmware_vga.c.
#include "compositor.h"
#include "display.h"
#include "kprintf.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "module_abi.h"
#include "pci.h"
#include "serial.h"
#include "tty.h"
#include "svga_fifo.h"

MODULE_IMPORT_SYMBOL(PciFindDevice);
MODULE_IMPORT_SYMBOL(PciConfigRead16);
MODULE_IMPORT_SYMBOL(PciConfigRead32);
MODULE_IMPORT_SYMBOL(PciConfigWrite16);
MODULE_IMPORT_SYMBOL(PciConfigWrite32);
MODULE_IMPORT_SYMBOL(VmmAllocPages);
MODULE_IMPORT_SYMBOL(VmmDetachPage);
MODULE_IMPORT_SYMBOL(VmmFreePages);
MODULE_IMPORT_SYMBOL(VmmMapPage);
MODULE_IMPORT_SYMBOL(VmmVirtToPhys);
MODULE_IMPORT_SYMBOL(PmmFreePage);
MODULE_IMPORT_SYMBOL(KernelPageTable);
MODULE_IMPORT_SYMBOL(DisplayRegister);
MODULE_IMPORT_SYMBOL(CompositorRemapLinearDisplay);
MODULE_IMPORT_SYMBOL(TtyGetFramebufferPhys);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(SerialPrintf);

namespace {
using namespace brook;

constexpr uint16_t Vendor = 0x15ad, Device = 0x0405;
constexpr uint32_t Version2 = 0x90000002;
constexpr uint32_t FifoMappingLimit = 64 * 1024;
constexpr uint32_t PageBytes = 4096;
constexpr uint32_t MaxScreenDimension = 8192;

enum Register : uint32_t {
    Id = 0, Enable = 1, Width = 2, Height = 3, MaxWidth = 4, MaxHeight = 5,
    BitsPerPixel = 7, RedMask = 9, GreenMask = 10, BlueMask = 11, Stride = 12,
    FbStart = 13, FbOffset = 14, VramSize = 15, FbSize = 16, Capabilities = 17,
    FifoStart = 18, FifoSize = 19, ConfigDone = 20, Sync = 21,
};

struct Aperture {
    uint32_t base;
    uint32_t bytes;
};

struct Driver {
    PciDevice pci{};
    Aperture framebuffer{}, commands{};
    uint16_t port = 0, initialCommand = 0;
    uint32_t maxWidth = 0, maxHeight = 0, reportedVram = 0;
    svga::Geometry mode{};
    volatile uint32_t* fifo = nullptr;
    uint32_t fifoBytes = 0, mappedPages = 0;
    bool lock = false;
    bool changingMode = false, fullRefresh = false;
    uint32_t firstPending = 0, endPending = 0;
};

Driver g;

void Write(Register index, uint32_t value)
{
    const uint16_t valuePort = g.port + 1;
    asm volatile("outl %0, %1" :: "a"(uint32_t(index)), "d"(g.port) : "memory");
    asm volatile("outl %0, %1" :: "a"(value), "d"(valuePort) : "memory");
}

uint32_t Read(Register index)
{
    const uint16_t valuePort = g.port + 1;
    uint32_t value;
    asm volatile("outl %0, %1" :: "a"(uint32_t(index)), "d"(g.port) : "memory");
    asm volatile("inl %1, %0" : "=a"(value) : "d"(valuePort) : "memory");
    return value;
}

bool TryLock() { return !__atomic_exchange_n(&g.lock, true, __ATOMIC_ACQUIRE); }
void Unlock() { __atomic_store_n(&g.lock, false, __ATOMIC_RELEASE); }

bool SizedAperture(unsigned bar, Aperture& result)
{
    const uint32_t original = g.pci.bar[bar];
    if (!original || (original & 7u)) return false; // SVGA II memory BARs are 32-bit.
    const auto offset = uint8_t(0x10 + bar * 4);
    PciConfigWrite32(g.pci.bus, g.pci.dev, g.pci.fn, offset, 0xffffffffu);
    const uint32_t mask = PciConfigRead32(g.pci.bus, g.pci.dev, g.pci.fn, offset) & ~15u;
    PciConfigWrite32(g.pci.bus, g.pci.dev, g.pci.fn, offset, original);
    const uint32_t length = 0u - mask;
    const uint32_t base = original & ~15u;
    if (!base || length < PageBytes || (length & (length - 1)) ||
        (base & (length - 1))) return false;
    result = {base, length};
    return true;
}

bool PreparePci()
{
    if (!PciFindDevice(Vendor, Device, g.pci) || g.pci.classCode != 3 ||
        !PciBarIsIo(g.pci.bar[0])) return false;
    const uint32_t base = g.pci.bar[0] & ~3u;
    if (!base || base > 0xfff0) return false;
    g.port = static_cast<uint16_t>(base);
    const uint16_t command = PciConfigRead16(g.pci.bus, g.pci.dev, g.pci.fn, 4);
    if (command == 0xffff) return false;
    g.initialCommand = command;
    PciConfigWrite16(g.pci.bus, g.pci.dev, g.pci.fn, 4, command & ~3u);
    const bool sized = SizedAperture(1, g.framebuffer) && SizedAperture(2, g.commands);
    PciConfigWrite16(g.pci.bus, g.pci.dev, g.pci.fn, 4, sized ? (command | 3u) : command);
    return sized;
}

void ReleaseFifo()
{
    const VirtualAddress base(reinterpret_cast<uint64_t>(g.fifo));
    for (uint32_t i = 0; i < g.mappedPages; ++i)
        VmmDetachPage(KernelPageTable, base + uint64_t(i) * PageBytes);
    VmmFreePages(base, g.fifoBytes / PageBytes);
    g.fifo = nullptr;
    g.mappedPages = 0;
}

bool MapFifo(uint32_t reported)
{
    const uint32_t limit = reported < FifoMappingLimit ? reported : FifoMappingLimit;
    g.fifoBytes = limit & ~(PageBytes - 1);
    if (reported > g.commands.bytes || g.fifoBytes < PageBytes + svga::MinCommandBytes)
        return false;
    const uint32_t pages = g.fifoBytes / PageBytes;
    const auto base = VmmAllocPages(pages, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, 0);
    if (!base) return false;
    g.fifo = reinterpret_cast<volatile uint32_t*>(base.raw());
    for (; g.mappedPages < pages; ++g.mappedPages) {
        const uint64_t offset = uint64_t(g.mappedPages) * PageBytes;
        const auto address = base + offset;
        const auto backing = VmmVirtToPhys(KernelPageTable, address);
        if (!VmmMapPage(KernelPageTable, address, PhysicalAddress(g.commands.base + offset),
                        VMM_WRITABLE | VMM_NO_EXEC | VMM_CACHE_DISABLE | VMM_WRITE_THRU,
                        MemTag::Device, 0)) {
            ReleaseFifo();
            return false;
        }
        if (backing) PmmFreePage(backing);
    }
    return true;
}

svga::Geometry ReadGeometry()
{
    return {Read(Width), Read(Height), Read(Stride), Read(FbOffset)};
}

bool ValidGeometry(svga::Geometry mode, uint32_t reportedFb)
{
    const uint32_t limit = reportedFb < g.reportedVram ? reportedFb : g.reportedVram;
    return svga::FitsFramebuffer(mode, g.maxWidth, g.maxHeight, g.framebuffer.bytes, limit) &&
           Read(BitsPerPixel) == 32 && Read(RedMask) == 0x00ff0000 &&
           Read(GreenMask) == 0x0000ff00 && Read(BlueMask) == 0x000000ff;
}

void ProgramMode(uint32_t width, uint32_t height)
{
    Write(Width, width);
    Write(Height, height);
    Write(BitsPerPixel, 32);
    Write(Enable, 1);
}

bool PublishUpdate(uint32_t first, uint32_t end)
{
    uint32_t words[5];
    if (!svga::UpdateSpan(g.mode, first, end, words)) return true;
    svga::FifoWindow window{g.fifo[svga::FifoMin], g.fifo[svga::FifoMax],
                            g.fifo[svga::FifoNext], g.fifo[svga::FifoStop]};
    if (!svga::FifoCanWrite(window, g.fifoBytes, sizeof(words) / sizeof(words[0]))) {
        Write(Sync, 1);
        window.stop = g.fifo[svga::FifoStop];
        if (!svga::FifoCanWrite(window, g.fifoBytes, sizeof(words) / sizeof(words[0])))
            return false;
    }
    uint32_t cursor = window.next;
    for (uint32_t word : words) {
        g.fifo[cursor / sizeof(uint32_t)] = word;
        cursor = svga::FifoAdvance(window, cursor);
    }
    asm volatile("sfence" ::: "memory");
    g.fifo[svga::FifoNext] = cursor;
    Write(Sync, 1);
    return true;
}

void Flush(uint32_t first, uint32_t end)
{
    if (!TryLock()) {
        __atomic_store_n(&g.fullRefresh, true, __ATOMIC_RELEASE);
        return;
    }
    if (__atomic_load_n(&g.changingMode, __ATOMIC_ACQUIRE)) {
        Unlock();
        return;
    }
    if (__atomic_exchange_n(&g.fullRefresh, false, __ATOMIC_ACQUIRE)) {
        first = 0;
        end = g.mode.height;
    }
    if (first < g.mode.height && first < end) {
        if (g.firstPending >= g.endPending || first < g.firstPending) g.firstPending = first;
        if (end > g.endPending) g.endPending = end;
    }
    if (g.firstPending < g.endPending && PublishUpdate(g.firstPending, g.endPending))
        g.firstPending = g.endPending = 0;
    Unlock();
}

void PanicPresent()
{
    if (!TryLock()) return;
    if (!__atomic_load_n(&g.changingMode, __ATOMIC_ACQUIRE))
        (void)PublishUpdate(0, g.mode.height);
    Unlock();
}

bool SetMode(uint32_t width, uint32_t height)
{
    if (!width || !height || width > g.maxWidth || height > g.maxHeight ||
        uint64_t(width) * height * 4 > g.framebuffer.bytes) return false;
    if (!TryLock()) return false;
    if (__atomic_load_n(&g.changingMode, __ATOMIC_ACQUIRE)) {
        Unlock();
        return false;
    }
    if (width == g.mode.width && height == g.mode.height) {
        Unlock();
        return true;
    }
    Write(Sync, 1);
    if (g.fifo[svga::FifoNext] != g.fifo[svga::FifoStop]) {
        Unlock();
        return false;
    }
    __atomic_store_n(&g.changingMode, true, __ATOMIC_RELEASE);
    const auto previous = g.mode;
    ProgramMode(width, height);
    const auto candidate = ReadGeometry();
    const uint32_t fbBytes = Read(FbSize);
    const bool hardwareOk = candidate.width == width && candidate.height == height &&
                            ValidGeometry(candidate, fbBytes);
    Unlock();

    const uint64_t physical = uint64_t(g.framebuffer.base) + candidate.offset;
    const bool committed = hardwareOk &&
        CompositorRemapLinearDisplay(physical, width, height, candidate.stride / 4);

    // No ordinary flush reads device state during the handoff.
    while (!TryLock()) asm volatile("pause" ::: "memory");
    if (committed) g.mode = candidate;
    else ProgramMode(previous.width, previous.height);
    g.firstPending = 0;
    g.endPending = g.mode.height;
    if (PublishUpdate(0, g.mode.height)) g.endPending = 0;
    __atomic_store_n(&g.changingMode, false, __ATOMIC_RELEASE);
    Unlock();
    if (committed) SerialPrintf("vmware-svga: mode %ux%u stride=%u\n",
                                 width, height, candidate.stride);
    return committed;
}

void GetMode(DisplayMode* out)
{
    while (!TryLock()) asm volatile("pause" ::: "memory");
    *out = {g.mode.width, g.mode.height, g.mode.stride, 32};
    Unlock();
}

uint64_t FramebufferPhys()
{
    while (!TryLock()) asm volatile("pause" ::: "memory");
    const uint64_t physical = uint64_t(g.framebuffer.base) + g.mode.offset;
    Unlock();
    return physical;
}

volatile uint32_t* Framebuffer()
{
    return reinterpret_cast<volatile uint32_t*>(PhysToVirt(PhysicalAddress(FramebufferPhys())).raw());
}

constexpr DisplayOps Ops{"vmware-svga2", SetMode, GetMode, Framebuffer,
                         FramebufferPhys, Flush, PanicPresent};

int Initialize()
{
    if (!PreparePci()) return -1;
    const uint32_t oldId = Read(Id);
    Write(Id, Version2);
    auto abandon = [oldId]() {
        Write(Id, oldId);
        PciConfigWrite16(g.pci.bus, g.pci.dev, g.pci.fn, 4, g.initialCommand);
        return -1;
    };
    if (Read(Id) != Version2 || Read(FbStart) != g.framebuffer.base ||
        Read(FifoStart) != g.commands.base) return abandon();
    g.maxWidth = Read(MaxWidth);
    g.maxHeight = Read(MaxHeight);
    if (g.maxWidth > MaxScreenDimension) g.maxWidth = MaxScreenDimension;
    if (g.maxHeight > MaxScreenDimension) g.maxHeight = MaxScreenDimension;
    g.reportedVram = Read(VramSize);
    if (!g.maxWidth || !g.maxHeight || !g.reportedVram || !MapFifo(Read(FifoSize)))
        return abandon();

    const uint32_t enable = Read(Enable), configured = Read(ConfigDone);
    const uint32_t oldWidth = Read(Width), oldHeight = Read(Height), oldBits = Read(BitsPerPixel);
    uint64_t firmware = 0;
    uint32_t width = 0, height = 0, stride = 0;
    if (!TtyGetFramebufferPhys(&firmware, &width, &height, &stride) ||
        !width || !height || width > g.maxWidth || height > g.maxHeight ||
        uint64_t(width) * height * 4 > g.framebuffer.bytes) {
        width = g.maxWidth < 1024 ? g.maxWidth : 1024;
        height = g.maxHeight < 768 ? g.maxHeight : 768;
    }
    ProgramMode(width, height);
    const auto candidate = ReadGeometry();
    if (candidate.width != width || candidate.height != height ||
        !ValidGeometry(candidate, Read(FbSize)) ||
        !CompositorRemapLinearDisplay(uint64_t(g.framebuffer.base) + candidate.offset,
                                      width, height, candidate.stride / 4)) {
        Write(Width, oldWidth);
        Write(Height, oldHeight);
        Write(BitsPerPixel, oldBits);
        Write(Enable, enable);
        ReleaseFifo();
        return abandon();
    }

    g.mode = candidate;
    if (configured) Write(Sync, 1);
    Write(ConfigDone, 0);
    // Keep one page for the extended FIFO registers, including the full
    // fixed register range used by Workstation. Legacy QEMU needs four.
    g.fifo[svga::FifoMin] = PageBytes;
    g.fifo[svga::FifoMax] = g.fifoBytes;
    g.fifo[svga::FifoNext] = PageBytes;
    g.fifo[svga::FifoStop] = PageBytes;
    asm volatile("sfence" ::: "memory");
    Write(ConfigDone, 1);
    (void)PublishUpdate(0, g.mode.height);
    DisplayRegister(&Ops);
    KPrintf("vmware-svga: %ux%u stride=%u caps=0x%x fifo=%u\n",
            width, height, candidate.stride, Read(Capabilities), g.fifoBytes);
    return 0;
}
}

DECLARE_MODULE("vmware_svga", Initialize, nullptr, "VMware SVGA II linear display and FIFO");
