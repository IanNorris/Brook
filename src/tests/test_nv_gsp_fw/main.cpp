// Host test for nv_gsp_fw.h: GSP ELF, bin_hdr, HS v2 booter, RISC-V
// bootloader desc, ACR bl, VBIOS image walk + 0xE0 remap, FWSEC desc v2/v3
// and the FRTS DMEM-mapper patch.
//
// Synthetic containers always run. The real 570.144 blobs and the 25 Nov
// VBIOS dump run when these are set (ctest passes the CMake cache values):
//   BROOK_NV_GSP_FW_DIR  dir with gsp/booter_load/booter_unload/bootloader-570.144.bin
//   BROOK_NV_GSP_ACR_BL  nvidia/tu104/acr/bl.bin
//   BROOK_NV_GSP_VBIOS   full ROM (333824 B), BROOK_NV_GSP_PCIROM partial (55808 B)
// Goldens for the real files: nouveau's dmesg of the 25 Nov capture
// (binHdr/hsHeader/hsLoadHeader dumps, "patch idx:0 src:0000003c dst:00008700",
// flcnBlDmemDesc) and an independent Python walk of the ROM.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "nv_gsp/nv_gsp_fw.h"
#include "nv_gsp/nv_gsp_wpr.h"
#include "../nv_gsp_fake/synth_fw.h"

using namespace brook::nvgsp;
using synth_fw::Bytes;

static Span S(const Bytes& b) { return Span{b.data(), b.size()}; }
static bool Load(const std::string& path, Bytes* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out->empty();
}
static const char* Env(const char* n) { const char* v = std::getenv(n); return (v && *v) ? v : nullptr; }

static void TestElfSynthetic() {
    Bytes e = synth_fw::Elf(0x3456, 0x1000);
    GspElf g;
    assert(ParseGspElf(S(e), &g) == FwStatus::Ok);
    assert(g.fwimage.n == 0x3456 && g.fwimage.p == e.data() + 0x40);
    assert(g.signature.n == 0x1000 && g.signature.p[0] == 0x55);
    Span x;
    assert(ElfSection(S(e), ".fwsignature_ga100", &x) == FwStatus::NotFound);
    assert(ElfSection(S(e), ".fwimag", &x) == FwStatus::NotFound);       // no prefix matches
    Bytes bad = e; bad[1] = 'X';
    assert(ElfSection(S(bad), ".fwimage", &x) == FwStatus::BadMagic);
    bad = e; bad[4] = 1;
    assert(ElfSection(S(bad), ".fwimage", &x) == FwStatus::Unsupported);
    for (size_t n = 0; n < e.size(); n += 97) {                          // every truncation is refused
        Bytes t(e.begin(), e.begin() + long(n));
        assert(ParseGspElf(S(t), &g) != FwStatus::Ok);
    }
}

static void TestBooterSynthetic() {
    Bytes b = synth_fw::Booter();
    BooterImage bi;
    assert(ParseBooter(S(b), &bi) == FwStatus::Ok);
    assert(bi.image.n == 0x600 && bi.sig_count == 1 && bi.sig_size == 0x10 && bi.sig_dst == 0x500);
    assert(bi.nmem_base == 0 && bi.nmem_size == 0x100 && bi.imem_base_img == 0x100);
    assert(bi.imem_base == 0x100 && bi.imem_size == 0x300);
    assert(bi.dmem_base_img == 0x400 && bi.dmem_size == 0x200 && bi.dmem_sign == 0x100 && bi.boot_addr == 0);
    std::vector<uint8_t> img(bi.image.n);
    assert(BuildBooterImage(bi, img.data(), img.size()));
    assert(std::memcmp(img.data() + 0x500, b.data() + 0x60, 0x10) == 0);
    assert(std::memcmp(img.data(), b.data() + 0x100, 0x500) == 0);
    Bytes bad = b; bad[0] = 0;
    assert(ParseBooter(S(bad), &bi) == FwStatus::BadMagic);
    bad = b; synth_fw::P32(bad, 0x88, 0);                                 // zero signatures
    assert(ParseBooter(S(bad), &bi) == FwStatus::BadHeader);
    bad = b; synth_fw::P32(bad, 0x80, 0x5f8);                             // patch past image
    assert(ParseBooter(S(bad), &bi) == FwStatus::BadHeader);
    for (size_t n = 0; n < b.size(); n += 13) {
        Bytes t(b.begin(), b.begin() + long(n));
        assert(ParseBooter(S(t), &bi) != FwStatus::Ok);
    }
}

