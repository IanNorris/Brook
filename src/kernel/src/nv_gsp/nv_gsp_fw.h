#pragma once
// RTX 2080 (TU104) GSP-RM 570.144 firmware and VBIOS parsers (M0, offline).
// Pure, allocation-free and bounds-checked: every accessor takes a Span and
// refuses out-of-range reads, so a truncated or hostile file yields a status,
// never a wild read. Host-tested against the real blobs (test_nv_gsp_fw).
//
// Clean-room from these MIT-licensed sources (Linux master 6e4122f0):
//   N1 nvkm/subdev/gsp/tu102.c   booter ctor 76-116, fwsec_load_bld 118-145
//   N2 nvkm/subdev/gsp/fwsec.c   patch 89-136, v2 172-218, v3 220-258, init 260-306
//   N3 nvkm/subdev/gsp/rm/r535/gsp.c rm_boot_ctor 1837-1866, elf_section 1873-1891,
//                                oneinit 2155-2200 (.fwimage, signature section)
//   N4 nvkm/subdev/bios/base.c   nvbios_addr 31-47 (0xE0 remap), new 150-178
//   N5 nvkm/subdev/bios/image.c, pcir.c, npde.c (image walk), bit.c, pmu.c 30-75
//   N6 nvkm/falcon/fw.c          fw_patch 30-62, fw_sign 177-195
#include <stdint.h>
#include <stddef.h>
#include "nvfw.h"
#include "nvrm_r570.h"

