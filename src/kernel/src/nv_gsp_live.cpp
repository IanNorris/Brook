// GSP-RM M1 live path (BROOK_NV_GSP): the gated bridge verbs that run the
// host-tested sequences in nv_gsp/nv_gsp_p3.h on the real card.
//
//   bridge hardware nv-gsp status    read-only: marker, firmware hashes, VT-d,
//                                    PCI command, preflight registers, latches
//   bridge hardware nv-gsp p3        P3: FWSEC-FRTS alone (once per boot)
//   bridge hardware nv-gsp fini      P3b: FWSEC-SB, then booter_unload (once per boot)
//   bridge hardware nv-gsp trace     the BAR0 access log of the last p3/fini
//   bridge hardware nv-gsp p4 [keep-display]   GSP-RM to INIT_DONE; with keep-display the
//                                    nv-aux lock is held across the boot (no compositor
//                                    BAR1 traffic) and BAR1 is identity-mapped before release
//   bridge hardware nv-gsp static [off]        parsed GET_GSP_STATIC_INFO + 1536 raw bytes at off
//   bridge hardware nv-gsp bar1 map [2m]|unmap|status   interim display handover (nv_gsp_bar1.h)
//   bridge hardware nv-gsp p5        M2: RM client/device/channel, NOP + semaphore (nv_gsp_m2.h)
// With GSP-RM up, a pre-reset quiesce hook runs the RM fini (UNLOADING_GUEST_DRIVER,
// FWSEC-SB, booter_unload) before nv-disp's quiesce and the FLR, so WPR2 is cleared.
//
// Gates: built only with BROOK_NV_GSP; every verb needs /etc/nv-gsp = "1\n".
// p3 and fini refuse before the first GPU write unless the firmware set named
// by /etc/nv-gsp-firmware matches the pinned sizes and SHA-256 (ChipOps), the
// VBIOS FWSEC image read from PROM matches the one the Linux reference ran,
// and the RunP3 checks pass. Then (in order) the DMA buffer is flushed, PCI bus
// mastering is enabled (command 0x0003 -> 0x0007) and the falcon runs. A
// failure is captured (CPUCTL, mailboxes, 0x1438, WPR2, TRACEPC) and never
// retried in the same boot. The direct nv-disp path and the GOP are not
// touched: only GSP/SEC2 falcon registers are written.
#include "nv_gsp_live.h"
#include "nv_gsp/nv_gsp_p3.h"
#include "nv_gsp/nv_gsp_rm.h"
#include "nv_gsp/nv_gsp_bar1.h"
#include "nv_gsp/nv_gsp_m2.h"
#include "nv_gsp/nv_gsp_m3.h"
#include "nv_gsp/nv_gsp_ce_present.h"
#include "nv_disp_live.h"
#include "platform_power.h"
#include "scheduler.h"
#include "serial.h"
#include "pci.h"
#include "acpi.h"
#include "boot_resource_map.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "net.h"
#include "kprintf.h"
#include "tsc_clock.h"
#include "apic.h"
#include "string.h"
#include "vfs.h"
#include "../../shared/update/payload.h"

