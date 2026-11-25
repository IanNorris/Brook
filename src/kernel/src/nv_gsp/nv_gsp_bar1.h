#pragma once
// Interim display handover (before M8): give Brook's CPU path back its
// "BAR1 offset == VRAM address" view after GSP-RM switched BAR1 to virtual
// mode, by building the BAR1 page tables under RM's BAR1 root the way
// nouveau does under GSP-RM.
//
// How the standard GSP client does it (nouveau r535/bar.c r535_bar_bar1_init
// 125-147): GET_GSP_STATIC_INFO.bar1PdeBase names the VRAM page directory
// (PD3) of the BAR1 VA space RM created and bound (0xb80f40 virtual, instance
// block -> PDB). The client wraps that PD3 as the root of *its own* BAR1 VMM
// and writes the lower levels and PTEs itself; there is no RM object or RPC
// that maps BAR1 for a GSP client (BAR1 mappings are client-side state, as
// the kernel RM's kbusMapFbAperture is in open-gpu's client/GSP split).
// nouveau replaces PD3 entry 0 with its own tree; so does this.
//
// Plan (all refusal points before the first write):
//   block     0xb80f40: virtual mode, VRAM target -> instance block address
//   inst_pdb  instance block +0x200 (gf100_vmm_join_: target 1:0, addr 63:12) == bar1PdeBase
//   rm_tree   PD3[0..3] read through PRAMIN; RM's existing PD3[0] subtree over
//             [0, size) is walked (VRAM tables only) and its valid PTEs counted
//             and the first few recorded (evidence; RM itself never uses BAR1)
//   build     a private sysmem (coherent) tree: PD2/PD1/PD0 and 64 KiB-page
//             PTs (RM's BAR1 default big page; 2 MiB PD0 PTEs optional, not
//             known to be honoured by BAR1 host translation), BAR1 VA x ->
//             VRAM x for x in [0, size)
//   commit    RM's PD3[0] := our PD3[0] (hi dword, then lo), FB flush, MMU
//             invalidate (PAGE_ALL | HUB_ONLY | ALL_PDB, tu102_vmm_flush)
//   probe     read back through BAR1 vs PRAMIN at caller-chosen VRAM offsets
// Undo: restore the saved PD3[0], flush, invalidate. RM fini also puts BAR1
// back into physical mode (P4: 0xb80f40 bit 31 clear after unload).
//
// The PRAMIN window (BAR0 0x1700 / 0x700000) is saved and restored around
// every access; nouveau uses the same window while GSP-RM runs (instmem
// slow path before BAR2 is up).
#include <stdint.h>
#include <stddef.h>
#include "nv_gsp_chip.h"
#include "nv_gmmu.h"