static void TestSmallContainers() {
    Bytes r = synth_fw::RiscvBl();
    RiscvBootloader rb;
    assert(ParseRiscvBootloader(S(r), &rb) == FwStatus::Ok);
    assert(rb.desc.version == 4 && rb.desc.appVersion == 0 && rb.payload.n == 0x1000);
    Bytes a = synth_fw::AcrBl();
    AcrBl bl;
    assert(ParseAcrBl(S(a), &bl) == FwStatus::Ok);
    assert(bl.bl.start_tag == 0xfd && bl.code.n == 0x200 && bl.boot_addr == 0xfd00);
    assert(bl.code.p == a.data() + 0x40);
}

static void CheckFrtsPatch(const FwsecImage& f, const std::vector<uint8_t>& img, const FwsecPatchInfo& pi,
                           uint64_t frts, uint64_t size) {
    auto U = [&](uint32_t o) { uint32_t v; std::memcpy(&v, img.data() + o, 4); return v; };
    assert(U(pi.init_cmd_img) == 0x15);
    const uint32_t c = pi.cmd_in_img;
    assert(U(c) == 1 && U(c + 4) == 24 && U(c + 8) == 0 && U(c + 12) == 0 && U(c + 16) == 0 && U(c + 20) == 2);
    assert(U(c + 24) == 1 && U(c + 28) == 20 && U(c + 32) == uint32_t(frts >> 12) &&
           U(c + 36) == uint32_t(size >> 12) && U(c + 40) == 2);
    (void)f;
}