extern "C" void NvGpuMarkEnginesUsed();
namespace brook {
namespace {
using namespace nvgsp;

// ------------------------------------------------------------------ reply
bool g_outQuiet = false;   // the boot-time autostart runs verbs with no bridge client
struct Out {
    char t[6000]; uint32_t n = 0;
    void raw(const char* s) { while (*s && n + 1 < sizeof(t)) t[n++] = *s++; t[n] = 0; }
    void dec(uint64_t v) { char b[24]; int i = 0; do { b[i++] = char('0' + v % 10); v /= 10; } while (v);
        while (i && n + 1 < sizeof(t)) t[n++] = b[--i]; t[n] = 0; }
    void hex(uint64_t v) { raw("\"0x"); char b[17]; int i = 0; do { b[i++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
        while (i && n + 1 < sizeof(t)) t[n++] = b[--i]; t[n] = 0; raw("\""); }
    void str(const char* s) { raw("\""); raw(s); raw("\""); }
    void k(const char* key) { if (n && t[n - 1] != '{' && t[n - 1] != '[') raw(","); raw("\""); raw(key); raw("\":"); }
    void kh(const char* key, uint64_t v) { k(key); hex(v); }
    void kd(const char* key, uint64_t v) { k(key); dec(v); }
    void ks(const char* key, const char* s) { k(key); str(s); }
    void kb(const char* key, bool b) { k(key); raw(b ? "true" : "false"); }
    void send() { raw("}\n"); if (!g_outQuiet) DebugChannelSend(t); }
};

uint64_t NowUs() { return TscClockAvailable() ? TscClockReadNs() / 1000 : ApicTickCount() * 1000; }

// ------------------------------------------------------------- BAR0 bus
// Single-page UC mappings that are never remapped (the caller may migrate
// between CPUs; invlpg is local), plus a contiguous UC alias for the PROM.
struct Bar0Bus {
    static constexpr uint32_t kSlots = 48;
    uint64_t bar0 = 0;
    VirtualAddress va[kSlots]{};
    uint64_t page[kSlots]{};
    uint32_t used = 0;
    bool failed = false;
    const char* phase = "";
    const char* step = "";
    // access log for `nv-gsp trace`: consecutive identical reads collapse
    struct Rec { char op; uint32_t off, val, count; const char* phase; const char* step; };
    static constexpr uint32_t kLog = 1024;
    Rec log[kLog];
    uint32_t logN = 0, dropped = 0;
    bool rec = false;
    void Note(char op, uint32_t off, uint32_t v) {
        if (!rec) return;
        // PIO bursts (IMEMD/DMEMD ports) collapse too: count + last value
        const uint32_t port = off & 0xfff;
        if (op == 'W' && (port == 0x184 || port == 0x1c4) && logN && log[logN - 1].op == 'W' && log[logN - 1].off == off) {
            ++log[logN - 1].count; log[logN - 1].val = v; return;
        }
        if (op == 'R' && logN && log[logN - 1].op == 'R' && log[logN - 1].off == off && log[logN - 1].val == v) {
            ++log[logN - 1].count; return;
        }
        if (logN >= kLog) { ++dropped; return; }
        log[logN++] = Rec{op, off, v, 1, phase, step};
    }
    // The PRAMIN window (BAR0 0x700000, 1 MiB) gets one contiguous UC alias: page tables and
    // the CE presenter's back-buffer remaps touch far more of it than the single-page slots hold.
    VirtualAddress pramin{}; bool praminFailed = false;
    volatile uint32_t* MapPramin(uint32_t off) {
        if (!pramin.raw() && !praminFailed) {
            VirtualAddress v = VmmAllocPages(256, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
            praminFailed = !v;
            for (uint32_t i = 0; v && i < 256; ++i) {
                const VirtualAddress pv(v.raw() + i * 4096ull);
                const auto leaf = VmmDetachPage(KernelPageTable, pv); if (leaf) PmmFreePage(VmmLeafPhysical(leaf));
                __asm__ volatile("invlpg (%0)" ::"r"(pv.raw()) : "memory");
                if (!VmmMapPage(KernelPageTable, pv, PhysicalAddress(bar0 + 0x700000 + i * 4096ull),
                                VMM_WRITABLE | VMM_NO_EXEC | VMM_CACHE_DISABLE | VMM_WRITE_THRU, MemTag::Device, KernelPid)) {
                    praminFailed = true; break; }
            }
            if (!praminFailed) pramin = v;
        }
        if (!pramin.raw()) { failed = true; return nullptr; }
        return reinterpret_cast<volatile uint32_t*>(pramin.raw() + (off - 0x700000));
    }
    volatile uint32_t* Map(uint32_t off) {
        if (off >= 0x700000 && off < 0x800000 && bar0) return MapPramin(off);
        const uint64_t p = (bar0 + off) & ~0xfffull;
        for (uint32_t i = 0; i < used; ++i) if (page[i] == p) return reinterpret_cast<volatile uint32_t*>(va[i].raw() + (off & 0xfff));
        if (used >= kSlots) { failed = true; return nullptr; }
        VirtualAddress v = VmmAllocPages(1, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
        if (!v) { failed = true; return nullptr; }
        const auto leaf = VmmDetachPage(KernelPageTable, v); if (leaf) PmmFreePage(VmmLeafPhysical(leaf));
        __asm__ volatile("invlpg (%0)" ::"r"(v.raw()) : "memory");
        if (!VmmMapPage(KernelPageTable, v, PhysicalAddress(p), VMM_WRITABLE | VMM_NO_EXEC | VMM_CACHE_DISABLE | VMM_WRITE_THRU,
                        MemTag::Device, KernelPid)) { failed = true; return nullptr; }
        va[used] = v; page[used] = p; ++used;
        return reinterpret_cast<volatile uint32_t*>(v.raw() + (off & 0xfff));
    }
    uint32_t rd32(uint32_t off) { volatile uint32_t* r = Map(off); const uint32_t v = r ? *r : 0xffffffffu; Note('R', off, v); return v; }
    void wr32(uint32_t off, uint32_t v) { volatile uint32_t* r = Map(off); Note('W', off, v); if (r) *r = v; }
    void udelay(uint32_t us) { const uint64_t end = NowUs() + us; while (NowUs() < end) __asm__ volatile("pause"); }
    uint64_t now_us() { return NowUs(); }
    void mark(const char* p, const char* s) { phase = p; step = s; }
};
Bar0Bus g_bus;   // .bss

// ---------------------------------------------------------- live state
struct Gpu { uint8_t bus = 0, dev = 0, fn = 0; uint64_t bar0 = 0; bool ok = false; };
volatile uint32_t g_busy = 0;
bool g_ceActiveFlag = false;   // mirrors g_cePresentState.active for Status (declared later)
struct AutostartState { const char* status = "not run"; uint64_t start_ms = 0, p4_us = 0, ce_us = 0; } g_autostart;
bool PathExists(const char* path) { auto* f = VfsOpen(path, VFS_O_STREAM); if (!f) return false; VfsRelease(f); return true; }
bool g_p3Done = false, g_finiDone = false, g_p4Done = false;   // latched at arm (the first write)
bool g_rmRunning = false;                    // GSP-RM reached INIT_DONE this boot
DmaBuf g_dma;                                 // 16 contiguous pages, kept for the boot
constexpr uint32_t kDmaPages = 16;
uint8_t* g_rom = nullptr; uint32_t g_romBytes = 0;     // PROM copy (VMALLOC)
constexpr uint32_t kRomMax = 512 * 1024;
uint8_t g_romSha[32]{}; bool g_romRead = false;
uint16_t g_cmdBefore = 0, g_cmdAfter = 0;
uint64_t g_runUs = 0;
// The FWSEC descriptor + ucode span of this card's VBIOS that the 25 Nov
// Linux reference ran (vbios-nouveau.rom 0x3f500..0x4964c, 41292 B), and the
// whole 333824-byte image (sha256 0f94b4a7...). FWSEC must match.
constexpr uint8_t kFwsecSpanSha[32] = {
    0xba,0x96,0xbe,0xce,0xdc,0x9a,0x48,0x23,0xd7,0xf9,0x34,0xc6,0xc0,0x89,0xf3,0x93,
    0x61,0x6e,0x3a,0x46,0x28,0xba,0x4c,0xd9,0xe4,0x51,0xef,0x3f,0xd1,0xd0,0x37,0x91};
constexpr uint8_t kRomSha[32] = {
    0x0f,0x94,0xb4,0xa7,0xc7,0x46,0xdf,0x84,0x3b,0xa4,0x7e,0x6d,0xfe,0x13,0x44,0x3f,
    0xf6,0xf6,0x0a,0xd2,0x46,0xd5,0x7f,0xa0,0x98,0x5c,0x2d,0x8d,0xc3,0xe0,0xba,0x4c};

bool FindGpu(Gpu& g) {
    for (uint32_t i = 0; i < PciBootInventoryCount(); ++i) {
        PciBootEntry e{};
        if (PciBootInventoryRead(i, &e) && e.device.vendorId == 0x10de && e.device.classCode == 0x03 && e.device.fn == 0) {
            g.bus = e.device.bus; g.dev = e.device.dev; g.fn = e.device.fn;
            const uint32_t b0 = PciConfigRead32(g.bus, g.dev, g.fn, 0x10);
            const uint16_t cmd = PciConfigRead16(g.bus, g.dev, g.fn, 4);
            g.bar0 = b0 & ~0xfull;
            BootResourceClassification c{};
            constexpr uint32_t allowed = (1u << uint32_t(MemoryType::Reserved)) | (1u << uint32_t(MemoryType::Mmio));
            g.ok = !(b0 & 1) && g.bar0 && (cmd & 2) && AcpiClassifyBootRange(g.bar0, 16u << 20, c) && !(c.types & ~allowed);
            return g.ok;
        }
    }
    return false;
}

bool MarkerSet(const char* path) {
    auto* f = VfsOpen(path, VFS_O_STREAM); if (!f) return false;
    VnodeStat st{}; uint64_t off = 0; char t[2]{};
    const bool ok = !VfsStat(f, &st) && !st.isDir && st.size == 2 && VfsRead(f, t, 2, &off) == 2 && t[0] == '1' && t[1] == '\n';
    VfsRelease(f); return ok;
}

// ------------------------------------------------------------ firmware
// /etc/nv-gsp-firmware: one line, the firmware root (".../lib/firmware"):
// "/boot/nvgsp" when the set rides in the signed initrd, or a /nix/store
// path once the desktop store carries nix/nv-gsp-firmware.nix.
struct FwState {
    char root[160]{};
    bool record = false;
    uint8_t ok[kFwCount]{};          // 1 = size + sha256 match
    uint64_t bytes[kFwCount]{};
    bool all = false;
};
FwState g_fw;
bool ReadRecord(char* out, uint32_t cap) {
    auto* f = VfsOpen("/etc/nv-gsp-firmware", VFS_O_STREAM); if (!f) return false;
    uint64_t off = 0; char t[160]{};
    const int n = VfsRead(f, t, sizeof(t) - 1, &off); VfsRelease(f);
    if (n < 3 || t[0] != '/') return false;
    uint32_t i = 0;
    for (; i < uint32_t(n) && t[i] != '\n'; ++i) {
        const char c = t[i];
        const bool okc = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.' ||
                         c == '-' || c == '_' || c == '+';
        if (!okc || i + 1 >= cap) return false;
        if (c == '.' && i && t[i - 1] == '.') return false;   // no ".."
        out[i] = c;
    }
    if (i == uint32_t(n) || i + 1 != uint32_t(n)) return false;   // exactly one line
    out[i] = 0; return true;
}
void Join(char* out, uint32_t cap, const char* a, const char* b) {
    uint32_t n = 0; for (const char* p = a; *p && n + 1 < cap; ++p) out[n++] = *p;
    if (n + 1 < cap) out[n++] = '/';
    for (const char* p = b; *p && n + 1 < cap; ++p) out[n++] = *p;
    out[n] = 0;
}
// Stream-hash one file; copy it into `dst` when given (cap >= bytes).
bool CheckFile(const ChipOps& c, uint32_t role, uint8_t* dst, uint32_t cap) {
    char path[256]; Join(path, sizeof(path), g_fw.root, c.fw[role].path);
    auto* f = VfsOpen(path, VFS_O_STREAM); if (!f) return false;
    VnodeStat st{}; bool ok = !VfsStat(f, &st) && !st.isDir && st.size == c.fw[role].bytes;
    g_fw.bytes[role] = st.size;
    if (ok && dst && cap < c.fw[role].bytes) ok = false;
    static uint8_t chunk[64 * 1024];
    BrookSha256 h; BrookShaInit(&h);
    uint64_t off = 0, total = 0;
    while (ok) {
        const int n = VfsRead(f, chunk, sizeof(chunk), &off);
        if (n < 0) { ok = false; break; }
        if (!n) break;
        if (total + uint64_t(n) > c.fw[role].bytes) { ok = false; break; }
        if (dst) memcpy(dst + total, chunk, uint32_t(n));
        BrookShaAdd(&h, chunk, BrookUpdateSize(n));
        total += uint64_t(n);
    }
    VfsRelease(f);
    if (!ok || total != c.fw[role].bytes) return false;
    uint8_t d[32]; BrookShaFinish(&h, d);
    return BrookUpdateEqual(d, c.fw[role].sha256, 32);
}
bool VerifyFirmware(const ChipOps& c) {
    g_fw = FwState{};
    g_fw.record = ReadRecord(g_fw.root, sizeof(g_fw.root));
    if (!g_fw.record) return false;
    bool all = true;
    for (uint32_t r = 0; r < kFwCount; ++r) { g_fw.ok[r] = CheckFile(c, r, nullptr, 0) ? 1 : 0; all = all && g_fw.ok[r]; }
    g_fw.all = all;
    return all;
}
uint8_t* LoadFile(const ChipOps& c, uint32_t role) {
    const uint32_t pages = (c.fw[role].bytes + 4095) / 4096;
    VirtualAddress v = VmmAllocPages(pages, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
    if (!v) return nullptr;
    uint8_t* p = reinterpret_cast<uint8_t*>(v.raw());
    if (!CheckFile(c, role, p, pages * 4096)) { VmmFreePages(v, pages); return nullptr; }
    return p;
}

// --------------------------------------------------------------- VBIOS
// The full ROM from the PROM window (BAR0 0x300000, read-only, contiguous UC
// alias), then the image walk; FWSEC must equal the reference image.
bool ReadRom(const Gpu& g) {
    if (g_romRead) return true;
    constexpr uint32_t pages = kRomMax / 4096;
    VirtualAddress alias = VmmAllocPages(pages, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
    VirtualAddress copy = VmmAllocPages(pages, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
    if (!alias || !copy) return false;
    for (uint32_t i = 0; i < pages; ++i) {
        const VirtualAddress v(alias.raw() + i * 4096ull);
        const PhysicalAddress old = VmmVirtToPhys(KernelPageTable, v);
        VmmUnmapPage(KernelPageTable, v); if (old) PmmFreePage(old);
        if (!VmmMapPage(KernelPageTable, v, PhysicalAddress(g.bar0 + 0x300000 + i * 4096ull),
                        VMM_NO_EXEC | VMM_CACHE_DISABLE | VMM_WRITE_THRU, MemTag::Device, KernelPid)) return false;
    }
    const volatile uint32_t* src = reinterpret_cast<const volatile uint32_t*>(alias.raw());
    uint8_t* dst = reinterpret_cast<uint8_t*>(copy.raw());
    for (uint32_t i = 0; i < kRomMax / 4; ++i) { const uint32_t w = src[i]; memcpy(dst + i * 4, &w, 4); }
    if (dst[0] != 0x55 || dst[1] != 0xaa) return false;
    // Extent = end of the last image of the walk.
    Vbios vb;
    if (ParseVbios(Span{dst, kRomMax}, &vb) != FwStatus::Ok || !vb.count) return false;
    const VbiosImage& last = vb.images[vb.count - 1];
    g_romBytes = last.base + last.size;
    if (g_romBytes > kRomMax || g_romBytes < 0x10000) return false;
    g_rom = dst;
    BrookSha256 h; BrookShaInit(&h); BrookShaAdd(&h, dst, g_romBytes); BrookShaFinish(&h, g_romSha);
    g_romRead = true;
    return true;
}
bool FwsecMatches(const FwsecImage& f) {
    const uint8_t* a = g_rom + f.desc_addr;
    const uint8_t* b = f.ucode.p + f.ucode.n;
    if (a < g_rom || b > g_rom + g_romBytes || b <= a) return false;
    uint8_t d[32]; BrookSha256 h; BrookShaInit(&h); BrookShaAdd(&h, a, BrookUpdateSize(b - a)); BrookShaFinish(&h, d);
    return BrookUpdateEqual(d, kFwsecSpanSha, 32);
}

// --------------------------------------------------------------- VT-d
// Read-only: each DRHD's global status (0x1c) bit 31 = translation enabled.
// The falcon DMAs to physical addresses, so translation must be off.
struct Vtd { uint32_t units = 0, enabled = 0, unread = 0; bool table = false; };
Vtd ReadVtd() {
    Vtd v;
    static uint8_t dmar[4096];
    uint32_t bytes = 0;
    if (!AcpiUniqueDmarCopy(dmar, sizeof(dmar), bytes) || bytes < 48) return v;
    v.table = true;
    for (uint32_t o = 48; o + 4 <= bytes;) {
        uint16_t type, len; memcpy(&type, dmar + o, 2); memcpy(&len, dmar + o + 2, 2);
        if (len < 4 || o + len > bytes) break;
        if (type == 0 && len >= 16) {
            uint64_t base; memcpy(&base, dmar + o + 8, 8);
            ++v.units;
            BootResourceClassification c{};
            constexpr uint32_t allowed = (1u << uint32_t(MemoryType::Reserved)) | (1u << uint32_t(MemoryType::Mmio));
            if (!base || (base & 0xfff) || !AcpiClassifyBootRange(base, 4096, c) || !c.types || (c.types & ~allowed)) { ++v.unread; }
            else {
                VirtualAddress w = VmmAllocPages(1, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
                if (!w) { ++v.unread; }
                else {
                    const auto leaf = VmmDetachPage(KernelPageTable, w); if (leaf) PmmFreePage(VmmLeafPhysical(leaf));
                    __asm__ volatile("invlpg (%0)" ::"r"(w.raw()) : "memory");
                    if (VmmMapPage(KernelPageTable, w, PhysicalAddress(base), VMM_NO_EXEC | VMM_CACHE_DISABLE | VMM_WRITE_THRU,
                                   MemTag::Device, KernelPid)) {
                        const uint32_t gsts = reinterpret_cast<volatile uint32_t*>(w.raw())[0x1c / 4];
                        if (gsts & 0x80000000u) ++v.enabled;
                        (void)VmmDetachPage(KernelPageTable, w);
                        __asm__ volatile("invlpg (%0)" ::"r"(w.raw()) : "memory");
                    } else ++v.unread;
                }
            }
        }
        o += len;
    }
    return v;
}

// ---------------------------------------------------------------- DMA
bool EnsureDma() {
    if (g_dma.cpu) return true;
    const PhysicalAddress p = PmmAllocPages(kDmaPages, MemTag::Device, KernelPid);
    if (!p) return false;
    // FBIF PHYS_SYS_NCOH: physical addresses; keep well inside the falcon's reach.
    if (p.raw() + kDmaPages * 4096ull > (1ull << 39)) return false;   // leaked on purpose (never)
    g_dma.cpu = reinterpret_cast<uint8_t*>(PhysToVirt(p).raw());
    g_dma.bus = p.raw();
    g_dma.bytes = kDmaPages * 4096;
    memset(g_dma.cpu, 0, g_dma.bytes);
    return true;
}
// Non-coherent DMA: write the patched image back to memory before the falcon reads it.
void FlushDma() {
    for (size_t o = 0; o < g_dma.bytes; o += 64) __asm__ volatile("clflush (%0)" ::"r"(g_dma.cpu + o) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

// ----------------------------------------------------------- reporting
void Diag(Out& o, const FalconDiag& d) {
    o.k("diag"); o.raw("{");
    o.kh("cpuctl", d.cpuctl); o.kh("mbox0", d.mbox0); o.kh("mbox1", d.mbox1); o.kh("sctl", d.sctl);
    o.kh("dmactl", d.dmactl); o.kh("hwcfg", d.hwcfg); o.kh("frts_err_1438", d.frts_err); o.kh("sb_err_1454", d.sb_err);
    o.kh("wpr2_lo", d.wpr2_lo); o.kh("wpr2_hi", d.wpr2_hi); o.kh("riscv_cpuctl", d.riscv_cpuctl); o.kh("tidx", d.tidx);
    o.k("tracepc"); o.raw("[");
    for (uint32_t i = 0; i < d.trace_n; ++i) { if (i) o.raw(","); o.hex(d.trace[i]); }
    o.raw("]}");
}
void Pre(Out& o, const Preflight& p) {
    o.k("pre"); o.raw("{");
    o.kh("boot0", p.boot0); o.kh("devinit", p.devinit); o.kh("devinit_scratch", p.devinit_scratch);
    o.kh("vga_625f04", p.vga_ws); o.kh("fb_100ce0", p.fb_size_reg); o.kh("wpr2_lo", p.wpr2_lo); o.kh("wpr2_hi", p.wpr2_hi);
    o.kh("gsp_cpuctl", p.gsp_cpuctl); o.kh("sec2_cpuctl", p.sec2_cpuctl); o.kh("riscv_cpuctl", p.gsp_riscv_cpuctl);
    o.ks("status", BootStatusName(p.status));
    o.raw("}");
}
void Layout(Out& o, const FbLayout& L) {
    o.k("layout"); o.raw("{");
    o.kh("fb", L.fb_size); o.kh("vga", L.vga_workspace.addr); o.kh("frts", L.frts.addr); o.kh("frts_size", L.frts.size);
    o.kh("boot", L.boot.addr); o.kh("elf", L.elf.addr); o.kh("heap", L.heap.addr); o.kh("wpr2", L.wpr2.addr);
    o.kh("non_wpr_heap", L.non_wpr_heap.addr); o.kd("reserved_mib", L.reserved_top >> 20);
    o.kh("expect_wpr2_lo", uint32_t((L.frts.addr >> 12) << 4));
    o.kh("expect_wpr2_hi", uint32_t(((L.frts.end() - 0x20000) >> 12) << 4));
    o.raw("}");
}
void Firmware(Out& o, const ChipOps& c) {
    o.k("firmware"); o.raw("{"); o.kb("record", g_fw.record); o.ks("root", g_fw.root); o.kb("all_ok", g_fw.all);
    for (uint32_t r = 0; r < kFwCount; ++r) o.kb(FwRoleName(r), g_fw.ok[r]);
    o.ks("version", c.fw_version); o.raw("}");
}

// Common front: marker, GPU, firmware, VBIOS, VT-d, DMA. Fills P3Inputs.
struct Front { Gpu g; const ChipOps* chip = nullptr; P3Inputs in; Vtd vtd; bool fwsec_ok = false; const char* refuse = nullptr; };
uint8_t* g_acr = nullptr;
bool Prepare(Front& f, bool needUnload, uint8_t** unload) {
    if (!FindGpu(f.g)) { f.refuse = "no NVIDIA display function with BAR0 decode"; return false; }
    g_bus.bar0 = f.g.bar0;
    f.chip = ChipForBoot0(g_bus.rd32(0));
    if (!f.chip) { f.refuse = "unsupported chip"; return false; }
    if (!VerifyFirmware(*f.chip)) { f.refuse = "firmware set missing or hash mismatch"; return false; }
    if (!g_acr && !(g_acr = LoadFile(*f.chip, kFwAcrBl))) { f.refuse = "acr bl load failed"; return false; }
    if (needUnload && !(*unload = LoadFile(*f.chip, kFwBooterUnload))) { f.refuse = "booter_unload load failed"; return false; }
    if (!ReadRom(f.g)) { f.refuse = "PROM read or image walk failed"; return false; }
    {
        Vbios vb; FwsecImage fw;
        if (ParseVbios(Span{g_rom, g_romBytes}, &vb) != FwStatus::Ok || ParseFwsec(vb, &fw) != FwStatus::Ok) {
            f.refuse = "no FWSEC in the PROM image"; return false;
        }
        f.fwsec_ok = FwsecMatches(fw);
        if (!f.fwsec_ok) { f.refuse = "FWSEC image differs from the reference VBIOS"; return false; }
    }
    f.vtd = ReadVtd();
    if (f.vtd.enabled) { f.refuse = "VT-d translation enabled"; return false; }
    if (!EnsureDma()) { f.refuse = "no low contiguous DMA buffer"; return false; }
    f.in.vbios = Span{g_rom, g_romBytes};
    f.in.acr_bl = Span{g_acr, f.chip->fw[kFwAcrBl].bytes};
    // Layout below FRTS needs the ELF/bootloader sizes; P3 itself uses only FRTS.
    f.in.elf_size = 28528288;   // gsp-570.144 .fwimage (pinned file: the M0 parser reads it in P4)
    f.in.boot_size = 0x1000;
    return true;
}
void FrontOut(Out& o, const Front& f) {
    o.kh("bar0", f.g.bar0);
    if (f.chip) { o.ks("chip", f.chip->name); Firmware(o, *f.chip); }
    o.kb("rom_read", g_romRead); o.kd("rom_bytes", g_romBytes);
    o.kb("rom_matches_reference", g_romRead && BrookUpdateEqual(g_romSha, kRomSha, 32));
    o.kb("fwsec_matches_reference", f.fwsec_ok);
    o.k("vtd"); o.raw("{"); o.kb("dmar", f.vtd.table); o.kd("units", f.vtd.units); o.kd("translating", f.vtd.enabled);
    o.kd("unread", f.vtd.unread); o.raw("}");
    if (g_dma.cpu) o.kh("dma", g_dma.bus);
}
// Arm: after every check, before the first write. Latch, flush, bus master.
bool Arm(const Gpu& g, bool* latch) {
    *latch = true;
    NvGpuMarkEnginesUsed();   // the pre-reset GPU reset adds the PMC engine reset (debug_hardware.cpp)
    FlushDma();
    g_cmdBefore = PciConfigRead16(g.bus, g.dev, g.fn, 4);
    if (!(g_cmdBefore & 2)) return false;
    if (!(g_cmdBefore & 4)) PciConfigWrite16(g.bus, g.dev, g.fn, 4, uint16_t(g_cmdBefore | 4));
    g_cmdAfter = PciConfigRead16(g.bus, g.dev, g.fn, 4);
    g_runUs = NowUs();
    return (g_cmdAfter & 6) == 6;
}

void Status() {
    Out o; o.raw("{\"nv_gsp\":\"status\"");
    Front f;
    o.kb("marker", MarkerSet("/etc/nv-gsp"));
    o.kb("p3_done", g_p3Done); o.kb("fini_done", g_finiDone);
    o.ks("autostart", g_autostart.status); o.kb("rm_running", g_rmRunning); o.kb("ce_present", g_ceActiveFlag);
    if (FindGpu(f.g)) {
        g_bus.bar0 = f.g.bar0;
        o.kh("pci_cmd", PciConfigRead16(f.g.bus, f.g.dev, f.g.fn, 4));
        f.chip = ChipForBoot0(g_bus.rd32(0));
        if (f.chip) {
            (void)VerifyFirmware(*f.chip);
            const Preflight p = RunPreflight(g_bus, *f.chip);   // reads only
            Pre(o, p);
            o.kh("frts_err_1438", g_bus.rd32(f.chip->reg_frts_err)); o.kh("sb_err_1454", g_bus.rd32(f.chip->reg_sb_err));
            o.kh("sec2_boot_1180f8", g_bus.rd32(f.chip->reg_booter_boot_status));
        }
        f.vtd = ReadVtd();
    }
    FrontOut(o, f);
    o.send();
}

void P3() {
    Out o; o.raw("{\"nv_gsp\":\"p3\"");
    if (g_p3Done || g_finiDone) { o.ks("refused", "already ran this boot (no retry)"); o.send(); return; }
    Front f;
    if (!Prepare(f, false, nullptr)) { o.ks("refused", f.refuse); FrontOut(o, f); o.send(); return; }
    g_bus.logN = 0; g_bus.dropped = 0; g_bus.rec = true;
    KPrintf("nv-gsp: P3 FWSEC-FRTS starting (dma %lx)\n", (unsigned long)g_dma.bus);
    const P3Result r = RunP3(g_bus, *f.chip, f.in, g_dma, [&] { return Arm(f.g, &g_p3Done); });
    g_bus.rec = false;
    const uint64_t us = g_p3Done ? NowUs() - g_runUs : 0;
    o.ks("stage", r.stage); o.ks("status", BootStatusName(r.status)); o.ks("fw", FwStatusName(r.fw));
    o.kb("wrote", r.wrote); o.kh("pci_cmd_before", g_cmdBefore); o.kh("pci_cmd_after", g_cmdAfter); o.kd("run_us", us);
    Pre(o, r.pre);
    if (r.layout.fb_size) Layout(o, r.layout);
    o.kd("fwsec_version", r.fwsec.version); o.kh("fwsec_ucode_bytes", r.fwsec.ucode.n);
    o.k("run"); o.raw("{"); o.ks("step", r.run.step); o.kh("mbox0", r.run.mbox0); o.kh("mbox1", r.run.mbox1);
    o.kh("err", r.run.err); o.kh("wpr2_lo", r.run.wpr2_lo); o.kh("wpr2_hi", r.run.wpr2_hi); o.raw("}");
    if (r.diag_valid) Diag(o, r.diag);
    o.kd("trace_records", g_bus.logN); o.kd("trace_dropped", g_bus.dropped);
    FrontOut(o, f);
    KPrintf("nv-gsp: P3 %s at %s: mbox0 %x err %x WPR2 %08x/%08x (%lu us)\n", BootStatusName(r.status), r.stage,
            r.run.mbox0, r.run.err, r.run.wpr2_lo, r.run.wpr2_hi, (unsigned long)us);
    o.send();
}

RmFiniResult RmUnload(uint64_t budget_us);   // P4 section below
// Everything the fini needs, prepared once (P4 caches it so the pre-reset hook
// does no VFS reads or hashing): the checked front, booter_unload and its image buffer.
struct FiniCtx { Front f; uint8_t* unload = nullptr; uint8_t* img = nullptr; uint32_t ubytes = 0, pages = 0; bool ready = false; };
FiniCtx g_fc;
bool g_bar1Mapped = false;
bool FiniReady(Out* o) {
    if (g_fc.ready) return true;
    Front f; uint8_t* unload = nullptr;
    if (!Prepare(f, true, &unload)) { if (o) { o->ks("refused", f.refuse); FrontOut(*o, f); } return false; }
    const uint32_t ubytes = f.chip->fw[kFwBooterUnload].bytes, pages = (ubytes + 4095) / 4096;
    VirtualAddress img = VmmAllocPages(pages, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
    if (!img) { if (o) o->ks("refused", "no booter image buffer"); return false; }
    g_fc.f = f; g_fc.unload = unload; g_fc.img = reinterpret_cast<uint8_t*>(img.raw()); g_fc.ubytes = ubytes; g_fc.pages = pages;
    g_fc.ready = true;
    return true;
}
bool CePresentStop(bool wait_lock, uint32_t* freed);   // CE present section below
struct FiniOutcome { bool rm = false; RmFiniResult rmf; FiniResult r; };
FiniOutcome FiniCore(uint64_t rm_budget_us) {
    FiniOutcome x;
    x.rm = g_rmRunning;
    if (x.rm) {   // GSP-RM is up (p4 this boot): UNLOADING_GUEST_DRIVER first, then the P3b sequence
        uint32_t ceFreed = 0;
        (void)CePresentStop(rm_budget_us > 2000000, &ceFreed);   // no CE submissions once RM starts to unload
        g_finiDone = true;
        x.rmf = RmUnload(rm_budget_us);
        g_rmRunning = false;
        g_bar1Mapped = false;    // RM unload returns BAR1 to physical mode
    }
    if (!(x.rm && x.rmf.status != BootStatus::Ok))
        x.r = RunFini(g_bus, *g_fc.f.chip, g_fc.f.in, Span{g_fc.unload, g_fc.ubytes}, g_dma, g_fc.img, g_fc.pages * 4096,
                      [&] { return Arm(g_fc.f.g, &g_finiDone); }, !x.rm);
    return x;
}
void Fini() {
    Out o; o.raw("{\"nv_gsp\":\"fini\"");
    if (g_finiDone) { o.ks("refused", "already ran this boot (no retry)"); o.send(); return; }
    if (!FiniReady(&o)) { o.send(); return; }
    const Front& f = g_fc.f;
    g_bus.logN = 0; g_bus.dropped = 0; g_bus.rec = true;
    KPrintf("nv-gsp: P3b fini (FWSEC-SB, booter_unload) starting\n");
    const FiniOutcome x = FiniCore(5000000);
    const FiniResult& r = x.r;
    g_bus.rec = false;
    if (x.rm) {
        o.k("rm_unload"); o.raw("{"); o.ks("stage", x.rmf.stage); o.ks("status", BootStatusName(x.rmf.status));
        o.kd("q", uint32_t(x.rmf.q)); o.kb("halted", x.rmf.halted); o.kh("mbox0", x.rmf.mbox0_after); o.raw("}");
    }
    const uint64_t us = g_finiDone ? NowUs() - g_runUs : 0;
    o.ks("stage", r.stage); o.ks("status", BootStatusName(r.status)); o.ks("fw", FwStatusName(r.fw)); o.kb("wrote", r.wrote);
    o.kh("pci_cmd_before", g_cmdBefore); o.kh("pci_cmd_after", g_cmdAfter); o.kd("run_us", us);
    o.kh("wpr2_lo_before", r.wpr2_lo_before); o.kh("wpr2_hi_before", r.wpr2_hi_before);
    o.k("sb"); o.raw("{"); o.ks("step", r.sb.step); o.kh("mbox0", r.sb.mbox0); o.kh("err", r.sb.err);
    o.kh("wpr2_lo", r.sb.wpr2_lo); o.kh("wpr2_hi", r.sb.wpr2_hi); o.raw("}");
    o.k("unload"); o.raw("{"); o.ks("step", r.unload.step); o.kh("debug", r.unload.debug); o.kh("mbox0", r.unload.mbox0);
    o.kh("mbox1", r.unload.mbox1); o.kh("wpr2_lo", r.unload.wpr2_lo); o.kh("wpr2_hi", r.unload.wpr2_hi); o.raw("}");
    o.kh("bar1_block_after", g_bus.rd32(f.chip->reg_bar1_block));
    if (r.diag_valid) Diag(o, r.diag);
    o.kd("trace_records", g_bus.logN);
    FrontOut(o, f);
    KPrintf("nv-gsp: P3b %s at %s: SB err %x, unload mbox0 %x, WPR2 %08x/%08x -> %08x/%08x\n", BootStatusName(r.status),
            r.stage, r.sb.err, r.unload.mbox0, r.wpr2_lo_before, r.wpr2_hi_before, r.unload.wpr2_lo, r.unload.wpr2_hi);
    o.send();
}

// Pre-reset quiesce (registered once GSP-RM reached INIT_DONE). Runs before
// nv-disp's quiesce and the GPU FLR (hooks run newest first): RM fini while
// BAR1/display are as RM left them; afterwards BAR1 is physical again, so the
// nv-disp teardown through BAR1 works as before P4. Serial output only; no
// VFS; skipped when a verb holds the GPU.
void GspRmQuiesceForReset() {
    if (!g_rmRunning || g_finiDone || !g_fc.ready) return;
    if (__atomic_exchange_n(&g_busy, 1u, __ATOMIC_ACQUIRE)) { SerialPuts("nv-gsp: pre-reset RM fini skipped (verb busy)\n"); return; }
    PlatformPowerExtendResetWatchdog(5000);   // RM unload (1.5 s budget) + FWSEC-SB + booter_unload
    SerialPuts("nv-gsp: pre-reset RM fini (UNLOADING_GUEST_DRIVER, FWSEC-SB, booter_unload)\n");
    g_bus.rec = false;
    const FiniOutcome x = FiniCore(1500000);
    SerialPrintf("nv-gsp: pre-reset RM unload %s (mbox0 %x), fini %s at %s, WPR2 %08x/%08x\n", BootStatusName(x.rmf.status),
                 x.rmf.mbox0_after, BootStatusName(x.r.status), x.r.stage, g_bus.rd32(kRegWpr2Lo), g_bus.rd32(kRegWpr2Hi));
    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
}
bool g_quiesceHooked = false;

// ------------------------------------------------------------------ P4
// GSP-RM to INIT_DONE. All sysmem comes from contiguous PMM runs (direct
// map, physical = bus address); nothing is freed while RM may use it.
struct RmState {
    RmObjects objs;
    RpcChannel* ch = nullptr;
    uint8_t* rx = nullptr;
    uint32_t rxcap = 0;
    // every region handed to the GPU, for the one-time flush after building
    static constexpr uint32_t kMaxRegions = 8192;
    DmaRegion regions[kMaxRegions];
    uint32_t nregions = 0;
    bool overflow = false;
    uint64_t wait_deadline_us = 0;
    uint32_t waits = 0;
};
RmState* g_rm = nullptr;
bool RmAlloc(uint32_t bytes, DmaRegion* r) {
    const uint32_t pages = (bytes + 4095) / 4096;
    const PhysicalAddress p = PmmAllocPages(pages, MemTag::Device, KernelPid);
    if (!p) return false;
    if (p.raw() + uint64_t(pages) * 4096 > (1ull << 39)) return false;
    r->cpu = reinterpret_cast<uint8_t*>(PhysToVirt(p).raw());
    r->bus = p.raw();
    r->bytes = pages * 4096;
    memset(r->cpu, 0, r->bytes);
    if (g_rm->nregions < RmState::kMaxRegions) g_rm->regions[g_rm->nregions++] = *r; else g_rm->overflow = true;
    return true;
}
void FlushRange(const uint8_t* p, uint32_t n) {
    for (uint32_t o = 0; o < n; o += 64) __asm__ volatile("clflush (%0)" ::"r"(p + o) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}
struct RmSync {   // CPU writes -> memory before a doorbell / boot step (cmdq region)
    void operator()() { if (g_rm && g_rm->objs.q.base) FlushRange(g_rm->objs.q.cmdq(), kQueueBytes); }
};
struct RmWait {   // msgq poll: drop stale lines, short pause, bounded by the deadline
    bool operator()() {
        ++g_rm->waits;
        FlushRange(g_rm->objs.q.msgq(), kQueueBytes);
        g_bus.udelay(20);
        return NowUs() < g_rm->wait_deadline_us;
    }
};
RmSync g_rmSync;
RmFiniResult RmUnload(uint64_t budget_us) {
    RmWait w; g_rm->wait_deadline_us = NowUs() + budget_us;
    return RunRmUnload(g_bus, *ChipForBoot0(g_bus.rd32(0)), g_rm->ch, g_rm->rx, g_rm->rxcap, w, g_rmSync);
}
bool PciIdentityOf(const Gpu& g, PciIdentity* p) {
    p->bar0 = g.bar0;
    const uint32_t b1l = PciConfigRead32(g.bus, g.dev, g.fn, 0x14), b1h = PciConfigRead32(g.bus, g.dev, g.fn, 0x18);
    const uint32_t b3l = PciConfigRead32(g.bus, g.dev, g.fn, 0x1c), b3h = PciConfigRead32(g.bus, g.dev, g.fn, 0x20);
    p->bar1 = (uint64_t(b1h) << 32) | (b1l & ~0xfu);
    p->bar3 = (uint64_t(b3h) << 32) | (b3l & ~0xfu);
    const uint32_t id = PciConfigRead32(g.bus, g.dev, g.fn, 0), ss = PciConfigRead32(g.bus, g.dev, g.fn, 0x2c);
    p->vendor = uint16_t(id); p->device = uint16_t(id >> 16);
    p->subvendor = uint16_t(ss); p->subdevice = uint16_t(ss >> 16);
    p->revision = uint8_t(PciConfigRead32(g.bus, g.dev, g.fn, 8));
    p->bus = g.bus; p->dev = g.dev; p->fn = g.fn;
    return p->bar1 && p->bar3 && p->vendor == 0x10de;
}
template <class T> T* NewIn(uint64_t bytes) {
    VirtualAddress v = VmmAllocPages((bytes + 4095) / 4096, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
    if (!v) return nullptr;
    memset(reinterpret_cast<void*>(v.raw()), 0, size_t(bytes));
    return reinterpret_cast<T*>(v.raw());
}

// ------------------------------------------------- static info, BAR1 handover
uint64_t g_bar1Phys = 0;
StaticInfo g_static;
uint8_t g_staticRaw[kStaticInfoBytes];
bool g_staticValid = false;
Bar1Map g_bar1;
constexpr uint64_t kBar1Bytes = 256ull << 20;   // TU104 BAR1 without ReBAR (design §3.3); checked against the boot map
void StaticJson(Out& o, const StaticInfo& s) {
    o.k("static"); o.raw("{");
    char u[42]; FormatUuid(s.uuid, u);
    o.ks("uuid", s.uuid_ok ? u : "?"); o.kh("gid_flags", s.gid_flags); o.ks("name", s.name); o.ks("short_name", s.short_name);
    o.kh("fb_length", s.fb_length); o.kd("fb_bus_width", s.fb_bus_width); o.kd("fb_ram_type", s.fb_ram_type);
    o.kd("l2_cache", s.l2_cache); o.kh("bar1_pde", s.bar1_pde); o.kh("bar2_pde", s.bar2_pde);
    o.kh("h_internal_client", s.h_internal_client); o.kh("h_internal_device", s.h_internal_device);
    o.kh("h_internal_subdevice", s.h_internal_subdevice); o.kb("split_vas", s.split_vas); o.kb("uefi", s.uefi);
    o.kb("efi_init", s.efi_init); o.kb("vbios_valid", s.vbios_valid);
    o.kh("fw_non_wpr_heap_offset", s.fw_non_wpr_heap_offset); o.kh("fw_frts_offset", s.fw_frts_offset);
    o.kh("usable_lo", s.usable_lo); o.kh("usable_hi", s.usable_hi);
    o.k("fb_regions"); o.raw("[");
    for (uint32_t i = 0; i < s.fb_regions; ++i) {
        const FbRegion& f = s.fb[i];
        if (i) o.raw(",");
        o.raw("{"); o.kh("base", f.base); o.kh("limit", f.limit); o.kh("reserved", f.reserved); o.kh("perf", f.performance);
        o.kb("comp", f.compressed); o.kb("iso", f.iso); o.kb("prot", f.prot); o.raw("}");
    }
    o.raw("]}");
}
void Bar1Json(Out& o, const Bar1Map& m) {
    o.k("bar1"); o.raw("{"); o.ks("stage", m.stage); o.ks("status", m.status); o.kh("block", m.block); o.kh("inst", m.inst);
    o.kh("inst_pdb_raw", m.inst_pdb_raw); o.kh("pdb", m.pdb);
    o.k("pd3_old"); o.raw("["); for (uint32_t i = 0; i < 4; ++i) { if (i) o.raw(","); o.hex(m.pd3_old[i]); } o.raw("]");
    o.kb("rm_tree", m.rm_tree); o.kb("rm_tree_sys", m.rm_tree_sys); o.kd("rm_valid", m.rm_valid);
    o.k("rm_first"); o.raw("[");
    for (uint32_t i = 0; i < m.rm_valid && i < 4; ++i) { if (i) o.raw(","); o.raw("["); o.hex(m.rm_first_va[i]); o.raw(","); o.hex(m.rm_first_pte[i]); o.raw("]"); }
    o.raw("]"); o.kh("pd3_new", m.pd3_new); o.kd("tables", m.tables); o.kd("ptes", m.ptes); o.kd("page_shift", m.page_shift);
    o.kb("committed", m.committed); o.kb("invalidated", m.invalidated); o.kd("probes", m.probes); o.kd("probes_equal", m.probes_equal);
    o.k("probe"); o.raw("[");
    for (uint32_t i = 0; i < 3; ++i) { if (i) o.raw(","); o.raw("["); o.hex(m.probe_va[i]); o.raw(","); o.hex(m.probe_pramin[i]);
        o.raw(","); o.hex(m.probe_bar1[i]); o.raw("]"); }
    o.raw("]}");
}
// One UC page of BAR1, remapped per read (never freed; the first RAM leaf is released).
VirtualAddress g_bar1ProbeVa{};
bool Bar1Read(uint64_t off, uint32_t* v) {
    if (!g_bar1Phys || off + 4 > kBar1Bytes) return false;
    if (!g_bar1ProbeVa) {
        VirtualAddress va = VmmAllocPages(1, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
        if (!va) return false;
        const auto leaf = VmmDetachPage(KernelPageTable, va); if (leaf) PmmFreePage(VmmLeafPhysical(leaf));
        g_bar1ProbeVa = va;
    } else (void)VmmDetachPage(KernelPageTable, g_bar1ProbeVa);
    __asm__ volatile("invlpg (%0)" ::"r"(g_bar1ProbeVa.raw()) : "memory");
    if (!VmmMapPage(KernelPageTable, g_bar1ProbeVa, PhysicalAddress((g_bar1Phys + off) & ~0xfffull),
                    VMM_WRITABLE | VMM_NO_EXEC | VMM_CACHE_DISABLE | VMM_WRITE_THRU, MemTag::Device, KernelPid)) return false;
    *v = reinterpret_cast<volatile uint32_t*>(g_bar1ProbeVa.raw() + (off & 0xfff))[0];
    return true;
}
struct Bar1PageAlloc {
    bool operator()(uint64_t* bus, uint8_t** cpu) { DmaRegion d; if (!RmAlloc(0x1000, &d)) return false; *bus = d.bus; *cpu = d.cpu; return true; }
    void flush(uint8_t* p) { FlushRange(p, 0x1000); }
};
// Caller holds the nv-aux lock (no compositor BAR1/PRAMIN traffic).
bool Bar1MapLocked(uint8_t shift) {
    if (!g_rmRunning || !g_staticValid || g_bar1Mapped) return false;
    BootResourceClassification bc{};
    constexpr uint32_t allowed = (1u << uint32_t(MemoryType::Reserved)) | (1u << uint32_t(MemoryType::Mmio));
    if (!g_bar1Phys || !AcpiClassifyBootRange(g_bar1Phys, kBar1Bytes, bc) || (bc.types & ~allowed)) {
        g_bar1 = Bar1Map{}; g_bar1.status = "BAR1 range not MMIO in the boot map"; return false;
    }
    Bar1PageAlloc pa;
    const uint64_t probes[3] = {0x0, 0x4343000, 0xff13000};   // GOP surface, a back buffer, the nv-disp instmem MiB
    g_bar1 = MapBar1Identity(g_bus, *ChipForBoot0(g_bus.rd32(0)), pa, g_static.bar1_pde, kBar1Bytes, probes, 3,
                             [](uint64_t off, uint32_t* v) { return Bar1Read(off, v); }, shift);
    g_bar1Mapped = g_bar1.committed;
    KPrintf("nv-gsp: BAR1 identity map %s at %s (RM PD3[0] %lx -> %lx, %u ptes, probes %u/%u)\n", g_bar1.status, g_bar1.stage,
            (unsigned long)g_bar1.pd3_old[0], (unsigned long)g_bar1.pd3_new, g_bar1.ptes, g_bar1.probes_equal, g_bar1.probes);
    return g_bar1.ok();
}

void P4(bool keepDisplay) {
    Out o; o.raw("{\"nv_gsp\":\"p4\"");
    if (g_p3Done || g_p4Done || g_finiDone) { o.ks("refused", "a falcon run already happened this boot (no retry)"); o.send(); return; }
    Front f; uint8_t* unloadBlob = nullptr;
    if (!Prepare(f, true, &unloadBlob)) { o.ks("refused", f.refuse); FrontOut(o, f); o.send(); return; }
    const ChipOps& c = *f.chip;
    PciIdentity pci;
    if (!PciIdentityOf(f.g, &pci)) { o.ks("refused", "PCI identity/BARs"); o.send(); return; }
    g_bar1Phys = pci.bar1;
    if (!g_fc.ready) {   // cache the fini inputs now (the pre-reset hook must not touch the VFS)
        const uint32_t ubytes = c.fw[kFwBooterUnload].bytes, upages = (ubytes + 4095) / 4096;
        VirtualAddress uimg = VmmAllocPages(upages, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
        if (!uimg) { o.ks("refused", "no booter image buffer"); o.send(); return; }
        g_fc.f = f; g_fc.unload = unloadBlob; g_fc.img = reinterpret_cast<uint8_t*>(uimg.raw());
        g_fc.ubytes = ubytes; g_fc.pages = upages; g_fc.ready = true;
    }
    // firmware: the ELF (28.5 MB), the RISC-V bootloader and booter_load, hash-checked again on load
    uint8_t* elfBlob = LoadFile(c, kFwGsp);
    uint8_t* rblBlob = LoadFile(c, kFwBootloader);
    uint8_t* loadBlob = LoadFile(c, kFwBooterLoad);
    if (!elfBlob || !rblBlob || !loadBlob) { o.ks("refused", "firmware load failed"); o.send(); return; }
    GspElf elf; RiscvBootloader rbl; BooterImage booter;
    if (ParseGspElf(Span{elfBlob, c.fw[kFwGsp].bytes}, &elf) != FwStatus::Ok ||
        ParseRiscvBootloader(Span{rblBlob, c.fw[kFwBootloader].bytes}, &rbl) != FwStatus::Ok ||
        ParseBooter(Span{loadBlob, c.fw[kFwBooterLoad].bytes}, &booter) != FwStatus::Ok) {
        o.ks("refused", "firmware parse failed"); o.send(); return;
    }
    uint8_t* bimg = NewIn<uint8_t>(booter.image.n);
    if (!g_rm) g_rm = NewIn<RmState>(sizeof(RmState));
    if (!bimg || !g_rm || !BuildBooterImage(booter, bimg, booter.image.n)) { o.ks("refused", "no memory"); o.send(); return; }
    g_rm->ch = NewIn<RpcChannel>(sizeof(RpcChannel));
    g_rm->rxcap = 2 * kMaxElemBytes;
    g_rm->rx = NewIn<uint8_t>(g_rm->rxcap);
    if (!g_rm->ch || !g_rm->rx) { o.ks("refused", "no memory"); o.send(); return; }
    f.in.elf_size = elf.fwimage.n;
    f.in.boot_size = rbl.payload.n;
    auto prepare = [&](const FbLayout& L) -> const char* {
        auto alloc = [](uint32_t bytes, DmaRegion* r) { return RmAlloc(bytes, r); };
        if (const char* e = BuildRmObjects(alloc, elf, rbl, L, &g_rm->objs)) return e;
        if (g_rm->overflow) return "region list overflow";
        for (uint32_t i = 0; i < g_rm->nregions; ++i) FlushRange(g_rm->regions[i].cpu, g_rm->regions[i].bytes);
        return nullptr;
    };
    g_bus.logN = 0; g_bus.dropped = 0; g_bus.rec = true;
    KPrintf("nv-gsp: P4 GSP-RM boot starting (FWSEC-FRTS, booter_load, INIT_DONE)\n");
    RmWait wait;
    g_rm->wait_deadline_us = ~0ull;
    uint64_t armUs = 0;
    auto arm = [&] {
        if (!Arm(f.g, &g_p4Done)) return false;
        armUs = NowUs();
        g_rm->wait_deadline_us = armUs + 15000000;   // INIT_DONE took ~0.9 s after booter under Linux
        return true;
    };
    // keep-display: no compositor/nv-disp BAR1 traffic from the boot until BAR1 is remapped
    SpinLock* lock = keepDisplay ? NvAuxSharedLock() : nullptr;
    if (lock) SpinLockAcquire(lock);
    const uint64_t lockUs = NowUs();
    static P4Result r;
    r = RunP4(g_bus, c, f.in, g_dma, pci, booter, bimg, &g_rm->objs, g_rm->ch, g_rm->rx, g_rm->rxcap,
              prepare, arm, wait, g_rmSync);
    g_bus.rec = false;
    g_rmRunning = r.init_done;
    if (r.static_ok) { g_static = r.si; memcpy(g_staticRaw, r.static_raw, kStaticInfoBytes); g_staticValid = true; }
    bool mapped = false;
    if (lock && r.init_done && r.si.ok) mapped = Bar1MapLocked(16);
    if (lock) SpinLockRelease(lock);
    const uint64_t heldUs = lock ? NowUs() - lockUs : 0;
    if (r.init_done && !g_quiesceHooked) g_quiesceHooked = PlatformPowerAddQuiesce(GspRmQuiesceForReset);
    o.ks("stage", r.stage); o.ks("status", BootStatusName(r.status)); o.kd("q", uint32_t(r.q)); o.kb("wrote", r.p3.wrote);
    o.kh("pci_cmd_before", g_cmdBefore); o.kh("pci_cmd_after", g_cmdAfter);
    Pre(o, r.p3.pre);
    if (r.p3.layout.fb_size) Layout(o, r.p3.layout);
    o.k("fwsec"); o.raw("{"); o.ks("step", r.p3.run.step); o.kh("mbox0", r.p3.run.mbox0); o.kh("err", r.p3.run.err);
    o.kh("wpr2_lo", r.p3.run.wpr2_lo); o.kh("wpr2_hi", r.p3.run.wpr2_hi); o.raw("}");
    o.k("booter"); o.raw("{"); o.ks("step", r.booter.step); o.kh("debug", r.booter.debug); o.kh("mbox0", r.booter.mbox0);
    o.kh("mbox1", r.booter.mbox1); o.kh("wpr2_lo", r.booter.wpr2_lo); o.kh("wpr2_hi", r.booter.wpr2_hi);
    o.kd("t_us", r.t_booter_us); o.raw("}");
    o.k("rm"); o.raw("{"); o.kb("init_done", r.init_done); o.kd("t_init_done_us", r.t_init_done_us);
    o.kd("seq_runs", r.seq_runs); o.ks("seq_status", SeqStatusName(r.seq.status)); o.kd("seq_commands", r.seq.commands);
    o.kd("seq_fail_index", r.seq.fail_index); o.kd("events", r.events); o.kd("nocat", r.nocat);
    o.kd("libos_print", r.libos_print); o.kd("other_events", r.other_events); o.kh("last_other_fn", r.last_other_fn);
    o.kd("waits", g_rm->waits); o.kd("static_len", r.static_len); o.kb("static_ok", r.static_ok);
    o.k("static_head"); o.raw("\""); for (uint32_t i = 0; i < 32; ++i) { const char* hx = "0123456789abcdef";
        char b[3] = {hx[r.static_head[i] >> 4], hx[r.static_head[i] & 15], 0}; o.raw(b); } o.raw("\"");
    o.kd("sysmem_kib", g_rm->objs.dma_bytes >> 10); o.kh("libos", g_rm->objs.libos.bus); o.kh("meta", g_rm->objs.meta.bus);
    o.kh("shm", g_rm->objs.shm.bus); o.raw("}");
    if (r.si.ok) StaticJson(o, r.si);
    o.kh("bar1_block", g_bus.rd32(c.reg_bar1_block)); o.kh("bar2_block", g_bus.rd32(c.reg_bar2_block));
    o.kb("keep_display", keepDisplay); o.kd("lock_held_us", heldUs); o.kb("pre_reset_hook", g_quiesceHooked);
    if (lock) { o.kb("bar1_mapped", mapped); Bar1Json(o, g_bar1); }
    o.kh("mbox0_now", g_bus.rd32(c.gsp_falcon + kFMbox0)); o.kh("riscv_cpuctl_now", g_bus.rd32(c.gsp_riscv + kRiscvCpuctl));
    if (r.diag_valid) Diag(o, r.diag);
    o.kd("trace_records", g_bus.logN); o.kd("trace_dropped", g_bus.dropped);
    FrontOut(o, f);
    KPrintf("nv-gsp: P4 %s at %s: booter mbox0 %x, WPR2 %08x/%08x, INIT_DONE %d at %lu us, seq %s, static %u B\n",
            BootStatusName(r.status), r.stage, r.booter.mbox0, r.booter.wpr2_lo, r.booter.wpr2_hi, r.init_done ? 1 : 0,
            (unsigned long)r.t_init_done_us, SeqStatusName(r.seq.status), r.static_len);
    o.send();
}

// `static [off]`: the parsed reply and 1536 raw bytes from `off` (decimal).
void StaticVerb(uint32_t off) {
    Out o; o.raw("{\"nv_gsp\":\"static\"");
    if (!g_staticValid || off >= kStaticInfoBytes) { o.ks("error", "no static info this boot (run p4) or offset past 0x678"); o.send(); return; }
    o.kd("offset", off); o.kd("bytes", kStaticInfoBytes);
    const uint32_t n = kStaticInfoBytes - off < 1536 ? kStaticInfoBytes - off : 1536;
    o.k("hex"); o.raw("\"");
    for (uint32_t i = 0; i < n; ++i) { const char* hx = "0123456789abcdef"; char c2[3] = {hx[g_staticRaw[off + i] >> 4], hx[g_staticRaw[off + i] & 15], 0}; o.raw(c2); }
    o.raw("\"");
    o.send();
    Out p; p.raw("{\"nv_gsp\":\"static_parsed\""); StaticJson(p, g_static); p.send();
}
// `bar1 map|unmap|status` (mode 1/2/0).
void Bar1Verb(int mode, uint8_t shift) {
    Out o; o.raw("{\"nv_gsp\":\"bar1\"");
    const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
    if (!c || !g_rmRunning) { o.ks("refused", "GSP-RM is not running this boot"); o.send(); return; }
    SpinLock* lock = NvAuxSharedLock();
    if (mode == 1) {
        if (g_bar1Mapped) { o.ks("refused", "already mapped"); }
        else { SpinLockAcquire(lock); (void)Bar1MapLocked(shift); SpinLockRelease(lock); }
    } else if (mode == 2) {
        if (!g_bar1Mapped) o.ks("refused", "not mapped");
        else { SpinLockAcquire(lock); const bool ok = UnmapBar1Identity(g_bus, *c, g_bar1); SpinLockRelease(lock);
               g_bar1Mapped = false; o.kb("unmapped", ok); }
    }
    o.kb("mapped", g_bar1Mapped); o.kh("bar1_block", g_bus.rd32(c->reg_bar1_block)); o.kh("bar2_block", g_bus.rd32(c->reg_bar2_block));
    Bar1Json(o, g_bar1);
    o.send();
}

// ------------------------------------------------------------------ P5 (M2)
bool g_p5Done = false;
CePresent g_cePresentState;   // `ce-present on`: the kept CE channel
const char* Find(const char* h, const char* n) {
    for (; *h; ++h) { uint32_t i = 0; while (n[i] && h[i] == n[i]) ++i; if (!n[i]) return h; }
    return nullptr;
}
struct PraminIo {   // VramIo over the BAR0 window (nv_gsp_bar1.h Pramin)
    Pramin<Bar0Bus>& p;
    uint32_t rd32(uint64_t a) { return p.rd32(a); }
    void wr32(uint64_t a, uint32_t v) { p.wr32(a, v); }
};
// M3 buffers: contiguous, 2 MiB aligned (one PTE per 2 MiB), never freed (once per boot).
bool M3Sys(uint64_t bytes, M3Buf* b) {
    const uint64_t pages = (bytes + 0x1fffff) / 4096 + 512;
    const PhysicalAddress p = PmmAllocPages(pages, MemTag::Device, KernelPid);
    if (!p) return false;
    const uint64_t a = (p.raw() + 0x1fffff) & ~0x1fffffull;
    if (a + bytes > (1ull << 39)) return false;
    b->bus = a; b->cpu = reinterpret_cast<uint8_t*>(PhysToVirt(PhysicalAddress(a)).raw());
    b->bytes = (bytes + 0x1fffff) & ~0x1fffffull;
    return true;
}
void RateJson(Out& o, const char* k, const M3Rate& r) {
    o.k(k); o.raw("{"); o.kd("bytes", r.bytes); o.kd("best_us", r.best_us); o.kd("avg_us", r.n ? r.sum_us / r.n : 0);
    o.kd("n", r.n); o.kd("mb_per_s", r.mbps()); o.raw("}");
}
void P5(uint32_t chid, bool dev, bool m3 = false) {
    Out o; o.raw("{\"nv_gsp\":\"p5\"");
    const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
    if (!c || !g_rmRunning || !g_staticValid) { o.ks("refused", "GSP-RM is not running this boot"); o.send(); return; }
    if (g_p5Done) { o.ks("refused", "already ran this boot (done, or objects left behind)"); o.send(); return; }
    if (g_cePresentState.k.kept) { o.ks("refused", "ce-present holds the M2 handles (ce-present off first)"); o.send(); return; }
    M2Inputs in;
    in.si = g_static;
    in.vram_lo = 0x100000000ull; in.vram_hi = in.vram_lo + (1u << 20);   // 4 GiB: far from the display buffers (< 256 MiB)
    in.chid = chid; in.parent_device = dev;
    auto sys = [](M2Sys* m) { DmaRegion d; if (!RmAlloc(0x1000, &d)) return false; m->cpu = d.cpu; m->bus = d.bus; m->bytes = d.bytes; return true; };
    if (!sys(&in.gpfifo) || !sys(&in.push) || !sys(&in.sem)) { o.ks("refused", "no sysmem"); o.send(); return; }
    SpinLock* lock = NvAuxSharedLock();
    SpinLockAcquire(lock);                       // PRAMIN is shared with nv-disp
    Pramin<Bar0Bus> pr(g_bus, *c);
    PraminIo vio{pr};
    RmWait wait; g_rm->wait_deadline_us = NowUs() + 5000000;
    GspDoorbell<Bar0Bus, RmSync> bell{g_bus, *c, g_rmSync};
    auto mthd = [](uint32_t bytes, M2Sys* m) { DmaRegion d; if (!RmAlloc(bytes, &d)) return false;
        m->cpu = d.cpu; m->bus = d.bus; m->bytes = d.bytes; return true; };
    const uint64_t fenceEnd = NowUs() + 2000000;
    auto fence = [&] { FlushRange(in.sem.cpu, 64); g_bus.udelay(5); return NowUs() < fenceEnd; };
    struct SubmitSync { M2Inputs& in; void operator()() {
        FlushRange(in.gpfifo.cpu, 0x1000); FlushRange(in.push.cpu, 0x1000); FlushRange(in.sem.cpu, 0x1000); } } ss{in};
    M3Plan pl; M3Result m3r;
    if (m3) {
        const uint64_t surf = uint64_t(pl.width) * pl.bpp * pl.height;
        pl.v1 = 0x101000000ull; pl.v2 = 0x104000000ull;                   // 4 GiB + 16 / + 64 MiB, client FB
        if (!M3Sys(surf, &pl.s) || !M3Sys(surf, &pl.r)) { SpinLockRelease(lock); o.ks("refused", "no M3 sysmem"); o.send(); return; }
        if (!VramRangeIsClient(g_static, pl.v1, pl.v1 + 0x2000000) || !VramRangeIsClient(g_static, pl.v2, pl.v2 + 0x2000000)) {
            SpinLockRelease(lock); o.ks("refused", "M3 VRAM not in a client FB region"); o.send(); return;
        }
    }
    auto flush = [](uint8_t* p, uint64_t n) { FlushRange(p, uint32_t(n)); };
    M3Extra<decltype(flush)> ex{pl, m3r, flush};
    KPrintf("nv-gsp: P5 (M2 objects, NOP + semaphore%s) starting\n", m3 ? ", M3 copy engine" : "");
    const M2Result m = m3 ? RunM2(g_bus, *c, vio, in, g_rm->ch, g_rm->rx, g_rm->rxcap, wait, bell, mthd, fence, ss, ex)
                          : RunM2(g_bus, *c, vio, in, g_rm->ch, g_rm->rx, g_rm->rxcap, wait, bell, mthd, fence, ss);
    pr.Restore();
    SpinLockRelease(lock);
    // a clean failure (everything freed, no doorbell rung) may be retried with other options
    g_p5Done = m.ok() || m.freed != m.allocated || m.token;
    o.kd("chid", chid); o.kb("parent_device", dev); o.kb("latched", g_p5Done);
    o.ks("stage", m.stage); o.ks("status", m.status); o.kh("rm_status", m.rm_status); o.kh("rpc_result", m.rpc_result);
    o.kd("q", uint32_t(m.q)); o.kh("ce_engine", m.ce_engine); o.kd("runlist", m.runlist); o.kd("devinfo_entries", m.devinfo_entries);
    o.kh("mthdbuf_size", m.mthdbuf_size); o.kh("inst", m.inst); o.kh("userd", m.userd); o.kh("pd3", m.pd3);
    o.kd("pt_pages", m.pt_pages); o.kd("ptes", m.ptes); o.kh("token", m.token); o.kh("token_rm", m.token_rm);
    o.kb("token_ctrl_ok", m.token_ctrl_ok); o.kb("schedule_on_tsg", m.schedule_on_tsg);
    o.kd("allocated", m.allocated); o.kd("freed", m.freed);
    o.kb("fence_ok", m.fence_ok); o.kh("sem_value", m.sem_value); o.kd("fence_us", m.fence_us); o.kd("gp_get", m.gp_get);
    o.k("events"); o.raw("{"); o.kd("total", m.ev.total); o.kd("rc", m.ev.rc); o.kd("mmu_fault", m.ev.mmu_fault);
    o.kd("os_error", m.ev.os_error); o.kd("post_event", m.ev.post_event); o.kd("nocat", m.ev.nocat); o.kd("other", m.ev.other);
    o.kh("last_other_fn", m.ev.last_other_fn);
    o.k("last_rc"); o.raw("\""); for (uint32_t i = 0; i < 32; ++i) { const char* hx = "0123456789abcdef";
        char b[3] = {hx[m.ev.last_rc[i] >> 4], hx[m.ev.last_rc[i] & 15], 0}; o.raw(b); } o.raw("\"");
    o.raw("}");
    if (m3) {
        o.k("m3"); o.raw("{"); o.ks("status", m3r.status); o.ks("step", m3r.step); o.kb("object", m3r.object);
        o.kh("rm_status", m3r.rm_status); o.kd("ptes", m3r.ptes); o.kh("s_bus", pl.s.bus); o.kh("r_bus", pl.r.bus);
        o.kh("v1", pl.v1); o.kh("v2", pl.v2); o.kd("width", pl.width); o.kd("height", pl.height);
        RateJson(o, "sysmem_to_vram", m3r.s2v); RateJson(o, "vram_to_sysmem", m3r.v2s); RateJson(o, "vram_to_vram", m3r.v2v);
        RateJson(o, "vram_rect", m3r.rect); RateJson(o, "present_rect", m3r.present);
        o.kb("verify_full", m3r.verify_full); o.kb("verify_rect", m3r.verify_rect);
        if (m3r.mismatch_at != ~0ull) { o.kh("mismatch_at", m3r.mismatch_at); o.kh("got", m3r.mismatch_got); o.kh("want", m3r.mismatch_want); }
        o.kd("events_rc", m3r.events_rc); o.kd("events_mmu", m3r.events_mmu); o.raw("}");
    }
    o.kh("mbox0_now", g_bus.rd32(c->gsp_falcon + kFMbox0)); o.kh("riscv_cpuctl_now", g_bus.rd32(c->gsp_riscv + kRiscvCpuctl));
    KPrintf("nv-gsp: P5 %s at %s (rm 0x%x): fence %d value %x in %lu us, ce %x runlist %u token %x\n", m.status, m.stage,
            m.rm_status, m.fence_ok ? 1 : 0, m.sem_value, (unsigned long)m.fence_us, m.ce_engine, m.runlist, m.token);
    o.send();
}

// ------------------------------------------------------------------ CE present (head 0)
// `ce-present on`: a kept TURING_DMA_COPY_A channel (RunM2 + CeExtra; its own VRAM arena at
// 4 GiB + 128 MiB), then nv-disp's head-0 presents kick copies through g_ceOps (CeKickHook/CeWaitHook). `off` and every
// RM fini (verb or pre-reset) clear the hook under the nv-aux lock first, then free the objects.
uint32_t g_ceOnUs = 0; const char* g_ceOnStatus = "never"; const ChipOps* g_ceChip = nullptr;
struct CeHookSync { void operator()() {
    FlushRange(g_cePresentState.in.gpfifo.cpu, 0x1000); FlushRange(g_cePresentState.in.push.cpu, 0x1000); } };
// Kernel VA -> physical. The direct map (2 MiB pages, where nv-disp's contiguous shadow
// lives) is linear; VMALLOC pages go through the page tables.
uint64_t CeV2P(uintptr_t a) {
    if (a >= DIRECT_MAP_BASE && a < VMALLOC_BASE) return VirtToPhys(VirtualAddress(a)).raw();
    const PhysicalAddress p = VmmVirtToPhys(KernelPageTable, VirtualAddress(a));
    return p ? p.raw() : 0;
}
int CeKickHook(const NvCeJob& job, uint64_t* bytes, uint32_t* ticket) {   // nv-aux lock held
    const ChipOps* c = g_ceChip;
    if (!c || !g_rmRunning) return 0;
    Pramin<Bar0Bus> pr(g_bus, *c);
    PraminIo vio{pr};
    CeHookSync sync;
    const bool rec = g_bus.rec; g_bus.rec = false;
    const int r = CePresentKick(g_bus, *c, vio, g_cePresentState, job, CeV2P, sync, bytes, ticket);
    g_bus.rec = rec;
    pr.Restore();
    if (r < 0) { g_cePresentState.active = false; g_ceActiveFlag = false; KPrintf("nv-gsp: CE present kick failed (%s)\n", g_cePresentState.last_error); }
    return r;
}
int CeWaitHook(uint32_t ticket, uint32_t budget_us) {   // nv-aux lock held
    if (!g_rmRunning) return -1;
    const int r = CePresentWait(g_bus, g_cePresentState, ticket, budget_us, [] { __asm__ volatile("pause"); });
    if (r < 0) { g_cePresentState.active = false; g_ceActiveFlag = false; KPrintf("nv-gsp: CE present wait failed (%s)\n", g_cePresentState.last_error); }
    return r;
}
const NvCeOps g_ceOps{CeKickHook, CeWaitHook};
// Clear the hook (nv-aux lock: no present is mid-submission afterwards), then free the objects
// while RM runs. wait_lock=false (pre-reset): try the lock for 100 ms.
bool CePresentStop(bool wait_lock, uint32_t* freed) {
    *freed = 0;
    if (!g_cePresentState.k.kept && !g_cePresentState.active) return true;
    SpinLock* lock = NvAuxSharedLock();
    bool held = false;
    if (wait_lock) { SpinLockAcquire(lock); held = true; }
    else { const uint64_t end = NowUs() + 100000; while (!(held = SpinLockTryAcquire(lock)) && NowUs() < end) __asm__ volatile("pause"); }
    if (held) NvDispSetCePresent(nullptr);   // finishes a kicked copy, then nv-disp copies with the CPU
    g_cePresentState.active = false; g_ceActiveFlag = false;         // (without the lock: kicks are refused from here on)
    if (held) SpinLockRelease(lock);
    if (g_cePresentState.k.kept && g_rmRunning && g_rm) {
        RmWait wait; g_rm->wait_deadline_us = NowUs() + 1000000;
        const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
        GspDoorbell<Bar0Bus, RmSync> bell{g_bus, *c, g_rmSync};
        alignas(8) static uint8_t scratch[kRmApiBuf];
        RmApi<RmWait, GspDoorbell<Bar0Bus, RmSync>> api(g_rm->ch, g_rm->rx, g_rm->rxcap, wait, bell, scratch);
        *freed = M2FreeKept(g_cePresentState.k, api);
    }
    g_cePresentState.k.kept = false;
    return held;
}
void CeStatsJson(Out& o) {
    const CePresent& cp = g_cePresentState; const CeStats& st = cp.st;
    o.kb("active", cp.active); o.kb("kept", cp.k.kept); o.kb("broken", cp.broken); o.ks("last_error", cp.last_error);
    o.ks("on_status", g_ceOnStatus); o.kd("on_us", g_ceOnUs);
    o.ks("autostart", g_autostart.status); o.kd("autostart_p4_us", g_autostart.p4_us); o.kd("autostart_ce_us", g_autostart.ce_us);
    o.kd("frames", st.frames); o.kd("rects", st.rects); o.kd("bytes", st.bytes); o.kd("submits", st.submits);
    o.kd("avg_us", st.submits ? st.us_sum / st.submits : 0); o.kd("max_us", st.us_max);
    o.kd("remaps", st.remaps); o.kd("remap_us_max", st.remap_us_max); o.kd("map_pages", st.map_pages);
    o.kd("refused", st.refused); o.kd("timeouts", st.timeouts); o.kd("slot", cp.slot); o.kb("big_pages", cp.big);
    o.kd("wait_us_sum", st.wait_us_sum); o.kd("waits_blocked", st.waits_blocked); o.kd("inflight", cp.inflight);
    o.kh("bb_lo", cp.bb_lo); o.kh("bb_hi", cp.bb_hi); o.kd("pt_pages", cp.k.pt_pages); o.kd("ptes", cp.k.ptes);
    o.kh("arena_next", cp.k.arena.next);
}
void CePresentVerb(int mode) {   // 1 on, 2 off, 0 status
    Out o; o.raw("{\"nv_gsp\":\"ce_present\"");
    const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
    if (mode == 1) {
        if (!c || !g_rmRunning || !g_staticValid) { o.ks("refused", "GSP-RM is not running this boot"); o.send(); return; }
        if (!g_bar1Mapped) { o.ks("refused", "BAR1 is not identity-mapped (p4 keep-display)"); o.send(); return; }
        if (g_cePresentState.k.kept || g_cePresentState.broken) { o.ks("refused", "already on, or failed this boot"); o.send(); return; }
        CePresent& cp = g_cePresentState;
        cp.in = M2Inputs{};
        cp.in.keep = &cp.k; cp.in.si = g_static; cp.in.chid = kM2Chid;
        cp.in.vram_lo = 0x108000000ull; cp.in.vram_hi = cp.in.vram_lo + (8u << 20);
        auto sys = [](M2Sys* m) { DmaRegion d; if (!RmAlloc(0x1000, &d)) return false; m->cpu = d.cpu; m->bus = d.bus; m->bytes = d.bytes; return true; };
        if (!sys(&cp.in.gpfifo) || !sys(&cp.in.push) || !sys(&cp.in.sem)) { o.ks("refused", "no sysmem"); o.send(); return; }
        SpinLock* lock = NvAuxSharedLock();
        SpinLockAcquire(lock);
        Pramin<Bar0Bus> pr(g_bus, *c);
        PraminIo vio{pr};
        RmWait wait; g_rm->wait_deadline_us = NowUs() + 5000000;
        GspDoorbell<Bar0Bus, RmSync> bell{g_bus, *c, g_rmSync};
        auto mthd = [](uint32_t bytes, M2Sys* m) { DmaRegion d; if (!RmAlloc(bytes, &d)) return false;
            m->cpu = d.cpu; m->bus = d.bus; m->bytes = d.bytes; return true; };
        const uint64_t fenceEnd = NowUs() + 2000000;
        auto fence = [&] { FlushRange(cp.in.sem.cpu, 64); g_bus.udelay(5); return NowUs() < fenceEnd; };
        CeHookSync ss;
        const uint64_t t0 = NowUs();
        const M2Result m = RunM2(g_bus, *c, vio, cp.in, g_rm->ch, g_rm->rx, g_rm->rxcap, wait, bell, mthd, fence, ss, CeExtra{});
        pr.Restore();
        g_ceOnUs = uint32_t(NowUs() - t0);
        g_ceOnStatus = m.status;
        if (m.ok() && cp.k.kept) {
            // touch every BAR0 page the hook uses, so it never adds a Bar0Bus slot at present time
            (void)g_bus.rd32(c->reg_fb_flush); (void)g_bus.rd32(c->reg_mmu_inval_trigger);
            (void)g_bus.rd32(c->reg_mmu_inval_pdb_lo); (void)g_bus.rd32(c->usermode);
            g_ceChip = c; cp.active = true; g_ceActiveFlag = true; NvDispSetCePresent(&g_ceOps);
        }
        else cp.broken = true;
        SpinLockRelease(lock);
        o.ks("stage", m.stage); o.ks("status", m.status); o.kh("rm_status", m.rm_status); o.kh("ce_engine", m.ce_engine);
        o.kd("runlist", m.runlist); o.kh("token", m.token); o.kd("allocated", m.allocated); o.kd("freed", m.freed);
        o.kb("fence_ok", m.fence_ok); o.kd("events_rc", m.ev.rc); o.kd("events_mmu", m.ev.mmu_fault);
        KPrintf("nv-gsp: CE present on: %s at %s (%u us)\n", m.status, m.stage, g_ceOnUs);
    } else if (mode == 2) {
        uint32_t freed = 0;
        const bool held = CePresentStop(true, &freed);
        o.kb("lock_held", held); o.kd("freed", freed);
    }
    CeStatsJson(o);
    o.send();
}

// LOGINIT/LOGINTR/LOGRM: the put pointer and 1536 raw bytes at `off`
// (libos-encoded records; decode host-side against the ELF's log sections).
void Log(const char* which, uint32_t off) {
    Out o; o.raw("{\"nv_gsp\":\"log\"");
    const int id = !strcmp(which, "init") ? kLogInit : !strcmp(which, "intr") ? kLogIntr : !strcmp(which, "rm") ? kLogRm : -1;
    if (id < 0 || !g_rm || !g_rm->objs.log[id].cpu || off >= kLogBytes) { o.ks("error", "Expected init|intr|rm <offset> after p4"); o.send(); return; }
    const DmaRegion& b = g_rm->objs.log[id];
    FlushRange(b.cpu, kLogBytes);
    uint64_t put; memcpy(&put, b.cpu, 8);
    o.ks("buffer", which); o.kd("put", put); o.kd("offset", off);
    const uint32_t n = kLogBytes - off < 1536 ? kLogBytes - off : 1536;
    o.k("hex"); o.raw("\"");
    for (uint32_t i = 0; i < n; ++i) { const char* hx = "0123456789abcdef"; char c2[3] = {hx[b.cpu[off + i] >> 4], hx[b.cpu[off + i] & 15], 0}; o.raw(c2); }
    o.raw("\"");
    o.send();
}

// One JSON line per record, in the M0 raw-log schema (scripts/nv_gsp_trace_diff.py
// --brook); repeated identical reads carry "count".
void Trace(uint32_t from) {
    for (uint32_t i = from; i < g_bus.logN && i < from + 400; ++i) {
        const auto& a = g_bus.log[i];
        Out o; o.raw("{\"op\":\""); char op[2] = {a.op, 0}; o.raw(op); o.raw("\",\"off\":"); o.dec(a.off);
        o.raw(",\"val\":"); o.dec(a.val); o.raw(",\"phase\":"); o.str(a.phase); o.raw(",\"step\":"); o.str(a.step);
        if (a.count > 1) { o.raw(",\"count\":"); o.dec(a.count); }
        o.send();
    }
    Out o; o.raw("{\"nv_gsp\":\"trace\""); o.kd("records", g_bus.logN); o.kd("from", from);
    o.kd("dropped", g_bus.dropped); o.send();
}

}  // namespace

void NvGspAutostart() {
    if (!MarkerSet("/etc/nv-gsp")) { g_autostart.status = "no /etc/nv-gsp"; return; }
    if (PathExists("/etc/nv-gsp-no-autostart")) { g_autostart.status = "opted out"; KPrintf("nv-gsp: autostart off (/etc/nv-gsp-no-autostart)\n"); return; }
    SchedulerSleepMs(3000);   // let the desktop settle after the head bring-up
    bool got = false;
    for (int i = 0; i < 100 && !(got = !__atomic_exchange_n(&g_busy, 1u, __ATOMIC_ACQUIRE)); ++i) SchedulerSleepMs(100);
    if (!got) { g_autostart.status = "busy (a verb held the GPU)"; return; }
    g_autostart.start_ms = NowUs() / 1000;
    g_outQuiet = true;
    uint64_t t0 = NowUs();
    P4(true);
    g_autostart.p4_us = NowUs() - t0;
    if (g_rmRunning && g_bar1Mapped) {
        t0 = NowUs(); CePresentVerb(1); g_autostart.ce_us = NowUs() - t0;
        g_autostart.status = g_cePresentState.active ? "rm + ce present" : "rm up, ce present failed (CPU copies)";
    } else g_autostart.status = g_rmRunning ? "rm up, BAR1 not mapped (no CE)" : "p4 failed (no RM)";
    g_outQuiet = false;
    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
    KPrintf("nv-gsp: autostart %s (p4 %lu us, ce %lu us)\n", g_autostart.status, (unsigned long)g_autostart.p4_us,
            (unsigned long)g_autostart.ce_us);
}

bool NvGspHandle(const char* command) {
    constexpr char prefix[] = "bridge hardware nv-gsp ";
    if (strncmp(command, prefix, sizeof(prefix) - 1)) return false;
    const char* verb = command + sizeof(prefix) - 1;
    if (!MarkerSet("/etc/nv-gsp")) { DebugChannelSend("{\"nv_gsp\":\"error\",\"msg\":\"/etc/nv-gsp marker not set\"}\n"); return true; }
    if (__atomic_exchange_n(&g_busy, 1u, __ATOMIC_ACQUIRE)) { DebugChannelSend("{\"nv_gsp\":\"error\",\"msg\":\"busy\"}\n"); return true; }
    if (!strcmp(verb, "status")) Status();
    else if (!strcmp(verb, "p3")) P3();
    else if (!strcmp(verb, "fini")) Fini();
    else if (!strcmp(verb, "p4")) P4(false);
    else if (!strcmp(verb, "p4 keep-display")) P4(true);
    else if (!strncmp(verb, "static", 6) && (!verb[6] || verb[6] == ' ')) {
        uint32_t off = 0; for (const char* p = verb + 6; *p; ++p) if (*p >= '0' && *p <= '9') off = off * 10 + uint32_t(*p - '0');
        StaticVerb(off);
    }
    else if (!strcmp(verb, "bar1 map")) Bar1Verb(1, 16);
    else if (!strcmp(verb, "bar1 map 2m")) Bar1Verb(1, 21);
    else if (!strcmp(verb, "bar1 unmap")) Bar1Verb(2, 0);
    else if (!strcmp(verb, "bar1 status")) Bar1Verb(0, 0);
    else if (!strncmp(verb, "p5", 2) && (!verb[2] || verb[2] == ' ')) {
        // p5 [chid N] [dev]
        uint32_t chid = kM2Chid; bool dev = false;
        const char* q = Find(verb, "chid ");
        if (q) { chid = 0; for (q += 5; *q >= '0' && *q <= '9'; ++q) chid = chid * 10 + uint32_t(*q - '0'); }
        if (Find(verb, " dev")) dev = true;
        const bool m3 = Find(verb, " m3") != nullptr;
        if (chid < 1 || chid > 511) DebugChannelSend("{\"nv_gsp\":\"error\",\"msg\":\"chid 1..511\"}\n");
        else P5(chid, dev, m3);
    }
    else if (!strcmp(verb, "ce-present on")) CePresentVerb(1);
    else if (!strcmp(verb, "ce-present off")) CePresentVerb(2);
    else if (!strcmp(verb, "ce-present status")) CePresentVerb(0);
    else if (!strncmp(verb, "log ", 4)) {
        char which[8]{}; uint32_t i = 0; const char* p = verb + 4;
        while (*p && *p != ' ' && i < 7) which[i++] = *p++;
        uint32_t off = 0; while (*p == ' ') ++p;
        for (; (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'); ++p) off = off * 16 + uint32_t(*p <= '9' ? *p - '0' : *p - 'a' + 10);
        Log(which, off);
    }
    else if (!strncmp(verb, "trace", 5) && (!verb[5] || verb[5] == ' ')) {
        uint32_t from = 0; for (const char* p = verb + 5; *p; ++p) if (*p >= '0' && *p <= '9') from = from * 10 + uint32_t(*p - '0');
        Trace(from);
    } else DebugChannelSend("{\"nv_gsp\":\"error\",\"msg\":\"Expected status|p3|p4 [keep-display]|fini|static [off]|bar1 map [2m]|unmap|status|p5|log|trace [from]\"}\n");
    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
    return true;
}

}  // namespace brook