namespace brook::nvgsp {

struct Span {
    const uint8_t* p = nullptr;
    size_t n = 0;
    bool Has(size_t off, size_t len) const { return off <= n && len <= n - off; }
    Span Sub(size_t off, size_t len) const {
        return Has(off, len) ? Span{p + off, len} : Span{};
    }
    bool Valid() const { return p != nullptr; }
};

inline bool Rd8(Span s, size_t o, uint8_t* v) {
    if (!s.Has(o, 1)) return false; *v = s.p[o]; return true;
}
inline bool Rd16(Span s, size_t o, uint16_t* v) {
    if (!s.Has(o, 2)) return false;
    *v = uint16_t(s.p[o] | (s.p[o + 1] << 8)); return true;
}
inline bool Rd32(Span s, size_t o, uint32_t* v) {
    if (!s.Has(o, 4)) return false;
    *v = uint32_t(s.p[o]) | (uint32_t(s.p[o + 1]) << 8) |
         (uint32_t(s.p[o + 2]) << 16) | (uint32_t(s.p[o + 3]) << 24);
    return true;
}
inline bool Rd64(Span s, size_t o, uint64_t* v) {
    uint32_t lo, hi;
    if (!Rd32(s, o, &lo) || !Rd32(s, o + 4, &hi)) return false;
    *v = uint64_t(lo) | (uint64_t(hi) << 32); return true;
}
inline void Wr32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline void Wr64(uint8_t* p, uint64_t v) { Wr32(p, uint32_t(v)); Wr32(p + 4, uint32_t(v >> 32)); }
inline void CopyBytes(uint8_t* d, const uint8_t* s, size_t n) { for (size_t i = 0; i < n; ++i) d[i] = s[i]; }
inline void ZeroBytes(uint8_t* d, size_t n) { for (size_t i = 0; i < n; ++i) d[i] = 0; }
inline bool StrEq(const uint8_t* a, size_t amax, const char* b) {
    size_t i = 0;
    for (; b[i]; ++i) if (i >= amax || a[i] != uint8_t(b[i])) return false;
    return i < amax && a[i] == 0;
}

enum class FwStatus : uint8_t {
    Ok, Truncated, BadMagic, BadHeader, NotFound, Unsupported,
};
inline const char* FwStatusName(FwStatus s) {
    switch (s) {
    case FwStatus::Ok: return "ok";
    case FwStatus::Truncated: return "truncated";
    case FwStatus::BadMagic: return "bad-magic";
    case FwStatus::BadHeader: return "bad-header";
    case FwStatus::NotFound: return "not-found";
    case FwStatus::Unsupported: return "unsupported";
    }
    return "?";
}

// ------------------------------------------------------------------ ELF64
// N3 1873-1891: section by name via e_shoff/e_shstrndx.
inline FwStatus ElfSection(Span elf, const char* name, Span* out) {
    if (!elf.Has(0, 0x40)) return FwStatus::Truncated;
    if (elf.p[0] != 0x7f || elf.p[1] != 'E' || elf.p[2] != 'L' || elf.p[3] != 'F')
        return FwStatus::BadMagic;
    if (elf.p[4] != 2 || elf.p[5] != 1) return FwStatus::Unsupported; // ELF64, LE
    uint64_t shoff; uint16_t shentsize, shnum, shstrndx;
    if (!Rd64(elf, 0x28, &shoff) || !Rd16(elf, 0x3a, &shentsize) ||
        !Rd16(elf, 0x3c, &shnum) || !Rd16(elf, 0x3e, &shstrndx)) return FwStatus::Truncated;
    if (shentsize < 0x40 || shstrndx >= shnum) return FwStatus::BadHeader;
    if (!elf.Has(shoff, uint64_t(shnum) * shentsize)) return FwStatus::Truncated;
    uint64_t stroff, strsize;
    const size_t strhdr = size_t(shoff) + size_t(shstrndx) * shentsize;
    if (!Rd64(elf, strhdr + 0x18, &stroff) || !Rd64(elf, strhdr + 0x20, &strsize))
        return FwStatus::Truncated;
    if (!elf.Has(stroff, strsize)) return FwStatus::Truncated;
    for (uint16_t i = 0; i < shnum; ++i) {
        const size_t h = size_t(shoff) + size_t(i) * shentsize;
        uint32_t nm; uint64_t off, size;
        if (!Rd32(elf, h, &nm) || !Rd64(elf, h + 0x18, &off) || !Rd64(elf, h + 0x20, &size))
            return FwStatus::Truncated;
        if (nm >= strsize) continue;
        if (StrEq(elf.p + stroff + nm, size_t(strsize - nm), name)) {
            if (!elf.Has(off, size)) return FwStatus::Truncated;
            *out = elf.Sub(size_t(off), size_t(size));
            return FwStatus::Ok;
        }
    }
    return FwStatus::NotFound;
}

struct GspElf {
    Span fwimage;     // .fwimage: copied to sysmem pages, mapped by radix3
    Span signature;   // .fwsignature_tu10x (TU102/TU104/TU106)
};
inline FwStatus ParseGspElf(Span elf, GspElf* out) {
    FwStatus s = ElfSection(elf, ".fwimage", &out->fwimage);
    if (s != FwStatus::Ok) return s;
    return ElfSection(elf, ".fwsignature_tu10x", &out->signature);
}

// --------------------------------------------------------------- bin_hdr
inline FwStatus ParseBinHdr(Span blob, nvfw::BinHdr* h) {
    if (!Rd32(blob, 0, &h->bin_magic) || !Rd32(blob, 4, &h->bin_ver) ||
        !Rd32(blob, 8, &h->bin_size) || !Rd32(blob, 12, &h->header_offset) ||
        !Rd32(blob, 16, &h->data_offset) || !Rd32(blob, 20, &h->data_size))
        return FwStatus::Truncated;
    if (h->bin_magic != nvfw::kBinMagic) return FwStatus::BadMagic;
    if (!blob.Has(h->data_offset, h->data_size) || !blob.Has(h->header_offset, 4))
        return FwStatus::Truncated;
    return FwStatus::Ok;
}

// ------------------------------------------------- booter (HS v2 on SEC2)
// N1 76-116: image = data[data_offset, data_size); signature patched at
// image offset `patch_loc` (prod signature idx 0; Turing SEC2 debug fuse
// would select a dbg signature but the 570.144 booters ship prod only).
struct BooterImage {
    nvfw::BinHdr bin;
    nvfw::HsHeaderV2 hs;
    Span image;
    Span sig;                         // the selected (prod, index 0) signature
    uint32_t sig_count = 0, sig_size = 0, sig_dst = 0;
    // falcon load layout (gm200_flcn_fw_load)
    uint32_t nmem_base_img = 0, nmem_base = 0, nmem_size = 0;   // non-secure IMEM
    uint32_t imem_base_img = 0, imem_base = 0, imem_size = 0;   // secure IMEM (app 0)
    uint32_t dmem_base_img = 0, dmem_base = 0, dmem_size = 0;
    uint32_t dmem_sign = 0, boot_addr = 0;
};
inline FwStatus ParseBooter(Span blob, BooterImage* b) {
    FwStatus s = ParseBinHdr(blob, &b->bin);
    if (s != FwStatus::Ok) return s;
    const size_t ho = b->bin.header_offset;
    uint32_t* f = &b->hs.sig_prod_offset;
    for (int i = 0; i < 9; ++i) if (!Rd32(blob, ho + 4 * i, &f[i])) return FwStatus::Truncated;
    uint32_t loc, sig, cnt;
    if (!Rd32(blob, b->hs.patch_loc, &loc) || !Rd32(blob, b->hs.patch_sig, &sig) ||
        !Rd32(blob, b->hs.num_sig, &cnt)) return FwStatus::Truncated;
    if (cnt == 0 || b->hs.sig_prod_size % cnt) return FwStatus::BadHeader;
    b->sig_count = cnt; b->sig_size = b->hs.sig_prod_size / cnt; b->sig_dst = loc;
    b->sig = blob.Sub(size_t(b->hs.sig_prod_offset) + sig, b->sig_size);
    b->image = blob.Sub(b->bin.data_offset, b->bin.data_size);
    if (!b->sig.Valid() || !b->image.Valid()) return FwStatus::Truncated;
    if (!b->image.Has(loc, b->sig_size)) return FwStatus::BadHeader;
    const size_t lo = b->hs.header_offset;
    uint32_t oc, ocs, od, ods, napps, a0o, a0s;
    if (!Rd32(blob, lo, &oc) || !Rd32(blob, lo + 4, &ocs) || !Rd32(blob, lo + 8, &od) ||
        !Rd32(blob, lo + 12, &ods) || !Rd32(blob, lo + 16, &napps) ||
        !Rd32(blob, lo + 20, &a0o) || !Rd32(blob, lo + 24, &a0s)) return FwStatus::Truncated;
    if (napps < 1) return FwStatus::BadHeader;
    b->nmem_base_img = 0;   b->nmem_base = oc;  b->nmem_size = ocs;
    b->imem_base_img = ocs; b->imem_base = a0o; b->imem_size = a0s;
    b->dmem_base_img = od;  b->dmem_base = 0;   b->dmem_size = ods;
    b->dmem_sign = loc - od; b->boot_addr = oc;
    if (!b->image.Has(b->imem_base_img, b->imem_size) ||
        !b->image.Has(b->dmem_base_img, b->dmem_size) || loc < od) return FwStatus::BadHeader;
    return FwStatus::Ok;
}
// Copy the booter image into `dst` (>= image.n bytes) with the signature in.
inline bool BuildBooterImage(const BooterImage& b, uint8_t* dst, size_t cap) {
    if (cap < b.image.n) return false;
    CopyBytes(dst, b.image.p, b.image.n);
    CopyBytes(dst + b.sig_dst, b.sig.p, b.sig_size);
    return true;
}

// --------------------------------------- GSP RISC-V bootloader (desc v4)
// N3 1837-1866: boot.fw = data[data_offset, data_size); offsets from the desc.
struct RiscvBootloader {
    nvfw::BinHdr bin;
    rm570::RM_RISCV_UCODE_DESC desc;
    Span payload;
};
inline FwStatus ParseRiscvBootloader(Span blob, RiscvBootloader* r) {
    FwStatus s = ParseBinHdr(blob, &r->bin);
    if (s != FwStatus::Ok) return s;
    uint32_t* f = &r->desc.version;
    for (size_t i = 0; i < sizeof(r->desc) / 4; ++i)
        if (!Rd32(blob, r->bin.header_offset + 4 * i, &f[i])) return FwStatus::Truncated;
    if (r->desc.version < 1 || r->desc.version > 5) return FwStatus::Unsupported;
    r->payload = blob.Sub(r->bin.data_offset, r->bin.data_size);
    return FwStatus::Ok;
}

// ----------------------------------------- ACR falcon bootloader (bl.bin)
// Needed by FWSEC desc v2 (N2 186-205): code at data+code_off, IMEM tag
// start_tag, boot address start_tag << 8.
struct AcrBl {
    nvfw::BinHdr bin;
    nvfw::BlDesc bl;
    Span code;
    uint32_t boot_addr = 0;
};
inline FwStatus ParseAcrBl(Span blob, AcrBl* a) {
    FwStatus s = ParseBinHdr(blob, &a->bin);
    if (s != FwStatus::Ok) return s;
    uint32_t* f = &a->bl.start_tag;
    for (int i = 0; i < 6; ++i) if (!Rd32(blob, a->bin.header_offset + 4 * i, &f[i])) return FwStatus::Truncated;
    a->code = blob.Sub(size_t(a->bin.data_offset) + a->bl.code_off, a->bl.code_size);
    if (!a->code.Valid() || a->bl.code_size % 4) return FwStatus::BadHeader;
    a->boot_addr = a->bl.start_tag << 8;
    return FwStatus::Ok;
}

// ---------------------------------------------------------------- VBIOS
// N4/N5: walk images (PCIR/NPDS + NPDE sizes), then remap pointers that lie
// past image 0 into the first 0xE0 (extended) image.
struct VbiosImage { uint32_t base, size; uint8_t type; bool last; };
struct Vbios {
    Span rom;
    VbiosImage images[8] = {};
    int count = 0;
    uint32_t image0_size = 0;
    uint32_t imaged_addr = 0;          // 0: no 0xE0 image in this dump
    uint32_t bit_offset = 0;
    bool Remap(uint32_t a, uint32_t* out, uint32_t len) const {
        if (a >= image0_size && imaged_addr) a = a - image0_size + imaged_addr;
        if (!rom.Has(a, len)) return false;
        *out = a; return true;
    }
};
inline FwStatus ParseVbios(Span rom, Vbios* v) {
    v->rom = rom; v->count = 0;
    uint32_t base = 0, size = 0; bool last = false;
    while (!last && v->count < 8) {
        base += size;
        uint16_t sig, pcir_off;
        if (!Rd16(rom, base, &sig) || (sig != 0xaa55 && sig != 0xbb77 && sig != 0x4e56)) break;
        if (!Rd16(rom, base + 0x18, &pcir_off) || pcir_off == 0) break;
        const uint32_t p = base + pcir_off;
        uint32_t magic; uint16_t hdr, blocks; uint8_t type, ind;
        if (!Rd32(rom, p, &magic)) break;
        if (magic != 0x52494350 /*PCIR*/ && magic != 0x53494752 /*RGIS*/ && magic != 0x5344504e /*NPDS*/) break;
        if (!Rd16(rom, p + 0x0a, &hdr) || !Rd16(rom, p + 0x10, &blocks) ||
            !Rd8(rom, p + 0x14, &type) || !Rd8(rom, p + 0x15, &ind)) break;
        size = uint32_t(blocks) * 512; last = ind & 0x80;
        if (type != 0x70) {
            const uint32_t n = (p + hdr + 0x0f) & ~0x0fu;
            uint32_t nm; uint16_t nblocks; uint8_t nind;
            if (Rd32(rom, n, &nm) && nm == 0x4544504e /*NPDE*/ &&
                Rd16(rom, n + 8, &nblocks) && Rd8(rom, n + 0x0a, &nind)) {
                size = uint32_t(nblocks) * 512; last = nind & 0x80;
            }
        } else {
            last = true;
        }
        v->images[v->count++] = VbiosImage{base, size, type, last};
        if (size == 0) break;
    }
    if (v->count == 0) return FwStatus::BadMagic;
    v->image0_size = v->images[0].size;
    v->imaged_addr = 0;
    for (int i = 1; i < v->count; ++i)
        if (v->images[i].type == 0xe0) { v->imaged_addr = v->images[i].base; break; }
    // BIT signature "\xff\xb8BIT"
    v->bit_offset = 0;
    for (size_t i = 0; i + 5 <= rom.n; ++i)
        if (rom.p[i] == 0xff && rom.p[i + 1] == 0xb8 && rom.p[i + 2] == 'B' &&
            rom.p[i + 3] == 'I' && rom.p[i + 4] == 'T') { v->bit_offset = uint32_t(i); break; }
    return v->bit_offset ? FwStatus::Ok : FwStatus::NotFound;
}

struct BitEntry { uint8_t id, version; uint16_t length, offset; };
inline bool FindBit(const Vbios& v, uint8_t id, BitEntry* e) {
    uint8_t entries, esz;
    if (!Rd8(v.rom, v.bit_offset + 10, &entries) || !Rd8(v.rom, v.bit_offset + 9, &esz)) return false;
    uint32_t ent = v.bit_offset + 12;
    for (uint8_t i = 0; i < entries; ++i, ent += esz) {
        uint8_t eid;
        if (!Rd8(v.rom, ent, &eid)) return false;
        if (eid != id) continue;
        return Rd8(v.rom, ent + 1, &e->version) && Rd16(v.rom, ent + 2, &e->length) &&
               Rd16(v.rom, ent + 4, &e->offset) && ((e->id = eid), true);
    }
    return false;
}

// Falcon ucode table behind BIT 'p' v2 (pmu.c): {ver, hdr, len, cnt} then
// cnt entries of {u8 type, u8 ?, u32 data-pointer}.
struct FalconUcodeEntry { uint8_t type; uint32_t ptr; uint32_t resolved; bool in_rom; };
struct FwsecLocation {
    uint32_t bit_p_offset = 0, table_ptr = 0, table_addr = 0;
    uint8_t table_hdr = 0, table_len = 0, table_cnt = 0;
    int entry_index = -1;
    FalconUcodeEntry entry = {};
};
inline FwStatus LocateFalconUcode(const Vbios& v, uint8_t type, FwsecLocation* loc) {
    BitEntry p;
    if (!FindBit(v, 'p', &p)) return FwStatus::NotFound;
    if (p.version != 2 || p.length < 4) return FwStatus::Unsupported;
    loc->bit_p_offset = p.offset;
    uint32_t a;
    if (!v.Remap(p.offset, &a, 4) || !Rd32(v.rom, a, &loc->table_ptr)) return FwStatus::Truncated;
    if (!v.Remap(loc->table_ptr, &loc->table_addr, 4)) return FwStatus::Truncated; // e.g. no 0xE0 image
    if (!Rd8(v.rom, loc->table_addr + 1, &loc->table_hdr) ||
        !Rd8(v.rom, loc->table_addr + 2, &loc->table_len) ||
        !Rd8(v.rom, loc->table_addr + 3, &loc->table_cnt)) return FwStatus::Truncated;
    for (int i = 0; i < loc->table_cnt; ++i) {
        const uint32_t e = loc->table_addr + loc->table_hdr + uint32_t(i) * loc->table_len;
        uint8_t t; uint32_t ptr;
        if (!Rd8(v.rom, e, &t) || !Rd32(v.rom, e + 2, &ptr)) return FwStatus::Truncated;
        if (t != type) continue;
        loc->entry_index = i;
        loc->entry.type = t; loc->entry.ptr = ptr;
        loc->entry.in_rom = v.Remap(ptr, &loc->entry.resolved, 4);
        return loc->entry.in_rom ? FwStatus::Ok : FwStatus::Truncated;
    }
    return FwStatus::NotFound;
}

// FWSEC (falcon ucode type 0x85), run on the GSP falcon in HS mode.
struct FwsecImage {
    FwsecLocation loc;
    uint32_t desc_addr = 0, hdr = 0, desc_size = 0, version = 0;
    nvfw::FalconUcodeDescV2 v2 = {};
    nvfw::FalconUcodeDescV3 v3 = {};
    Span ucode;                        // IMEM + DMEM load image (desc + desc_size)
    uint32_t interface_offset = 0;     // inside DMEM image
    // falcon load layout (N2 v2 172-218 / v3 220-258)
    uint32_t nmem_base_img = 0, nmem_base = 0, nmem_size = 0;
    uint32_t imem_base_img = 0, imem_base = 0, imem_size = 0;
    uint32_t dmem_base_img = 0, dmem_base = 0, dmem_size = 0;
    uint32_t dmem_sign = 0, boot_addr = 0;
    bool needs_acr_bl = false;         // v2 boots through acr/bl.bin + DMA
    Span v3_sigs;                      // v3: SignatureCount * 384 bytes at desc+0x2c
};
constexpr uint8_t kFalconUcodeFwsec = 0x85;
inline FwStatus ParseFwsec(const Vbios& v, FwsecImage* f) {
    FwStatus s = LocateFalconUcode(v, kFalconUcodeFwsec, &f->loc);
    if (s != FwStatus::Ok) return s;
    f->desc_addr = f->loc.entry.resolved;
    if (!Rd32(v.rom, f->desc_addr, &f->hdr)) return FwStatus::Truncated;
    if (!(f->hdr & 1)) return FwStatus::BadHeader;
    f->desc_size = f->hdr >> 16;
    f->version = (f->hdr >> 8) & 0xff;
    const Span rom = v.rom;
    if (f->version == 2) {
        uint32_t* w = &f->v2.Hdr;
        for (int i = 0; i < 15; ++i) if (!Rd32(rom, f->desc_addr + 4 * i, &w[i])) return FwStatus::Truncated;
        f->ucode = rom.Sub(size_t(f->desc_addr) + f->desc_size, size_t(f->v2.IMEMLoadSize) + f->v2.DMEMLoadSize);
        if (!f->ucode.Valid() || f->v2.IMEMSecSize > f->v2.IMEMLoadSize) return FwStatus::Truncated;
        f->nmem_base_img = 0; f->nmem_base = f->v2.IMEMPhysBase;
        f->nmem_size = f->v2.IMEMLoadSize - f->v2.IMEMSecSize;
        f->imem_base_img = 0; f->imem_base = f->v2.IMEMSecBase; f->imem_size = f->v2.IMEMSecSize;
        f->dmem_base_img = f->v2.DMEMOffset; f->dmem_base = f->v2.DMEMPhysBase;
        f->dmem_size = f->v2.DMEMLoadSize;
        f->interface_offset = f->v2.InterfaceOffset;
        f->needs_acr_bl = true;
    } else if (f->version == 3) {
        uint32_t w[11];
        for (int i = 0; i < 11; ++i) if (!Rd32(rom, f->desc_addr + 4 * i, &w[i])) return FwStatus::Truncated;
        f->v3.Hdr = w[0]; f->v3.StoredSize = w[1]; f->v3.PKCDataOffset = w[2];
        f->v3.InterfaceOffset = w[3]; f->v3.IMEMPhysBase = w[4]; f->v3.IMEMLoadSize = w[5];
        f->v3.IMEMVirtBase = w[6]; f->v3.DMEMPhysBase = w[7]; f->v3.DMEMLoadSize = w[8];
        f->v3.EngineIdMask = uint16_t(w[9]); f->v3.UcodeId = uint8_t(w[9] >> 16);
        f->v3.SignatureCount = uint8_t(w[9] >> 24);
        f->v3.SignatureVersions = uint16_t(w[10]); f->v3.Reserved = uint16_t(w[10] >> 16);
        f->ucode = rom.Sub(size_t(f->desc_addr) + f->desc_size, size_t(f->v3.IMEMLoadSize) + f->v3.DMEMLoadSize);
        f->v3_sigs = rom.Sub(size_t(f->desc_addr) + 0x2c, size_t(f->v3.SignatureCount) * 96 * 4);
        if (!f->ucode.Valid() || !f->v3_sigs.Valid() || f->v3.SignatureCount == 0) return FwStatus::Truncated;
        f->imem_base_img = 0; f->imem_base = f->v3.IMEMPhysBase; f->imem_size = f->v3.IMEMLoadSize;
        f->dmem_base_img = f->v3.IMEMLoadSize; f->dmem_base = f->v3.DMEMPhysBase;
        f->dmem_size = (f->v3.DMEMLoadSize + 255) & ~255u;
        f->dmem_sign = f->v3.PKCDataOffset;
        f->interface_offset = f->v3.InterfaceOffset;
        f->boot_addr = 0;
    } else {
        return FwStatus::Unsupported;
    }
    return FwStatus::Ok;
}

// Locations nvkm_gsp_fwsec_patch() writes, as offsets into the ucode image.
struct FwsecPatchInfo {
    uint32_t appif_count = 0;
    uint32_t dmemmapper_img = 0;       // DMEM mapper structure (image offset)
    uint32_t init_cmd_img = 0;         // dmemmapper.init_cmd (image offset)
    uint32_t cmd_in_img = 0;           // FRTS/SB command buffer (image offset)
    uint32_t cmd_in_size = 0;
    uint32_t sig_img = 0, sig_bytes = 0;  // v3 only
};
// Patch a private copy of the FWSEC ucode (N2 89-136, 239-251): init_cmd,
// read_vbios {1, 24, 0, 0, flags 2}, and for FRTS the region {1, 20,
// addr >> 12, size >> 12, FB}. `img` must hold f.ucode.n bytes.
inline FwStatus PatchFwsec(const FwsecImage& f, uint8_t* img, size_t cap, uint32_t init_cmd,
                           uint64_t frts_addr, uint64_t frts_size, FwsecPatchInfo* info) {
    if (cap < f.ucode.n) return FwStatus::Truncated;
    CopyBytes(img, f.ucode.p, f.ucode.n);
    const Span s{img, f.ucode.n};
    if (f.version == 3) {
        info->sig_img = f.dmem_base_img + f.v3.PKCDataOffset; info->sig_bytes = 96 * 4;
        if (!s.Has(info->sig_img, info->sig_bytes)) return FwStatus::BadHeader;
        CopyBytes(img + info->sig_img, f.v3_sigs.p, info->sig_bytes);  // index 0 (no fuse select on TU10x)
    }
    const uint32_t hdr_img = f.dmem_base_img + f.interface_offset;
    uint8_t ver, hlen, elen, cnt;
    if (!Rd8(s, hdr_img, &ver) || !Rd8(s, hdr_img + 1, &hlen) || !Rd8(s, hdr_img + 2, &elen) ||
        !Rd8(s, hdr_img + 3, &cnt)) return FwStatus::Truncated;
    if (ver != 1) return FwStatus::Unsupported;
    info->appif_count = cnt;
    for (uint32_t i = 0; i < cnt; ++i) {
        uint32_t id, dmem_base;
        const uint32_t e = hdr_img + hlen + i * elen;
        if (!Rd32(s, e, &id) || !Rd32(s, e + 4, &dmem_base)) return FwStatus::Truncated;
        if (id != nvfw::kAppIfIdDmemMapper) continue;
        const uint32_t dm = f.dmem_base_img + dmem_base;
        uint32_t cmd_in, cmd_in_size;
        if (!Rd32(s, dm + offsetof(nvfw::DmemMapperV3, cmd_in_buffer_offset), &cmd_in) ||
            !Rd32(s, dm + offsetof(nvfw::DmemMapperV3, cmd_in_buffer_size), &cmd_in_size) ||
            !s.Has(dm, sizeof(nvfw::DmemMapperV3))) return FwStatus::Truncated;
        const uint32_t c = f.dmem_base_img + cmd_in;
        if (!s.Has(c, sizeof(nvfw::FwsecFrtsCmd))) return FwStatus::Truncated;
        info->dmemmapper_img = dm;
        info->init_cmd_img = dm + uint32_t(offsetof(nvfw::DmemMapperV3, init_cmd));
        info->cmd_in_img = c; info->cmd_in_size = cmd_in_size;
        Wr32(img + info->init_cmd_img, init_cmd);
        Wr32(img + c + 0, 1); Wr32(img + c + 4, 24); Wr64(img + c + 8, 0);
        Wr32(img + c + 16, 0); Wr32(img + c + 20, 2);
        if (init_cmd == nvfw::kDmemMapperCmdFrts) {
            Wr32(img + c + 24, 1); Wr32(img + c + 28, 20);
            Wr32(img + c + 32, uint32_t(frts_addr >> 12));
            Wr32(img + c + 36, uint32_t(frts_size >> 12));
            Wr32(img + c + 40, nvfw::kFrtsRegionTypeFb);
        }
        return FwStatus::Ok;
    }
    return FwStatus::NotFound;
}

} // namespace brook::nvgsp
