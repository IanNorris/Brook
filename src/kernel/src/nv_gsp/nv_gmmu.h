#pragma once
// GMMU page-table formats and a small page-table builder + software walker.
// Chip-neutral: the level geometry lives in a GmmuFormat that ChipOps points
// at (TU10x..AD10x use the v2 format; GH100 would add a v3 table).
//
// GMMU v2 (Pascal..Ada), as nouveau writes it (clean-room from Linux nouveau,
// MIT: nvkm/subdev/mmu/vmmgp100.c gp100_vmm_pde 237-250, pd0_pde 254-266,
// pd0_pte 215-226, pgt_pte 132-142, valid 480-497, desc_16/desc_12 403-419;
// vmmtu102.c tu102_vmm_flush 26-47):
//   level  VA bits  entries  entry  notes
//   PD3    48:47    4        8 B    root (4 KiB allocation)
//   PD2    46:38    512      8 B
//   PD1    37:29    512      8 B
//   PD0    28:21    256      16 B   dual: [0] big-page PT PDE or a 2 MiB PTE, [1] small-page PT PDE
//   PT     20:12    512      8 B    4 KiB pages   (big-page PT: 20:16, 32 entries, 64 KiB pages)
//   PDE: aperture 2:1 (0 invalid, 1 video, 2 sys coherent, 3 sys non-coherent),
//        VOL 3 (nouveau sets it for coherent sysmem), address >> 4 (addr 12+ at bit 8).
//   PTE: VALID 0, aperture 2:1 (0 video, 1 peer, 2 sys coherent, 3 sys non-coh),
//        VOL 3, PRIV 5, RO 6, ATOMIC_DISABLE 7, address >> 4, kind 63:56.
#include <stdint.h>
#include <stddef.h>

