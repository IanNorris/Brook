// Intel I219-V (PCH-integrated "ich8lan" MAC, PCH_SPT class) on the Z370
// desktop: 8086:15b8 only. Reference: Linux drivers/net/ethernet/intel/e1000e
// (ich8lan.c, phy.c, netdev.c), board_pch_spt. One legacy RX/TX queue,
// standard MTU, polled (the firmware leaves no INTx line).
//
// Staged bring-up. Stage 0 (always, at load) is read-only apart from the
// SW/FW/HW semaphore and MDIC PHY reads. Later stages run on the module's
// worker thread, only when the initrd marker or the operator asks for them:
//   1 "attach":  program the rings over the firmware-initialised MAC/PHY (no
//                reset), set link up, register the interface (DHCP follows);
//   2 "reset":   Linux's MAC+PHY reset path (e1000_reset_hw_ich8lan and the
//                post-reset steps) before attaching.
// Never touched: NVM/flash (no flash-controller cycles), ME firmware state
// beyond the documented H2ME ULP handshake, EFI variables, PCI config space
// other than the command register.
#include "e1000_ring.h"

#include "apic.h"
#include "boot_status.h"
#include "debug_hardware.h"
#include "kprintf.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "module_abi.h"
#include "net.h"
#include "pci.h"
#include "pci_quiesce.h"
#include "platform_power.h"
#include "scheduler.h"
#include "serial.h"
#include "spinlock.h"
#include "vfs.h"
#ifdef BROOK_HOST_TEST
#include <string.h>
#else
#include "string.h"
#endif

MODULE_IMPORT_SYMBOL(ApicTickCount);
MODULE_IMPORT_SYMBOL(BootStatusAddProvider);
MODULE_IMPORT_SYMBOL(DebugNicCommandSetHandler);
MODULE_IMPORT_SYMBOL(PlatformPowerAddQuiesce);
MODULE_IMPORT_SYMBOL(PciQuiesceFunctionForReset);
MODULE_IMPORT_SYMBOL(KernelWorkerStart);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(NetReceive);
MODULE_IMPORT_SYMBOL(NetRegisterIf);
MODULE_IMPORT_SYMBOL(PciConfigRead16);
MODULE_IMPORT_SYMBOL(PciConfigRead32);
MODULE_IMPORT_SYMBOL(PciConfigWrite16);
MODULE_IMPORT_SYMBOL(PciFindDevice);
MODULE_IMPORT_SYMBOL(PmmFreePage);
MODULE_IMPORT_SYMBOL(SchedulerSleepMs);
MODULE_IMPORT_SYMBOL(SerialPrintf);
MODULE_IMPORT_SYMBOL(UsbStatusChanged);
MODULE_IMPORT_SYMBOL(VfsClose);
MODULE_IMPORT_SYMBOL(VfsOpen);
MODULE_IMPORT_SYMBOL(VmmAllocPages);
MODULE_IMPORT_SYMBOL(VmmAllocPagesAccounted);
MODULE_IMPORT_SYMBOL(VmmDetachPage);
MODULE_IMPORT_SYMBOL(VmmMapPage);
MODULE_IMPORT_SYMBOL(VmmVirtToPhys);