namespace brook::nvgsp {

// PRAMIN accessor over a Bus (rd32/wr32). Moves the 1 MiB window lazily and
// restores the original base on Restore().
template <class Bus>
struct Pramin {
    Bus& bus;
    const ChipOps& c;
    uint32_t saved = 0, cur = 0;
    bool moved = false;
    Pramin(Bus& b, const ChipOps& ch) : bus(b), c(ch) { saved = cur = bus.rd32(c.reg_pramin_base); }
    uint32_t Off(uint64_t vram) {
        const uint32_t base = uint32_t(vram >> 16) & 0x00ffffff;    // 64 KiB units, VRAM target (bits 25:24 = 0)
        const uint64_t within = vram - (uint64_t(base) << 16);
        if (cur != base || within + 8 > 0x100000) {
            cur = base; moved = true; bus.wr32(c.reg_pramin_base, base);
        }
        return c.pramin + uint32_t(vram - (uint64_t(cur) << 16));
    }
    uint32_t rd32(uint64_t vram) { return bus.rd32(Off(vram)); }
    void wr32(uint64_t vram, uint32_t v) { bus.wr32(Off(vram), v); }
    uint64_t rd64(uint64_t vram) { const uint32_t o = Off(vram); return bus.rd32(o) | (uint64_t(bus.rd32(o + 4)) << 32); }
    void Restore() { if (moved || cur != saved) { bus.wr32(c.reg_pramin_base, saved); cur = saved; moved = false; } }
};

// Page-table memory: VRAM through PRAMIN (read; writes only to RM's PD3 in
// Commit), sysmem pages from `Alloc` (bool(uint64_t* bus, uint8_t** cpu)).
template <class Bus, class Alloc>
struct Bar1PtMem {
    Pramin<Bus>& pr;
    Alloc& alloc_page;
    static constexpr uint32_t kMaxPages = 24;
    uint64_t bus_[kMaxPages] = {};
    uint8_t* cpu_[kMaxPages] = {};
    uint32_t n = 0;
    bool bad = false;
    bool alloc(uint64_t* addr, Aperture* aper) {
        if (n >= kMaxPages) return false;
        uint64_t b; uint8_t* p;
        if (!alloc_page(&b, &p) || !p || (b & 0xfff)) return false;
        for (uint32_t i = 0; i < 4096; ++i) p[i] = 0;
        bus_[n] = b; cpu_[n] = p; ++n;
        *addr = b; *aper = Aperture::SysCoh;
        return true;
    }
    uint64_t* Sys(uint64_t a) {
        for (uint32_t i = 0; i < n; ++i)
            if (a >= bus_[i] && a + 8 <= bus_[i] + 4096) return reinterpret_cast<uint64_t*>(cpu_[i] + (a - bus_[i]));
        return nullptr;
    }
    uint64_t rd64(Aperture a, uint64_t addr) {
        if (a == Aperture::Video) return pr.rd64(addr);
        if (uint64_t* p = Sys(addr)) return *p;
        bad = true; return 0;             // RM sysmem table: not readable here
    }
    void wr64(Aperture a, uint64_t addr, uint64_t v) {
        if (a != Aperture::Video) { if (uint64_t* p = Sys(addr)) { *p = v; return; } }
        bad = true;                        // never written through this path
    }
};

struct Bar1Map {
    const char* stage = "block";
    const char* status = "ok";
    uint32_t block = 0;
    uint64_t inst = 0, inst_pdb_raw = 0, pdb = 0;
    uint64_t pd3_old[4] = {};
    bool rm_tree = false, rm_tree_sys = false;
    uint32_t rm_valid = 0;                 // valid RM PTEs over [0, size)
    uint64_t rm_first_va[4] = {}, rm_first_pte[4] = {};
    uint64_t pd3_new = 0;
    uint32_t tables = 0, ptes = 0;
    uint8_t page_shift = 0;
    bool committed = false, invalidated = false;
    uint32_t probes = 0, probes_equal = 0;
    uint64_t probe_va[4] = {};
    uint32_t probe_bar1[4] = {}, probe_pramin[4] = {};
    bool ok() const { return status[0] == 'o' && status[1] == 'k' && !status[2]; }
};

template <class Bus>
inline bool Bar1FlushInvalidate(Bus& bus, const ChipOps& c, uint64_t pdb) {
    bus.wr32(c.reg_fb_flush, 1);
    for (uint32_t i = 0; i < 2000 && (bus.rd32(c.reg_fb_flush) & 2); ++i) bus.udelay(1);
    bus.wr32(c.reg_mmu_inval_pdb_lo, uint32_t(pdb >> 8));
    bus.wr32(c.reg_mmu_inval_pdb_hi, 0);
    bus.wr32(c.reg_mmu_inval_trigger, 0x80000000u | 0x7);   // PAGE_ALL | HUB_ONLY | ALL_PDB
    for (uint32_t i = 0; i < 200000; ++i) {
        if (!(bus.rd32(c.reg_mmu_inval_trigger) & 0x80000000u)) return true;
        bus.udelay(10);
    }
    return false;
}

// Read-only part: block -> instance block -> PDB check, PD3 and RM's tree.
template <class Bus, class Mem>
inline bool Bar1Inspect(Bus& bus, const ChipOps& c, Pramin<Bus>& pr, Mem& mem, uint64_t bar1_pde, uint64_t size,
                        Bar1Map* r) {
    r->stage = "block";
    r->block = bus.rd32(c.reg_bar1_block);
    if (!(r->block & 0x80000000u)) { r->status = "BAR1 is in physical mode (nothing to do)"; return false; }
    if ((r->block >> 28) & 3) { r->status = "BAR1 instance block not in VRAM"; return false; }
    r->inst = uint64_t(r->block & 0x0fffffffu) << 12;
    r->stage = "inst_pdb";
    r->inst_pdb_raw = pr.rd64(r->inst + 0x200);
    r->pdb = r->inst_pdb_raw & ~0xfffull;
    if ((r->inst_pdb_raw & 3) != 0) { r->status = "BAR1 PDB not in VRAM"; return false; }
    if (!bar1_pde || r->pdb != bar1_pde) { r->status = "instance block PDB != static info bar1PdeBase"; return false; }
    if (!size || size > (1ull << 38) || (size & ((2ull << 20) - 1))) { r->status = "BAR1 size"; return false; }
    r->stage = "rm_tree";
    for (uint32_t i = 0; i < 4; ++i) r->pd3_old[i] = pr.rd64(r->pdb + 8 * i);
    r->rm_tree = gmmu::PdeValid(r->pd3_old[0]);
    if (r->rm_tree) {
        // walk 4 KiB at a time only where tables exist: per 2 MiB, check the
        // PD0 entry first (via a walk of the 2 MiB base), then its 4 KiB pages
        for (uint64_t va = 0; va < size; va += 2ull << 20) {
            Translation t = GmmuWalk(mem, *c.gmmu, r->pdb, Aperture::Video, va);
            if (mem.bad) { r->rm_tree_sys = true; mem.bad = false; break; }
            if (t.ok && t.page_shift >= 21) {
                if (r->rm_valid < 4) { r->rm_first_va[r->rm_valid] = va; r->rm_first_pte[r->rm_valid] = t.pte; }
                ++r->rm_valid;
                continue;
            }
            if (!t.ok && t.fail_level <= c.gmmu->dual_level) continue;   // no small-page table: nothing in this 2 MiB
            for (uint64_t p = 0; p < (2ull << 20); p += 0x1000) {
                const Translation q = GmmuWalk(mem, *c.gmmu, r->pdb, Aperture::Video, va + p);
                if (mem.bad) { r->rm_tree_sys = true; mem.bad = false; break; }
                if (!q.ok) continue;
                if (r->rm_valid < 4) { r->rm_first_va[r->rm_valid] = va + p; r->rm_first_pte[r->rm_valid] = q.pte; }
                ++r->rm_valid;
            }
        }
    }
    return true;
}

// `bar1_rd32(off, &v)` reads the CPU's BAR1 mapping at aperture offset `off`
// (false = not readable). `alloc_page.flush(cpu)` is called for every private
// table page before the commit (write the CPU cache back for the GMMU walk).
template <class Bus, class Alloc, class Bar1Read>
inline Bar1Map MapBar1Identity(Bus& bus, const ChipOps& c, Alloc& alloc_page, uint64_t bar1_pde, uint64_t size,
                               const uint64_t* probe_vram, uint32_t nprobe, Bar1Read bar1_rd32, uint8_t page_shift = 16) {
    Bar1Map r;
    Pramin<Bus> pr(bus, c);
    Bar1PtMem<Bus, Alloc> mem{pr, alloc_page};
    if (!Bar1Inspect(bus, c, pr, mem, bar1_pde, size, &r)) { pr.Restore(); return r; }
    r.stage = "build";
    GmmuBuilder<Bar1PtMem<Bus, Alloc>> b{mem, *c.gmmu};
    if ((page_shift != 16 && page_shift != 21) || !b.Init() || !b.Map(0, 0, size, page_shift, Aperture::Video, 0, false)) {
        r.status = b.err ? b.err : "build"; pr.Restore(); return r;
    }
    r.tables = b.tables; r.ptes = b.ptes; r.page_shift = page_shift;
    r.pd3_new = mem.rd64(b.root_aper, b.root);
    // self-check on the private tree before it is published
    const uint64_t checks[3] = {0, size / 2 + 0x1234, size - 4};
    for (uint64_t va : checks) {
        const Translation t = GmmuWalk(mem, *c.gmmu, b.root, b.root_aper, va);
        if (!t.ok || t.pa != va || t.aper != Aperture::Video) { r.status = "private tree self-check"; pr.Restore(); return r; }
    }
    for (uint32_t i = 0; i < mem.n; ++i) alloc_page.flush(mem.cpu_[i]);
    r.stage = "commit";
    pr.wr32(r.pdb + 4, uint32_t(r.pd3_new >> 32));
    pr.wr32(r.pdb, uint32_t(r.pd3_new));
    r.committed = true;
    r.invalidated = Bar1FlushInvalidate(bus, c, r.pdb);
    if (!r.invalidated) r.status = "MMU invalidate did not complete";
    r.stage = "probe";
    for (uint32_t i = 0; i < nprobe && i < 4; ++i) {
        const uint64_t a = probe_vram[i] & ~3ull;
        if (a + 4 > size) continue;
        uint32_t v = 0;
        r.probe_va[i] = a;
        r.probe_pramin[i] = pr.rd32(a);
        if (!bar1_rd32(a, &v)) continue;
        r.probe_bar1[i] = v; ++r.probes;
        if (v == r.probe_pramin[i]) ++r.probes_equal;
    }
    pr.Restore();
    if (r.ok() && r.probes && r.probes_equal != r.probes) r.status = "BAR1 read-back differs from PRAMIN";
    r.stage = "done";
    return r;
}

// Put RM's PD3[0] back (Bar1Map from MapBar1Identity).
template <class Bus>
inline bool UnmapBar1Identity(Bus& bus, const ChipOps& c, const Bar1Map& m) {
    if (!m.committed) return false;
    Pramin<Bus> pr(bus, c);
    pr.wr32(m.pdb, 0);                                   // invalid first
    pr.wr32(m.pdb + 4, uint32_t(m.pd3_old[0] >> 32));
    pr.wr32(m.pdb, uint32_t(m.pd3_old[0]));
    pr.Restore();
    return Bar1FlushInvalidate(bus, c, m.pdb);
}

} // namespace brook::nvgsp
