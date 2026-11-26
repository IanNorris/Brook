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
#include "nv_gsp/nv_gsp_m4.h"
#include "nv_gsp/nak_m4_kernel.h"
#include "nv_disp_live.h"
#include "stall_watch.h"
#include "nv_drm.h"
#include "nv_vram_heap.h"
#include "memory/mapping_ranges.h"
#include "platform_power.h"
#include "scheduler.h"
#include "process.h"
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
// Slow BAR0/PRAMIN reads through Bar0Bus (26 Nov GR idle fault: a host read that stalls ~1 s
// while GR is power-gated). Every read over 1 ms is kept (offset, duration); the drainer logs them.
struct SlowRead { uint32_t off; uint32_t us; uint64_t at_ms; };
constexpr uint32_t kSlowReads = 32;
SlowRead g_slowRead[kSlowReads]; uint32_t g_slowReadN = 0, g_slowReadLogged = 0, g_slowReadMaxUs = 0, g_slowReadMaxOff = 0;
inline void NoteSlowRead(uint32_t off, uint64_t us) {
    if (us > g_slowReadMaxUs) { g_slowReadMaxUs = uint32_t(us); g_slowReadMaxOff = off; }
    const uint32_t i = __atomic_fetch_add(&g_slowReadN, 1u, __ATOMIC_RELAXED);
    if (i < kSlowReads) g_slowRead[i] = SlowRead{off, uint32_t(us), NowUs() / 1000};
}

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
    uint32_t rd32(uint32_t off) {
        volatile uint32_t* r = Map(off);
        const uint64_t t0 = NowUs();
        const uint32_t v = r ? *r : 0xffffffffu;
        const uint64_t dt = NowUs() - t0; if (dt > 1000) NoteSlowRead(off, dt);
        Note('R', off, v); return v;
    }
    void wr32(uint32_t off, uint32_t v) { volatile uint32_t* r = Map(off); Note('W', off, v); if (r) *r = v; }
    void udelay(uint32_t us) { const uint64_t end = NowUs() + us; while (NowUs() < end) __asm__ volatile("pause"); }
    uint64_t now_us() { return NowUs(); }
    void mark(const char* p, const char* s) { phase = p; step = s; }
};
Bar0Bus g_bus;   // .bss

// ---------------------------------------------------------- live state
struct Gpu { uint8_t bus = 0, dev = 0, fn = 0; uint64_t bar0 = 0; bool ok = false; };
volatile uint32_t g_busy = 0;
struct LiveEvents { uint32_t total = 0, rc = 0, mmu = 0, os_error = 0, libos = 0, other = 0, drains = 0;
                    uint32_t pstate = 0, pstate_changes = 0, pstate_polls = 0, pstate_errors = 0; uint64_t last_ms = 0; };