namespace {
using namespace brook;
using namespace brook::e1000;

enum Reg : uint32_t {
    Ctrl = 0x0000, Status = 0x0008, Strap = 0x000c, CtrlExt = 0x0018, Fextnvm6 = 0x0010,
    Mdic = 0x0020, Fextnvm4 = 0x0024, Fextnvm = 0x0028, Fextnvm3 = 0x003c, Fextnvm7 = 0x00e4,
    Icr = 0x00c0, Ims = 0x00d0, Imc = 0x00d8,
    Rctl = 0x0100, Tctl = 0x0400, Tipg = 0x0410, Ledctl = 0x0e00,
    ExtcnfCtrl = 0x0f00, ExtcnfSize = 0x0f08, PhyCtrl = 0x0f10, Pcieanacfg = 0x0f18, Iosfpc = 0x0f28,
    Pba = 0x1000, Pbs = 0x1008, Pbeccsts = 0x100c,
    Rdbal = 0x2800, Rdbah = 0x2804, Rdlen = 0x2808, Rdh = 0x2810, Rdt = 0x2818, Rdtr = 0x2820,
    Rxdctl = 0x2828, Radv = 0x282c,
    Kabgtxd = 0x3004,
    Tdbal = 0x3800, Tdbah = 0x3804, Tdlen = 0x3808, Tdh = 0x3810, Tdt = 0x3818, Tidv = 0x3820,
    Txdctl0 = 0x3828, Tadv = 0x382c, Tarc0 = 0x3840, Txdctl1 = 0x3928, Tarc1 = 0x3940,
    Rxcsum = 0x5000, Rfctl = 0x5008, Mta = 0x5200, Ral0 = 0x5400, Rah0 = 0x5404,
    Manc = 0x5820, H2me = 0x5b50, Fwsm = 0x5b54, Fextnvm9 = 0x5bb4, Fextnvm11 = 0x5bbc,
    // Statistics (clear on read).
    Crcerrs = 0x4000, Mpc = 0x4010, Gprc = 0x4074, Gptc = 0x4080,
};

constexpr uint32_t CtrlMasterDisable = 1u << 2, CtrlSlu = 1u << 6, CtrlFrcSpd = 1u << 11,
                   CtrlFrcDpx = 1u << 12, CtrlLanPhyPcOverride = 1u << 16, CtrlLanPhyPcValue = 1u << 17,
                   CtrlMehe = 1u << 19, CtrlRst = 1u << 26, CtrlPhyRst = 1u << 31;
constexpr uint32_t StatusFd = 1u << 0, StatusLu = 1u << 1, StatusLanInitDone = 1u << 9,
                   StatusPhyRa = 1u << 10, StatusMasterEnable = 1u << 19;
constexpr uint32_t CtrlExtLpcd = 1u << 2, CtrlExtForceSmbus = 1u << 11, CtrlExtRoDis = 1u << 17,
                   CtrlExtPhyPden = 1u << 20;
constexpr uint32_t MdicReady = 1u << 28, MdicError = 1u << 30;
constexpr uint32_t ExtcnfSwFlag = 1u << 5;
constexpr uint32_t FwsmRspciphy = 1u << 6, FwsmUlpCfgDone = 1u << 10, FwsmFwValid = 1u << 15;
constexpr uint32_t H2meUlp = 1u << 11, H2meEnforce = 1u << 12;
constexpr uint32_t FextnvmSwConfigIch8m = 1u << 27;
constexpr uint32_t RahValid = 1u << 31;
constexpr uint32_t RxEnable = 1u << 1, RxBroadcast = 1u << 15, RxStripFcs = 1u << 26;
constexpr uint32_t TxEnable = 1u << 1, TxPsp = 1u << 3, TxRtlc = 1u << 24;
constexpr uint32_t RfctlNfsDisable = (1u << 6) | (1u << 7), RfctlExsten = 1u << 15;
constexpr uint32_t PhyAddrPage0 = 2, PhyAddrHv = 1;
constexpr uint16_t PciMemory = 1u << 1, PciMaster = 1u << 2, PciIntDisable = 1u << 10;
constexpr uint32_t PageSize = 4096, MmioBytes = 0x20000, MmioPages = MmioBytes / PageSize;
constexpr uint32_t PacketPages = RingSize * BufferSize / PageSize;
constexpr uint32_t MtaCount = 32; // ich8lan mta_reg_count

constexpr uint32_t PhyReg(uint32_t page, uint32_t reg) { return (page << 5) | (reg & 0x1f); }
constexpr uint32_t HvOemBits = PhyReg(768, 25), CvSmbCtrl = PhyReg(769, 23), BmPortGenCfg = PhyReg(769, 17),
                   HvPmCtrl = PhyReg(770, 17), PllClockGate = PhyReg(772, 28), I218UlpConfig1 = PhyReg(779, 16),
                   SptPtrGap = PhyReg(776, 20);

struct DmaRing { VirtualAddress descriptors, packets; };

PciDevice g_device{};
uint64_t g_bar = 0;
volatile uint32_t* g_regs = nullptr;
uint16_t g_firmwareCommand = 0;
NetIf g_if{};
DmaRing g_rxDma{}, g_txDma{};
RxRing g_rx{};
TxRing g_tx{};
SpinLock g_rxLock{}, g_txLock{}, g_phyLock{};
volatile uint32_t g_requested = 0;   // highest stage asked for
volatile uint32_t g_reached = 0;     // highest stage completed
volatile bool g_registered = false, g_linkUp = false, g_autonegRequested = false;
uint64_t g_nextLinkSample = 0;
uint64_t g_rxFrames = 0, g_txFrames = 0, g_txBusy = 0, g_rxDropped = 0;
const char* g_failure = nullptr;
char g_steps[512];                   // short log of what the stages did
uint32_t g_stepsLength = 0;

struct Text {
    char* out; uint32_t cap, n = 0;
    void c(char ch) { if (n + 1 < cap) out[n++] = ch; }
    void s(const char* t) { while (t && *t) c(*t++); }
    void x(uint64_t v, unsigned digits) {
        for (int i = int(digits) - 1; i >= 0; --i) c("0123456789abcdef"[(v >> (4 * i)) & 15]);
    }
    void u(uint64_t v) { char b[21]; int i = 0; do { b[i++] = char('0' + v % 10); v /= 10; } while (v); while (i) c(b[--i]); }
    void r(const char* name, uint32_t v) { c(' '); s(name); c('='); x(v, 8); }
    void nl() { c('\n'); }
};

uint32_t Read(uint32_t reg) { return g_regs[reg / 4]; }
void Write(uint32_t reg, uint32_t value) { g_regs[reg / 4] = value; }
void Flush() { (void)Read(Status); }

void Step(const char* text)
{
    SerialPrintf("i219: %s\n", text);
    KPrintf("i219: %s\n", text);
    while (*text && g_stepsLength + 2 < sizeof(g_steps)) g_steps[g_stepsLength++] = *text++;
    if (g_stepsLength + 2 < sizeof(g_steps)) g_steps[g_stepsLength++] = ';';
    g_steps[g_stepsLength] = 0;
}

// Delays: Linux's usleep/msleep minimums are all honoured by sleeping at
// least that long. Before the scheduler runs (module init), spin on the tick.
bool g_threadContext = false;
void SleepMs(uint32_t ms)
{
    if (g_threadContext) { SchedulerSleepMs(ms ? ms : 1); return; }
    const uint64_t end = ApicTickCount() + ms + 1;
    for (uint32_t spin = 0; ApicTickCount() < end && spin < 50000000u; ++spin) __asm__ volatile("pause");
}

// SW/FW/HW semaphore for PHY and selected CSR access (EXTCNF_CTRL.SWFLAG).
bool AcquireSwFlag()
{
    // The driver-side lock is bounded too: a sleeping stage may hold it.
    // Every wait has a spin bound as well as a tick bound, in case the boot
    // clock does not advance where this runs (module init). An MMIO read of
    // the PCH takes about a microsecond.
    if (Read(Status) == 0xffffffffu) return false; // device not answering
    uint64_t start = ApicTickCount();
    for (uint32_t spin = 0; !SpinLockTryAcquire(&g_phyLock); ++spin) {
        if (ApicTickCount() - start > 2000 || spin > 100000000u) return false;
        __asm__ volatile("pause");
    }
    start = ApicTickCount();
    for (uint32_t spin = 0; Read(ExtcnfCtrl) & ExtcnfSwFlag; ++spin) {
        if (ApicTickCount() - start > 100 || spin > 200000u) { SpinLockRelease(&g_phyLock); return false; }
        __asm__ volatile("pause");
    }
    Write(ExtcnfCtrl, Read(ExtcnfCtrl) | ExtcnfSwFlag);
    start = ApicTickCount();
    for (uint32_t spin = 0; !(Read(ExtcnfCtrl) & ExtcnfSwFlag); ++spin) {
        if (ApicTickCount() - start > 1000 || spin > 2000000u) {
            Write(ExtcnfCtrl, Read(ExtcnfCtrl) & ~ExtcnfSwFlag);
            SpinLockRelease(&g_phyLock);
            return false;
        }
        __asm__ volatile("pause");
    }
    return true;
}

void ReleaseSwFlag()
{
    const uint32_t value = Read(ExtcnfCtrl);
    if (value & ExtcnfSwFlag) Write(ExtcnfCtrl, value & ~ExtcnfSwFlag);
    SpinLockRelease(&g_phyLock);
}

// One MDIC transaction (e1000e_{read,write}_phy_reg_mdic). Caller holds SWFLAG.
bool MdicOp(uint32_t address, uint32_t reg, bool write, uint16_t& data)
{
    uint32_t value = ((reg & 0x1f) << 16) | ((address & 0x1f) << 21) | ((write ? 1u : 2u) << 26);
    if (write) value |= data;
    Write(Mdic, value);
    const uint64_t start = ApicTickCount();
    for (uint32_t spin = 0;; ++spin) {
        value = Read(Mdic);
        if (value & MdicReady) break;
        if (ApicTickCount() - start > 20 || spin > 2000000u) return false;
        __asm__ volatile("pause");
    }
    if (value & MdicError) return false;
    if (((value >> 16) & 0x1f) != (reg & 0x1f)) return false;
    if (!write) data = static_cast<uint16_t>(value);
    // Linux delays after MDIC on 82579; harmless here and avoids reading
    // stale data in a back-to-back transaction.
    for (int i = 0; i < 4; ++i) Flush();
    return true;
}

// HV/LPT-style paged PHY access (__e1000_{read,write}_phy_reg_hv) for page 0
// and pages >= 768. Pages 1..767 and the wakeup page 800 are never used.
bool PhyAccessLocked(uint32_t offset, bool write, uint16_t& data)
{
    uint32_t page = offset >> 5;
    const uint32_t reg = offset & 0x1f;
    if ((page > 0 && page < 768) || page == 800) return false;
    const uint32_t address = page >= 768 ? PhyAddrHv : PhyAddrPage0;
    if (page == 768) page = 0;
    if (reg > 15) {
        uint16_t select = static_cast<uint16_t>(page << 5);
        if (!MdicOp(address, 0x1f, true, select)) return false;
    }
    return MdicOp(address, reg, write, data);
}

bool PhyRead(uint32_t offset, uint16_t& data)
{
    if (!AcquireSwFlag()) return false;
    const bool ok = PhyAccessLocked(offset, false, data);
    ReleaseSwFlag();
    return ok;
}

bool PhyWrite(uint32_t offset, uint16_t data)
{
    if (!AcquireSwFlag()) return false;
    const bool ok = PhyAccessLocked(offset, true, data);
    ReleaseSwFlag();
    return ok;
}

// e1000_phy_is_accessible_pchlan: PHY ID registers readable (caller: SWFLAG).
uint32_t PhyIdLocked()
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        uint16_t high = 0, low = 0;
        if (!MdicOp(PhyAddrPage0, 2, false, high) || high == 0xffff) continue;
        if (!MdicOp(PhyAddrPage0, 3, false, low) || low == 0xffff) continue;
        return (uint32_t(high) << 16) | low;
    }
    return 0;
}