static void TestVbiosSynthetic() {
    synth_fw::SynthVbios sv = synth_fw::Vbios(true, 2);
    Vbios v;
    assert(ParseVbios(S(sv.rom), &v) == FwStatus::Ok);
    assert(v.count == 3 && v.image0_size == 0x1000 && v.imaged_addr == 0x2000);
    assert(v.images[1].type == 0x03 && v.images[2].type == 0xe0 && v.images[2].last);
    uint32_t a;
    assert(v.Remap(0x0fff, &a, 1) && a == 0x0fff);                       // below image0: unchanged
    assert(v.Remap(0x1100, &a, 4) && a == 0x2100);                       // past image0: into 0xE0
    FwsecImage f;
    assert(ParseFwsec(v, &f) == FwStatus::Ok);
    assert(f.loc.table_ptr == 0x1100 && f.loc.table_addr == 0x2100 && f.loc.entry_index == 1);
    assert(f.desc_addr == 0x2200 && f.version == 2 && f.desc_size == 0x3c && f.needs_acr_bl);
    assert(f.ucode.p == sv.rom.data() + sv.ucode_addr && f.ucode.n == 0x500);
    assert(f.nmem_size == 0x100 && f.imem_base == 0x100 && f.imem_size == 0x300);
    assert(f.dmem_base_img == 0x400 && f.dmem_size == 0x100 && f.interface_offset == 0x10);
    std::vector<uint8_t> img(f.ucode.n);
    FwsecPatchInfo pi;
    assert(PatchFwsec(f, img.data(), img.size(), 0x15, 0x1ffee0000ull, 0x100000, &pi) == FwStatus::Ok);
    assert(pi.appif_count == 1 && pi.dmemmapper_img == 0x440 && pi.cmd_in_img == 0x480);
    CheckFrtsPatch(f, img, pi, 0x1ffee0000ull, 0x100000);
    assert(std::memcmp(img.data(), f.ucode.p, 0x400) == 0);              // IMEM untouched
    // SB command: init_cmd 0x19, read_vbios only
    assert(PatchFwsec(f, img.data(), img.size(), 0x19, 0, 0, &pi) == FwStatus::Ok);
    uint32_t w; std::memcpy(&w, img.data() + pi.init_cmd_img, 4); assert(w == 0x19);
    std::memcpy(&w, img.data() + pi.cmd_in_img + 24, 4); assert(w == 0);

    // v3 descriptor: signature 0 patched at DMEM + PKCDataOffset
    synth_fw::SynthVbios s3 = synth_fw::Vbios(true, 3);
    assert(ParseVbios(S(s3.rom), &v) == FwStatus::Ok);
    FwsecImage f3;
    assert(ParseFwsec(v, &f3) == FwStatus::Ok);
    assert(f3.version == 3 && !f3.needs_acr_bl && f3.v3.SignatureCount == 1 && f3.v3.UcodeId == 1);
    assert(f3.imem_size == 0x400 && f3.dmem_base_img == 0x400 && f3.dmem_size == 0x300 && f3.dmem_sign == 0x100);
    std::vector<uint8_t> img3(f3.ucode.n);
    assert(PatchFwsec(f3, img3.data(), img3.size(), 0x15, 0x1ffee0000ull, 0x100000, &pi) == FwStatus::Ok);
    assert(pi.sig_img == 0x500 && pi.sig_bytes == 384);
    assert(std::memcmp(img3.data() + 0x500, s3.rom.data() + s3.desc_addr + 0x2c, 384) == 0);
    CheckFrtsPatch(f3, img3, pi, 0x1ffee0000ull, 0x100000);

    // Without the 0xE0 image the table pointer cannot be resolved: this is
    // what the 23 Nov 128 KiB dump and the 55808-byte PCI ROM read look like.
    synth_fw::SynthVbios s0 = synth_fw::Vbios(false, 2);
    assert(ParseVbios(S(s0.rom), &v) == FwStatus::Ok);
    assert(v.imaged_addr == 0 && v.count == 2);
    assert(ParseFwsec(v, &f) != FwStatus::Ok);

    // Corrupt or truncated ROMs never pass.
    for (size_t n = 0x100; n < sv.rom.size(); n += 0x101) {
        Bytes t(sv.rom.begin(), sv.rom.begin() + long(n));
        if (ParseVbios(S(t), &v) == FwStatus::Ok) assert(ParseFwsec(v, &f) != FwStatus::Ok || n >= 0x273c);
    }
    uint32_t seed = 1;
    for (int it = 0; it < 2000; ++it) {
        Bytes t = sv.rom;
        for (int k = 0; k < 4; ++k) { seed = seed * 1103515245 + 12345; t[(seed >> 8) % 0x2800] ^= uint8_t(seed >> 3); }
        if (ParseVbios(S(t), &v) != FwStatus::Ok) continue;
        if (ParseFwsec(v, &f) != FwStatus::Ok) continue;
        std::vector<uint8_t> o(f.ucode.n);
        (void)PatchFwsec(f, o.data(), o.size(), 0x15, 0x1000000, 0x100000, &pi);
    }
}

