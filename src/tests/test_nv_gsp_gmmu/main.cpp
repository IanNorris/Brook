// GMMU v2 encodings, builder and software walker (nv_gsp/nv_gmmu.h), and the
// static-info parser (nv_gsp/nv_gsp_static.h) on a hand-built reply.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <vector>
#include "nv_gsp/nv_gmmu.h"
#include "nv_gsp/nv_gsp_static.h"

using namespace brook::nvgsp;

// Two apertures, sparse pages; allocations come from VRAM or sysmem.
struct Mem {
    std::map<uint64_t, std::vector<uint8_t>> vid, sys;
    uint64_t next_vid = 0x1000000, next_sys = 0x80000000;
    bool use_sys = false;
    uint32_t allocs = 0;
    std::map<uint64_t, std::vector<uint8_t>>& M(Aperture a) { return a == Aperture::Video ? vid : sys; }
    bool alloc(uint64_t* a, Aperture* ap) {
        ++allocs;
        *ap = use_sys ? Aperture::SysCoh : Aperture::Video;
        *a = use_sys ? (next_sys += 0x1000) : (next_vid += 0x1000);
        M(*ap)[*a].assign(0x1000, 0);
        return true;
    }
    uint64_t rd64(Aperture a, uint64_t addr) {
        auto& m = M(a); auto it = m.find(addr & ~0xfffull);
        if (it == m.end()) return 0;
        uint64_t v; std::memcpy(&v, it->second.data() + (addr & 0xfff), 8); return v;
    }
    void wr64(Aperture a, uint64_t addr, uint64_t v) {
        auto& m = M(a); auto& pg = m[addr & ~0xfffull]; if (pg.empty()) pg.assign(0x1000, 0);
        std::memcpy(pg.data() + (addr & 0xfff), &v, 8);
    }
};

