#pragma once
// Radix3 page table that maps the GSP-RM ELF .fwimage for booter/GSP.
// Level 0: one page, entry 0 = bus address of the level-1 page.
// Level 1: one page, one entry per level-2 page (<= 512).
// Level 2: up to 512 pages of 512 entries, one per 4 KiB image page.
// So the image is limited to 512 * 512 * 4 KiB = 1 GiB.
//
// Clean-room from Linux nouveau (MIT) nvkm/subdev/gsp/rm/r535/gsp.c 1630-1716
// (nvkm_gsp_radix3_sg). Difference: nouveau sizes level 2 from
// floor(size / 4 KiB) entries; Brook uses ceil, so a trailing partial page
// is always mapped (for the 570.144 image, 28528288 B, both give 14 pages).
#include <stdint.h>
#include <stddef.h>

namespace brook::nvgsp {

constexpr uint64_t kGspPageSize = 0x1000;
constexpr uint64_t kRadix3EntriesPerPage = kGspPageSize / sizeof(uint64_t);  // 512

struct Radix3Plan {
    uint64_t image_pages = 0;        // ceil(size / 4 KiB)
    uint64_t lvl2_pages = 0;         // ceil(image_pages / 512)
    uint64_t total_pages = 0;        // lvl0 + lvl1 + lvl2
    bool ok = false;
};
inline Radix3Plan PlanRadix3(uint64_t image_size) {
    Radix3Plan p;
    p.image_pages = (image_size + kGspPageSize - 1) / kGspPageSize;
    p.lvl2_pages = (p.image_pages + kRadix3EntriesPerPage - 1) / kRadix3EntriesPerPage;
    p.total_pages = 2 + p.lvl2_pages;
    p.ok = image_size != 0 && p.lvl2_pages <= kRadix3EntriesPerPage;
    return p;
}

// A page the builder writes: CPU pointer plus the bus address the GPU uses.
struct Radix3Page { uint64_t* cpu; uint64_t bus; };

// Fill the table. `lvl2` holds plan.lvl2_pages pages; `image_page(i)` returns
// the bus address of image page i (pages need not be contiguous). Unused
// level-1/level-2 slots are zeroed.
template <class ImagePageFn>
inline bool BuildRadix3(const Radix3Plan& plan, Radix3Page lvl0, Radix3Page lvl1,
                        const Radix3Page* lvl2, ImagePageFn image_page) {
    if (!plan.ok || !lvl0.cpu || !lvl1.cpu || ((lvl0.bus | lvl1.bus) & (kGspPageSize - 1))) return false;
    for (uint64_t i = 0; i < kRadix3EntriesPerPage; ++i) { lvl0.cpu[i] = 0; lvl1.cpu[i] = 0; }
    lvl0.cpu[0] = lvl1.bus;
    for (uint64_t j = 0; j < plan.lvl2_pages; ++j) {
        if (!lvl2[j].cpu || (lvl2[j].bus & (kGspPageSize - 1))) return false;
        lvl1.cpu[j] = lvl2[j].bus;
        for (uint64_t k = 0; k < kRadix3EntriesPerPage; ++k) {
            const uint64_t i = j * kRadix3EntriesPerPage + k;
            if (i < plan.image_pages) {
                const uint64_t a = image_page(i);
                if (a & (kGspPageSize - 1)) return false;
                lvl2[j].cpu[k] = a;
            } else {
                lvl2[j].cpu[k] = 0;
            }
        }
    }
    return true;
}

// Software walk, as booter does it: image offset -> bus address (0 on a
// hole). `read64(bus)` reads one table entry.
template <class Read64>
inline uint64_t Radix3Translate(uint64_t lvl0_bus, uint64_t offset, Read64 read64) {
    const uint64_t page = offset / kGspPageSize;
    const uint64_t l1 = read64(lvl0_bus);
    if (!l1) return 0;
    const uint64_t l2 = read64(l1 + (page / kRadix3EntriesPerPage) * 8);
    if (!l2) return 0;
    const uint64_t pg = read64(l2 + (page % kRadix3EntriesPerPage) * 8);
    if (!pg) return 0;
    return pg + offset % kGspPageSize;
}

} // namespace brook::nvgsp