namespace brook::nvgsp {

enum class Aperture : uint8_t { Video = 0, Peer = 1, SysCoh = 2, SysNcoh = 3 };
inline const char* ApertureName(Aperture a) {
    static const char* const n[4] = {"vid", "peer", "sys", "sysnc"};
    return n[uint8_t(a) & 3];
}

struct GmmuLevel { uint8_t shift, bits, entry_bytes; };
struct GmmuFormat {
    const char* name;
    uint8_t levels;          // including the leaf PT
    GmmuLevel lv[5];         // root first
    uint8_t dual_level;      // index of the dual PDE level (PD0)
    uint8_t big_shift;       // big-page size (64 KiB) for the PD0 [0] table
};
inline constexpr GmmuFormat kGmmuV2 = {
    "gmmu-v2", 5, {{47, 2, 8}, {38, 9, 8}, {29, 9, 8}, {21, 8, 16}, {12, 9, 8}}, 3, 16};

namespace gmmu {
constexpr uint64_t kPteValid = 1ull << 0, kVol = 1ull << 3, kPriv = 1ull << 5, kRo = 1ull << 6;
inline uint64_t AddrField(uint64_t pa) { return (pa >> 4) & 0x00ffffffffffff00ull; }
inline uint64_t FieldAddr(uint64_t e) { return (e & 0x00ffffffffffff00ull) << 4; }
// PDE pointing at a table in `a` (never Peer).
inline uint64_t Pde(uint64_t table, Aperture a) {
    const uint64_t ap = a == Aperture::Video ? 1 : a == Aperture::SysCoh ? 2 : 3;
    return (ap << 1) | (a == Aperture::SysCoh ? kVol : 0) | AddrField(table);
}
inline bool PdeValid(uint64_t e) { return (e >> 1) & 3; }
// PD0 low qword as a big-page PT pointer: the address field starts at bit 4
// (addr >> 8 in 32:4, dev_mmu.h dual PDE), so a 256-byte-aligned table fits.
inline uint64_t PdeBig(uint64_t table, Aperture a) {
    const uint64_t ap = a == Aperture::Video ? 1 : a == Aperture::SysCoh ? 2 : 3;
    return (ap << 1) | (a == Aperture::SysCoh ? kVol : 0) | ((table >> 4) & 0x00fffffffffffff0ull);
}
inline uint64_t FieldAddrBig(uint64_t e) { return (e & 0x00fffffffffffff0ull) << 4; }
inline Aperture PdeAperture(uint64_t e) {
    const uint32_t ap = (e >> 1) & 3;
    return ap == 1 ? Aperture::Video : ap == 2 ? Aperture::SysCoh : Aperture::SysNcoh;
}
inline uint64_t Pte(uint64_t pa, Aperture a, uint8_t kind = 0, bool vol = false, bool priv = false, bool ro = false) {
    return kPteValid | (uint64_t(uint8_t(a)) << 1) | (vol ? kVol : 0) | (priv ? kPriv : 0) | (ro ? kRo : 0) |
           AddrField(pa) | (uint64_t(kind) << 56);
}
inline bool PteValid(uint64_t e) { return e & kPteValid; }
inline Aperture PteAperture(uint64_t e) { return Aperture((e >> 1) & 3); }
}  // namespace gmmu

struct Translation {
    bool ok = false;
    uint64_t pa = 0, pte = 0;
    Aperture aper = Aperture::Video;
    uint8_t page_shift = 0;
    uint8_t fail_level = 0xff;   // level whose entry was missing (0 = root)
    const char* why = "";
};

// Mem: page-table memory. Required:
//   bool alloc(uint64_t* addr, Aperture* aper)   one zeroed 4 KiB page
//   uint64_t rd64(Aperture, uint64_t addr)
//   void wr64(Aperture, uint64_t addr, uint64_t v)
template <class Mem>
inline Translation GmmuWalk(Mem& m, const GmmuFormat& f, uint64_t root, Aperture root_aper, uint64_t va) {
    Translation t;
    uint64_t table = root;
    Aperture ta = root_aper;
    for (uint32_t l = 0; l < f.levels; ++l) {
        const GmmuLevel& L = f.lv[l];
        const uint64_t idx = (va >> L.shift) & ((1ull << L.bits) - 1);
        const uint64_t e0 = m.rd64(ta, table + idx * L.entry_bytes);
        if (l == f.dual_level) {
            const uint64_t e1 = m.rd64(ta, table + idx * L.entry_bytes + 8);
            if (gmmu::PteValid(e0)) {   // 2 MiB page
                t.ok = true; t.pte = e0; t.aper = gmmu::PteAperture(e0); t.page_shift = L.shift;
                t.pa = gmmu::FieldAddr(e0) + (va & ((1ull << L.shift) - 1));
                return t;
            }
            if (gmmu::PdeValid(e0)) {   // big-page PT first
                const uint64_t bi = (va >> f.big_shift) & ((1ull << (L.shift - f.big_shift)) - 1);
                const uint64_t b = m.rd64(gmmu::PdeAperture(e0), gmmu::FieldAddrBig(e0) + bi * 8);
                if (gmmu::PteValid(b)) {
                    t.ok = true; t.pte = b; t.aper = gmmu::PteAperture(b); t.page_shift = f.big_shift;
                    t.pa = gmmu::FieldAddr(b) + (va & ((1ull << f.big_shift) - 1));
                    return t;
                }
            }
            if (!gmmu::PdeValid(e1)) { t.why = "no page table"; t.fail_level = uint8_t(l); return t; }
            table = gmmu::FieldAddr(e1); ta = gmmu::PdeAperture(e1);
            continue;
        }
        if (l + 1 == f.levels) {
            if (!gmmu::PteValid(e0)) { t.why = "invalid PTE"; t.fail_level = uint8_t(l); return t; }
            t.ok = true; t.pte = e0; t.aper = gmmu::PteAperture(e0); t.page_shift = L.shift;
            t.pa = gmmu::FieldAddr(e0) + (va & ((1ull << L.shift) - 1));
            return t;
        }
        if (!gmmu::PdeValid(e0)) { t.why = "invalid PDE"; t.fail_level = uint8_t(l); return t; }
        table = gmmu::FieldAddr(e0); ta = gmmu::PdeAperture(e0);
    }
    t.why = "format";
    return t;
}

// Builder over a root it owns (or adopts). Maps 4 KiB, 64 KiB or 2 MiB pages;
// does not free tables. Counts what it wrote for the reports.
template <class Mem>
struct GmmuBuilder {
    Mem& m;
    const GmmuFormat& f;
    uint64_t root = 0;
    Aperture root_aper = Aperture::SysCoh;
    uint32_t tables = 0, ptes = 0;
    const char* err = nullptr;

    bool Init() { if (!m.alloc(&root, &root_aper)) { err = "root alloc"; return false; } ++tables; return true; }
    void Adopt(uint64_t r, Aperture a) { root = r; root_aper = a; }