LiveEvents g_liveEv;
uint32_t g_evPendLost = 0, g_seqRuns = 0, g_seqFails = 0;   // runtime event hook counters
bool g_swgen0Ack = false; uint32_t g_swgen0Acks = 0;   // /etc/nv-gsp-swgen0-ack
constexpr uint32_t kNocatKeep = 8, kNocatBytes = 1280;
uint8_t g_nocat[kNocatKeep][kNocatBytes]; uint32_t g_nocatLen[kNocatKeep], g_nocatN = 0;
// `nv-gsp nocat K`: the K-th newest kept NOCAT record (0 = newest), payload hex.
void NocatVerb(uint32_t k) {
    Out o; o.raw("{\"nv_gsp\":\"nocat\""); o.kd("kept", g_nocatN < kNocatKeep ? g_nocatN : kNocatKeep); o.kd("total", g_nocatN);
    if (k >= kNocatKeep || k >= g_nocatN) { o.ks("error", "no such record"); o.send(); return; }
    const uint32_t slot = (g_nocatN - 1 - k) % kNocatKeep, n = g_nocatLen[slot];
    o.kd("bytes", n); o.k("hex"); o.raw("\"");
    for (uint32_t i = 0; i < n; ++i) { const char* hx = "0123456789abcdef"; char c2[3] = {hx[g_nocat[slot][i] >> 4], hx[g_nocat[slot][i] & 15], 0}; o.raw(c2); }
    o.raw("\""); o.send();
}
extern const nvdrm::Backend g_nvdBackend;   // M5 live backend (below)
void NvdMarkDead(uint32_t chid);
bool g_ceActiveFlag = false;   // mirrors g_cePresentState.active for Status (declared later)
struct AutostartState { const char* status = "not run"; uint64_t start_ms = 0, p4_us = 0, ce_us = 0; } g_autostart;
bool PathExists(const char* path) { auto* f = VfsOpen(path, VFS_O_STREAM); if (!f) return false; VfsRelease(f); return true; }
bool g_p3Done = false, g_finiDone = false, g_p4Done = false;   // latched at arm (the first write)
bool g_rmRunning = false;                    // GSP-RM reached INIT_DONE this boot
uint64_t g_flushPage = 0; uint32_t g_flushRegBefore = 0, g_flushRegAfter = 0;   // SysmemFlushPageInit
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
// /etc/nv-gsp-registry: up to 16 "KEY VALUE" lines (value decimal or 0x hex), appended to the
// three default GSP-RM registry keys at P4. Keys: [A-Za-z0-9_], up to 47 chars.
void LoadExtraRegistry() {
    g_extraRegN = 0;
    auto* f = VfsOpen("/etc/nv-gsp-registry", VFS_O_STREAM); if (!f) return;
    uint64_t off = 0; static char t[1024]; __builtin_memset(t, 0, sizeof(t));
    const int n = VfsRead(f, t, sizeof(t) - 1, &off); VfsRelease(f);
    if (n <= 0) return;
    const char* p = t;
    while (*p && g_extraRegN < kMaxExtraReg) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
        if (!*p) break;
        if (*p == '#') { while (*p && *p != '\n') ++p; continue; }
        char* k = g_extraRegKeys[g_extraRegN]; uint32_t kl = 0;
        while (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') && kl < 47) k[kl++] = *p++;
        k[kl] = 0;
        while (*p == ' ' || *p == '\t' || *p == '=') ++p;
        uint32_t v = 0; bool any = false;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2;
            for (;; ++p) { const char c = *p; uint32_t d;
                if (c >= '0' && c <= '9') d = uint32_t(c - '0'); else if (c >= 'a' && c <= 'f') d = uint32_t(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = uint32_t(c - 'A' + 10); else break;
                v = v * 16 + d; any = true; } }
        else for (; *p >= '0' && *p <= '9'; ++p) { v = v * 10 + uint32_t(*p - '0'); any = true; }
        if (kl && any) { g_extraReg[g_extraRegN] = {k, v}; KPrintf("nv-gsp: registry %s = 0x%x\n", k, v); ++g_extraRegN; }
        else KPrintf("nv-gsp: /etc/nv-gsp-registry: bad line ignored\n");
        while (*p && *p != '\n') ++p;
    }
}
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
    o.kh("flush_page", g_flushPage); o.kh("flush_reg_before", g_flushRegBefore); o.kh("flush_reg", g_flushRegAfter);
}
// NV_PFB_NISO_FLUSH_SYSMEM_ADDR (0x100c10, address >> 8): the page FB/L2 flushes to sysmem
// target. nouveau programs it before booting GSP-RM on TU102 (nvkm_fb_preinit ->
// gf100_fb_sysmem_flush_page_init, one zeroed GFP_DMA32 page, kept for the driver's life); RM
// does not. Left at 0 (Brook before gsp16n), sysmem flushes went to physical 0: the 26 Nov GR
// idle fault (PCIe Engine NOCAT, PMU halt, Xid 62 ~1 s after GR work) matches that.
void FlushRange(const uint8_t* p, uint32_t n);
bool SysmemFlushPageInit() {
    if (!g_flushPage) {
        const PhysicalAddress p = PmmAllocPages(1, MemTag::Device, KernelPid);   // never freed
        if (!p || p.raw() + 4096 > (1ull << 32)) return false;
        memset(reinterpret_cast<void*>(PhysToVirt(p).raw()), 0, 4096);
        FlushRange(reinterpret_cast<const uint8_t*>(PhysToVirt(p).raw()), 4096);
        g_flushPage = p.raw();
    }
    g_flushRegBefore = g_bus.rd32(0x100c10);
    g_bus.wr32(0x100c10, uint32_t(g_flushPage >> 8));
    g_flushRegAfter = g_bus.rd32(0x100c10);
    KPrintf("nv-gsp: sysmem flush page %lx (0x100c10 %x -> %x)\n", (unsigned long)g_flushPage, g_flushRegBefore, g_flushRegAfter);
    return g_flushRegAfter == uint32_t(g_flushPage >> 8);
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
    o.k("rm_events"); o.raw("{"); o.kd("total", g_liveEv.total); o.kd("rc", g_liveEv.rc); o.kd("mmu", g_liveEv.mmu);
    o.kd("os_error", g_liveEv.os_error); o.kd("libos", g_liveEv.libos); o.kd("other", g_liveEv.other); o.kd("drains", g_liveEv.drains);
    o.kd("last_ms", g_liveEv.last_ms); o.kd("seq_runs", g_seqRuns); o.kd("seq_fails", g_seqFails); o.kd("lost", g_evPendLost); o.kd("swgen0_acks", g_swgen0Acks); o.kd("slow_reads", g_slowReadN); o.kd("slow_read_max_us", g_slowReadMaxUs); o.kh("slow_read_max_off", g_slowReadMaxOff); o.kh("pstate", g_liveEv.pstate); o.kd("pstate_polls", g_liveEv.pstate_polls);
    o.kd("pstate_changes", g_liveEv.pstate_changes); o.kd("pstate_errors", g_liveEv.pstate_errors); o.raw("}");
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
uint32_t GrStopLive();                                   // GR section below
struct FiniOutcome { bool rm = false; RmFiniResult rmf; FiniResult r; };
FiniOutcome FiniCore(uint64_t rm_budget_us) {
    FiniOutcome x;
    x.rm = g_rmRunning;
    if (x.rm) {   // GSP-RM is up (p4 this boot): UNLOADING_GUEST_DRIVER first, then the P3b sequence
        uint32_t ceFreed = 0;
        NvDrmRetract();                                           // renderD130 refuses new opens before RM goes away
        (void)CePresentStop(rm_budget_us > 2000000, &ceFreed);   // no CE submissions once RM starts to unload
        (void)GrStopLive();                                       // GR scrubber WAR teardown + free (nouveau order)
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
// The channel method buffer, sysmem: optionally naturally aligned to its power-of-two size, as
// Linux dma_alloc_coherent gives nouveau (26 Nov A/B; RmAlloc alone is only page aligned).
bool g_mthdbufAlign = false;
bool MthdbufAlloc(uint32_t bytes, M2Sys* m) {
    uint32_t al = 0x1000; while (al < bytes) al <<= 1;
    DmaRegion d;
    if (!RmAlloc(g_mthdbufAlign ? bytes + al : bytes, &d)) return false;
    const uint64_t bus = g_mthdbufAlign ? (d.bus + al - 1) & ~uint64_t(al - 1) : d.bus;
    m->cpu = d.cpu + (bus - d.bus); m->bus = bus; m->bytes = bytes;
    // one PmmAllocPages run: physically contiguous by construction (frames bus, bus+0x1000, ...)
    KPrintf("nv-gsp: method buffer %lx..%lx (%u B, %s)\n", (unsigned long)bus, (unsigned long)(bus + bytes - 1), bytes,
            g_mthdbufAlign ? "size-aligned" : "page-aligned");
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
using LiveRmApi = RmApi<RmWait, GspDoorbell<Bar0Bus, RmSync>>;
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
// BAR1 page-table pages (27 Nov fix): VRAM from a client FB arena [0x10ff00000, 0x110000000) that
// GSP-RM's static info marks client-usable (checked in Bar1MapLocked), below the GR arena; the
// old sysmem tree stays selectable with /etc/nv-gsp-bar1-sysmem-tables for A/B.
constexpr uint64_t kBar1PtLo = 0x10ff00000ull, kBar1PtHi = 0x110000000ull;
struct Bar1PageAlloc {
    bool vram = true;
    uint64_t next = kBar1PtLo;
    bool operator()(uint64_t* bus, uint8_t** cpu) {
        if (vram) { if (next + 0x1000 > kBar1PtHi) return false; *bus = next; *cpu = nullptr; next += 0x1000; return true; }
        DmaRegion d; if (!RmAlloc(0x1000, &d)) return false; *bus = d.bus; *cpu = d.cpu; return true; }
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
    pa.vram = !PathExists("/etc/nv-gsp-bar1-sysmem-tables");
    if (pa.vram && !VramRangeIsClient(g_static, kBar1PtLo, kBar1PtHi)) {
        g_bar1 = Bar1Map{}; g_bar1.status = "BAR1 table arena not in a client FB region"; return false;
    }
    KPrintf("nv-gsp: BAR1 identity tables in %s\n", pa.vram ? "VRAM (client FB arena 0x10ff00000)" : "sysmem (A/B)");
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
        if (!SysmemFlushPageInit()) return false;
        armUs = NowUs();
        g_rm->wait_deadline_us = armUs + 15000000;   // INIT_DONE took ~0.9 s after booter under Linux
        return true;
    };
    // keep-display: no compositor/nv-disp BAR1 traffic from the boot until BAR1 is remapped
    SpinLock* lock = keepDisplay ? NvAuxSharedLock() : nullptr;
    if (lock) SpinLockAcquire(lock);
    const uint64_t lockUs = NowUs();
    LoadExtraRegistry();
    g_grSkipScrubber = PathExists("/etc/nv-gsp-no-scrubber");
    g_swgen0Ack = PathExists("/etc/nv-gsp-swgen0-ack");
    g_sysInfoPrimary = PathExists("/etc/nv-gsp-primary");
    if (g_sysInfoPrimary) KPrintf("nv-gsp: SET_SYSTEM_INFO bIsPrimary=1 (/etc/nv-gsp-primary)\n");
    if (g_swgen0Ack) KPrintf("nv-gsp: SWGEN0 ack in the event drainer (/etc/nv-gsp-swgen0-ack)\n");
    if (g_grSkipScrubber) KPrintf("nv-gsp: GR scrubber channel skipped (/etc/nv-gsp-no-scrubber)\n");
    static P4Result r;
    r = RunP4(g_bus, c, f.in, g_dma, pci, booter, bimg, &g_rm->objs, g_rm->ch, g_rm->rx, g_rm->rxcap,
              prepare, arm, wait, g_rmSync);
    g_bus.rec = false;
    g_rmRunning = r.init_done;
    if (r.static_ok) { g_static = r.si; memcpy(g_staticRaw, r.static_raw, kStaticInfoBytes); g_staticValid = true; }
    bool mapped = false;
    // 27 Nov split: keep-display boot without patching RM's BAR1 page directory (/etc/nv-gsp-no-bar1-identity)
    if (lock && r.init_done && r.si.ok && PathExists("/etc/nv-gsp-no-bar1-identity"))
        KPrintf("nv-gsp: keep-display P4 without the BAR1 identity map (/etc/nv-gsp-no-bar1-identity)\n");
    else if (lock && r.init_done && r.si.ok) mapped = Bar1MapLocked(16);
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
    if (r < 0) {
        g_cePresentState.active = false; g_ceActiveFlag = false;
        const CeTimeout& t = g_cePresentState.last_timeout;
        KPrintf("nv-gsp: CE present wait failed (%s) at %lu ms: kick age %lu us, poll gap %lu us, spin %lu us, "
                "sem 0x%x want 0x%x, %u polls, job %lu B\n", g_cePresentState.last_error, (unsigned long)(t.at_us / 1000),
                (unsigned long)t.kick_age_us, (unsigned long)t.poll_gap_us, (unsigned long)t.spin_us, t.sem, t.want, t.polls,
                (unsigned long)t.job_bytes);
        StallWatchTrigger("ce timeout");
    }
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
    { const CeTimeout& t = cp.last_timeout;
      o.k("last_timeout"); o.raw("{"); o.kd("at_ms", t.at_us / 1000); o.kd("kick_age_us", t.kick_age_us); o.kd("poll_gap_us", t.poll_gap_us);
      o.kd("spin_us", t.spin_us); o.kh("sem", t.sem); o.kh("want", t.want); o.kd("polls", t.polls); o.kd("job_bytes", t.job_bytes); o.raw("}"); }
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

namespace {
void AutostartRun(bool display);
void AutostartHeadlessThread(void*) {
    SchedulerSleepMs(10000);   // root filesystem, USB system and network first
    AutostartRun(false);
}
}  // namespace
void NvGspAutostart() { AutostartRun(true); }
// Headless boot (no GOP, BootTimingHeadless): GSP-RM still boots, with a plain p4 (no heads to
// keep) and no CE present; the GPU works without displays and RM's power management still
// applies. Runs on its own thread so it never delays the boot.
void NvGspAutostartHeadless() {
    Process* t = KernelThreadCreate("nv_gsp_auto", AutostartHeadlessThread, nullptr, 3);
    if (t) SchedulerAddProcess(t); else KPrintf("nv-gsp: headless autostart thread failed\n");
}
namespace {
// ---- RM async events outside RPCs (26 Nov): every 500 ms, when no verb runs and the nv-aux
// lock is free, drain the message queue and log each event with its tick. RC (exceptType =
// Xid), MMU fault and OS error records are decoded; LIBOS prints are only counted. With
// /etc/nv-gsp-pstate-poll, GET_CURRENT_PSTATE every 5 s on RM's internal subdevice, logged on
// change (off by default until it is shown safe on its own).
struct NoWait { bool operator()() { return false; } };
// Events captured under the nv-aux lock (RPC waits and the drainer), logged by the drainer later.
constexpr uint32_t kEvPend = 16, kEvPendBytes = 1536;   // NOCAT records are 1244 B
alignas(8) uint8_t g_evPend[kEvPend][kEvPendBytes];
uint32_t g_evPendLen[kEvPend], g_evPendN = 0;
void RuntimeEvent(const uint8_t* m, uint32_t len) {   // nv-aux lock held
    const uint32_t fn = Get32(m + offsetof(rm570::rpc_message_header_v, function));
    if (fn == rm570::NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER && g_rm && len > kRpcHdr) {
        // GSP-RM asks the CPU to run a register sequence (nouveau r535_gsp_msg_run_cpu_sequencer)
        const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
        if (c) {
            SeqBus<Bar0Bus> sb{g_bus, GspCoreOps<Bar0Bus>{g_bus, *c, g_rm->objs.libos.bus, g_rm->objs.app_version}};
            const SeqResult r = RunSequencer(sb, m + kRpcHdr, len - kRpcHdr);
            ++g_seqRuns; if (r.status != SeqStatus::Ok) ++g_seqFails;
        }
    }
    if (fn == rm570::NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT) { ++g_liveEv.libos; return; }
    if (g_evPendN >= kEvPend) { ++g_evPendLost; return; }
    const uint32_t keep = len < kEvPendBytes ? len : kEvPendBytes;
    __builtin_memcpy(g_evPend[g_evPendN], m, keep); g_evPendLen[g_evPendN] = keep; ++g_evPendN;
}
void FaultShadowDrain();
void LogEvent(const uint8_t* m, uint32_t len) {
    namespace rm = rm570;
    const uint32_t fn = Get32(m + offsetof(rm::rpc_message_header_v, function));
    const uint8_t* p = m + kRpcHdr;
    const uint32_t n = len > kRpcHdr ? len - kRpcHdr : 0;
    auto u32 = [&](uint32_t off) { return off + 4 <= n ? Get32(p + off) : 0u; };
    const unsigned long ms = (unsigned long)(NowUs() / 1000);
    ++g_liveEv.total; g_liveEv.last_ms = ms;
    switch (fn) {
        case rm::NV_VGPU_MSG_EVENT_RC_TRIGGERED:
            ++g_liveEv.rc;
            KPrintf("nv-gsp: RM event at %lu ms: RC Xid %u engine %u chid %u level %u scope %u fault %x:%08x type %u\n", ms,
                    u32(16), u32(0), u32(4), u32(12), u32(20), u32(32), u32(28), u32(36));
            NvdMarkDead(u32(4));
            StallWatchTrigger("rm rc");
            break;
        case rm::NV_VGPU_MSG_EVENT_MMU_FAULT_QUEUED: ++g_liveEv.mmu; KPrintf("nv-gsp: RM event at %lu ms: MMU fault queued\n", ms); FaultShadowDrain(); break;
        case rm::NV_VGPU_MSG_EVENT_OS_ERROR_LOG: {
            ++g_liveEv.os_error;
            char text[97]; uint32_t i = 0;
            for (; i < 96 && 12 + i < n && p[12 + i]; ++i) text[i] = (p[12 + i] >= 32 && p[12 + i] < 127) ? char(p[12 + i]) : '.';
            // (the string is 256 bytes; the first 96 are logged)
            text[i] = 0;
            KPrintf("nv-gsp: RM event at %lu ms: OS error Xid %u runlist %u chid %u: %s\n", ms, u32(0), u32(4), u32(8), text);
            break;
        }
        case rm::NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT: ++g_liveEv.libos; break;
        case rm::NV_VGPU_MSG_EVENT_GSP_POST_NOCAT_RECORD: {
            // NV2080CtrlNocatJournalInsertRecord (ctrl2080nvd.h): flags@0, timestamp@8, recType@16,
            // bugcheck@20, source[65]@24, subsystem@92, errorCode@96, faultingEngine[65]@104,
            // tdrReason@172, diagBufferLen@176, diagBuffer[1024]@180
            ++g_liveEv.other;
            auto str = [&](uint32_t off, char* out, uint32_t cap) {
                uint32_t i = 0;
                for (; i + 1 < cap && off + i < n && p[off + i]; ++i) out[i] = (p[off + i] >= 32 && p[off + i] < 127) ? char(p[off + i]) : '.';
                out[i] = 0; };
            char src[66], eng[66];
            str(24, src, sizeof(src)); str(104, eng, sizeof(eng));
            const uint32_t rec = n > 16 ? p[16] : 0, diag = u32(176);
            KPrintf("nv-gsp: RM event at %lu ms: NOCAT type %u bugcheck %x source \"%s\" subsystem %u error %x:%08x engine \"%s\" tdr %u diag %u B\n",
                    ms, rec, u32(20), src, u32(92), u32(100), u32(96), eng, u32(172), diag);
            for (uint32_t o = 0; o < 128 && o < diag && 180 + o + 16 <= n; o += 16)
                KPrintf("nv-gsp:   diag+%02x %08x %08x %08x %08x\n", o, u32(180 + o), u32(184 + o), u32(188 + o), u32(192 + o));
            {   // keep the last 8 records whole for `nv-gsp nocat N` (diag is a protobuf register dump)
                const uint32_t slot = g_nocatN++ % kNocatKeep, keep = n < kNocatBytes ? n : kNocatBytes;
                __builtin_memcpy(g_nocat[slot], p, keep); g_nocatLen[slot] = keep;
            }
            break;
        }
        default: ++g_liveEv.other; KPrintf("nv-gsp: RM event at %lu ms: function 0x%x (%u bytes)\n", ms, fn, len); break;
    }
}
void EventThread(void*) {
    uint32_t ticks = 0;
    alignas(8) static uint8_t buf[kMaxRpcBytes];
    constexpr uint32_t kPend = 16, kPendBytes = 1536;   // NOCAT records are 1244 B; RC/OS error are small
    alignas(8) static uint8_t pend[kPend][kPendBytes];
    static uint32_t pend_len[kPend];
    uint32_t npend = 0;
    for (;;) {
        SchedulerSleepMs(500);
        ++ticks;
        if (!g_rmRunning || !g_rm || !g_rm->ch || g_finiDone) continue;
        if (__atomic_exchange_n(&g_busy, 1u, __ATOMIC_ACQUIRE)) continue;   // a verb is talking to RM
        SpinLock* lock = NvAuxSharedLock();
        if (!SpinLockTryAcquire(lock)) { __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE); continue; }
        if (g_rmRunning) {
            // A/B (/etc/nv-gsp-swgen0-ack): ack the GSP falcon's SWGEN0 "message queued" interrupt
            // each drain, as nouveau's r535_gsp_intr does (IRQSTAT 0x110008, IRQSCLR 0x110004).
            if (g_swgen0Ack && (g_bus.rd32(0x110008) & 0x40)) { g_bus.wr32(0x110004, 0x40); ++g_swgen0Acks; }
            FlushRange(g_rm->objs.q.msgq(), kQueueBytes);
            NoWait nw;
            // copy events out under the lock (bounded), log them after releasing it: serial
            // KPrintf of long records under the spinlock stalled the system ~1 s (26 Nov)
            for (int i = 0; i < 16 && g_rm->ch->MsgPending(); ++i) {
                uint32_t len = 0;
                if (g_rm->ch->Recv(buf, sizeof(buf), &len, 0, nw) != QStatus::Ok) break;
                RuntimeEvent(buf, len);
                FlushRange(g_rm->objs.q.msgq(), kQueueBytes);
            }
            npend = g_evPendN < kPend ? g_evPendN : kPend;
            for (uint32_t i = 0; i < npend; ++i) { __builtin_memcpy(pend[i], g_evPend[i], g_evPendLen[i]); pend_len[i] = g_evPendLen[i]; }
            g_evPendN = 0;
            ++g_liveEv.drains;
            if (ticks % 10 == 0 && g_staticValid && PathExists("/etc/nv-gsp-pstate-poll")) {
                const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
                RmWait wait; g_rm->wait_deadline_us = NowUs() + 1000000;
                GspDoorbell<Bar0Bus, RmSync> bell{g_bus, *c, g_rmSync};
                alignas(8) static uint8_t scratch[kRmApiBuf];
                LiveRmApi api(g_rm->ch, g_rm->rx, g_rm->rxcap, wait, bell, scratch);
                uint32_t ps = 0;
                ++g_liveEv.pstate_polls;
                if (api.Control(g_static.h_internal_client, g_static.h_internal_subdevice, 0x20802068u, &ps, sizeof(ps))) {
                    if (ps != g_liveEv.pstate) {
                        KPrintf("nv-gsp: pstate mask 0x%x -> 0x%x at %lu ms\n", g_liveEv.pstate, ps, (unsigned long)(NowUs() / 1000));
                        g_liveEv.pstate = ps; ++g_liveEv.pstate_changes;
                    }
                } else ++g_liveEv.pstate_errors;
                g_liveEv.total += api.ev.total; g_liveEv.rc += api.ev.rc; g_liveEv.mmu += api.ev.mmu_fault;
            }
        }
        SpinLockRelease(lock);
        __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
        for (uint32_t i = 0; i < npend; ++i) LogEvent(pend[i], pend_len[i]);
        npend = 0;
        for (const uint32_t n = g_slowReadN < kSlowReads ? g_slowReadN : kSlowReads; g_slowReadLogged < n; ++g_slowReadLogged) {
            const SlowRead& sr = g_slowRead[g_slowReadLogged];
            KPrintf("nv-gsp: slow BAR0 read 0x%x took %u us at %lu ms\n", sr.off, sr.us, (unsigned long)sr.at_ms);
        }
    }
}
void EventThreadStart() {
    static bool started = false;
    if (started) return;
    started = true;
    Process* t = KernelThreadCreate("nv_gsp_ev", EventThread, nullptr, 2);
    if (t) SchedulerAddProcess(t); else KPrintf("nv-gsp: event thread failed\n");
}
}  // namespace

namespace {
void AutostartRun(bool display) {
    if (!MarkerSet("/etc/nv-gsp")) { g_autostart.status = "no /etc/nv-gsp"; return; }
    // 27 Nov display A/B split: nv-disp keeps driving the heads, but GSP-RM boots as if headless
    // (plain p4: no keep-display, no BAR1 identity map, no CE present, no head-0 fence).
    if (display && PathExists("/etc/nv-gsp-headless-p4")) { KPrintf("nv-gsp: autostart headless p4 with nv-disp running (/etc/nv-gsp-headless-p4)\n"); display = false; }
    if (PathExists("/etc/nv-gsp-no-autostart")) { g_autostart.status = "opted out"; KPrintf("nv-gsp: autostart off (/etc/nv-gsp-no-autostart)\n"); return; }
    if (display) SchedulerSleepMs(3000);   // let the desktop settle after the head bring-up
    bool got = false;
    for (int i = 0; i < 100 && !(got = !__atomic_exchange_n(&g_busy, 1u, __ATOMIC_ACQUIRE)); ++i) SchedulerSleepMs(100);
    if (!got) { g_autostart.status = "busy (a verb held the GPU)"; return; }
    g_autostart.start_ms = NowUs() / 1000;
    g_outQuiet = true;
    uint64_t t0 = NowUs();
    // Fence head-0 flips across the RM bring-up and CE present on (26 Nov: window 0 was re-armed
    // with a foreign ISO ctxdma 0xfb000000 while flips ran, and head 0 died with an exception).
    const NvHead0Arm before = display ? NvDispHead0Fence(true, "p4 start") : NvHead0Arm{};
    P4(display);   // keep-display only when heads are lit
    g_autostart.p4_us = NowUs() - t0;
    if (!display) g_autostart.status = g_rmRunning ? "rm (headless)" : "p4 failed (no RM)";
    else if (g_rmRunning && g_bar1Mapped && PathExists("/etc/nv-gsp-no-ce-present")) {
        g_autostart.status = "rm up, CE present off (/etc/nv-gsp-no-ce-present)";   // 27 Nov display A/B split
    } else if (g_rmRunning && g_bar1Mapped) {
        t0 = NowUs(); CePresentVerb(1); g_autostart.ce_us = NowUs() - t0;
        g_autostart.status = g_cePresentState.active ? "rm + ce present" : "rm up, ce present failed (CPU copies)";
    } else g_autostart.status = g_rmRunning ? "rm up, BAR1 not mapped (no CE)" : "p4 failed (no RM)";
    if (display) {
        const NvHead0Arm after = NvDispHead0Fence(false, "rm + ce up");
        if (before.live && (after.iso != before.iso || after.offset != before.offset || !after.live))
            KPrintf("nv-gsp: head 0 window ARM changed across the RM bring-up: iso %x -> %x, offset %x -> %x, live %d -> %d\n",
                    before.iso, after.iso, before.offset, after.offset, before.live ? 1 : 0, after.live ? 1 : 0);
    }
    g_outQuiet = false;
    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
    if (g_rmRunning) {
        g_rmEventHook = RuntimeEvent;
        EventThreadStart();
        // M5.1: publish the device for renderD130 (Mesa NVK). Classes: what nouveau lists for a
        // TU104 channel (3D, compute, copy, 2D, inline-to-memory). GPC/TPC: RTX 2080 (6 / 46);
        // TODO read them from RM (GR info) once a safe control is chosen.
        nvdrm::DeviceInfo d;
        const uint32_t boot0 = g_bus.rd32(0);
        d.chipset = uint16_t((boot0 >> 20) & 0x1ff); d.revision = uint8_t(boot0 & 0xff); d.pci_device = 0x1e87;
        d.vram_bytes = g_static.fb_length; d.vram_user = g_static.usable_hi > g_static.usable_lo ? g_static.usable_hi - g_static.usable_lo : 0;
        d.bar1_bytes = kBar1Bytes;
        // GR geometry from RM, as nouveau r570 does (GPU_GET_FERMI_GPC_INFO / _TPC_INFO on the internal
        // subdevice): GRAPH_UNITS = gpcs | tpcs << 8. 27 Nov: we hard-coded 46 "TPCs"; the RTX 2080 has
        // 46 SMs = 23 TPCs, and NVK sizes SLM and per-TPC state from this.
        {
            uint32_t gpcs = 0, tpcs = 0; bool ok = false;
            const ChipOps* cc = ChipForBoot0(g_bus.rd32(0));
            bool busy = false;
            for (int i = 0; i < 50 && !(busy = !__atomic_exchange_n(&g_busy, 1u, __ATOMIC_ACQUIRE)); ++i) SchedulerSleepMs(10);
            if (cc && g_staticValid && busy) {
                SpinLock* lk = NvAuxSharedLock(); SpinLockAcquire(lk);
                RmWait wait; g_rm->wait_deadline_us = NowUs() + 2000000;
                GspDoorbell<Bar0Bus, RmSync> bell{g_bus, *cc, g_rmSync};
                alignas(8) static uint8_t scratch[kRmApiBuf];
                LiveRmApi api(g_rm->ch, g_rm->rx, g_rm->rxcap, wait, bell, scratch);
                uint32_t gm = 0;
                ok = api.Control(g_static.h_internal_client, g_static.h_internal_subdevice, 0x20800137u, &gm, sizeof(gm));
                for (uint32_t g = 0; ok && g < 32; ++g) if (gm & (1u << g)) {
                    struct { uint32_t gpcId, tpcMask; } t = {g, 0};
                    ok = api.Control(g_static.h_internal_client, g_static.h_internal_subdevice, 0x20800138u, &t, sizeof(t));
                    tpcs += __builtin_popcount(t.tpcMask); ++gpcs;
                }
                SpinLockRelease(lk);
                if (ok) KPrintf("nv-drm: GR units from RM: gpc mask %x, %u GPC, %u TPC\n", gm, gpcs, tpcs);
            }
            if (busy) __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
            if (!ok || !gpcs || !tpcs) { gpcs = 6; tpcs = 23; KPrintf("nv-drm: GR units query failed; using RTX 2080 defaults 6 GPC / 23 TPC\n"); }
            d.gpc_count = uint8_t(gpcs); d.tpc_count = uint16_t(tpcs);
        }
        const uint32_t cls[] = {0xc597, 0xc5c0, 0xc5b5, 0x902d, 0xa140};
        for (uint32_t i = 0; i < 5; ++i) d.classes[i] = cls[i];
        d.nclasses = 5;
        const char chip[] = "TU104", name[] = "NVIDIA GeForce RTX 2080";
        __builtin_memcpy(d.chip, chip, sizeof(chip)); __builtin_memcpy(d.name, name, sizeof(name));
        d.bar1_bytes = 0;   // no CPU-visible VRAM window yet (BAR1 is the 256 MiB identity map): VRAM|MAPPABLE -> GART
        NvDrmPublish(d, &g_nvdBackend);
    }
    KPrintf("nv-gsp: autostart %s (p4 %lu us, ce %lu us)\n", g_autostart.status, (unsigned long)g_autostart.p4_us,
            (unsigned long)g_autostart.ce_us);
    if (display) {   // a delayed re-arm (RM work after INIT_DONE) shows up here
        const NvHead0Arm a0 = NvDispHead0Arm();
        SchedulerSleepMs(2000);
        const NvHead0Arm a1 = NvDispHead0Arm();
        KPrintf("nv-gsp: head 0 check +2 s: live %d, iso %x -> %x (flips move the offset)\n", a1.live ? 1 : 0, a0.iso, a1.iso);
    }
}
}  // namespace

namespace {
// ------------------------------------------------------------------ M4: GR
// `gr golden` (once per boot, after p4): context-buffer info, the golden context (TURING_A on
// an RM-managed-VAS channel), the TU10x scrubber. VRAM from [4 GiB + 256 MiB, +512 MiB).
GrState g_gr; GrResult g_grLast; bool g_grTried = false;
uint32_t GrStopLive() {
    if (!g_gr.up || !g_rmRunning || !g_rm) return 0;
    const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
    SpinLock* lock = NvAuxSharedLock();
    SpinLockAcquire(lock);
    Pramin<Bar0Bus> pr(g_bus, *c); PraminIo vio{pr};
    RmWait wait; g_rm->wait_deadline_us = NowUs() + 1000000;
    GspDoorbell<Bar0Bus, RmSync> bell{g_bus, *c, g_rmSync};
    alignas(8) static uint8_t scratch[kRmApiBuf];
    LiveRmApi api(g_rm->ch, g_rm->rx, g_rm->rxcap, wait, bell, scratch);
    GrResult r;
    const uint32_t f = GrStop(g_bus, *c, vio, api, g_gr, r);
    pr.Restore();
    SpinLockRelease(lock);
    return f;
}
void GrJson(Out& o, const GrResult& r) {
    o.ks("stage", r.stage); o.ks("status", r.status); o.kh("rm_status", r.rm_status); o.kh("rpc_result", r.rpc_result);
    o.kd("q", uint32_t(r.q)); o.kb("golden_ok", r.golden_ok); o.kb("scrubber_ok", r.scrubber_ok);
    o.kd("golden_entries", r.golden_entries); o.kd("scrubber_entries", r.scrubber_entries);
    o.kd("allocated", r.allocated); o.kd("freed", r.freed); o.kh("vram_used", r.vram_used);
    o.kd("events_total", r.ev.total); o.kd("events_rc", r.ev.rc); o.kd("events_mmu", r.ev.mmu_fault);
    o.kh("last_other_fn", r.ev.last_other_fn);
    o.k("bufs"); o.raw("[");
    for (uint32_t i = 0; i < r.bufs.n; ++i) {
        const GrBuf& b = r.bufs.b[i];
        if (i) o.raw(",");
        o.raw("{"); o.kd("id", b.id); o.kh("size", b.size); o.kd("page", b.page); o.kd("align", b.align);
        o.kb("global", b.global); o.kb("init", b.init); o.kb("ro", b.ro); o.kh("vram", b.vram); o.raw("}");
    }
    o.raw("]");
    o.k("raw_sizes"); o.raw("[");
    for (uint32_t i = 0; i < rm570::gr::kEngineIdCount; ++i) { if (i) o.raw(","); o.dec(r.bufs.raw_sizes[i]); }
    o.raw("]");
}
// `perf status|boost|clear`: RM perf controls on the scrubber scope's subdevice (needs `gr golden`).
// status = GET_CURRENT_PSTATE only. boost is accepted by RM but did not leave P8 on 26 Nov.
// boost = NV2080_CTRL_CMD_PERF_BOOST's GSP half (INTERNAL_PERF_BOOST_SET_2X), BOOST_TO_MAX for
// 60 s: RM's own DVFS policy within the board's stock limits; it expires by itself, `perf clear` ends it.
// `nv-gsp fault dump`: TU102 MMU fault state (nouveau tu102 fault regs): both fault buffers'
// LO/HI/GET/PUT/SIZE (0xb83000 + 0x20*i), the info-fault latch (0xb83080-0xb83094), and the
// newest non-replayable entries when that buffer is in VRAM (gv100 entry layout, 32 B).
// Read-only; PRAMIN under the nv-aux lock.
void FaultDump() {
    Out o; o.raw("{\"nv_gsp\":\"fault\"");
    const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
    if (!c) { o.ks("refused", "no GPU"); o.send(); return; }
    SpinLock* lock = NvAuxSharedLock();
    SpinLockAcquire(lock);
    uint32_t b[2][5];
    for (int i = 0; i < 2; ++i) for (int k = 0; k < 5; ++k) b[i][k] = g_bus.rd32(0xb83000 + 0x20 * i + 4 * k);
    uint32_t info[6]; for (int k = 0; k < 6; ++k) info[k] = g_bus.rd32(0xb83080 + 4 * k);
    struct E { uint32_t w[8]; } ent[8]; uint32_t ne = 0;
    {   // buffer 1 = non-replayable: LO bits 1:0 aperture (0 VRAM), PUT/GET in entries
        const uint64_t base = (uint64_t(b[1][1]) << 32) | (b[1][0] & ~0xfffu);
        const uint32_t size = b[1][4] & 0xfffff, put = b[1][3] & 0xfffff;
        if ((b[1][0] & 3) == 0 && base && size) {
            Pramin<Bar0Bus> pr(g_bus, *c);
            for (uint32_t k = 0; k < 8 && k < size; ++k) {
                const uint32_t idx = (put + size - 1 - k) % size;
                for (int w = 0; w < 8; ++w) ent[ne].w[w] = pr.rd32(base + uint64_t(idx) * 32 + 4 * w);
                ++ne;
            }
            pr.Restore();
        }
    }
    SpinLockRelease(lock);
    for (int i = 0; i < 2; ++i) {
        o.k(i ? "nonreplay" : "replay"); o.raw("{"); o.kh("lo", b[i][0]); o.kh("hi", b[i][1]); o.kh("get", b[i][2]);
        o.kh("put", b[i][3]); o.kh("size", b[i][4]); o.raw("}");
    }
    o.k("info"); o.raw("{"); o.kh("addr_lo", info[0]); o.kh("addr_hi", info[1]); o.kh("info0", info[2]); o.kh("inst_hi", info[3]);
    o.kh("info1", info[4]); o.kh("valid", info[5]); o.raw("}");
    o.k("entries"); o.raw("[");
    for (uint32_t k = 0; k < ne; ++k) {
        const uint32_t* w = ent[k].w;
        if (k) o.raw(",");
        o.raw("{"); o.kh("inst", (uint64_t(w[1]) << 32) | w[0]); o.kh("addr", (uint64_t(w[3]) << 32) | w[2]);
        o.kh("engine", w[6] & 0xff); o.kh("valid", w[7] >> 31); o.kh("gpc", (w[7] >> 24) & 0x1f); o.kh("hub", (w[7] >> 20) & 1);
        o.kh("access", (w[7] >> 16) & 0xf); o.kh("client", (w[7] >> 8) & 0x7f); o.kh("reason", w[7] & 0x1f); o.raw("}");
    }
    o.raw("]"); o.send();
}
// Client shadow non-replayable fault buffer (27 Nov, NVK Xid 44 decode), as CPU-RM's
// kgmmuClientShadowFaultBufferRegister: GMMU_GET_STATIC_INFO sizes, a Queue page (capacity @0,
// msgSize @24, getIdx @64, putIdx @128; containers/queue.h) and the packet pages, registered with
// INTERNAL_GMMU_REGISTER_CLIENT_SHADOW_FAULT_BUFFER. GSP-RM then copies each non-replayable fault
// (NVC369_BUF_ENTRY, 32 B) into it and posts MMU_FAULT_QUEUED; LogEvent decodes new entries.
uint8_t* g_fsQueue = nullptr; uint8_t* g_fsBuf = nullptr; uint32_t g_fsCap = 0;
inline uint64_t FsGet64(const uint8_t* p) { uint64_t v; __builtin_memcpy(&v, p, 8); return v; }
inline void FsPut64(uint8_t* p, uint64_t v) { __builtin_memcpy(p, &v, 8); }
void FaultShadowOn() {
    Out o; o.raw("{\"nv_gsp\":\"fault shadow\"");
    const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
    if (!c || !g_rmRunning || !g_staticValid) { o.ks("refused", "GSP-RM not running"); o.send(); return; }
    if (g_fsQueue) { o.ks("status", "already on"); o.send(); return; }
    struct Sizes { uint32_t rsize, rmeta, nsize, nmeta; } sz = {};
    struct Reg { uint64_t queuePhys; uint32_t size, metaSize; uint64_t pte[3000]; uint32_t type, pad; uint64_t sharedPhys; };
    static Reg reg; __builtin_memset(&reg, 0, sizeof(reg));
    static_assert(sizeof(Reg) == 24032, "REGISTER_CLIENT_SHADOW_FAULT_BUFFER_PARAMS");
    SpinLock* lock = NvAuxSharedLock(); SpinLockAcquire(lock);
    RmWait wait; g_rm->wait_deadline_us = NowUs() + 5000000;
    GspDoorbell<Bar0Bus, RmSync> bell{g_bus, *c, g_rmSync};
    alignas(8) static uint8_t scratch[32768];
    LiveRmApi api(g_rm->ch, g_rm->rx, g_rm->rxcap, wait, bell, scratch); api.bufcap = sizeof(scratch);
    const char* st = "ok";
    bool ok = api.Control(g_static.h_internal_client, g_static.h_internal_subdevice, 0x20800a59u, &sz, sizeof(sz));
    if (!ok) st = "GMMU_GET_STATIC_INFO";
    DmaRegion q{}, b{};
    const uint32_t pages = ((sz.nsize + 4095) / 4096) + ((sz.nmeta + 4095) / 4096);
    if (ok && (!sz.nsize || pages > 3000)) { ok = false; st = "bad sizes"; }
    if (ok && (!RmAlloc(4096, &q) || !RmAlloc(pages * 4096, &b))) { ok = false; st = "sysmem"; }
    if (ok) {
        g_fsCap = sz.nsize / 32;
        FsPut64(q.cpu + 0, g_fsCap); FsPut64(q.cpu + 24, 32); FsPut64(q.cpu + 64, 0); FsPut64(q.cpu + 128, 0);
        FlushRange(q.cpu, 4096); FlushRange(b.cpu, pages * 4096);
        reg.queuePhys = q.bus; reg.size = sz.nsize; reg.metaSize = sz.nmeta; reg.type = 0;   // NON_REPLAYABLE
        for (uint32_t i = 0; i < pages; ++i) reg.pte[i] = b.bus + uint64_t(i) * 4096;
        ok = api.Control(g_static.h_internal_client, g_static.h_internal_subdevice, 0x20800a9du, &reg, sizeof(reg));
        if (!ok) st = "REGISTER_CLIENT_SHADOW_FAULT_BUFFER";
    }
    const uint32_t rm = api.status, qs = uint32_t(api.q);
    SpinLockRelease(lock);
    if (ok) { g_fsQueue = q.cpu; g_fsBuf = b.cpu; }
    o.ks("status", st); o.kh("rm_status", rm); o.kd("queue", qs); o.kd("nr_size", sz.nsize); o.kd("nr_meta", sz.nmeta);
    o.kd("capacity", g_fsCap); o.kh("queue_phys", q.bus); o.kh("buf_phys", b.bus); o.send();
    KPrintf("nv-gsp: client shadow fault buffer: %s (rm 0x%x), %u entries\n", st, rm, g_fsCap);
}
void FaultShadowDrain() {   // drainer context; sysmem only
    if (!g_fsQueue) return;
    FlushRange(g_fsQueue, 192);
    uint64_t get = FsGet64(g_fsQueue + 64); const uint64_t put = FsGet64(g_fsQueue + 128);
    {   // raw view (first decode of this buffer layout): queue words and any non-empty slot among the first 16
        KPrintf("nv-gsp: shadow queue cap %lx msg %lx get %lx put %lx [8] %lx [16] %lx\n", (unsigned long)FsGet64(g_fsQueue),
                (unsigned long)FsGet64(g_fsQueue + 24), (unsigned long)get, (unsigned long)put,
                (unsigned long)FsGet64(g_fsQueue + 8), (unsigned long)FsGet64(g_fsQueue + 16));
        FlushRange(g_fsBuf, 16 * 32);
        for (uint32_t k = 0; k < 16; ++k) {
            const uint8_t* e = g_fsBuf + k * 32; uint32_t any = 0; for (int w = 0; w < 8; ++w) any |= Get32(e + 4 * w);
            if (any) KPrintf("nv-gsp: shadow slot %u: %08x %08x %08x %08x %08x %08x %08x %08x\n", k, Get32(e), Get32(e + 4), Get32(e + 8),
                             Get32(e + 12), Get32(e + 16), Get32(e + 20), Get32(e + 24), Get32(e + 28));
        }
    }
    for (uint32_t n = 0; get != put && n < 16; ++n) {
        const uint8_t* e = g_fsBuf + (get % g_fsCap) * 32;
        FlushRange(e, 32);
        const uint32_t w0 = Get32(e), w1 = Get32(e + 4), w2 = Get32(e + 8), w3 = Get32(e + 12), w6 = Get32(e + 24), w7 = Get32(e + 28);
        KPrintf("nv-gsp: MMU FAULT va %08x%08x inst %08x%08x (ap %u) engine 0x%x fault_type 0x%x client 0x%x access 0x%x "
                "mmu_client_type %u gpc %u replayable %u valid %u\n",
                w3, w2 & ~0xfffu, w1, w0 & ~0xfffu, (w0 >> 8) & 3, w6 & 0x1ff, w7 & 0x1f, (w7 >> 8) & 0x7f, (w7 >> 16) & 0xf,
                (w7 >> 20) & 1, (w7 >> 24) & 0x1f, (w7 >> 7) & 1, w7 >> 31);
        ++get;
    }
    FsPut64(g_fsQueue + 64, get); FlushRange(g_fsQueue + 64, 8);
}
void PerfVerb(int mode) {   // 0 status, 1 boost, 2 clear
    Out o; o.raw("{\"nv_gsp\":\"perf\"");
    const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
    if (!c || !g_rmRunning || !g_gr.up) { o.ks("refused", "needs GSP-RM and `gr golden` this boot"); o.send(); return; }
    SpinLock* lock = NvAuxSharedLock();
    SpinLockAcquire(lock);
    RmWait wait; g_rm->wait_deadline_us = NowUs() + 5000000;
    GspDoorbell<Bar0Bus, RmSync> bell{g_bus, *c, g_rmSync};
    alignas(8) static uint8_t scratch[kRmApiBuf];
    LiveRmApi api(g_rm->ch, g_rm->rx, g_rm->rxcap, wait, bell, scratch);
    const uint32_t hc = g_gr.scrub.base, hs = g_gr.scrub.base + grh::kSubdevice;
    if (mode == 1 || mode == 2) {
        struct { uint8_t flags; uint8_t pad[3]; uint32_t duration; } b = {uint8_t(mode == 1 ? 2 : 0), {}, mode == 1 ? 60u : 0u};
        static_assert(sizeof(b) == 8, "INTERNAL_PERF_BOOST_SET_PARAMS_2X");
        const bool ok = api.Control(hc, hs, 0x20800a9au, &b, sizeof(b));   // NV2080_CTRL_CMD_INTERNAL_PERF_BOOST_SET_2X
        o.kb(mode == 1 ? "boost_ok" : "clear_ok", ok); o.kh("boost_status", api.status);
    }
    uint32_t pstate = 0;
    const bool pok = api.Control(hc, hs, 0x20802068u, &pstate, sizeof(pstate));   // NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE
    o.kb("pstate_ok", pok); o.kh("pstate_status", api.status); o.kh("pstate_mask", pstate);   // bit n = Pn
    // No GET_LEVEL_INFO_V2 here: on 26 Nov an all-domain-bits probe of it (rm 0x1f) was followed by
    // every RM_ALLOC failing NV_ERR_RESET_REQUIRED (PDB_PROP_GPU_IN_FATAL_ERROR) for the rest of the
    // boot. Clock domains are not in the open 570.144 headers; don't probe them.
    SpinLockRelease(lock);
    KPrintf("nv-gsp: perf %s: pstate mask 0x%x (rm 0x%x)\n", mode == 1 ? "boost" : mode == 2 ? "clear" : "status", pstate, api.status);
    o.send();
}
void GrVerb(int mode) {   // 1 golden, 2 off, 0 status
    Out o; o.raw("{\"nv_gsp\":\"gr\"");
    const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
    if (mode == 1) {
        if (!c || !g_rmRunning || !g_staticValid) { o.ks("refused", "GSP-RM is not running this boot"); o.send(); return; }
        if (g_grTried) { o.ks("refused", "gr golden already ran this boot"); o.send(); return; }
        g_grTried = true;
        SpinLock* lock = NvAuxSharedLock();
        SpinLockAcquire(lock);
        Pramin<Bar0Bus> pr(g_bus, *c); PraminIo vio{pr};
        RmWait wait; g_rm->wait_deadline_us = NowUs() + 10000000;
        GspDoorbell<Bar0Bus, RmSync> bell{g_bus, *c, g_rmSync};
        alignas(8) static uint8_t scratch[kRmApiBuf];
        LiveRmApi api(g_rm->ch, g_rm->rx, g_rm->rxcap, wait, bell, scratch);
        auto sysalloc = [](uint32_t bytes, M2Sys* m) { return MthdbufAlloc(bytes, m); };   // golden/scrubber method buffers
        const uint64_t t0 = NowUs();
        KPrintf("nv-gsp: GR golden context + scrubber starting\n");
        g_grLast = RunGrGolden(g_bus, *c, vio, api, g_static, 0x110000000ull, 0x130000000ull, sysalloc, g_gr);
        const uint64_t us = NowUs() - t0;
        pr.Restore();
        SpinLockRelease(lock);
        o.kd("run_us", us);
        KPrintf("nv-gsp: GR %s at %s (rm 0x%x): golden %d scrubber %d, %lu us\n", g_grLast.status, g_grLast.stage,
                g_grLast.rm_status, g_grLast.golden_ok ? 1 : 0, g_grLast.scrubber_ok ? 1 : 0, (unsigned long)us);
    } else if (mode == 2) {
        o.kd("freed", GrStopLive());
    } else if (mode == 3 || mode == 5 || mode == 6 || mode == 7) {   // step 3: user GR channel, TURING_A clears of a 3840x2160 A8R8G8B8 target (5: block-linear)
        if (!c || !g_rmRunning || !g_gr.up) { o.ks("refused", "needs GSP-RM and `gr golden` this boot"); o.send(); return; }
        static uint32_t runs = 0;
        if (runs >= 16) { o.ks("refused", "gr clear ran 16 times this boot"); o.send(); return; }
        ++runs;
        GrClearPlan pl;
        pl.block_linear = mode == 5;
        if (mode == 6) pl.hold_ms = 3000;                     // gr clear hold: work, then 3 s alive
        if (mode == 7) { pl.hold_ms = 3000; pl.no_work = true; }   // gr idle: no work, 3 s alive
        auto sys = [](M2Sys* m, uint32_t bytes) { DmaRegion d; if (!RmAlloc(bytes, &d)) return false;
            m->cpu = d.cpu; m->bus = d.bus; m->bytes = d.bytes; return true; };
        if (!sys(&pl.gpfifo, 0x1000) || !sys(&pl.push, 0x1000) || !sys(&pl.sem, 0x1000) || !MthdbufAlloc(g_gr.mthdbuf_size, &pl.mthdbuf)) {
            o.ks("refused", "no sysmem"); o.send(); return; }
        SpinLock* lock = NvAuxSharedLock();
        SpinLockAcquire(lock);
        Pramin<Bar0Bus> pr(g_bus, *c); PraminIo vio{pr};
        RmWait wait; g_rm->wait_deadline_us = NowUs() + 10000000;
        GspDoorbell<Bar0Bus, RmSync> bell{g_bus, *c, g_rmSync};
        alignas(8) static uint8_t scratch[kRmApiBuf];
        LiveRmApi api(g_rm->ch, g_rm->rx, g_rm->rxcap, wait, bell, scratch);
        struct ClearSync { GrClearPlan& p; void operator()() { FlushRange(p.gpfifo.cpu, 0x1000); FlushRange(p.push.cpu, 0x1000); } } cs{pl};
        GrClearResult cr;
        const uint64_t t0 = NowUs();
        const GrResult g = RunGrClear(g_bus, *c, vio, api, g_gr, pl, cs, [] { __asm__ volatile("pause"); }, cr);
        pr.Restore();
        SpinLockRelease(lock);
        o.kd("run_us", NowUs() - t0);
        o.k("clear"); o.raw("{"); o.ks("layout", pl.block_linear ? "block-linear" : "pitch"); o.kd("hold_ms", pl.hold_ms);
        o.kb("no_work", pl.no_work); o.kh("mthdbuf_bus", pl.mthdbuf.bus); o.kh("mthdbuf_size", g_gr.mthdbuf_size); o.kd("events_total", g.ev.total); o.ks("stage", g.stage); o.ks("status", g.status); o.kh("rm_status", g.rm_status);
        o.kh("rt_vram", cr.rt_vram); o.kh("token", cr.token); o.kb("first_ok", cr.first_ok); o.kb("verify_ok", cr.verify_ok);
        o.kh("sample_got", cr.sample_got); o.kh("sample_want", cr.sample_want); o.kd("first_us", cr.first_us);
        o.kd("best_us", cr.best_us); o.kd("avg_us", cr.n ? cr.sum_us / cr.n : 0); o.kd("n", cr.n); o.kd("bytes", cr.bytes);
        o.kd("mb_per_s", cr.best_us ? cr.bytes / cr.best_us : 0); o.kd("events_rc", cr.events_rc); o.kd("events_mmu", cr.events_mmu);
        o.kd("allocated", g.allocated); o.kd("freed", g.freed); o.raw("}");
        KPrintf("nv-gsp: GR clear (%s) %s at %s: first %d verify %d best %u us\n", pl.block_linear ? "bl" : "pitch", g.status,
                g.stage, cr.first_ok ? 1 : 0, cr.verify_ok ? 1 : 0, cr.best_us);
    } else if (mode == 4) {   // step 4: TURING_COMPUTE_A dispatch of the NAK-compiled kernel (nak_m4_kernel.h)
        if (!c || !g_rmRunning || !g_gr.up) { o.ks("refused", "needs GSP-RM and `gr golden` this boot"); o.send(); return; }
        static bool tried = false;
        if (tried) { o.ks("refused", "gr compute already ran this boot"); o.send(); return; }
        tried = true;
        GrClearPlan pl;
        auto sys = [](M2Sys* m, uint32_t bytes) { DmaRegion d; if (!RmAlloc(bytes, &d)) return false;
            m->cpu = d.cpu; m->bus = d.bus; m->bytes = d.bytes; return true; };
        if (!sys(&pl.gpfifo, 0x1000) || !sys(&pl.push, 0x1000) || !sys(&pl.sem, 0x1000) || !MthdbufAlloc(g_gr.mthdbuf_size, &pl.mthdbuf)) {
            o.ks("refused", "no sysmem"); o.send(); return; }
        namespace nk = nak_m4;
        const GrComputeKernel k{nk::kCode, uint32_t(sizeof(nk::kCode) / 4), nk::kQmd, uint32_t(sizeof(nk::kQmd) / 4),
                                nk::kShaderVa, nk::kCb0Va, nk::kInvocations, nk::kLocalSize, nk::kStoreValue};
        const uint32_t invocations = nk::kInvocations - nk::kLocalSize / 2 - 3;   // a partial last block: tests the OOB exit
        SpinLock* lock = NvAuxSharedLock();
        SpinLockAcquire(lock);
        Pramin<Bar0Bus> pr(g_bus, *c); PraminIo vio{pr};
        RmWait wait; g_rm->wait_deadline_us = NowUs() + 10000000;
        GspDoorbell<Bar0Bus, RmSync> bell{g_bus, *c, g_rmSync};
        alignas(8) static uint8_t scratch[kRmApiBuf];
        LiveRmApi api(g_rm->ch, g_rm->rx, g_rm->rxcap, wait, bell, scratch);
        struct CSync { GrClearPlan& p; void operator()() { FlushRange(p.gpfifo.cpu, 0x1000); FlushRange(p.push.cpu, 0x1000); } } cs{pl};
        GrComputeResult cr;
        const uint64_t t0 = NowUs();
        const GrResult g = RunGrCompute(g_bus, *c, vio, api, g_gr, pl, k, invocations, cs, [] { __asm__ volatile("pause"); }, cr);
        pr.Restore();
        SpinLockRelease(lock);
        o.kd("run_us", NowUs() - t0);
        o.k("compute"); o.raw("{"); o.ks("stage", g.stage); o.ks("status", g.status); o.kh("rm_status", g.rm_status);
        o.kh("block_vram", cr.block_vram); o.kh("token", cr.token); o.kd("code_words", k.code_words);
        o.kd("invocations", cr.invocations); o.kd("threads", cr.threads); o.kb("first_ok", cr.first_ok); o.kb("verify_ok", cr.verify_ok);
        o.kd("bad_index", cr.bad_index); o.kh("bad_got", cr.bad_got); o.kh("bad_want", cr.bad_want);
        o.kd("first_us", cr.first_us); o.kd("best_us", cr.best_us); o.kd("avg_us", cr.n ? cr.sum_us / cr.n : 0); o.kd("n", cr.n);
        o.kd("events_rc", cr.events_rc); o.kd("events_mmu", cr.events_mmu);
        o.kd("allocated", g.allocated); o.kd("freed", g.freed); o.raw("}");
        KPrintf("nv-gsp: GR compute %s at %s: first %d verify %d best %u us\n", g.status, g.stage, cr.first_ok ? 1 : 0,
                cr.verify_ok ? 1 : 0, cr.best_us);
    }
    o.kb("up", g_gr.up);
    GrJson(o, g_grLast);
    o.send();
}

}  // namespace


namespace {
// ------------------------------------------------------------- M5: renderD130 live backend
// nv_drm_core.h's Backend on GSP-RM: one client + external VAS per file (GrCtx::OpenVas), GR0
// channels in it (GrCtx::ChannelInVas + non-golden PROMOTE_CTX + engine objects), 4 KiB PTEs
// through GmmuBuilder over PRAMIN with an MMU invalidate per batch, VRAM BOs from a heap above
// Brook's fixed users, GART BOs as contiguous pinned pages. Every call runs under the nv-aux lock.
namespace nvd {
constexpr uint32_t kMaxVas = 16, kMaxChans = 32, kChidFirst = 8, kChidCount = 56;
constexpr uint32_t kClientBase = 0xc1d20000;            // + vas * 0x400
constexpr uint64_t kCtxVa = 0x0000008000000000ull;      // kernel-managed (NVK keeps [1<<39, 1<<40) for us)
constexpr uint64_t kRingVa = 0x0000009000000000ull;     // per channel: gpfifo, kernel push, semaphore
constexpr uint64_t kHeapLo = 0x140000000ull;            // above CE (0x108..) and GR (0x110..0x130) arenas
struct Vas { bool used = false; GrScope s; };
struct Chan {
    bool used = false, scheduled = false; uint32_t vas = 0, chid = 0, gp = 0, token = 0;
    GrScope s; M2Sys gpfifo, kpush, sem, mthdbuf; uint64_t va = 0;
};
Vas g_vas[kMaxVas];
Chan g_chan[kMaxChans];
volatile bool g_dead[kChidFirst + kChidCount];
uint32_t g_grce = 0;                  // GR runlist copy engine (RM engine type), from the device info table
volatile bool g_minObjects = false;   // `nv-gsp drm minobj on`: only TURING_A + COMPUTE_A on channels (Xid 62 A/B)
nvdrm::VramHeap g_heap;
bool g_heapInit = false;
}  // namespace nvd

template <class F> bool NvdWithRm(const char* what, F f) {
    const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
    if (!c || !g_rmRunning || g_finiDone) return false;
    SpinLock* lock = NvAuxSharedLock();
    SpinLockAcquire(lock);
    Pramin<Bar0Bus> pr(g_bus, *c); PraminIo vio{pr};
    RmWait wait; g_rm->wait_deadline_us = NowUs() + 5000000;
    GspDoorbell<Bar0Bus, RmSync> bell{g_bus, *c, g_rmSync};
    alignas(8) static uint8_t scratch[kRmApiBuf];
    LiveRmApi api(g_rm->ch, g_rm->rx, g_rm->rxcap, wait, bell, scratch);
    GrResult r;
    bool ok = true;
    if (!g_gr.up) {   // first use: golden context + scrubber (as `gr golden`), which also sets the GR arena
        auto sysalloc = [](uint32_t bytes, M2Sys* m) { return MthdbufAlloc(bytes, m); };   // golden/scrubber method buffers
        g_grTried = true;
        g_grLast = RunGrGolden(g_bus, *c, vio, api, g_static, 0x110000000ull, 0x130000000ull, sysalloc, g_gr);
        ok = g_gr.up;
        if (!ok) KPrintf("nv-drm: GR golden failed: %s at %s\n", g_grLast.status, g_grLast.stage);
    }
    if (ok) {
        GrCtx<Bar0Bus, LiveRmApi, PraminIo> x{g_bus, api, vio, *c, g_gr.arena, r};
        x.trace = [](const char* w, uint32_t h, uint32_t cls, uint32_t st) {
            KPrintf("nv-drm: rm %s %x class %x -> %x at %lu ms\n", w, h, cls, st, (unsigned long)(NowUs() / 1000)); };
        ok = f(x, vio, *c);
        if (!ok) {   // rm 0 with a queue error = the RPC itself failed (timeout, RPC error), not RM refusing
            static const char* const qn[] = {"ok", "timeout", "too-big", "bad-element", "bad-checksum", "not-continuation", "overflow", "rpc-error"};
            const uint32_t qi = uint32_t(r.q);
            KPrintf("nv-drm: %s failed: %s (rm 0x%x, queue %s, rpc_result 0x%x)\n", what, r.status, r.rm_status,
                    qi < 8 ? qn[qi] : "?", r.rpc_result);
        }
    }
    pr.Restore();
    SpinLockRelease(lock);
    return ok;
}
// Full MMU invalidate (all VA, all PDB, hub + GPC TLBs) after renderD130 PTE writes; BAR1 keeps
// the HUB_ONLY form (Bar1FlushInvalidate). NV_PFB_PRI_MMU_INVALIDATE: ALL_VA 0, ALL_PDB 1, HUBTLB_ONLY 2, TRIGGER 31.
bool NvdMmuInvalidate(const ChipOps& c, uint64_t pdb) {
    g_bus.wr32(c.reg_fb_flush, 1);
    for (uint32_t i = 0; i < 2000 && (g_bus.rd32(c.reg_fb_flush) & 2); ++i) g_bus.udelay(1);
    g_bus.wr32(c.reg_mmu_inval_pdb_lo, uint32_t(pdb >> 8));
    g_bus.wr32(c.reg_mmu_inval_pdb_hi, 0);
    g_bus.wr32(c.reg_mmu_inval_trigger, 0x80000000u | 0x3);
    for (uint32_t i = 0; i < 200000; ++i) { if (!(g_bus.rd32(c.reg_mmu_inval_trigger) & 0x80000000u)) return true; g_bus.udelay(10); }
    return false;
}
void NvdMarkDead(uint32_t chid) { if (chid >= nvd::kChidFirst && chid < nvd::kChidFirst + nvd::kChidCount) nvd::g_dead[chid] = true; }

bool NvdVramAlloc(void*, uint64_t bytes, uint64_t align, uint64_t* pa) {
    if (!nvd::g_heapInit) {
        if (g_static.usable_hi <= nvd::kHeapLo) return false;
        nvd::g_heap.Init(nvd::kHeapLo, g_static.usable_hi); nvd::g_heapInit = true;
        KPrintf("nv-drm: VRAM heap %lx..%lx (%lu MiB)\n", (unsigned long)nvd::kHeapLo, (unsigned long)g_static.usable_hi,
                (unsigned long)((g_static.usable_hi - nvd::kHeapLo) >> 20));
    }
    return nvd::g_heap.Alloc(bytes, align, pa);
}
void NvdVramFree(void*, uint64_t pa, uint64_t bytes) { (void)nvd::g_heap.Release(pa, bytes); }
void NvdSysRelease(void* object) {
    auto* p = static_cast<NvDrmSysPages*>(object);
    for (uint64_t o = 0; o < p->bytes; o += 4096) PmmFreePage(PhysicalAddress(p->phys + o));
    kfree(p);
}
bool NvdSysAlloc(void*, uint64_t bytes, uint64_t* cookie) {
    const uint64_t pages = (bytes + 4095) / 4096;
    const PhysicalAddress pa = PmmAllocPages(pages, MemTag::Device, KernelPid);
    if (!pa) return false;
    auto* p = static_cast<NvDrmSysPages*>(kmalloc(sizeof(NvDrmSysPages)));
    if (!p) { for (uint64_t i = 0; i < pages; ++i) PmmFreePage(PhysicalAddress(pa.raw() + i * 4096)); return false; }
    p->phys = pa.raw(); p->bytes = pages * 4096;
    memset(reinterpret_cast<void*>(PhysToVirt(pa).raw()), 0, p->bytes);
    p->life = MappingBackingCreate(p, NvdSysRelease);
    if (!p->life || p->phys + p->bytes > (1ull << 39)) { if (p->life) MappingBackingUnref(p->life); else NvdSysRelease(p); return false; }
    *cookie = reinterpret_cast<uint64_t>(p);
    return true;
}
void NvdSysFree(void*, uint64_t cookie) { MappingBackingUnref(reinterpret_cast<NvDrmSysPages*>(cookie)->life); }

bool NvdVasCreate(void*, uint64_t, uint64_t, uint32_t* out) {
    uint32_t i = 0; while (i < nvd::kMaxVas && nvd::g_vas[i].used) ++i;
    if (i == nvd::kMaxVas) return false;
    nvd::Vas& v = nvd::g_vas[i];
    v = nvd::Vas{}; v.s.base = nvd::kClientBase + i * 0x400; v.s.external = true; v.s.h_vas = v.s.base + grh::kVaspace;
    if (!NvdWithRm("VM_INIT vaspace", [&](auto& x, auto&, const ChipOps&) {
            if (x.OpenVas(v.s)) return true;
            x.Close(v.s); return false; })) return false;
    v.used = true; *out = i;
    KPrintf("nv-drm: VAS %u created (client %x, root %lx)\n", i, v.s.base, (unsigned long)v.s.root);
    return true;
}
void NvdVasDestroy(void*, uint32_t vas) {
    if (vas >= nvd::kMaxVas || !nvd::g_vas[vas].used) return;
    KPrintf("nv-drm: VAS %u free: %u objects\n", vas, nvd::g_vas[vas].s.nlive);
    (void)NvdWithRm("vaspace free", [&](auto& x, auto&, const ChipOps&) { const uint32_t n = x.Close(nvd::g_vas[vas].s);
        KPrintf("nv-drm: VAS %u freed %u\n", vas, n); return true; });
    nvd::g_vas[vas].used = false;   // page tables stay in the GR arena (bump allocator) until RM unload
}
bool NvdVasMap(void*, uint32_t vas, uint64_t va, nvdrm::Aper aper, uint64_t backing, uint64_t off, uint64_t bytes, uint8_t kind) {
    if (vas >= nvd::kMaxVas || !nvd::g_vas[vas].used) return false;
    const bool sys = aper == nvdrm::Aper::Sys;
    const uint64_t pa = sys ? reinterpret_cast<const NvDrmSysPages*>(backing)->phys + off : backing + off;
    return NvdWithRm("VM_BIND map", [&](auto& x, auto&, const ChipOps&) {
        return x.builder(nvd::g_vas[vas].s, [&](auto& b) {
            return b.Map(va, pa, bytes, 12, sys ? Aperture::SysCoh : Aperture::Video, kind, sys); }); });
}
bool NvdVasUnmap(void*, uint32_t vas, uint64_t va, uint64_t bytes, bool sparse) {
    if (vas >= nvd::kMaxVas || !nvd::g_vas[vas].used) return false;
    return NvdWithRm("VM_BIND unmap", [&](auto& x, auto&, const ChipOps&) {
        return x.builder(nvd::g_vas[vas].s, [&](auto& b) {
            for (uint64_t o = 0; o < bytes; o += 4096) if (!b.UnmapPage(va + o, sparse)) return false;
            return true; }); });
}
bool NvdVasFlush(void*, uint32_t vas) {
    if (vas >= nvd::kMaxVas || !nvd::g_vas[vas].used) return false;
    return NvdWithRm("MMU invalidate", [&](auto&, auto&, const ChipOps& c) { return NvdMmuInvalidate(c, nvd::g_vas[vas].s.root); });
}

bool NvdChanCreate(void*, uint32_t vas, uint32_t engine, uint32_t* out) {
    namespace rm = rm570;
    if (vas >= nvd::kMaxVas || !nvd::g_vas[vas].used || engine != 0x01) return false;   // GR only for now
    uint32_t i = 0; while (i < nvd::kMaxChans && nvd::g_chan[i].used) ++i;
    uint32_t chid = 0; for (uint32_t k = 0; k < nvd::kChidCount && !chid; ++k) {
        bool taken = false; for (auto& c : nvd::g_chan) taken |= c.used && c.chid == nvd::kChidFirst + k;
        if (!taken) chid = nvd::kChidFirst + k; }
    if (i == nvd::kMaxChans || !chid) return false;
    nvd::Chan& ch = nvd::g_chan[i];
    ch = nvd::Chan{}; ch.vas = vas; ch.chid = chid; nvd::g_dead[chid] = false;
    auto sys = [](M2Sys* m, uint32_t bytes) { DmaRegion d; if (!RmAlloc(bytes, &d)) return false;
        m->cpu = d.cpu; m->bus = d.bus; m->bytes = d.bytes; return true; };
    if (!sys(&ch.gpfifo, 0x1000) || !sys(&ch.kpush, 0x1000) || !sys(&ch.sem, 0x1000) || !MthdbufAlloc(g_gr.mthdbuf_size ? g_gr.mthdbuf_size : 0x1000, &ch.mthdbuf))
        return false;
    nvd::Vas& v = nvd::g_vas[vas];
    ch.va = nvd::kRingVa + uint64_t(i) * 0x10000;
    GrScope& s = ch.s;
    s.base = v.s.base; s.chid = chid; s.external = true; s.priv = true; s.h_vas = v.s.h_vas;   // unprivileged channels hung their first RM_FREE (gsp16d/e)
    s.h_chan = v.s.base + 0x100 + i * 0x10; s.gpfifo_va = ch.va; s.va_next = nvd::kCtxVa + (uint64_t(i) << 32);
    const bool ok = NvdWithRm("CHANNEL_ALLOC", [&](auto& x, auto& vio, const ChipOps& c) {
        namespace h = rm::host;
        if (!x.builder(v.s, [&](auto& b) {
                return b.Map(ch.va, ch.gpfifo.bus, 0x1000, 12, Aperture::SysCoh, 0, true) &&
                       b.Map(ch.va + 0x1000, ch.kpush.bus, 0x1000, 12, Aperture::SysCoh, 0, true) &&
                       b.Map(ch.va + 0x2000, ch.sem.bus, 0x1000, 12, Aperture::SysCoh, 0, true); })) return false;
        if (!NvdMmuInvalidate(c, v.s.root)) return x.fail("MMU invalidate (rings)");
        // the channel scope shares the VAS page tables (context buffers map into the same root)
        s.root = v.s.root; s.lpt_page = v.s.lpt_page; s.lpt_off = v.s.lpt_off; s.lpt_aper = v.s.lpt_aper;
        uint32_t entries = 0;
        bool good = x.ChannelInVas(s, ch.mthdbuf, g_gr.mthdbuf_size) && x.Promote(s, g_gr.bufs, false, &entries);
        v.s.lpt_page = s.lpt_page; v.s.lpt_off = s.lpt_off; v.s.lpt_aper = s.lpt_aper;
        // NVK allocates TURING_DMA_COPY_A on its GR channel (nouveau_ws_3d_context_init, ENGINE_COPY)
        // and uses it for fills/copies: the GR runlist's copy engine (GRCE), as nouveau's
        // r535_ce_alloc does with engineType COPY0 + inst (27 Nov: without it the first copy
        // faulted, Xid 44 + MMU fault).
        if (good && !nvd::g_grce) {
            static rm::NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS t;
            __builtin_memset(&t, 0, sizeof(t));
            if (x.api.Control(g_static.h_internal_client, g_static.h_internal_subdevice, rm::NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE,
                              &t, sizeof(t))) {
                uint32_t gr_rl = ~0u;
                for (uint32_t k = 0; k < t.numEntries && k < rm::kDevInfoMaxEntries; ++k)
                    if (t.entries[k].engineData[rm::ENGINE_INFO_TYPE_RM_ENGINE_TYPE] == rm::RM_ENGINE_TYPE_GR0)
                        gr_rl = t.entries[k].engineData[rm::ENGINE_INFO_TYPE_RUNLIST];
                for (uint32_t k = 0; k < t.numEntries && k < rm::kDevInfoMaxEntries && !nvd::g_grce; ++k) {
                    const uint32_t e = t.entries[k].engineData[rm::ENGINE_INFO_TYPE_RM_ENGINE_TYPE];
                    if (e >= rm::RM_ENGINE_TYPE_COPY0 && e <= rm::RM_ENGINE_TYPE_COPY9 &&
                        t.entries[k].engineData[rm::ENGINE_INFO_TYPE_RUNLIST] == gr_rl) nvd::g_grce = e;
                }
                KPrintf("nv-drm: GR runlist %u copy engine (GRCE) engine type 0x%x\n", gr_rl, nvd::g_grce);
            }
        }
        good = good && x.alloc(s, s.h_chan, s.h_chan + 1, rm::gr::TURING_A, nullptr, 0, "RM_ALLOC TURING_A") &&
               x.alloc(s, s.h_chan, s.h_chan + 2, rm::gr::TURING_COMPUTE_A, nullptr, 0, "RM_ALLOC TURING_COMPUTE_A") &&
               (nvd::g_minObjects ||
                (x.alloc(s, s.h_chan, s.h_chan + 3, 0x902d, nullptr, 0, "RM_ALLOC FERMI_TWOD_A") &&
                 x.alloc(s, s.h_chan, s.h_chan + 4, 0xa140, nullptr, 0, "RM_ALLOC KEPLER_INLINE_TO_MEMORY_B")));
        if (good && nvd::g_grce) {
            struct { uint32_t version, engineType; } ce = {1, nvd::g_grce};   // NVC0B5_ALLOCATION_PARAMETERS
            good = x.alloc(s, s.h_chan, s.h_chan + 5, 0xc5b5, &ce, sizeof(ce), "RM_ALLOC TURING_DMA_COPY_A (GRCE)");
        }
        // BIND + GPFIFO_SCHEDULE + token wait for the first EXEC (NvdSchedule): a scheduled GR
        // channel left idle ~1 s preceded the RM asserts / Xid 62 seen on 26 Nov.
        (void)vio; (void)c; (void)h::kDoorbell;
        if (!good) x.Close(s);
        return good;
    });
    if (!ok) return false;
    ch.used = true; *out = i;
    KPrintf("nv-drm: channel %u (chid %u) in VAS %u, not scheduled until its first EXEC\n", i, chid, vas);
    return true;
}
void NvdChanDestroy(void*, uint32_t chan) {
    if (chan >= nvd::kMaxChans || !nvd::g_chan[chan].used) return;
    KPrintf("nv-drm: channel %u free: %u objects\n", chan, nvd::g_chan[chan].s.nlive);
    (void)NvdWithRm("CHANNEL_FREE", [&](auto& x, auto&, const ChipOps&) { const uint32_t n = x.Close(nvd::g_chan[chan].s);
        KPrintf("nv-drm: channel %u freed %u\n", chan, n); return true; });
    nvd::g_chan[chan].used = false;   // its sysmem pages are RM-lifetime regions (freed at RM unload)
}
bool NvdSchedule(nvd::Chan& ch) {
    namespace rm = rm570;
    GrScope& s = ch.s;
    const bool ok = NvdWithRm("channel schedule", [&](auto& x, auto&, const ChipOps&) {
        rm::NVA06F_CTRL_BIND_PARAMS bp = {rm::gr::RM_ENGINE_TYPE_GR0};
        rm::NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS sp = {1, 0};
        rm::NVC36F_CTRL_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS tk = {};
        if (!x.api.Control(s.base, s.h_chan, rm::NVA06F_CTRL_CMD_BIND, &bp, sizeof(bp))) return x.fail("BIND");
        if (!x.api.Control(s.base, s.h_chan, rm::NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &sp, sizeof(sp))) return x.fail("GPFIFO_SCHEDULE");
        if (!x.api.Control(s.base, s.h_chan, rm::NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN, &tk, sizeof(tk))) return x.fail("token");
        ch.token = tk.workSubmitToken;
        return true;
    });
    if (ok) { ch.scheduled = true; KPrintf("nv-drm: channel chid %u scheduled (token %x)\n", ch.chid, ch.token); }
    return ok;
}
bool NvdChanSubmit(void*, uint32_t chan, const uint64_t* va, const uint32_t* dw, const uint8_t* np, uint32_t n, uint32_t seq) {
    namespace h = rm570::host;
    if (chan >= nvd::kMaxChans || !nvd::g_chan[chan].used) return false;
    nvd::Chan& ch = nvd::g_chan[chan];
    if (!ch.scheduled && !NvdSchedule(ch)) { nvd::g_dead[ch.chid] = true; return false; }   // chan_error then fails the job
    const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
    if (!c || !g_rmRunning) return false;
    SpinLock* lock = NvAuxSharedLock();
    SpinLockAcquire(lock);
    Pramin<Bar0Bus> pr(g_bus, *c); PraminIo vio{pr};
    const uint32_t get = vio.rd32(ch.s.userd + h::USERD_GP_GET) % 512;
    const uint32_t used = (ch.gp + 512 - get) % 512;
    bool ok = used + n + 1 < 512;
    if (ok) {
        uint32_t* g = reinterpret_cast<uint32_t*>(ch.gpfifo.cpu);
        for (uint32_t i = 0; i < n; ++i) {
            g[ch.gp * 2] = uint32_t(va[i]); g[ch.gp * 2 + 1] = uint32_t(va[i] >> 32) | (dw[i] << 10) | (np[i] ? 0x80000000u : 0);
            ch.gp = (ch.gp + 1) % 512;
        }
        // kernel push: release the job's seqno after the GPU went idle on this channel (WFI)
        const uint32_t slot = seq % 128;
        uint32_t* p = reinterpret_cast<uint32_t*>(ch.kpush.cpu) + slot * 8;
        const uint64_t sem_va = ch.va + 0x2000;
        p[0] = h::IncHeader(0, h::SEM_ADDR_LO, 3); p[1] = uint32_t(sem_va); p[2] = uint32_t(sem_va >> 32); p[3] = seq;
        p[4] = h::IncHeader(0, h::SEM_EXECUTE, 1); p[5] = h::SEM_EXECUTE_RELEASE | h::SEM_EXECUTE_RELEASE_WFI;
        const uint64_t kva = ch.va + 0x1000 + slot * 32;
        g[ch.gp * 2] = uint32_t(kva); g[ch.gp * 2 + 1] = uint32_t(kva >> 32) | (6u << 10);
        ch.gp = (ch.gp + 1) % 512;
        FlushRange(ch.gpfifo.cpu, 0x1000); FlushRange(ch.kpush.cpu, 0x1000);
        vio.wr32(ch.s.userd + h::USERD_GP_PUT, ch.gp);
        g_bus.wr32(c->usermode + h::kDoorbell, ch.token);
    }
    pr.Restore();
    SpinLockRelease(lock);
    return ok;
}
uint32_t NvdChanCompleted(void*, uint32_t chan) {
    if (chan >= nvd::kMaxChans || !nvd::g_chan[chan].used) return 0;
    return __atomic_load_n(reinterpret_cast<const uint32_t*>(nvd::g_chan[chan].sem.cpu), __ATOMIC_ACQUIRE);
}
bool NvdChanError(void*, uint32_t chan) {
    return chan >= nvd::kMaxChans || !nvd::g_chan[chan].used || nvd::g_dead[nvd::g_chan[chan].chid] || !g_rmRunning;
}
const nvdrm::Backend g_nvdBackend{nullptr, NvdVramAlloc, NvdVramFree, NvdSysAlloc, NvdSysFree, NvdVasCreate, NvdVasDestroy,
                                  NvdVasMap, NvdVasUnmap, NvdVasFlush, NvdChanCreate, NvdChanDestroy, NvdChanSubmit,
                                  NvdChanCompleted, NvdChanError, false};

// `nv-gsp gr clear vas2 [1|2]` (27 Nov NVK Xid 44 A/B): NVK's kernel path without NVK. One VAS
// from NvdVasCreate, N GR channels from NvdChanCreate (1 or 2, default 2; as nv-drm makes them
// for NVK), a 256x256 pitch target and a push buffer mapped with NvdVasMap, the known-good clear
// submitted through NvdChanSubmit on the last channel, then on the first. Pixels read via PRAMIN.
void GrVas2(uint32_t nchan, bool priv = false) {
    namespace h = rm570::host;
    Out o; o.raw("{\"nv_gsp\":\"gr clear vas2\""); o.kd("channels", nchan); o.kb("priv_reg", priv);
    const ChipOps* c = ChipForBoot0(g_bus.rd32(0));
    if (!c || !g_rmRunning || !g_gr.up) { o.ks("refused", "needs GSP-RM and gr golden this boot"); o.send(); return; }
    uint32_t vas = 0, ch[2] = {~0u, ~0u}, made = 0; uint64_t rt = 0, push = 0;
    const char* status = "ok";
    const uint32_t W = 256, H = 256, pitch = W * 4; const uint64_t rt_bytes = 0x200000, va_rt = 0x1000000000ull, va_push = 0x1000200000ull;
    bool rt_mapped = false, push_mapped = false, vas_ok = false;
    uint32_t done_ok = 0, sample = 0;
    if (!NvdVasCreate(nullptr, 0, 0, &vas)) { status = "VAS create"; goto out; }
    vas_ok = true;
    for (; made < nchan; ++made) if (!NvdChanCreate(nullptr, vas, 1, &ch[made])) { status = "channel create"; goto out; }
    if (!NvdVramAlloc(nullptr, rt_bytes, rt_bytes, &rt)) { status = "VRAM"; goto out; }
    if (!NvdVasMap(nullptr, vas, va_rt, nvdrm::Aper::Vram, rt, 0, rt_bytes, 0)) { status = "map target"; goto out; }
    rt_mapped = true;
    if (!NvdSysAlloc(nullptr, 0x1000, &push)) { status = "push sysmem"; goto out; }
    if (!NvdVasMap(nullptr, vas, va_push, nvdrm::Aper::Sys, push, 0, 0x1000, 0)) { status = "map push"; goto out; }
    push_mapped = true;
    if (!NvdVasFlush(nullptr, vas)) { status = "flush"; goto out; }
    for (int pass = 0; pass < int(nchan); ++pass) {
        const uint32_t k = nchan - 1 - uint32_t(pass);   // last channel first, as NVK's chid 9
        uint32_t* w = reinterpret_cast<uint32_t*>(PhysToVirt(PhysicalAddress(reinterpret_cast<NvDrmSysPages*>(push)->phys)).raw());
        uint32_t n = 0;
        auto m1 = [&](uint32_t mthd, uint32_t v) { w[n++] = h::IncHeader(0, mthd, 1); w[n++] = v; };
        const float col[2][4] = {{1, 0, 1, 1}, {0, 1, 1, 1}};
        const uint32_t want = pass ? 0xff00ffffu : 0xffff00ffu;
        w[n++] = h::IncHeader(0, 0, 1); w[n++] = rm570::gr::TURING_A;
        if (priv) {   // 27 Nov NVK Xid 44 hypothesis: NVK's init SET_PRIV_REG (MME macro 22) as plain methods:
            // WAIT_FOR_IDLE, MME scratch FALCON_0..2 = {0, value 0, mask 8}, SET_FALCON04 = 0x419ba4
            // (FECS firmware writes the priv reg and sets scratch 0 to 1), then NOPs
            m1(0x0110, 0);
            w[n++] = h::IncHeader(0, 0x3400, 3); w[n++] = 0; w[n++] = 0; w[n++] = 8;
            m1(0x2310, 0x419ba4);
            for (int q = 0; q < 16; ++q) m1(0x0100, 0);
        }
        w[n++] = h::IncHeader(0, gr3d::SET_COLOR_TARGET_A0, 8); w[n++] = uint32_t(va_rt >> 32); w[n++] = uint32_t(va_rt);
        w[n++] = pitch; w[n++] = H; w[n++] = gr3d::FORMAT_A8R8G8B8; w[n++] = gr3d::MEMORY_LAYOUT_PITCH; w[n++] = 1; w[n++] = 0;
        m1(gr3d::SET_CT_SELECT, 1); m1(gr3d::SET_ZT_SELECT, 0);
        w[n++] = h::IncHeader(0, gr3d::SET_SURFACE_CLIP_HORIZONTAL, 2); w[n++] = W << 16; w[n++] = H << 16;
        w[n++] = h::IncHeader(0, gr3d::SET_WINDOW_OFFSET_X, 2); w[n++] = 0; w[n++] = 0;
        m1(gr3d::SET_SCISSOR_ENABLE0, 0); m1(gr3d::SET_CLEAR_SURFACE_CONTROL, 0); m1(gr3d::SET_RENDER_ENABLE_C, gr3d::RENDER_ENABLE_TRUE);
        w[n++] = h::IncHeader(0, gr3d::SET_COLOR_CLEAR_VALUE0, 4);
        for (int q = 0; q < 4; ++q) w[n++] = GrF(col[pass & 1][q]);
        m1(gr3d::CLEAR_SURFACE, gr3d::CLEAR_RGBA);
        FlushRange(reinterpret_cast<const uint8_t*>(w), n * 4);
        const uint64_t va = va_push; const uint32_t dw = n; const uint8_t np = 0;
        const uint32_t seq = 1;
        if (!NvdChanSubmit(nullptr, ch[k], &va, &dw, &np, 1, seq)) { status = "submit"; goto out; }
        const uint64_t end = NowUs() + 2000000;
        while (NowUs() < end && int32_t(NvdChanCompleted(nullptr, ch[k]) - seq) < 0 && !NvdChanError(nullptr, ch[k])) {
            SchedulerSleepMs(1);
            FlushRange(nvd::g_chan[ch[k]].sem.cpu, 64);
        }
        if (NvdChanError(nullptr, ch[k])) { status = pass ? "channel error (second pass)" : "channel error (first pass)"; goto out; }
        if (int32_t(NvdChanCompleted(nullptr, ch[k]) - seq) < 0) { status = pass ? "timeout (second pass)" : "timeout (first pass)"; goto out; }
        {
            SpinLock* lock = NvAuxSharedLock(); SpinLockAcquire(lock);
            Pramin<Bar0Bus> pr(g_bus, *c);
            sample = pr.rd32(rt + uint64_t(H / 2) * pitch + (W / 2) * 4);
            pr.Restore(); SpinLockRelease(lock);
        }
        if (sample != want) { status = pass ? "wrong pixels (second pass)" : "wrong pixels (first pass)"; goto out; }
        ++done_ok;
    }
out:
    o.ks("status", status); o.kd("passes_ok", done_ok); o.kh("sample", sample);
    for (uint32_t k = 0; k < made; ++k) { o.k(k ? "chid1" : "chid0"); o.dec(nvd::g_chan[ch[k]].chid); }
    for (uint32_t k = made; k-- > 0;) NvdChanDestroy(nullptr, ch[k]);
    if (push_mapped) (void)NvdVasUnmap(nullptr, vas, va_push, 0x1000, false);
    if (rt_mapped) (void)NvdVasUnmap(nullptr, vas, va_rt, rt_bytes, false);
    if (push) NvdSysFree(nullptr, push);
    if (rt) NvdVramFree(nullptr, rt, rt_bytes);
    if (vas_ok) NvdVasDestroy(nullptr, vas);
    KPrintf("nv-gsp: gr clear vas2 (%u channel(s)): %s, %u pass(es) ok\n", nchan, status, done_ok);
    o.send();
}
}  // namespace

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
    else if (!strcmp(verb, "gr golden")) GrVerb(1);
    else if (!strcmp(verb, "gr off")) GrVerb(2);
    else if (!strcmp(verb, "gr status")) GrVerb(0);
    else if (!strcmp(verb, "gr clear")) GrVerb(3);
    else if (!strcmp(verb, "gr compute")) GrVerb(4);
    else if (!strcmp(verb, "gr clear bl")) GrVerb(5);
    else if (!strcmp(verb, "gr clear hold")) GrVerb(6);
    else if (!strcmp(verb, "gr idle")) GrVerb(7);
    else if (!strcmp(verb, "gr mthdbuf vram") || !strcmp(verb, "gr mthdbuf sys") || !strcmp(verb, "gr mthdbuf sys-aligned")) {
        g_grMthdbufVram = verb[11] == 'v'; g_mthdbufAlign = !strcmp(verb, "gr mthdbuf sys-aligned");
        Out o; o.raw("{\"nv_gsp\":\"gr mthdbuf\""); o.ks("mthdbuf", g_grMthdbufVram ? "vram" : g_mthdbufAlign ? "sysmem, size-aligned" : "sysmem"); o.send();
    }
    else if (!strcmp(verb, "drm minobj on") || !strcmp(verb, "drm minobj off")) {
        nvd::g_minObjects = !strcmp(verb, "drm minobj on");
        DebugChannelSend(nvd::g_minObjects ? "{\"nv_gsp\":\"drm\",\"min_objects\":true}\n" : "{\"nv_gsp\":\"drm\",\"min_objects\":false}\n");
    }
    else if (!strcmp(verb, "fault dump")) FaultDump();
    else if (!strcmp(verb, "fault shadow on")) FaultShadowOn();
    else if (!strcmp(verb, "gr clear vas2") || !strcmp(verb, "gr clear vas2 2")) GrVas2(2);
    else if (!strcmp(verb, "gr clear vas2 1")) GrVas2(1);
    else if (!strcmp(verb, "gr clear vas2 priv")) GrVas2(1, true);
    else if (!strncmp(verb, "nocat ", 6)) { uint32_t k = 0; for (const char* q = verb + 6; *q >= '0' && *q <= '9'; ++q) k = k * 10 + uint32_t(*q - '0'); NocatVerb(k); }
    else if (!strcmp(verb, "perf status")) PerfVerb(0);
    else if (!strcmp(verb, "perf boost")) PerfVerb(1);
    else if (!strcmp(verb, "perf clear")) PerfVerb(2);
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