bool ResetBlocked() { return !(Read(Fwsm) & FwsmRspciphy); }

// e1000_check_reset_block_ich8lan: wait up to ~300 ms for RSPCIPHY.
bool WaitResetUnblocked()
{
    for (int i = 0; i < 31; ++i) {
        if (!ResetBlocked()) return true;
        SleepMs(10);
    }
    return false;
}

// ---- Status text ---------------------------------------------------------

const char* StageName(uint32_t stage)
{
    switch (stage) { case 0: return "probe"; case 1: return "attach"; case 2: return "reset"; default: return "?"; }
}

void DescribeRegisters(Text& t)
{
    const uint32_t status = Read(Status), fwsm = Read(Fwsm);
    t.s("I219 regs"); t.r("CTRL", Read(Ctrl)); t.r("STATUS", status); t.r("CTRL_EXT", Read(CtrlExt));
    t.r("FWSM", fwsm); t.r("EXTCNF", Read(ExtcnfCtrl)); t.r("H2ME", Read(H2me)); t.r("MANC", Read(Manc));
    t.r("PHY_CTRL", Read(PhyCtrl)); t.r("STRAP", Read(Strap)); t.nl();
    t.s("I219 regs"); t.r("FEXTNVM", Read(Fextnvm)); t.r("FEXTNVM3", Read(Fextnvm3)); t.r("FEXTNVM4", Read(Fextnvm4));
    t.r("FEXTNVM6", Read(Fextnvm6)); t.r("FEXTNVM7", Read(Fextnvm7)); t.r("FEXTNVM9", Read(Fextnvm9));
    t.r("FEXTNVM11", Read(Fextnvm11)); t.r("PCIEANACFG", Read(Pcieanacfg)); t.r("EXTCNF_SIZE", Read(ExtcnfSize)); t.nl();
    t.s("I219 regs"); t.r("RCTL", Read(Rctl)); t.r("TCTL", Read(Tctl)); t.r("TIPG", Read(Tipg));
    t.r("PBA", Read(Pba)); t.r("PBS", Read(Pbs)); t.r("PBECC", Read(Pbeccsts)); t.r("IOSFPC", Read(Iosfpc));
    t.r("TXDCTL", Read(Txdctl0)); t.r("TARC0", Read(Tarc0)); t.r("RXDCTL", Read(Rxdctl)); t.r("RFCTL", Read(Rfctl));
    t.r("LEDCTL", Read(Ledctl)); t.nl();
    t.s("I219 regs"); t.r("RDBAL", Read(Rdbal)); t.r("RDLEN", Read(Rdlen)); t.r("RDH", Read(Rdh)); t.r("RDT", Read(Rdt));
    t.r("TDBAL", Read(Tdbal)); t.r("TDLEN", Read(Tdlen)); t.r("TDH", Read(Tdh)); t.r("TDT", Read(Tdt));
    t.r("RAL0", Read(Ral0)); t.r("RAH0", Read(Rah0)); t.nl();
    t.s("I219 decode link="); t.s(status & StatusLu ? "up" : "down");
    static const char* speeds[] = {"10", "100", "1000", "1000"};
    t.s(" speed="); t.s(speeds[(status >> 6) & 3]); t.s(status & StatusFd ? " full" : " half");
    t.s(" fw_valid="); t.u((fwsm & FwsmFwValid) ? 1 : 0); t.s(" fw_mode="); t.u((fwsm >> 1) & 7);
    t.s(" phy_reset_blocked="); t.u((fwsm & FwsmRspciphy) ? 0 : 1);
    t.s(" ulp_cfg_done="); t.u((fwsm & FwsmUlpCfgDone) ? 1 : 0);
    t.s(" force_smbus="); t.u((Read(CtrlExt) & CtrlExtForceSmbus) ? 1 : 0);
    t.s(" sw_lcd_cfg="); t.u((Read(Fextnvm) & FextnvmSwConfigIch8m) ? 1 : 0);
    t.s(" master="); t.u((status & StatusMasterEnable) ? 1 : 0);
    t.s(" rx_en="); t.u((Read(Rctl) & RxEnable) ? 1 : 0); t.s(" tx_en="); t.u((Read(Tctl) & TxEnable) ? 1 : 0);
    t.nl();
}