int main() {
    // ---- encodings against nouveau's formulas (gp100_vmm_pde / gp100_vmm_valid)
    assert(gmmu::Pde(0x1f76c3000ull, Aperture::Video) == ((1ull << 1) | (0x1f76c3000ull >> 4)));
    assert(gmmu::Pde(0x12345000ull, Aperture::SysCoh) == ((2ull << 1) | (1ull << 3) | (0x12345000ull >> 4)));
    assert(gmmu::Pde(0x12345000ull, Aperture::SysNcoh) == ((3ull << 1) | (0x12345000ull >> 4)));
    assert(gmmu::Pte(0x200000, Aperture::Video) == (1ull | (0x200000ull >> 4)));
    assert(gmmu::Pte(0x5000, Aperture::SysCoh, 0x06, true) == (1ull | (2ull << 1) | (1ull << 3) | (0x5000ull >> 4) | (0x06ull << 56)));
    assert(gmmu::FieldAddr(gmmu::Pde(0x7ff123000ull, Aperture::Video)) == 0x7ff123000ull);
    assert(!gmmu::PdeValid(0) && gmmu::PdeValid(gmmu::Pde(0x1000, Aperture::Video)));

    for (int sys = 0; sys < 2; ++sys) {
        Mem m; m.use_sys = sys;
        GmmuBuilder<Mem> b{m, kGmmuV2};
        assert(b.Init());
        // 2 MiB, 64 KiB and 4 KiB mappings in different 2 MiB blocks
        assert(b.Map(0, 0, 8ull << 20, 21, Aperture::Video));
        assert(b.Map(0x10000000, 0x30000000, 2ull << 20, 16, Aperture::Video));
        assert(b.Map(0x2000000000ull, 0x5550000, 0x3000, 12, Aperture::SysCoh, 0, true));
        assert(b.Map(0x1fffffe000ull, 0x777000, 0x2000, 12, Aperture::SysCoh));   // crosses a PD1 boundary
        // conflicts are refused
        assert(!b.MapPage(0x100000, 0x100000, 12, Aperture::Video, 0, false));    // 4K inside a 2M page
        assert(!b.MapPage(0x10000000, 0, 21, Aperture::Video, 0, false));         // 2M over a big PT
        assert(!b.Map(0x1000, 0x1000, 0x1000, 16, Aperture::Video));              // alignment
        std::mt19937_64 rng(5);
        for (int i = 0; i < 2000; ++i) {
            const uint64_t o = rng() % (8ull << 20);
            Translation t = GmmuWalk(m, kGmmuV2, b.root, b.root_aper, o);
            assert(t.ok && t.pa == o && t.page_shift == 21 && t.aper == Aperture::Video);
            const uint64_t q = rng() % (2ull << 20);
            t = GmmuWalk(m, kGmmuV2, b.root, b.root_aper, 0x10000000 + q);
            assert(t.ok && t.pa == 0x30000000 + q && t.page_shift == 16);
        }
        Translation t = GmmuWalk(m, kGmmuV2, b.root, b.root_aper, 0x2000001234ull);
        assert(t.ok && t.pa == 0x5551234 && t.page_shift == 12 && t.aper == Aperture::SysCoh && (t.pte & gmmu::kVol));
        t = GmmuWalk(m, kGmmuV2, b.root, b.root_aper, 0x2000000000ull + 0x3000);
        assert(!t.ok && t.fail_level == 4);
        t = GmmuWalk(m, kGmmuV2, b.root, b.root_aper, 0x1ffffff010ull);   // second page, the next PD1 entry
        assert(t.ok && t.pa == 0x778010);
        t = GmmuWalk(m, kGmmuV2, b.root, b.root_aper, 0x4000000000ull);
        assert(!t.ok && t.fail_level == 1);
        t = GmmuWalk(m, kGmmuV2, b.root, b.root_aper, 0x8000000);   // PD0 slot never written
        assert(!t.ok && t.fail_level == 3);
        // 16 big-page PTs share a page: 4 + 1 (64K PT page) + small PTs
        std::printf("gmmu (%s tables): %u tables, %u ptes, %u pages\n", sys ? "sys" : "vid", b.tables, b.ptes, m.allocs);
    }
    // 256 MiB BAR1 identity with 64 KiB pages: PD3+PD2+PD1+PD0 + 8 LPT pages
    {
        Mem m; m.use_sys = true;
        GmmuBuilder<Mem> b{m, kGmmuV2};
        assert(b.Init() && b.Map(0, 0, 256ull << 20, 16, Aperture::Video));
        assert(m.allocs == 4 + 8 && b.ptes == 4096 && b.tables == 4 + 128);
        for (uint64_t va = 0; va < (256ull << 20); va += 0x10000 * 37 + 0x44) {
            const Translation t = GmmuWalk(m, kGmmuV2, b.root, b.root_aper, va);
            assert(t.ok && t.pa == va && t.page_shift == 16);
        }
    }

    // ---- static info
    {
        namespace si = rm570::static_info;
        std::vector<uint8_t> r(si::kBytes, 0);
        auto w32 = [&](uint32_t o, uint32_t v) { std::memcpy(r.data() + o, &v, 4); };
        auto w64 = [&](uint32_t o, uint64_t v) { std::memcpy(r.data() + o, &v, 8); };
        w32(si::kGidInfo + 4, 2); w32(si::kGidInfo + 8, 16);
        const uint8_t uuid[16] = {0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0,0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
        std::memcpy(r.data() + si::kGidInfo + 12, uuid, 16);
        w32(si::kFbRegionInfo, 2);
        const uint32_t r0 = si::kFbRegionInfo + 8, r1 = r0 + 0x30;
        w64(r0, 0); w64(r0 + 8, 0x1f76fffff); r[r0 + 28] = 1; r[r0 + 29] = 1;
        w64(r1, 0x1f7700000); w64(r1 + 8, 0x1ffffffff); w64(r1 + 16, 0x8900000); r[r1 + 30] = 1;
        w64(si::kBar1PdeBase, 0x1f76c3000); w64(si::kBar2PdeBase, 0x1f76c4000);
        w32(si::kInternalClient, 0xc1e00004); w32(si::kInternalSubdevice, 0x5d1d0001);
        std::memcpy(r.data() + si::kGpuName, "NVIDIA GeForce RTX 2080", 23);
        StaticInfo s;
        assert(ParseStaticInfo(r.data(), uint32_t(r.size()), &s));
        assert(s.uuid_ok && s.gid_flags == 2 && s.fb_regions == 2 && s.fb[0].usable() && !s.fb[1].usable());
        assert(s.usable_lo == 0 && s.usable_hi == 0x1f7700000ull);
        assert(s.bar1_pde == 0x1f76c3000ull && s.bar2_pde == 0x1f76c4000ull && s.h_internal_client == 0xc1e00004);
        assert(!std::strcmp(s.name, "NVIDIA GeForce RTX 2080"));
        char u[42]; FormatUuid(s.uuid, u);
        assert(!std::strcmp(u, "GPU-12345678-9abc-def0-0123-456789abcdef"));
        assert(VramRangeIsClient(s, 0, 0x10000000) && !VramRangeIsClient(s, 0x1f7600000, 0x1f7800000));
        assert(!ParseStaticInfo(r.data(), 0x600, &s));
        w32(si::kFbRegionInfo, 17);
        assert(!ParseStaticInfo(r.data(), uint32_t(r.size()), &s));
    }
    std::printf("test_nv_gsp_gmmu: OK\n");
    return 0;
}