// ------------------------------------------------------------ real files
static int TestRealFirmware() {
    const char* dir = Env("BROOK_NV_GSP_FW_DIR");
    if (!dir) { std::printf("SKIP real firmware (BROOK_NV_GSP_FW_DIR unset)\n"); return 0; }
    const std::string d(dir);
    Bytes gsp, bl, bul, rbl;
    assert(Load(d + "/gsp-570.144.bin", &gsp) && gsp.size() == 28542040);
    assert(Load(d + "/booter_load-570.144.bin", &bl) && bl.size() == 59272);
    assert(Load(d + "/booter_unload-570.144.bin", &bul) && bul.size() == 39304);
    assert(Load(d + "/bootloader-570.144.bin", &rbl) && rbl.size() == 4196);
    GspElf g;
    assert(ParseGspElf(S(gsp), &g) == FwStatus::Ok);
    assert(g.fwimage.p == gsp.data() + 0x40 && g.fwimage.n == 28528288);
    assert(g.signature.p == gsp.data() + 0x1b36f0c && g.signature.n == 0x1000);
    Span ver;
    assert(ElfSection(S(gsp), ".fwversion", &ver) == FwStatus::Ok && ver.n == 8);

    BooterImage b;
    assert(ParseBooter(S(bl), &b) == FwStatus::Ok);
    assert(b.bin.header_offset == 0x18 && b.bin.data_offset == 0x88 && b.bin.data_size == 0xe700);
    assert(b.hs.sig_prod_offset == 0x3c && b.hs.sig_prod_size == 0x10 && b.hs.patch_loc == 0x4c &&
           b.hs.patch_sig == 0x50 && b.hs.meta_data_offset == 0x54 && b.hs.meta_data_size == 0xc &&
           b.hs.num_sig == 0x60 && b.hs.header_offset == 0x64 && b.hs.header_size == 0x24);
    assert(b.sig_count == 1 && b.sig_size == 16 && b.sig_dst == 0x8700 && b.sig.p == bl.data() + 0x3c);
    assert(b.nmem_base == 0 && b.nmem_size == 0x100 && b.imem_base_img == 0x100 && b.imem_base == 0x100 &&
           b.imem_size == 0x8400 && b.dmem_base_img == 0x8500 && b.dmem_size == 0x6200 && b.dmem_sign == 0x200);
    BooterImage u;
    assert(ParseBooter(S(bul), &u) == FwStatus::Ok);
    assert(u.bin.data_size == 0x9900 && u.dmem_base_img == 0x4c00 && u.dmem_size == 0x4d00 &&
           u.imem_size == 0x4b00 && u.sig_count == 1);

    RiscvBootloader r;
    assert(ParseRiscvBootloader(S(rbl), &r) == FwStatus::Ok);
    assert(r.desc.version == 4 && r.bin.data_size == 0x1000 && r.desc.appVersion == 0);
    assert(r.desc.monitorCodeOffset == 0 && r.desc.monitorDataOffset == 0 && r.desc.manifestOffset == 0);

    // WPR2 layout from the real sizes and the trace's 0x625f04.
    FbLayoutInput in;
    in.fb_size = 8ull << 30; in.reg_625f04 = 0x00020e09;
    in.elf_size = g.fwimage.n; in.boot_size = r.bin.data_size;
    FbLayout L;
    assert(ComputeFbLayout(in, &L));
    assert(L.frts.addr == 0x1ffee0000ull && L.wpr2.addr == 0x1f7900000ull && L.non_wpr_heap.addr == 0x1f7800000ull);
    std::printf("real firmware: ok (fwimage %zu B, booter_load sig dst 0x%x, bootloader v%u)\n",
                g.fwimage.n, b.sig_dst, r.desc.version);

    if (const char* acr = Env("BROOK_NV_GSP_ACR_BL")) {
        Bytes ab;
        assert(Load(acr, &ab) && ab.size() == 1280);
        AcrBl a;
        assert(ParseAcrBl(S(ab), &a) == FwStatus::Ok);
        assert(a.bl.start_tag == 0xfd && a.bl.code_off == 0 && a.bl.code_size == 0x200 && a.boot_addr == 0xfd00);
        assert(a.bin.header_offset == 0x100 && a.bin.data_offset == 0x200);
    }
    return 1;
}