void DescribePhy(Text& t)
{
    t.s("I219 phy");
    if (!AcquireSwFlag()) { t.s(" swflag=busy"); t.r("EXTCNF", Read(ExtcnfCtrl)); t.nl(); return; }
    static const uint8_t regs[] = {0, 1, 2, 3, 4, 5, 6, 9, 10, 15, 17, 26};
    for (uint8_t reg : regs) {
        uint16_t value = 0;
        t.s(" r"); t.u(reg); t.c('=');
        if (MdicOp(PhyAddrPage0, reg, false, value)) t.x(value, 4); else t.s("err");
    }
    uint16_t id1 = 0;
    t.s(" a1r2=");
    if (MdicOp(PhyAddrHv, 2, false, id1)) t.x(id1, 4); else t.s("err");
    ReleaseSwFlag();
    t.nl();
}

uint32_t StatusText(char* out, uint32_t cap)
{
    Text t{out, cap};
    if (!g_regs) { t.s("I219 not mapped: "); t.s(g_failure ? g_failure : "-"); t.nl(); return t.n; }
    const auto& m = g_if.mac.b;
    t.s("I219 00:1f.6 8086:15b8 stage="); t.s(StageName(g_reached));
    if (g_requested > g_reached) { t.s(" requested="); t.s(StageName(g_requested)); }
    t.s(" link="); t.s(Read(Status) & StatusLu ? "up" : "down");
    t.s(" mac="); for (int i = 0; i < 6; ++i) { if (i) t.c(':'); t.x(m[i], 2); }
    t.s(" registered="); t.u(g_registered ? 1 : 0);
    if (g_registered) {
        t.s(" rx="); t.u(g_rxFrames); t.s(" tx="); t.u(g_txFrames); t.s(" busy="); t.u(g_txBusy);
        t.s(" drop="); t.u(g_rxDropped);
    }
    if (g_failure) { t.s(" FAILED "); t.s(g_failure); }
    t.nl();
    DescribeRegisters(t);
    if (g_stepsLength) { t.s("I219 steps "); t.s(g_steps); t.nl(); }
    return t.n;
}

// ---- MMIO / DMA ------------------------------------------------------------