    // Big-page PTs are 0x100 bytes: 16 share one 4 KiB page (nouveau desc_16 LPT size).
    uint64_t lpt_page = 0; Aperture lpt_aper = Aperture::SysCoh; uint32_t lpt_off = 0x1000;
    // Child table behind entry `slot` (byte offset within `table`); allocates it.
    bool Child(uint64_t table, Aperture ta, uint64_t slot, uint64_t* out, Aperture* oa, bool big_pt = false) {
        const uint64_t e = m.rd64(ta, table + slot);
        if (gmmu::PteValid(e)) { err = "PTE where a PDE is needed"; return false; }
        if (gmmu::PdeValid(e)) {
            *out = big_pt ? gmmu::FieldAddrBig(e) : gmmu::FieldAddr(e); *oa = gmmu::PdeAperture(e); return true;
        }
        uint64_t a; Aperture ap;
        if (big_pt) {
            if (lpt_off >= 0x1000) {
                if (!m.alloc(&lpt_page, &lpt_aper)) { err = "table alloc"; return false; }
                lpt_off = 0;
            }
            a = lpt_page + lpt_off; ap = lpt_aper; lpt_off += 0x100;
        } else if (!m.alloc(&a, &ap)) { err = "table alloc"; return false; }
        ++tables;
        m.wr64(ta, table + slot, big_pt ? gmmu::PdeBig(a, ap) : gmmu::Pde(a, ap));
        *out = a; *oa = ap;
        return true;
    }

    bool MapPage(uint64_t va, uint64_t pa, uint8_t shift, Aperture tgt, uint8_t kind, bool vol, bool priv = false,
                 bool ro = false) {
        const uint64_t pte = gmmu::Pte(pa, tgt, kind, vol, priv, ro);
        uint64_t table = root; Aperture ta = root_aper;
        for (uint32_t l = 0; l < f.levels; ++l) {
            const GmmuLevel& L = f.lv[l];
            const uint64_t idx = (va >> L.shift) & ((1ull << L.bits) - 1);
            const uint64_t slot = idx * L.entry_bytes;
            if (l == f.dual_level) {
                if (shift == L.shift) {
                    const uint64_t e = m.rd64(ta, table + slot);
                    if (gmmu::PdeValid(e) && !gmmu::PteValid(e)) { err = "2 MiB over a big-page PT"; return false; }
                    if (gmmu::PdeValid(m.rd64(ta, table + slot + 8))) { err = "2 MiB over a small-page PT"; return false; }
                    m.wr64(ta, table + slot, pte); ++ptes; return true;
                }
                if (shift == f.big_shift) {
                    uint64_t pt; Aperture pa_;
                    if (!Child(table, ta, slot, &pt, &pa_, true)) return false;
                    const uint64_t bi = (va >> f.big_shift) & ((1ull << (L.shift - f.big_shift)) - 1);
                    m.wr64(pa_, pt + bi * 8, pte); ++ptes; return true;
                }
                if (gmmu::PteValid(m.rd64(ta, table + slot))) { err = "4 KiB inside a 2 MiB page"; return false; }
                if (!Child(table, ta, slot + 8, &table, &ta)) return false;
                continue;
            }
            if (l + 1 == f.levels) {
                if (shift != L.shift) { err = "page size"; return false; }
                m.wr64(ta, table + slot, pte); ++ptes; return true;
            }
            if (!Child(table, ta, slot, &table, &ta)) return false;
        }
        err = "format"; return false;
    }

    // Map [va, va+bytes) -> [pa, ...) with `shift`-sized pages (all aligned).
    bool Map(uint64_t va, uint64_t pa, uint64_t bytes, uint8_t shift, Aperture tgt, uint8_t kind = 0, bool vol = false,
             bool priv = false, bool ro = false) {
        const uint64_t ps = 1ull << shift;
        if ((va | pa | bytes) & (ps - 1)) { err = "alignment"; return false; }
        for (uint64_t o = 0; o < bytes; o += ps)
            if (!MapPage(va + o, pa + o, shift, tgt, kind, vol, priv, ro)) return false;
        return true;
    }
    // The directory tables on the path to `va` for levels [0, depth) (root first), created
    // when missing; depth must stop above the dual level. For RM's COPY_SERVER_RESERVED_PDES.
    bool Path(uint64_t va, uint32_t depth, uint64_t* out, Aperture* apers) {
        uint64_t table = root; Aperture ta = root_aper;
        if (depth > f.dual_level) { err = "path depth"; return false; }
        for (uint32_t l = 0; l < depth; ++l) {
            out[l] = table; apers[l] = ta;
            if (l + 1 == depth) break;
            const GmmuLevel& L = f.lv[l];
            const uint64_t slot = ((va >> L.shift) & ((1ull << L.bits) - 1)) * L.entry_bytes;
            if (!Child(table, ta, slot, &table, &ta)) return false;
        }
        return true;
    }
};

} // namespace brook::nvgsp