static int TestRealVbios() {
    const char* path = Env("BROOK_NV_GSP_VBIOS");
    if (!path) { std::printf("SKIP real VBIOS (BROOK_NV_GSP_VBIOS unset)\n"); return 0; }
    Bytes rom;
    assert(Load(path, &rom) && rom.size() == 333824);
    Vbios v;
    assert(ParseVbios(S(rom), &v) == FwStatus::Ok);
    static const VbiosImage want[] = {
        {0x00000, 0x0e800, 0x00, false}, {0x0e800, 0x11600, 0x03, false}, {0x1fe00, 0x0c200, 0xe0, false},
        {0x2c000, 0x23c00, 0xe0, false}, {0x4fc00, 0x01c00, 0x70, true},
    };
    assert(v.count == 5);
    for (int i = 0; i < 5; ++i)
        assert(v.images[i].base == want[i].base && v.images[i].size == want[i].size &&
               v.images[i].type == want[i].type && v.images[i].last == want[i].last);
    assert(v.image0_size == 0xe800 && v.imaged_addr == 0x1fe00 && v.bit_offset == 0x1b0);
    FwsecImage f;
    assert(ParseFwsec(v, &f) == FwStatus::Ok);
    assert(f.loc.bit_p_offset == 0x3b5 && f.loc.table_ptr == 0xe924 && f.loc.table_addr == 0x1ff24);
    assert(f.loc.table_hdr == 6 && f.loc.table_len == 6 && f.loc.table_cnt == 16 && f.loc.entry_index == 9);
    assert(f.loc.entry.ptr == 0x2df00 && f.desc_addr == 0x3f500);
    assert(f.version == 2 && f.desc_size == 0x3c && f.hdr == 0x003c0201);
    assert(f.v2.StoredSize == 0xa110 && f.v2.IMEMLoadSize == 0x9d00 && f.v2.IMEMSecBase == 0x300 &&
           f.v2.IMEMSecSize == 0x9a00 && f.v2.DMEMOffset == 0x9d00 && f.v2.DMEMLoadSize == 0x410 &&
           f.v2.InterfaceOffset == 0xdc);
    // nouveau's flcnBlDmemDesc dump: nonSec 0x0+0x300, sec 0x300+0x9a00, data 0x410 at +0x9d00
    assert(f.nmem_base == 0 && f.nmem_size == 0x300 && f.imem_base == 0x300 && f.imem_size == 0x9a00 &&
           f.dmem_base_img == 0x9d00 && f.dmem_size == 0x410 && f.needs_acr_bl);
    assert(f.ucode.p == rom.data() + 0x3f53c && f.ucode.n == 0xa110);
    std::vector<uint8_t> img(f.ucode.n);
    FwsecPatchInfo pi;
    assert(PatchFwsec(f, img.data(), img.size(), 0x15, 0x1ffee0000ull, 0x100000, &pi) == FwStatus::Ok);
    CheckFrtsPatch(f, img, pi, 0x1ffee0000ull, 0x100000);
    size_t changed = 0;
    for (size_t i = 0; i < img.size(); ++i) changed += img[i] != f.ucode.p[i];
    std::printf("real VBIOS: FWSEC v2 at 0x%x, appif %u, DMEM mapper @dmem+0x%x, cmd @dmem+0x%x (%u B), "
                "%zu bytes patched\n", f.desc_addr, pi.appif_count, pi.dmemmapper_img - f.dmem_base_img,
                pi.cmd_in_img - f.dmem_base_img, pi.cmd_in_size, changed);
    assert(changed > 0 && changed <= 4 + 44);
    // The other falcon ucode types are listed but not used by route B.
    FwsecLocation loc;
    assert(LocateFalconUcode(v, 0x45, &loc) == FwStatus::Ok && loc.entry_index == 8);

    if (const char* pr = Env("BROOK_NV_GSP_PCIROM")) {
        Bytes pc;
        assert(Load(pr, &pc) && pc.size() == 55808);
        Vbios pv;
        assert(ParseVbios(S(pc), &pv) == FwStatus::Ok);
        assert(pv.imaged_addr == 0);                 // the PCI ROM BAR read stops before the 0xE0 images
        assert(ParseFwsec(pv, &f) != FwStatus::Ok);
        std::printf("PCI ROM dump: %d image(s), FWSEC unreachable (as expected)\n", pv.count);
    }
    return 1;
}

int main() {
    TestElfSynthetic();
    TestBooterSynthetic();
    TestSmallContainers();
    TestVbiosSynthetic();
    const int real = TestRealFirmware() + TestRealVbios();
    std::printf("test_nv_gsp_fw: OK (%d real-file group(s))\n", real);
    return 0;
}