bool MapMmio(uint64_t physical)
{
    const auto base = VmmAllocPages(MmioPages, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
    if (!base) return false;
    for (uint32_t i = 0; i < MmioPages; ++i) {
        const auto address = base + uint64_t(i) * PageSize;
        const auto backing = VmmVirtToPhys(KernelPageTable, address);
        if (!VmmMapPage(KernelPageTable, address, PhysicalAddress(physical + uint64_t(i) * PageSize),
                        VMM_WRITABLE | VMM_NO_EXEC | VMM_CACHE_DISABLE | VMM_WRITE_THRU, MemTag::Device, KernelPid))
            return false; // leaked mapping is harmless: the module stays resident
        if (backing) PmmFreePage(backing);
    }
    g_regs = reinterpret_cast<volatile uint32_t*>(base.raw());
    return true;
}

template <typename Descriptor>
bool AllocateDma(DmaRing& dma)
{
    constexpr uint64_t flags = VMM_WRITABLE | VMM_NO_EXEC;
    dma.descriptors = VmmAllocPagesAccounted(1, flags, MemTag::Device, KernelPid, PmmOwner::NetworkDma);
    dma.packets = VmmAllocPagesAccounted(PacketPages, flags, MemTag::Device, KernelPid, PmmOwner::NetworkDma);
    if (!dma.descriptors || !dma.packets) return false;
    auto* descriptors = reinterpret_cast<Descriptor*>(dma.descriptors.raw());
    memset(descriptors, 0, RingSize * sizeof(Descriptor));
    for (uint16_t i = 0; i < RingSize; ++i) {
        const auto physical = VmmVirtToPhys(KernelPageTable, dma.packets + uint64_t(i) * BufferSize);
        if (!physical) return false;
        descriptors[i].address = physical.raw();
    }
    return true;
}

// ---- Data path -------------------------------------------------------------

int Submit(const void* frame, uint32_t length, bool tryOnly)
{
    if (!frame || length < MinFrameSize || length > MaxFrameSize) return -22;
    if (tryOnly) { if (!SpinLockTryAcquire(&g_txLock)) return -11; }
    else SpinLockAcquire(&g_txLock);
    auto* descriptors = reinterpret_cast<TxDescriptor*>(g_txDma.descriptors.raw());
    g_tx.Reap(descriptors);
    int result = -11;
    if (!g_tx.Full()) {
        const uint16_t slot = g_tx.tail;
        memcpy(reinterpret_cast<void*>((g_txDma.packets + uint64_t(slot) * BufferSize).raw()), frame, length);
        descriptors[slot].length = static_cast<uint16_t>(length);
        descriptors[slot].command = TxCommand;
        descriptors[slot].status = 0;
        g_tx.tail = Next(slot);
        __asm__ volatile("sfence" ::: "memory");
        Write(Tdt, g_tx.tail);
        ++g_txFrames;
        result = 0;
    } else ++g_txBusy;
    SpinLockRelease(&g_txLock);
    return result;
}

int Transmit(NetIf*, const void* frame, uint32_t length) { return Submit(frame, length, false); }
int TryTransmit(NetIf*, const void* frame, uint32_t length) { return Submit(frame, length, true); }

void Poll(NetIf* nif)
{
    if (!SpinLockTryAcquire(&g_rxLock)) return;
    const uint64_t now = ApicTickCount();
    if (now >= g_nextLinkSample) {
        g_nextLinkSample = now + 250;
        const bool up = Read(Status) & StatusLu;
        if (up != g_linkUp) {
            g_linkUp = up;
            SerialPrintf("i219: link %s\n", up ? "up" : "down");
            UsbStatusChanged();
        }
    }
    auto* descriptors = reinterpret_cast<RxDescriptor*>(g_rxDma.descriptors.raw());
    uint16_t last = g_rx.next;
    uint32_t count = 0;
    for (; count < RingSize; ++count) {
        const uint16_t slot = g_rx.next;
        auto& descriptor = descriptors[slot];
        const uint8_t status = __atomic_load_n(&descriptor.status, __ATOMIC_ACQUIRE);
        if (!(status & DescriptorDone)) break;
        const uint16_t length = descriptor.length;
        if (g_rx.Accept(status, descriptor.errors, length)) {
            ++g_rxFrames;
            NetReceive(nif, reinterpret_cast<const void*>((g_rxDma.packets + uint64_t(slot) * BufferSize).raw()), length);
        } else ++g_rxDropped;
        descriptor.status = 0;
        g_rx.next = Next(slot);
        last = slot;
    }
    if (count) {
        __asm__ volatile("sfence" ::: "memory");
        Write(Rdt, last);
    }
    SpinLockRelease(&g_rxLock);
}

bool Snapshot(NetIf*, NetDriverSnapshot* out)
{
    *out = {};
    out->received = g_rxFrames; out->transmitted = g_txFrames; out->dropped = g_rxDropped; out->txBusy = g_txBusy;
    out->rxCompletions = g_rxFrames + g_rxDropped; out->txSubmitted = g_txFrames;
    out->rxDepth = RingSize; out->txDepth = RingSize;
    out->linkKnown = true; out->linkUp = g_regs && (Read(Status) & StatusLu);
    return true;
}

// ---- Bring-up steps ---------------------------------------------------------

bool DisableMaster()
{
    Write(Ctrl, Read(Ctrl) | CtrlMasterDisable);
    const uint64_t start = ApicTickCount();
    for (uint32_t spin = 0; Read(Status) & StatusMasterEnable; ++spin) {
        if (ApicTickCount() - start > 100 || spin > 1000) return false;
        SleepMs(1);
    }
    return true;
}

void StopUnits()
{
    Write(Imc, ~0u);
    Write(Rctl, 0);
    Write(Tctl, TxPsp);
    Flush();
    SleepMs(10);
}

// e1000_toggle_lanphypc_pch_lpt (caller holds SWFLAG).
void ToggleLanPhyPc()
{
    uint32_t value = Read(Fextnvm3);
    value = (value & ~0x0c000000u) | 0x08000000u; // PHY config counter 50 ms
    Write(Fextnvm3, value);
    value = Read(Ctrl);
    value |= CtrlLanPhyPcOverride;
    value &= ~CtrlLanPhyPcValue;
    Write(Ctrl, value);
    Flush();
    SleepMs(1);
    value &= ~CtrlLanPhyPcOverride;
    Write(Ctrl, value);
    Flush();
    for (int i = 0; i < 21 && !(Read(CtrlExt) & CtrlExtLpcd); ++i) SleepMs(6);
    SleepMs(30);
}

// Unforce SMBus in PHY and MAC once the PHY answers, when ME is not active
// (tail of e1000_phy_is_accessible_pchlan for LPT and newer).
void UnforceSmbusLocked()
{
    if (Read(Fwsm) & FwsmFwValid) return;
    uint16_t value = 0;
    if (PhyAccessLocked(CvSmbCtrl, false, value)) {
        value &= ~1u;
        (void)PhyAccessLocked(CvSmbCtrl, true, value);
    }
    Write(CtrlExt, Read(CtrlExt) & ~CtrlExtForceSmbus);
}

// e1000_disable_ulp_lpt_lp(force=true) and e1000_init_phy_workarounds_pchlan,
// for an I219 whose PHY does not answer. Returns the PHY ID or 0.
uint32_t RecoverPhyAccess()
{
    if (Read(Fwsm) & FwsmFwValid) {
        // ME is present: ask it to un-configure ULP (the documented H2ME
        // handshake) and wait up to 2.5 s for ULP_CFG_DONE to clear.
        Write(H2me, (Read(H2me) & ~H2meUlp) | H2meEnforce);
        int i = 0;
        while ((Read(Fwsm) & FwsmUlpCfgDone) && i++ < 250) SleepMs(10);
        Write(H2me, Read(H2me) & ~H2meEnforce);
        Step(i > 250 ? "ULP: ME did not clear ULP_CFG_DONE" : "ULP: ME un-configured ULP");
    }
    if (!AcquireSwFlag()) { Step("SWFLAG busy"); return 0; }
    uint32_t id = PhyIdLocked();
    if (!id) {
        Write(CtrlExt, Read(CtrlExt) | CtrlExtForceSmbus);
        SleepMs(50);
        id = PhyIdLocked();
        if (id) Step("PHY answered with MAC forced to SMBus");
    }
    if (!id) {
        if (ResetBlocked()) {
            ReleaseSwFlag();
            Step("LANPHYPC toggle blocked by ME (FWSM.RSPCIPHY=0)");
            return 0;
        }
        ToggleLanPhyPc();
        id = PhyIdLocked();
        if (!id) {
            Write(CtrlExt, Read(CtrlExt) & ~CtrlExtForceSmbus);
            id = PhyIdLocked();
        }
        Step(id ? "PHY answered after LANPHYPC toggle" : "PHY still silent after LANPHYPC toggle");
    }
    if (id) {
        UnforceSmbusLocked();
        if (!(Read(Fwsm) & FwsmFwValid)) {
            // Rest of the non-ME ULP exit: K1 back on, ULP configuration
            // cleared and committed, SMBus release on PERST# re-enabled.
            uint16_t value = 0;
            if (PhyAccessLocked(HvPmCtrl, false, value)) {
                value |= 0x4000;
                (void)PhyAccessLocked(HvPmCtrl, true, value);
            }
            if (PhyAccessLocked(I218UlpConfig1, false, value)) {
                value &= ~uint16_t(0x0004 | 0x0010 | 0x0100 | 0x0040 | 0x0020 | 0x0400 | 0x0800 | 0x1000);
                (void)PhyAccessLocked(I218UlpConfig1, true, value);
                value |= 0x0001;
                (void)PhyAccessLocked(I218UlpConfig1, true, value);
            }
            Write(Fextnvm7, Read(Fextnvm7) & ~0x20u);
            Step("non-ME ULP configuration cleared");
        }
    }
    ReleaseSwFlag();
    return id;
}

// Clear the host wake-up bit and apply the OEM bits after a PHY reset
// (e1000_post_phy_reset_ich8lan minus the NVM extended configuration, which
// needs flash reads; it only runs when FEXTNVM.SW_CONFIG is set).
void PostPhyReset()
{
    SleepMs(10);
    uint16_t value = 0;
    if (PhyRead(BmPortGenCfg, value)) (void)PhyWrite(BmPortGenCfg, static_cast<uint16_t>(value & ~(1u << 4))); // host WU bit
    if (Read(Fextnvm) & FextnvmSwConfigIch8m) {
        Step("FEXTNVM.SW_CONFIG set: NVM LCD config skipped (no flash access)");
        const uint32_t phyCtrl = Read(PhyCtrl);
        if (PhyRead(HvOemBits, value)) {
            value &= ~((1u << 6) | (1u << 2)); // GBE_DIS, LPLU
            if (phyCtrl & (1u << 6)) value |= 1u << 6;  // PHY_CTRL.GBE_DISABLE
            if (phyCtrl & (1u << 1)) value |= 1u << 2;  // PHY_CTRL.D0A_LPLU
            if (!ResetBlocked()) value |= 1u << 10;     // restart AN
            (void)PhyWrite(HvOemBits, value);
        }
    }
}

// e1000_reset_hw_ich8lan for pch_spt with an idle ring (no flush needed).
bool ResetHardware()
{
    if (!DisableMaster()) Step("PCIe master disable timed out");
    StopUnits();
    uint32_t ctrl = Read(Ctrl);
    const bool phyReset = WaitResetUnblocked();
    if (phyReset) ctrl |= CtrlPhyRst;
    else Step("PHY reset blocked by ME: MAC-only reset");
    const bool flag = AcquireSwFlag();
    Write(Ctrl, ctrl | CtrlRst);
    // No flush here: Linux notes that it hangs the hardware.
    SleepMs(20);
    if (flag) SpinLockRelease(&g_phyLock); // the reset clears SWFLAG itself
    if (phyReset) {
        // e1000_get_cfg_done_ich8lan: LAN_INIT_DONE, then clear PHYRA.
        uint32_t loop = 0;
        while (!(Read(Status) & StatusLanInitDone) && loop++ < 150) SleepMs(1);
        Write(Status, Read(Status) & ~StatusLanInitDone);
        const uint32_t status = Read(Status);
        if (status & StatusPhyRa) Write(Status, status & ~StatusPhyRa);
        if (loop >= 150) Step("LAN_INIT_DONE not seen after reset");
        PostPhyReset();
    }
    Write(Imc, ~0u);
    (void)Read(Icr);
    Write(Kabgtxd, Read(Kabgtxd) | 0x00050000u); // BGSQLBIAS
    Step(phyReset ? "MAC+PHY reset done" : "MAC reset done");
    return true;
}

bool ProgramMac(MacAddr& mac)
{
    const uint32_t low = Read(Ral0), high = Read(Rah0);
    for (unsigned i = 0; i < 4; ++i) mac.b[i] = static_cast<uint8_t>(low >> (8 * i));
    mac.b[4] = static_cast<uint8_t>(high);
    mac.b[5] = static_cast<uint8_t>(high >> 8);
    if (!(high & RahValid) || (mac.b[0] & 1) || !(low || (high & 0xffffu))) return false;
    return true;
}

// e1000_initialize_hw_bits_ich8lan, the init_hw TXDCTL policy and the SPT
// errata of e1000_configure_tx / e1000e_reset (legacy interrupt mode).
void InitHardwareBits()
{
    Write(CtrlExt, Read(CtrlExt) | (1u << 22) | CtrlExtPhyPden | CtrlExtRoDis);
    static constexpr uint32_t txdctl[] = {Txdctl0, Txdctl1};
    for (uint32_t reg : txdctl) {
        uint32_t v = Read(reg) | (1u << 22);
        v = (v & ~0x003f0000u) | 0x01010000u; // WTHRESH=1, GRAN
        v = (v & ~0x0000003fu) | 0x0100001fu; // PTHRESH=31
        Write(reg, v);
    }
    Write(Tarc0, Read(Tarc0) | (1u << 23) | (1u << 24) | (1u << 26) | (1u << 27));
    uint32_t tarc1 = Read(Tarc1) | (1u << 24) | (1u << 26) | (1u << 30) | (1u << 28);
    Write(Tarc1, tarc1);
    Write(Rfctl, (Read(Rfctl) | RfctlNfsDisable) & ~RfctlExsten);
    Write(Pbeccsts, Read(Pbeccsts) | 0x00010000u);
    Write(Ctrl, Read(Ctrl) | CtrlMehe);
    // SPT/KBL Si errata: avoid TX data corruption and hangs.
    Write(Iosfpc, Read(Iosfpc) | 0x00010000u);
    Write(Tarc0, (Read(Tarc0) & ~0x30000000u) | 0x20000000u);
    Write(Fextnvm7, Read(Fextnvm7) | 0x4u);
    Write(Fextnvm9, Read(Fextnvm9) | 0x1800u);
    // I217/I219 packet loss: beacon duration 8 us.
    Write(Fextnvm4, (Read(Fextnvm4) & ~0x7u) | 0x7u);
}

// The link-dependent part of e1000_check_for_copper_link_ich8lan for SPT.
void LinkTuning()
{
    const uint32_t status = Read(Status);
    if (!(status & StatusLu)) return;
    const uint32_t speed = (status >> 6) & 3; // 0=10 1=100 2/3=1000
    const bool full = status & StatusFd;
    uint32_t tipg = Read(Tipg) & ~0x3ffu;
    uint16_t emi = 1;
    if (!full && speed == 0) { tipg |= 0xff; emi = 0; }
    else if (full && speed < 2) tipg |= 0xc;
    else tipg |= 0x08;
    Write(Tipg, tipg);
    if (!AcquireSwFlag()) return;
    uint16_t address = 0xb20c; // I217_RX_CONFIG via EMI
    (void)PhyAccessLocked(0x10, true, address);
    (void)PhyAccessLocked(0x11, true, emi);
    uint16_t value = 0;
    if (PhyAccessLocked(PllClockGate, false, value)) {
        value = static_cast<uint16_t>((value & ~0x7ffu) | (speed < 2 ? 0x3e8 : 0xfa));
        (void)PhyAccessLocked(PllClockGate, true, value);
    }
    if (speed >= 2) {
        if (PhyAccessLocked(HvPmCtrl, false, value)) {
            value |= 1u << 9; // K1_CLK_REQ
            (void)PhyAccessLocked(HvPmCtrl, true, value);
        }
        if (PhyAccessLocked(SptPtrGap, false, value) && ((value >> 2) & 0x3ff) < 0x18) {
            value = static_cast<uint16_t>((value & ~(0x3ffu << 2)) | (0x18u << 2));
            (void)PhyAccessLocked(SptPtrGap, true, value);
        }
    } else {
        value = 0xc023;
        (void)PhyAccessLocked(SptPtrGap, true, value);
    }
    ReleaseSwFlag();
    // FEXTNVM6 K1-off mirrors PCIEANACFG on SPT.
    uint32_t fextnvm6 = Read(Fextnvm6);
    if (Read(Pcieanacfg) & 0x80000000u) fextnvm6 |= 0x80000000u; else fextnvm6 &= ~0x80000000u;
    Write(Fextnvm6, fextnvm6);
}

bool Attach()
{
    // Never start DMA over a ring the firmware may still own.
    if ((Read(Rctl) & RxEnable) || (Read(Tctl) & TxEnable)) {
        Step("firmware left RX/TX enabled: stopping units");
        StopUnits();
    }
    Write(Imc, ~0u);
    (void)Read(Icr);
    if (!AcquireSwFlag()) { g_failure = "swflag busy"; return false; }
    uint32_t id = PhyIdLocked();
    ReleaseSwFlag();
    if (!id) {
        Step("PHY silent: recovering access (ULP/SMBus)");
        id = RecoverPhyAccess();
        if (!id) { g_failure = "PHY inaccessible"; return false; }
    }
    if (!ProgramMac(g_if.mac)) { g_failure = "RAR0 invalid (MAC not loaded)"; return false; }
    // e1000_power_up_phy_copper: firmware without a network stack may leave
    // the PHY powered down (BMCR.PDOWN).
    uint16_t bmcr = 0;
    if (PhyRead(0, bmcr) && (bmcr & (1u << 11))) {
        (void)PhyWrite(0, static_cast<uint16_t>(bmcr & ~(1u << 11)));
        Step("PHY powered up (BMCR.PDOWN cleared)");
    }
    for (uint32_t i = 0; i < MtaCount; ++i) Write(Mta + i * 4, 0);
    InitHardwareBits();

    if (!g_rxDma.descriptors && (!AllocateDma<RxDescriptor>(g_rxDma) || !AllocateDma<TxDescriptor>(g_txDma))) {
        g_failure = "DMA allocation"; return false;
    }
    g_rx = {}; g_tx = {};
    auto* rxd = reinterpret_cast<RxDescriptor*>(g_rxDma.descriptors.raw());
    auto* txd = reinterpret_cast<TxDescriptor*>(g_txDma.descriptors.raw());
    for (uint16_t i = 0; i < RingSize; ++i) {
        rxd[i].status = 0; rxd[i].errors = 0; rxd[i].length = 0;
        txd[i].status = DescriptorDone; txd[i].command = 0; txd[i].length = 0;
    }
    const uint64_t rxPhys = VmmVirtToPhys(KernelPageTable, g_rxDma.descriptors).raw();
    const uint64_t txPhys = VmmVirtToPhys(KernelPageTable, g_txDma.descriptors).raw();

    // Bus mastering: firmware already enabled it (cmd 0006); keep INTx off.
    PciConfigWrite16(g_device.bus, g_device.dev, g_device.fn, 0x04,
                     g_firmwareCommand | PciMemory | PciMaster | PciIntDisable);
    Write(Ctrl, Read(Ctrl) & ~CtrlMasterDisable);

    Write(Tdbal, uint32_t(txPhys)); Write(Tdbah, uint32_t(txPhys >> 32));
    Write(Tdlen, RingSize * sizeof(TxDescriptor)); Write(Tdh, 0); Write(Tdt, 0);
    Write(Tidv, 0); Write(Tadv, 0);
    Write(Txdctl1, Read(Txdctl0));
    Write(Tctl, TxEnable | TxPsp | TxRtlc | (0x0fu << 4) | (0x3fu << 12));

    Write(Rctl, 0);
    Write(Rdtr, 0); Write(Radv, 0);
    Write(Rdbal, uint32_t(rxPhys)); Write(Rdbah, uint32_t(rxPhys >> 32));
    Write(Rdlen, RingSize * sizeof(RxDescriptor)); Write(Rdh, 0); Write(Rdt, 0);
    Write(Rxcsum, 0);
    Write(Rctl, RxEnable | RxBroadcast | RxStripFcs); // 2048-byte buffers
    __asm__ volatile("sfence" ::: "memory");
    Write(Rdt, RingSize - 1);

    // Link: firmware autonegotiation stays in charge; set link up.
    Write(Ctrl, (Read(Ctrl) | CtrlSlu) & ~(CtrlFrcSpd | CtrlFrcDpx));
    Flush();

    g_if.transmit = Transmit;
    g_if.tryTransmit = TryTransmit;
    g_if.poll = Poll;
    g_if.snapshot = Snapshot;
    g_linkUp = Read(Status) & StatusLu;
    if (!g_registered) {
        if (!NetRegisterIf(&g_if)) { g_failure = "NetRegisterIf (table full)"; return false; }
        g_registered = true;
    }
    Step("rings programmed, interface registered");
    const auto& mac = g_if.mac.b;
    KPrintf("i219: ready PHY=%08x MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%s\n", id,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], g_linkUp ? "up" : "down");
    return true;
}

void RestartAutoneg()
{
    uint16_t bmcr = 0;
    if (PhyRead(0, bmcr) && PhyWrite(0, static_cast<uint16_t>(bmcr | (1u << 12) | (1u << 9))))
        Step("autonegotiation restarted");
    else Step("autonegotiation restart failed");
}

void Worker(void*)
{
    g_threadContext = true;
    bool tuned = false;
    for (;;) {
        const uint32_t requested = __atomic_load_n(&g_requested, __ATOMIC_ACQUIRE);
        if (!g_failure && requested > g_reached && !g_registered) {
            if (requested >= 2) ResetHardware();
            if (Attach()) g_reached = requested;
            UsbStatusChanged();
        }
        if (__atomic_exchange_n(&g_autonegRequested, false, __ATOMIC_ACQ_REL)) { RestartAutoneg(); tuned = false; }
        if (g_registered) {
            const bool up = Read(Status) & StatusLu;
            if (up && !tuned) { LinkTuning(); tuned = true; Step("link tuning applied"); UsbStatusChanged(); }
            if (!up) tuned = false;
        }
        SchedulerSleepMs(250);
    }
}

bool Marker(const char* path)
{
    if (auto* file = VfsOpen(path)) { VfsClose(file); return true; }
    return false;
}

bool Equal(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }

uint32_t Command(const char* args, char* out, uint32_t cap)
{
    Text t{out, cap};
    if (!g_regs) { t.s("not mapped"); return t.n; }
    if (!*args || Equal(args, "status")) return StatusText(out, cap);
    if (Equal(args, "phy")) { DescribePhy(t); return t.n; }
    if (Equal(args, "attach") || Equal(args, "reset")) {
        const uint32_t stage = Equal(args, "reset") ? 2 : 1;
        if (g_registered) { t.s("already attached"); return t.n; }
        if (stage > g_requested) __atomic_store_n(&g_requested, stage, __ATOMIC_RELEASE);
        t.s("requested "); t.s(StageName(stage)); return t.n;
    }
    if (Equal(args, "autoneg")) { __atomic_store_n(&g_autonegRequested, true, __ATOMIC_RELEASE); t.s("queued"); return t.n; }
    if (Equal(args, "stats")) {
        t.s("CRCERRS="); t.u(Read(Crcerrs)); t.s(" MPC="); t.u(Read(Mpc));
        t.s(" GPRC="); t.u(Read(Gprc)); t.s(" GPTC="); t.u(Read(Gptc)); t.s(" (clear on read)");
        return t.n;
    }
    t.s("usage: bridge hardware nic [status|phy|stats|attach|reset|autoneg]");
    return t.n;
}

// Pre-reset quiesce (e1000e shutdown order): mask interrupts, stop receive
// and transmit, then drop bus mastering so no descriptor DMA is in flight
// when the platform reset lands. Bounded: register writes only, no waits
// beyond one short flush spin.
void QuiesceForReset()
{
    if (!g_regs) return;
    Write(Imc, ~0u);
    Write(Rctl, Read(Rctl) & ~RxEnable);
    Write(Tctl, Read(Tctl) & ~TxEnable);
    Flush();
    for (volatile uint32_t spin = 0; spin < 20000; ++spin) __asm__ volatile("pause");
    (void)PciQuiesceFunctionForReset(g_device.bus, g_device.dev, g_device.fn);
}

int ModuleInit()
{
    if (!PciFindDevice(0x8086, 0x15b8, g_device)) return -1;
    const uint32_t raw = g_device.bar[0];
    if (PciBarIsIo(raw)) return -1;
    g_bar = PciBarMemBase32(raw);
    if (PciBarIs64(raw)) g_bar |= uint64_t(g_device.bar[1]) << 32;
    if (!g_bar || g_bar % PageSize) return -1;
    g_firmwareCommand = PciConfigRead16(g_device.bus, g_device.dev, g_device.fn, 0x04);
    if (!(g_firmwareCommand & PciMemory)) {
        // Firmware left memory decoding off: enable it (no bus mastering yet).
        PciConfigWrite16(g_device.bus, g_device.dev, g_device.fn, 0x04, g_firmwareCommand | PciMemory);
    }
    if (!MapMmio(g_bar)) { g_failure = "MMIO map"; return -1; }
    (void)BootStatusAddProvider(StatusText);
    DebugNicCommandSetHandler(Command);
    (void)PlatformPowerAddQuiesce(QuiesceForReset);

    static char probe[4096];
    Text t{probe, sizeof(probe)};
    t.s("I219 probe bar0="); t.x(g_bar, 8); t.s(" cmd="); t.x(g_firmwareCommand, 4);
    t.s(" pmcsr="); t.x(PciConfigRead16(g_device.bus, g_device.dev, g_device.fn, 0xcc), 4);
    t.s(" cfg_e4="); t.x(PciConfigRead32(g_device.bus, g_device.dev, g_device.fn, 0xe4), 8);
    t.s(" subsys="); t.x(PciConfigRead32(g_device.bus, g_device.dev, g_device.fn, 0x2c), 8); t.nl();
    DescribeRegisters(t);
    DescribePhy(t);
    probe[t.n < sizeof(probe) ? t.n : sizeof(probe) - 1] = 0;
    for (char* line = probe; *line;) {
        char* end = line;
        while (*end && *end != '\n') ++end;
        const bool more = *end == '\n';
        *end = 0;
        KPrintf("%s\n", line);
        line = more ? end + 1 : end;
    }
    (void)ProgramMac(g_if.mac);

    uint32_t stage = 0;
    if (Marker("/etc/i219-attach")) stage = 1;
    if (Marker("/etc/i219-reset")) stage = 2;
    g_requested = stage;
    if (!KernelWorkerStart("i219", Worker, nullptr)) Step("worker queue full: stages unavailable");
    KPrintf("i219: probed (stage %s requested by marker)\n", StageName(stage));
    return 0;
}

void Stop()
{
    // The interface stays registered for the kernel's lifetime; nothing to do.
}

}

DECLARE_MODULE("i219", ModuleInit, Stop, "Intel I219-V (PCH SPT) Ethernet, staged");
